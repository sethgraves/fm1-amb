/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Seth Graves; built on fm1-base by Charles Vestal */
/* The drone (drone.h). One compilation unit, -O2. */
#include "drone.h"
#include "fastmath.h"

#if defined PLAT_HOST
#define DR_POOL
#else
#define DR_POOL __attribute__((section(".pool")))
#endif
#define BARRIER() __asm__ volatile("" ::: "memory")

/* the sine: 2048 points a turn plus one, read with linear interpolation (error about -130 dB, under
 * fm_sinf's own -120 dB). Indexed by the phase's top 11 bits; the 21 below are the fraction. */
#define TBITS 11
#define TN (1 << TBITS)
#define FBITS (32 - TBITS)
static float sint[TN + 1] DR_POOL;

/* the hand-over: main loop -> render */
static dr_state_t cur, pend;
static volatile uint8_t pend_full;

/* the render's own state */
static uint32_t ph[DR_MAXT];               /* each slot's phase, 2^32 a turn */
static uint16_t h_now[DR_MAXT];            /* the harmonic each slot sounds now (0: silent) */
static float al[DR_MAXT], ar[DR_MAXT];     /* each slot's left and right gain now */
static float unit_now;                     /* Hz: glides to cur.unit_hz */
static float gain_now;
static uint8_t started;

volatile float dr_level[DR_MAXT];
volatile uint32_t dr_unit_inc;
volatile float dr_peak;

void dr_init(void)
{
    int i;
    for (i = 0; i <= TN; i++)
        sint[i] = fm_sinf((float)i * (FM_TWO_PI / (float)TN));
    sint[TN] = sint[0];
    for (i = 0; i < DR_MAXT; i++) {
        ph[i] = 0;
        h_now[i] = 0;
        al[i] = ar[i] = 0.0f;
        dr_level[i] = 0.0f;
    }
    cur.n = 0;
    cur.playing = 0;
    cur.glide_s = 0.3f;
    cur.fade_s = 4.0f;
    unit_now = 0.0f;
    gain_now = 0.0f;
    started = 0;
    pend_full = 0;
}

int dr_commit(const dr_state_t *s)
{
    if (pend_full)
        return 0;
    pend = *s;
    BARRIER();
    pend_full = 1;
    return 1;
}

void dr_panic(void)
{
    int i;
    for (i = 0; i < DR_MAXT; i++) {
        al[i] = ar[i] = 0.0f;
        h_now[i] = 0;
        dr_level[i] = 0.0f;
    }
    cur.playing = 0;
}

#define BLK 64u
static float bl[BLK], br[BLK];

/* one block of k <= BLK frames */
static void block(int32_t *out, uint32_t k, float gain)
{
    uint32_t j, inc1;
    int i;
    const float fk = (float)k;
    float g0, g1, dg;

    /* the unit glides; every tone's step is a whole multiple of the unit's, so the ratios stay exact */
    if (!started) {
        unit_now = cur.unit_hz;            /* the first block: no glide up from nothing */
        started = 1;
    } else {
        float c = fm_minf(fk / (fm_maxf(cur.glide_s, 0.001f) * 0.25f * DR_SR), 1.0f);
        unit_now += (cur.unit_hz - unit_now) * c;
    }
    inc1 = (uint32_t)(fm_maxf(unit_now, 0.0f) * (4294967296.0f / DR_SR) + 0.5f);
    dr_unit_inc = inc1;

    for (j = 0; j < k; j++)
        bl[j] = br[j] = 0.0f;

    for (i = 0; i < DR_MAXT; i++) {
        float lvl = dr_level[i], tgt, c, gl, gr, l0, r0, l1, r1, dl, dr;
        uint32_t p, inc;
        int wanted = i < cur.n;
        uint16_t h = wanted ? cur.harm[i] : 0;
        if (h_now[i] == 0 && lvl <= 0.0f && !wanted)
            continue;                      /* an empty slot: nothing to do */
        {
            float tc = fm_maxf(cur.fade_s, 0.01f) * 0.25f;   /* a swell's time constant */
            if (h_now[i] != h && h_now[i]) {  /* a new harmonic for this slot: the old goes first, quicker (~2 s) */
                tc *= 0.2f;
                if (lvl > 1e-4f) {
                    tgt = 0.0f;
                } else {
                    h_now[i] = h;
                    lvl = 0.0f;
                    tc *= 5.0f;
                    tgt = h && cur.playing ? cur.amp[i] : 0.0f;
                }
            } else {
                h_now[i] = h;                 /* (an empty slot takes its harmonic at once) */
                tgt = h && cur.playing ? cur.amp[i] : 0.0f;
            }
            c = fm_minf(fk / (tc * DR_SR), 1.0f);
        }
        lvl += (tgt - lvl) * c;
        if (lvl < 1e-6f && tgt == 0.0f)
            lvl = 0.0f;
        dr_level[i] = lvl;
        if (h_now[i] == 0) {
            al[i] = ar[i] = 0.0f;
            continue;
        }
        {   /* equal-power pan */
            float a = (fm_clampf(i < cur.n ? cur.pan[i] : 0.0f, -1.0f, 1.0f) + 1.0f) * (0.25f * FM_PI);
            gl = fm_cosf(a);
            gr = fm_sinf(a);
        }
        l0 = al[i];
        r0 = ar[i];
        l1 = lvl * gl;
        r1 = lvl * gr;
        dl = (l1 - l0) / fk;
        dr = (r1 - r0) / fk;
        p = ph[i];
        inc = inc1 * (uint32_t)h_now[i];   /* exact: h times the unit's step */
        if (i < cur.n)
            inc += (uint32_t)cur.drift[i];  /* and the drift, if any */
        for (j = 0; j < k; j++) {
            uint32_t ix = p >> FBITS;
            float f = (float)(p & ((1u << FBITS) - 1u)) * (1.0f / (float)(1u << FBITS));
            float s = sint[ix] + (sint[ix + 1] - sint[ix]) * f;
            bl[j] += s * l0;
            br[j] += s * r0;
            l0 += dl;
            r0 += dr;
            p += inc;
        }
        ph[i] = p;
        al[i] = l1;
        ar[i] = r1;
        if (lvl == 0.0f && tgt == 0.0f && !wanted)
            h_now[i] = 0;
    }

    /* out: linear, 0.9 of full scale at most; the clamp is a guard that should never act */
    g0 = gain_now;
    g1 = gain;
    dg = (g1 - g0) / fk;
    {
        float pk = dr_peak;
        for (j = 0; j < k; j++) {
            float l = bl[j] * g0 * 0.9f, r = br[j] * g0 * 0.9f;
            float m = fm_maxf(fm_fabsf(l), fm_fabsf(r));
            if (m > pk)
                pk = m;
            l = fm_clampf(l, -1.0f, 1.0f);
            r = fm_clampf(r, -1.0f, 1.0f);
            out[2 * j] = (int32_t)(l * 8388607.0f);
            out[2 * j + 1] = (int32_t)(r * 8388607.0f);
            g0 += dg;
        }
        dr_peak = pk;
    }
    gain_now = g1;
}

void dr_render(int32_t *lr, uint32_t n, float gain)
{
    if (pend_full) {
        cur = pend;
        BARRIER();
        pend_full = 0;
    }
    while (n) {
        uint32_t k = n > BLK ? BLK : n;
        block(lr, k, gain);
        lr += 2 * k;
        n -= k;
    }
}
