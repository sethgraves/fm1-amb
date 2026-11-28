/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Seth Graves; built on fm1-base by Charles Vestal */
/* TAPE's engine (tape.h): the voices, the echo, the room, the tape, the loop. */
#include "tape.h"
#include "fastmath.h"

#if defined PLAT_HOST
#define TP_POOL
#else
#define TP_POOL __attribute__((section(".pool")))
#endif

volatile tp_cfg_t tp_cfg = {TP_CASSETTE, TP_FLUTE, 0, 0, 0.3f, 0.5f, 0.5f, 0.4f, 0.3f, 0.35f, 0.4f, 0.4f,
                            0.5f, 0.4f, 8.0f, 1.0f, 0, 0.5f, 0.5f, 432.0f};
volatile float tp_speed = 1.0f, tp_reel_turns, tp_drop, tp_peak;
volatile uint32_t tp_notes, tp_drops, tp_drags;
volatile uint8_t tp_voices;

const char *const TP_TYPE_NAME[TP_NTYPES] = {"CASSETTE", "MICROCASSETTE", "VHS", "REEL TO REEL"};
const char *const TP_PATCH_NAME[TP_NPATCH] = {"M-FLUTE", "M-STRINGS", "M-CHOIR", "PAD", "BELL", "VIBES"};

/* ------------------------------------------------------------- helpers --- */
static uint32_t rng = 0x9E3779B9u;
static inline uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static inline float rndf(void) { return (float)(rnd() >> 8) * (1.0f / 16777216.0f); }   /* 0..1 */
static inline float rnds(void) { return rndf() * 2.0f - 1.0f; }                         /* -1..1 */
/* sine of a phase already in [0, 1): no floor, as fm_sin_turns needs for any phase */
static inline float sin01(float ph) { return fm_sin_pi((ph < 0.5f ? ph : ph - 1.0f) * 6.2831853f); }
/* a cheap sine (parabola, corrected): fine for a modulator, never heard alone */
static inline float psin01(float ph)
{
    float x = ph < 0.5f ? ph : ph - 1.0f, y = 8.0f * x - 16.0f * x * fm_fabsf(x);
    return y * (0.775f + 0.225f * fm_fabsf(y));
}
static inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float a4 = 432.0f;              /* the tuning, from tp_cfg once a block */
static float mtof(float m) { return a4 * fm_exp2f((m - 69.0f) * (1.0f / 12.0f)); }
static float onepole(float hz) { return 1.0f - fm_exp2f(-6.2831853f * 1.442695f * hz / TP_SR); }   /* LP coefficient */
static float decay(float s) { return fm_exp2f(-1.442695f / (s * TP_SR)); }                      /* per sample, to 1/e in s */

/* ----------------------------------------------------------- tape types --- */
/* each figure as new .. old (AGE 0 .. 1) */
typedef struct {
    float wow0, wow1, wow_hz;          /* wow, cents, and its rate */
    float flut0, flut1, flut_hz;       /* flutter, cents, and its rate */
    float lp0, lp1, hp;                /* the highs (Hz, new .. old), the lows cut */
    float bump_hz, bump_db;            /* head bump */
    float hiss0, hiss1, hiss_lp;       /* hiss, dB new .. old, and how bright */
    float drive0, drive1;              /* saturation into the tape */
    float width;                       /* stereo: 0 mono .. 1 */
    float drag;                        /* motor drags a minute, at AGE 1 */
    float drop;                        /* dropouts a second, at AGE 1 */
    float mis_ms;                      /* head misalignment at AGE 1, ms of the right against the left */
} deck_t;

static const deck_t DECKS[TP_NTYPES] = {
    /* cassette */
    {4, 18, 0.55f, 1.5f, 6, 8.5f, 12000, 3500, 30, 90, 2.5f, -66, -46, 9000, 1.4f, 2.6f, 0.85f, 1.5f, 0.6f,
     0.5f},
    /* microcassette: thin, wobbly, mono, hissy */
    {10, 40, 0.8f, 4, 12, 11.0f, 6000, 2200, 250, 180, 3.0f, -56, -38, 6500, 2.0f, 3.5f, 0.12f, 4.0f, 1.4f,
     0.0f},
    /* VHS (the linear audio track): slow wobble, dull, near mono, tracking dropouts */
    {3, 14, 0.3f, 2, 8, 6.0f, 9000, 3000, 60, 110, 2.0f, -62, -44, 7500, 1.6f, 2.8f, 0.3f, 2.0f, 1.0f,
     0.15f},
    /* reel to reel: wide, steady, quiet, warm */
    {1.5f, 9, 0.4f, 0.5f, 3, 7.0f, 17000, 6000, 20, 60, 3.5f, -74, -54, 12000, 1.6f, 3.0f, 1.0f, 0.6f, 0.25f,
     0.3f},
};

/* ---------------------------------------------------------------- notes in --- */
/* the main loop's notes to the render: a ring, one writer and one reader */
#define NQ 64u
static volatile uint16_t nq[NQ];
static volatile uint32_t nq_w, nq_r;

static void nq_put(uint16_t v)
{
    uint32_t w = nq_w;
    if (w - nq_r >= NQ)
        return;                        /* full: dropped (64 notes in one block) */
    nq[w & (NQ - 1u)] = v;
    nq_w = w + 1u;
}

/* bits 0..6 the note, 7 on, 8..14 the velocity, 15 held (HOLD's); 15 without 7: a sounding note becomes held */
void tp_note_on(int note, int vel, int held)
{
    nq_put((uint16_t)((note & 127) | 128 | ((vel & 127) << 8) | (held ? 0x8000u : 0u)));
}
void tp_note_hold(int note) { nq_put((uint16_t)((note & 127) | 0x8000u)); }
void tp_note_off(int note) { nq_put((uint16_t)(note & 127)); }
void tp_all_off(void) { nq_put(255u); }

/* ----------------------------------------------------------------- patches --- */
/* A Mellotron plays a strip of tape for each key: a recording of a real flute, strings, a choir, eight seconds long.
 * The three here are synthesised in that spirit, and every voice is its own strip of tape: it starts a little flat
 * as the tape comes up to speed, wobbles on its own, and (TAPE END) runs out after eight seconds. PAD and BELL are
 * synths, on the same tape. */
typedef struct {
    float att, rel;                    /* seconds, at ATTACK and RELEASE 0.5 */
    float level;
    float settle;                      /* cents flat at the start, coming up to speed (the Mellotron's tape) */
    float own_wow;                     /* each voice's own wobble, cents, at WOW 0.5 */
    float vib, vib_hz, vib_after;      /* the player's vibrato (in the recording): semitones, Hz, from (s) */
    float bright;                      /* the filter, Hz (strings, pad) */
} patch_t;

static const patch_t PATCHES[TP_NPATCH] = {
    /* M-FLUTE: breathy, a chiff at the start, a late vibrato */
    {0.07f, 0.12f, 0.31f, 18.0f, 3.0f, 0.10f, 4.8f, 0.35f, 0.0f},
    /* M-STRINGS: a bowed section, three saws a few cents apart, a slow bow */
    {0.28f, 0.22f, 0.41f, 22.0f, 3.0f, 0.06f, 5.6f, 0.25f, 2800.0f},
    /* M-CHOIR: "aah", saws through the vowel's formants, breath in them */
    {0.33f, 0.28f, 1.10f, 20.0f, 3.5f, 0.05f, 5.2f, 0.30f, 0.0f},
    /* PAD: warm detuned saws, a slow swell */
    {1.2f, 2.2f, 0.43f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 1300.0f},
    /* BELL: soft FM, ringing out */
    {0.002f, 1.5f, 0.63f, 0.0f, 1.5f, 0.0f, 0.0f, 0.0f, 0.0f},
    /* VIBES: a vibraphone's bar (its 1st, 4th and 10th partials), the motor's tremolo on it; the pedal down */
    {0.002f, 1.8f, 0.50f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
};
#define MOTOR_HZ 5.2f                  /* the vibraphone's motor: its discs turning in the tubes */
#define MOTOR_DEPTH 0.45f

/* the choir's vowel: "aah", three formants (Chamberlin's filter coefficient, Q) */
#define F1 730.0f
#define F2 1090.0f
#define F3 2440.0f

static inline float blep(float t, float dt)    /* polyBLEP: the saw's step, rounded */
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}


/* ------------------------------------------------------------------ voices --- */
#define NVOICE 8
typedef struct {
    uint8_t on, gate, note, patch, held;
    float vel, env, t, settle;
    float ph[3], inc[3];
    float lp1, lp2;                    /* strings, pad */
    float sv[3][2];                    /* choir: three formant filters (low, band) */
    float n1, n2, chiff, bpf;          /* flute's breath: a band of noise round its second harmonic */
    float mph, minc, idx, ring;        /* bell; vibes: ring, idx, mph the 1st, 4th, 10th partials' levels */
    float wph, whz, wdepth;            /* its own tape's wobble */
    float pan;
    uint32_t start;                    /* for stealing: the oldest goes */
} voice_t;
static voice_t vx[NVOICE];
static uint32_t vstart;
static float f_c1, f_c2, f_c3;         /* the formants' coefficients */

static void voice_on(int note, float vel, int patch, int held)
{
    int i, pick = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    for (i = 0; i < NVOICE; i++)       /* the same note again: that voice */
        if (vx[i].on && vx[i].note == note) {
            pick = i;
            break;
        }
    if (pick < 0)
        for (i = 0; i < NVOICE; i++)
            if (!vx[i].on) {
                pick = i;
                break;
            }
    if (pick < 0)                      /* all busy: the oldest released, else the oldest played; HOLD's last */
        for (i = 0; i < NVOICE; i++) {
            uint32_t age = vx[i].start + (vx[i].gate ? 0x40000000u : 0u) + (vx[i].held && vx[i].gate ? 0x80000000u : 0u);
            if (age < oldest) {
                oldest = age;
                pick = i;
            }
        }
    {
        voice_t *v = &vx[pick];
        int fresh = !v->on;
        v->on = 1;
        v->gate = 1;
        v->note = (uint8_t)note;
        v->patch = (uint8_t)patch;
        v->held = (uint8_t)held;
        v->vel = vel;
        v->t = 0.0f;
        v->settle = PATCHES[patch].settle * (0.7f + 0.6f * rndf());
        v->start = ++vstart;
        if (fresh) {
            v->env = 0.0f;
            v->lp1 = v->lp2 = 0.0f;
            v->ph[0] = rndf();
            v->ph[1] = rndf();
            v->ph[2] = rndf();
        }
        v->chiff = 1.0f;
        v->mph = 0.0f;
        v->idx = 1.2f + 1.6f * vel;
        v->ring = 1.0f;
        if (patch == TP_VIBES) {       /* the mallet: the upper partials by how hard */
            v->idx = 0.30f + 0.25f * vel;
            v->mph = 0.10f + 0.10f * vel;
            v->ph[0] = v->ph[1] = v->ph[2] = 0.0f;
        }
        v->wph = rndf();
        v->whz = 0.25f + 0.6f * rndf();
        v->wdepth = 0.6f + 0.8f * rndf();
        v->pan = ((float)((note * 7) % 12) / 11.0f - 0.5f) * 0.9f;   /* spread across the keys, not by pitch */
        tp_notes++;
    }
}

static void voice_off(int note)
{
    int i;
    for (i = 0; i < NVOICE; i++)
        if (vx[i].on && vx[i].note == note) {
            vx[i].gate = 0;
            vx[i].held = 0;
        }
}

static void voice_hold(int note)
{
    int i;
    for (i = 0; i < NVOICE; i++)
        if (vx[i].on && vx[i].gate && vx[i].note == note)
            vx[i].held = 1;
}

/* once a block: a voice's pitch (the note, its tape's settle and wobble, the vibrato) into its oscillators' steps,
 * and the tape's end */
static void voice_block(voice_t *v, float dt, float wowk, int tape_end, float age)
{
    const patch_t *p = &PATCHES[v->patch];
    float cents, m;
    v->t += dt;
    v->settle *= fm_exp2f(-dt * 1.442695f / 0.07f);   /* up to speed in ~70 ms */
    v->wph += v->whz * dt;
    if (v->wph >= 1.0f)
        v->wph -= 1.0f;
    cents = -v->settle + p->own_wow * v->wdepth * (2.0f * wowk) * (0.5f + age) * sin01(v->wph);
    if (p->vib > 0.0f && v->t > p->vib_after) {
        float k = fm_minf((v->t - p->vib_after) * 2.0f, 1.0f);
        cents += 100.0f * p->vib * k * fm_sin_turns(v->t * p->vib_hz);
    }
    m = (float)v->note + cents * 0.01f;
    {
        float f = mtof(m) / TP_SR;
        switch (v->patch) {
        case TP_STRINGS:
            v->inc[0] = f * 0.9954f;   /* -8 cents, 0, +7 */
            v->inc[1] = f;
            v->inc[2] = f * 1.0041f;
            break;
        case TP_CHOIR:
        case TP_PAD:
            v->inc[0] = f * 0.9965f;
            v->inc[1] = f * 1.0035f;
            v->inc[2] = f;
            break;
        case TP_BELL:
            v->inc[0] = f;
            v->minc = 2.0f * f;
            break;
        case TP_VIBES:                 /* a bar tuned 1 : 4 : 10 */
            v->inc[0] = f;
            v->inc[1] = fm_minf(4.0f * f, 0.45f);
            v->inc[2] = fm_minf(10.0f * f, 0.45f);
            break;
        default:                       /* the flute */
            v->inc[0] = f;
            v->bpf = 2.0f * fm_sinf(3.1415927f * fm_minf(2.0f * f, 0.14f));   /* round 2 f0, up to ~6 kHz */
            break;
        }
    }
    if (tape_end && v->gate && v->t > 8.0f && v->patch <= TP_CHOIR)
        v->gate = 0;                   /* the strip of tape has run out */
}

static float motor_ph, motor_gain = 1.0f;
/* one sample of a voice */
static inline float voice_tick(voice_t *v, float ka, float kr, float a_lp)
{
    float s = 0.0f;
    int o;
    v->env += ((v->gate ? 1.0f : 0.0f) - v->env) * (v->gate ? ka : kr);
    switch (v->patch) {
    case TP_FLUTE: {
        float s1, n;
        v->ph[0] += v->inc[0];
        if (v->ph[0] >= 1.0f)
            v->ph[0] -= 1.0f;
        s1 = sin01(v->ph[0]);
        s = s1 + 0.22f * (1.0f - 2.0f * s1 * s1) + 0.06f * s1 * (3.0f - 4.0f * s1 * s1);
        /* breath: noise through a narrow band round the second harmonic, so it sounds of the note, not as static;
         * a touch more for the first ~20 ms (the chiff) */
        n = rnds();
        v->n1 += v->bpf * v->n2;
        v->n2 += v->bpf * (n - v->n1 - 0.3f * v->n2);
        v->chiff *= 0.9989f;
        s = s * 0.7f + v->n2 * 0.05f * (1.0f + 1.5f * v->chiff);
        break;
    }
    case TP_STRINGS:
        for (o = 0; o < 3; o++) {
            v->ph[o] += v->inc[o];
            if (v->ph[o] >= 1.0f)
                v->ph[o] -= 1.0f;
            s += 2.0f * v->ph[o] - 1.0f - blep(v->ph[o], v->inc[o]);
        }
        v->lp1 += (s - v->lp1) * a_lp;
        v->lp2 += (v->lp1 - v->lp2) * a_lp;
        s = v->lp2 * 0.4f;
        break;
    case TP_CHOIR: {
        float x = 0.0f, br = rnds() * 0.08f;
        for (o = 0; o < 2; o++) {
            v->ph[o] += v->inc[o];
            if (v->ph[o] >= 1.0f)
                v->ph[o] -= 1.0f;
            x += 2.0f * v->ph[o] - 1.0f - blep(v->ph[o], v->inc[o]);
        }
        x = x * 0.5f + br;
        /* Chamberlin's state-variable filter, three times, band-pass: the vowel */
        v->sv[0][0] += f_c1 * v->sv[0][1];
        v->sv[0][1] += f_c1 * (x - v->sv[0][0] - 0.16f * v->sv[0][1]);
        v->sv[1][0] += f_c2 * v->sv[1][1];
        v->sv[1][1] += f_c2 * (x - v->sv[1][0] - 0.14f * v->sv[1][1]);
        v->sv[2][0] += f_c3 * v->sv[2][1];
        v->sv[2][1] += f_c3 * (x - v->sv[2][0] - 0.12f * v->sv[2][1]);
        s = (v->sv[0][1] * 0.16f + v->sv[1][1] * 0.09f + v->sv[2][1] * 0.035f);
        break;
    }
    case TP_PAD:
        for (o = 0; o < 2; o++) {
            v->ph[o] += v->inc[o];
            if (v->ph[o] >= 1.0f)
                v->ph[o] -= 1.0f;
            s += 2.0f * v->ph[o] - 1.0f - blep(v->ph[o], v->inc[o]);
        }
        v->lp1 += (s - v->lp1) * a_lp;
        v->lp2 += (v->lp1 - v->lp2) * a_lp;
        s = v->lp2 * 0.5f;
        break;
    case TP_VIBES:
        for (o = 0; o < 3; o++) {
            v->ph[o] += v->inc[o];
            if (v->ph[o] >= 1.0f)
                v->ph[o] -= 1.0f;
        }
        s = (sin01(v->ph[0]) * v->ring + sin01(v->ph[1]) * v->idx + sin01(v->ph[2]) * v->mph) * motor_gain;
        v->ring *= 0.999985f;          /* the bar: ~1.5 s to half */
        v->idx *= 0.99965f;            /* the 4th: ~45 ms */
        v->mph *= 0.9985f;             /* the 10th: ~10 ms, the mallet */
        break;
    default: {                         /* BELL */
        float ph;
        v->mph += v->minc;
        if (v->mph >= 1.0f)
            v->mph -= 1.0f;
        v->ph[0] += v->inc[0];
        if (v->ph[0] >= 1.0f)
            v->ph[0] -= 1.0f;
        ph = v->ph[0] + v->idx * 0.159f * psin01(v->mph);
        ph -= (float)(int32_t)(ph + 1.0f) - 1.0f;
        s = sin01(ph) * v->ring;
        v->ring *= 0.99997f;           /* it rings out over a few seconds, held or not */
        v->idx *= 0.99992f;
        break;
    }
    }
    if (v->env < 0.0002f && !v->gate)
        v->on = 0;
    return s * v->env;
}

/* ------------------------------------------------------------- the echo --- */
/* A tape echo: its own loop of tape at 14.7 kHz, 16-bit, up to 1.1 s; the repeats darker and a little driven each
 * time round, and the delay glides when TIME moves (the pitch bends, as a tape echo's does). */
#define EN 16384u
#define EDIV 3u
static int16_t ebuf[EN] TP_POOL;
static uint32_t ew, esub;
static float eacc, eold, enew, ed = 4000.0f, elp, ehp;

static inline float echo_tick(float in, float time_s, float fb, float a_tone, float wob)
{
    float t;
    eacc += in;
    if (++esub >= EDIV) {
        float x = eacc * (1.0f / (float)EDIV), p, f, y, target = fm_clampf(time_s, 0.05f, 1.1f) * (TP_SR / EDIV);
        int32_t i;
        esub = 0;
        eacc = 0.0f;
        ed += (target - ed) * 0.0004f;                 /* ~170 ms to a new time */
        p = (float)ew - ed - wob;
        if (p < 0.0f)
            p += (float)EN;
        i = (int32_t)p;
        f = p - (float)i;
        {
            float a = (float)ebuf[(uint32_t)i & (EN - 1u)], b = (float)ebuf[(uint32_t)(i + 1) & (EN - 1u)];
            y = (a + (b - a) * f) * (1.0f / 32767.0f);
        }
        elp += (y - elp) * a_tone;                     /* each repeat darker */
        ehp += (elp - ehp) * 0.01f;                    /* and thinner at the bottom */
        y = elp - ehp;
        ebuf[ew] = (int16_t)(fm_clampf(x + fm_tanhf(y * fb * 1.3f) * (1.0f / 1.3f), -1.0f, 1.0f) * 32767.0f);
        ew = (ew + 1u) & (EN - 1u);
        eold = enew;
        enew = y;
    }
    t = (float)esub * (1.0f / (float)EDIV);
    return eold + (enew - eold) * t;
}

/* ---------------------------------------------------------------- the room --- */
/* four delay lines mixed by a Hadamard matrix (an FDN), 16-bit, with two diffusers in front */
#define RV_SCALE 16384.0f
static const uint16_t RV_LEN[4] = {1693, 2029, 2459, 2879};
static int16_t rv_buf[1693 + 2029 + 2459 + 2879] TP_POOL;
static int16_t *rv_line[4];
static uint32_t rv_pos[4];
static float rv_lp[4], ap1[142], ap2[379];
static uint32_t ap1_p, ap2_p;

static inline float allpass(float *b, uint32_t len, uint32_t *p, float x, float g)
{
    float d = b[*p], y = d - g * x;
    b[*p] = x + g * y;
    if (++*p >= len)
        *p = 0;
    return y;
}

static inline void room(float in, float fb, float damp, float *l, float *r)
{
    float o[4], m[4];
    int i;
    in = allpass(ap1, 142, &ap1_p, in, 0.6f);
    in = allpass(ap2, 379, &ap2_p, in, 0.6f);
    for (i = 0; i < 4; i++)
        o[i] = (float)rv_line[i][rv_pos[i]] * (1.0f / RV_SCALE);
    m[0] = 0.5f * (o[0] + o[1] + o[2] + o[3]);
    m[1] = 0.5f * (o[0] - o[1] + o[2] - o[3]);
    m[2] = 0.5f * (o[0] + o[1] - o[2] - o[3]);
    m[3] = 0.5f * (o[0] - o[1] - o[2] + o[3]);
    for (i = 0; i < 4; i++) {
        float v;
        rv_lp[i] += (m[i] * fb - rv_lp[i]) * damp;
        v = fm_clampf(rv_lp[i] + in * 0.5f, -1.9f, 1.9f);
        rv_line[i][rv_pos[i]] = (int16_t)(v * RV_SCALE);
        if (++rv_pos[i] >= RV_LEN[i])
            rv_pos[i] = 0;
    }
    *l = o[0] + 0.6f * o[2];
    *r = o[1] + 0.6f * o[3];
}

/* ------------------------------------------------------------------ the deck --- */
/* the tape between the heads: a stereo delay of 16-bit samples, read at a moving point */
#define DL 4096u
#define D0 1900.0f
static int16_t tape_l[DL] TP_POOL, tape_r[DL] TP_POOL;
static uint32_t tw;
static float wow_ph, flut_ph, wow_rw, wow_tg, flut_rw, drag_v, drag_off, drag_left, d_prev = D0;
static float hp_l, hp_r, lp1_l, lp2_l, lp1_r, lp2_r, az_r;
static float bq_b[3], bq_a[2], bq_z[2][2];
static float drop_g = 1.0f, drop_t, drop_depth, drop_len;
static float wander_l, wander_r, wander_tl, wander_tr;
static float hiss_lp[2], hiss_hp[2];

/* the deck's figures for a type and age, worked out once a block and glided */
typedef struct {
    float wow_a, flut_a, wow_hz, flut_hz, lp, hp, hiss, hiss_c, drive, width, drag, drop, mis, bump_db, bump_hz;
} deckp_t;
static deckp_t dk, dk_t;

/* wowk, flutk: the WOW and FLUTTER knobs, 0..1, the tape's own at 0.5 */
static void deck_target(const deck_t *d, float age, float drive, float wowk, float flutk)
{
    float a2 = age * age, c;
    c = lerp(d->wow0, d->wow1, age) * 0.000578f;       /* cents to a speed ratio, then to samples of delay */
    dk_t.wow_a = c * TP_SR / (6.2831853f * d->wow_hz) * 2.0f * wowk;
    dk_t.wow_hz = d->wow_hz;
    c = lerp(d->flut0, d->flut1, age) * 0.000578f;
    dk_t.flut_a = c * TP_SR / (6.2831853f * d->flut_hz) * 2.0f * flutk;
    dk_t.flut_hz = d->flut_hz;
    dk_t.lp = d->lp0 * fm_powf(d->lp1 / d->lp0, age);   /* the highs go, exponentially */
    dk_t.hp = d->hp;
    dk_t.hiss = fm_db2lin(lerp(d->hiss0, d->hiss1, age));
    dk_t.hiss_c = d->hiss_lp;
    dk_t.drive = lerp(d->drive0, d->drive1, age) * (0.6f + 1.4f * drive);
    dk_t.width = d->width;
    dk_t.drag = d->drag * a2;
    dk_t.drop = d->drop * a2 * age;
    dk_t.mis = d->mis_ms * age * TP_SR * 0.001f;
    dk_t.bump_db = d->bump_db;
    dk_t.bump_hz = d->bump_hz;
}

static void glide_deck(int snap)
{
    float k = snap ? 1.0f : 0.05f, *a = (float *)&dk, *b = (float *)&dk_t;
    uint32_t i;
    for (i = 0; i < sizeof dk / sizeof(float); i++)
        a[i] += (b[i] - a[i]) * k;
}

static void bump_set(float hz, float db)                /* RBJ's peaking filter, Q 0.8 */
{
    float A = fm_exp2f(db * (1.0f / 40.0f) * 3.3219281f), w = 6.2831853f * hz / TP_SR, s = fm_sinf(w),
          c = fm_cosf(w), al = s / 1.6f, a0 = 1.0f + al / A;
    bq_b[0] = (1.0f + al * A) / a0;
    bq_b[1] = (-2.0f * c) / a0;
    bq_b[2] = (1.0f - al * A) / a0;
    bq_a[0] = (-2.0f * c) / a0;
    bq_a[1] = (1.0f - al / A) / a0;
}

static inline float bump(int ch, float x)
{
    float y = bq_b[0] * x + bq_z[ch][0];
    bq_z[ch][0] = bq_b[1] * x - bq_a[0] * y + bq_z[ch][1];
    bq_z[ch][1] = bq_b[2] * x - bq_a[1] * y;
    return y;
}

static inline float tape_read(const int16_t *t, float d)
{
    float p = (float)tw - d, f;
    int32_t i;
    if (p < 0.0f)
        p += (float)DL;
    i = (int32_t)p;
    f = p - (float)i;
    {
        float a = (float)t[i & (DL - 1u)], b = (float)t[(i + 1) & (DL - 1u)];
        return (a + (b - a) * f) * (1.0f / 32767.0f);
    }
}

/* -------------------------------------------------------------- the loop --- */
/* After Basinski: the loop records the machine's output all the time, the last LENGTH seconds, unheard. FREEZE stops
 * the recording: what was just played goes round and round, and every pass the oxide comes off a little more, where
 * it already was coming off; BASINSKI is how much of it is heard. FREEZE again lets it go: a clean tape, recording. A cell's damage only grows: it dulls, quietens, then drops out,
 * and its neighbours start to go. Each pass writes in its own hiss, and the 8-bit mu-law adds its grain. */
uint8_t tp_damage[TP_NCELL];
volatile uint32_t tp_loop_pos, tp_loop_len = 176400u, tp_passes;
volatile uint8_t tp_frozen;
static uint8_t loop[TP_LOOP_BYTES] TP_POOL;
static float ulaw_dec[256];
static uint32_t lsub;
static float lacc, lold, lnew, lwear, lgen;
static float rpos, spd = 1.0f, lsum; /* frozen: the head (loop samples, fractional), its speed, the input since */
static uint32_t lcnt;
static int32_t wlast = -1;              /* the loop sample last worn (and written) */
volatile float tp_loop_speed = 1.0f;
static float haas[512];                /* the loop's right: its left a few ms later, for some width */
static uint32_t haas_w;

float tp_loop_max(void) { return (float)TP_LOOP_BYTES * (float)TP_LOOP_DIV / TP_SR; }

static uint8_t ulaw_enc(float x)                 /* x -1..1 */
{
    int32_t v = (int32_t)(fm_clampf(x, -1.0f, 1.0f) * 32635.0f), sign = 0, e, m;
    if (v < 0) {
        v = -v;
        sign = 0x80;
    }
    v += 0x84;
    for (e = 7; e > 0 && !(v & (0x4000 >> (7 - e))); e--)
        ;
    m = (v >> (e + 3)) & 0x0F;
    return (uint8_t)~(sign | (e << 4) | m);
}

static void loop_init(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        int u = ~i & 0xFF, sign = u & 0x80, e = (u >> 4) & 7, m = u & 0x0F;
        int32_t v = (((m << 3) + 0x84) << e) - 0x84;
        ulaw_dec[i] = (float)(sign ? -v : v) * (1.0f / 32768.0f);
    }
}

static void loop_clean(void)            /* a clean tape: no damage, no passes */
{
    uint32_t i;
    for (i = 0; i < TP_NCELL; i++)
        tp_damage[i] = 0;
    tp_passes = 0;
}

/* a pass ended: the damage grows where it is, flakes into its neighbours, and now and then starts somewhere new */
static void loop_wear(float decay, uint32_t ncell)
{
    uint32_t i, wear = (uint32_t)(decay * 100.0f), seeds = 1u + wear / 8u;
    if (!wear || !ncell)
        return;
    for (i = 0; i < ncell; i++) {
        uint32_t d = tp_damage[i];
        if (d) {
            d += 1u + (d * wear) / 900u;
            if (d > 255u)
                d = 255u;
            tp_damage[i] = (uint8_t)d;
            if (d > 40u && (rnd() & 63u) < wear / 6u) {
                uint32_t j = (i + ((rnd() & 1u) ? 1u : ncell - 1u)) % ncell;
                if (tp_damage[j] < 6u)
                    tp_damage[j] = 6u;
            }
        }
    }
    while (seeds--)
        if ((rnd() % 100u) < wear) {
            i = rnd() % ncell;
            if (!tp_damage[i])
                tp_damage[i] = 3u;
        }
}

/* a frozen loop's sample, as the head passes it: worn a little more (and, overdubbing, what was played since
 * the last one added) */
static void loop_pass_sample(uint32_t i, float decay, float add, int overdub)
{
    float x = ulaw_dec[loop[i]], d = (float)tp_damage[i / TP_CELL] * (1.0f / 255.0f), keep;
    lwear += (x - lwear) * (1.0f - 0.85f * d);                 /* worn spots go dull */
    lgen += (lwear - lgen) * (1.0f - 0.12f * decay);           /* and every pass a little dull everywhere */
    keep = (1.0f - 0.3f * d * d) * (1.0f - 0.006f * decay);   /* and quiet */
    if (d > 0.55f && (rnd() & 255u) < (uint32_t)(d * 70.0f))
        keep = 0.0f;                                           /* and gone */
    x = lgen * keep + rnds() * 0.0005f * (0.3f + decay);      /* the pass writes its own hiss */
    if (overdub)
        x = x * 0.85f + add * 0.8f;                            /* sound on sound: the old under the new */
    loop[i] = ulaw_enc(x);
}

/* one sample of the machine in; the loop's playback out (left, and the right a little later). Not frozen: it records
 * (the playback is what it records, LENGTH ago). Frozen: the head moves at the loop's speed, gliding (a reel's
 * weight), forwards or back; each sample it passes wears, and takes what is played if overdubbing. */
static inline void loop_tick(float in, float decay, float speed, int overdub, float *ol, float *or_)
{
    float t, y;
    uint32_t len = tp_loop_len;
    if (!tp_frozen) {
        lacc += in;
        if (++lsub >= TP_LOOP_DIV) {
            uint32_t pos = tp_loop_pos;
            float x = ulaw_dec[loop[pos]];
            lsub = 0;
            lwear = lgen = x;
            loop[pos] = ulaw_enc(lacc * (1.0f / (float)TP_LOOP_DIV));
            lold = lnew;
            lnew = x;
            lacc = 0.0f;
            if (++pos >= len)
                pos = 0;
            tp_loop_pos = pos;
        }
        t = (float)lsub * (1.0f / (float)TP_LOOP_DIV);
        y = lold + (lnew - lold) * t;
    } else {
        int32_t ip;
        uint32_t i1;
        float a, b;
        spd += (speed - spd) * 0.00004f;               /* ~0.6 s: the reel's weight */
        lsum += in;
        lcnt++;
        rpos += spd * (1.0f / (float)TP_LOOP_DIV);
        if (rpos >= (float)len) {                      /* round again, forwards or back: a pass */
            rpos -= (float)len;
            loop_wear(decay, (len + TP_CELL - 1u) / TP_CELL);
            tp_passes++;
        } else if (rpos < 0.0f) {
            rpos += (float)len;
            loop_wear(decay, (len + TP_CELL - 1u) / TP_CELL);
            tp_passes++;
        }
        ip = (int32_t)rpos;
        if (ip >= (int32_t)len)
            ip = (int32_t)len - 1;
        if (ip != wlast) {
            loop_pass_sample((uint32_t)ip, decay, lsum / (float)lcnt, overdub);
            wlast = ip;
            lsum = 0.0f;
            lcnt = 0;
        }
        i1 = (uint32_t)ip + 1u >= len ? 0u : (uint32_t)ip + 1u;
        a = ulaw_dec[loop[ip]];
        b = ulaw_dec[loop[i1]];
        y = a + (b - a) * (rpos - (float)ip);
        tp_loop_pos = (uint32_t)ip;
        tp_loop_speed = spd;
    }
    *ol = y;
    *or_ = haas[(haas_w - 300u) & 511u];              /* ~7 ms */
    haas[haas_w] = y;
    haas_w = (haas_w + 1u) & 511u;
}

/* ------------------------------------------------------------------ render --- */
static float gain_now, fz;              /* fz: the loop heard, 0..1, gliding with FREEZE */

void tp_init(void)
{
    uint32_t i, off = 0;
    for (i = 0; i < 4; i++) {
        rv_line[i] = rv_buf + off;
        off += RV_LEN[i];
    }
    for (i = 0; i < DL; i++)
        tape_l[i] = tape_r[i] = 0;
    for (i = 0; i < EN; i++)
        ebuf[i] = 0;
    f_c1 = 2.0f * fm_sinf(3.1415927f * F1 / TP_SR);
    f_c2 = 2.0f * fm_sinf(3.1415927f * F2 / TP_SR);
    f_c3 = 2.0f * fm_sinf(3.1415927f * F3 / TP_SR);
    deck_target(&DECKS[tp_cfg.type < TP_NTYPES ? tp_cfg.type : 0], tp_cfg.age, tp_cfg.drive, tp_cfg.wow,
                tp_cfg.flutter);
    glide_deck(1);
    bump_set(dk.bump_hz, dk.bump_db);
    loop_init();
    for (i = 0; i < TP_LOOP_BYTES; i++)
        loop[i] = 0xFF;                /* mu-law silence */
    loop_clean();
}

void tp_panic(void) { gain_now = 0.0f; }

void tp_render(int32_t *lr, uint32_t n, float gain)
{
    const tp_cfg_t c = tp_cfg;
    uint32_t i, k;
    float dt = (float)n / TP_SR, fb = 0.70f + 0.24f * c.space, wet = c.space * 0.55f,
          damp = onepole(lerp(5500.0f, 3200.0f, c.space)), pk = 0.0f;
    float a_lp, a_lpd, a_hp, a_az, a_hiss, sat_norm, ka[TP_NPATCH], kr[TP_NPATCH], a_str, a_pad, a_etone, wob_e;
    int nv = 0;

    a4 = fm_clampf(c.a4, 400.0f, 460.0f);
    /* the notes that came in */
    while (nq_r != nq_w) {
        uint16_t e = nq[nq_r & (NQ - 1u)];
        nq_r++;
        if (e == 255u) {
            for (k = 0; k < NVOICE; k++) {
                vx[k].gate = 0;
                vx[k].held = 0;
            }
        } else if (e & 128u) {
            voice_on(e & 127, (float)((e >> 8) & 127) * (1.0f / 127.0f), c.patch < TP_NPATCH ? c.patch : 0,
                     (e & 0x8000u) != 0u);
        } else if (e & 0x8000u) {
            voice_hold(e & 127);
        } else {
            voice_off(e & 127);
        }
    }
    /* FREEZE: a clean tape on letting go */
    if (c.freeze && !tp_frozen) {                      /* frozen: the head from where the recording was, at 1 */
        loop_clean();
        rpos = (float)tp_loop_pos;
        spd = 1.0f;
        wlast = -1;
        lsum = 0.0f;
        lcnt = 0;
        tp_loop_speed = 1.0f;
        tp_frozen = 1;
    } else if (!c.freeze && tp_frozen) {               /* let go: recording again from where the head is */
        tp_frozen = 0;
        tp_loop_speed = 1.0f;
        loop_clean();
    }
    {
        uint32_t len = (uint32_t)(fm_clampf(c.length, TP_LOOP_MIN, tp_loop_max()) * TP_SR / (float)TP_LOOP_DIV);
        len = len > TP_LOOP_BYTES ? TP_LOOP_BYTES : len;
        if (tp_loop_pos >= len)
            tp_loop_pos = 0;
        if (rpos >= (float)len)                        /* a shorter loop: the head into it */
            rpos = (float)((uint32_t)rpos % len);
        tp_loop_len = len;
    }

    /* the deck's figures */
    deck_target(&DECKS[c.type < TP_NTYPES ? c.type : 0], c.age, c.drive, c.wow, c.flutter);
    glide_deck(0);
    bump_set(dk.bump_hz, dk.bump_db);
    a_lp = onepole(dk.lp * (1.0f - 0.75f * tp_drop));
    a_lpd = onepole(dk.lp * 0.8f * (1.0f - 0.75f * tp_drop));
    a_hp = onepole(dk.hp);
    a_az = onepole(fm_maxf(dk.lp * (1.0f - 0.4f * c.age), 1500.0f));
    a_hiss = onepole(dk.hiss_c);
    sat_norm = 0.5f / fm_tanhf(dk.drive * 0.5f);

    /* the voices: envelopes' rates by patch (ATTACK and RELEASE scale them, x0.1 .. x10), pitches once a block */
    for (k = 0; k < TP_NPATCH; k++) {
        float as = PATCHES[k].att * fm_exp2f((c.attack - 0.5f) * 6.64f), rs = PATCHES[k].rel * fm_exp2f((c.release - 0.5f) * 6.64f);
        ka[k] = 1.0f - fm_exp2f(-1.442695f / (fm_maxf(as, 0.001f) * TP_SR) * 3.0f);
        kr[k] = 1.0f - fm_exp2f(-1.442695f / (fm_maxf(rs, 0.005f) * TP_SR) * 3.0f);
    }
    for (k = 0; k < NVOICE; k++)
        if (vx[k].on) {
            voice_block(&vx[k], dt, c.wow, c.tape_end, c.age);
            nv++;
        }
    tp_voices = (uint8_t)nv;
    a_str = onepole(PATCHES[TP_STRINGS].bright);
    a_pad = onepole(PATCHES[TP_PAD].bright);
    a_etone = onepole(lerp(4200.0f, 1800.0f, c.age)) * 3.0f;    /* at 14.7 kHz */
    if (a_etone > 1.0f)
        a_etone = 1.0f;

    {
        if (drag_left <= 0.0f && rndf() < dk.drag * (float)n / (60.0f * TP_SR)) {   /* the motor labours */
            drag_left = (0.4f + 1.4f * rndf()) * TP_SR;
            drag_v = 0.004f + 0.025f * rndf() * c.age;
            tp_drags++;
        }
        if (drop_t <= 0.0f && rndf() < dk.drop * (float)n / TP_SR) {   /* a dropout: the oxide thin here */
            drop_len = (0.02f + 0.25f * rndf() * rndf()) * TP_SR;
            drop_t = drop_len;
            drop_depth = 0.35f + 0.6f * rndf();
            tp_drops++;
        }
        if (rndf() < (float)n / (1.5f * TP_SR))
            wow_tg = rnds();
        if (rndf() < (float)n / (3.0f * TP_SR)) {       /* the channels wander */
            wander_tl = rnds() * c.age * 0.16f;
            wander_tr = rnds() * c.age * 0.16f;
        }
        wob_e = dk.wow_a * 0.33f * sin01(wow_ph);        /* the echo's tape wobbles with the deck's */

        for (i = 0; i < n; i++) {
            float l = 0.0f, r = 0.0f, x, d, dd, rl, rr, f0, f1;
            /* the vibraphone's motor: one for all its bars */
            motor_ph += MOTOR_HZ / TP_SR;
            if (motor_ph >= 1.0f)
                motor_ph -= 1.0f;
            motor_gain = 1.0f - MOTOR_DEPTH * (0.5f + 0.5f * sin01(motor_ph));
            /* the voices */
            for (k = 0; k < NVOICE; k++) {
                voice_t *v = &vx[k];
                if (!v->on)
                    continue;
                x = voice_tick(v, ka[v->patch], kr[v->patch], v->patch == TP_PAD ? a_pad : a_str)
                    * PATCHES[v->patch].level * (0.55f + 0.45f * v->vel);
                l += x * (0.5f - 0.5f * v->pan);
                r += x * (0.5f + 0.5f * v->pan);
            }
            /* the loop: it records the voices, the last LENGTH seconds; frozen, it plays them back into the echo,
             * the room and the tape with what is played now, BASINSKI the balance */
            {
                float ll, lrr, b;
                loop_tick(0.5f * (l + r), c.decay, c.loop_speed, c.overdub, &ll, &lrr);
                fz += ((tp_frozen ? 1.0f : 0.0f) - fz) * 0.0005f;   /* ~50 ms */
                b = c.basinski * fz;
                l = l * (1.0f - b) + ll * b * 1.6f;   /* the loop a shade up: the mu-law and the wear take some */
                r = r * (1.0f - b) + lrr * b * 1.6f;
            }
            /* the echo */
            if (c.echo > 0.005f || c.echo_fb > 0.0f) {
                x = echo_tick(0.5f * (l + r), c.echo_time, c.echo_fb, a_etone, wob_e);
                l += x * c.echo;
                r += x * c.echo;
            }
            /* the room */
            {
                float wl, wr;
                room(0.5f * (l + r), fb, damp, &wl, &wr);
                l += wl * wet;
                r += wr * wet;
            }

            /* the record head: saturation, a little bias for the even harmonics */
            l = (fm_tanhf(dk.drive * l + 0.05f) - 0.04995837f) * sat_norm;
            r = (fm_tanhf(dk.drive * r + 0.05f) - 0.04995837f) * sat_norm;
            tape_l[tw] = (int16_t)(fm_clampf(l, -1.0f, 1.0f) * 32767.0f);
            tape_r[tw] = (int16_t)(fm_clampf(r, -1.0f, 1.0f) * 32767.0f);

            /* the transport: wow, flutter, drag, as a moving delay */
            wow_ph += dk.wow_hz / TP_SR;
            if (wow_ph >= 1.0f)
                wow_ph -= 1.0f;
            flut_ph += dk.flut_hz / TP_SR;
            if (flut_ph >= 1.0f)
                flut_ph -= 1.0f;
            wow_rw += (wow_tg - wow_rw) * 0.00004f;
            flut_rw += (rnds() * 14.0f - flut_rw) * 0.002f;
            if (drag_left > 0.0f) {
                drag_left -= 1.0f;
                drag_off += drag_v;
            } else if (drag_off > 0.0f) {
                drag_off -= 0.0015f + drag_off * 0.00002f;
                if (drag_off < 0.0f)
                    drag_off = 0.0f;
            }
            drag_off = fm_minf(drag_off, 1500.0f);
            d = D0 + dk.wow_a * (sin01(wow_ph) + 1.5f * wow_rw) + dk.flut_a * (psin01(flut_ph) + 2.0f * flut_rw)
                + drag_off;
            d = fm_clampf(d, 4.0f, (float)DL - 40.0f);
            dd = d - d_prev;
            d_prev = d;
            tp_speed = 1.0f - dd;

            /* the play head */
            rl = tape_read(tape_l, d);
            rr = tape_read(tape_r, d + dk.mis);
            tw = (tw + 1u) & (DL - 1u);

            /* the playback: lows cut, head bump, highs gone, dropouts */
            hp_l += (rl - hp_l) * a_hp;
            hp_r += (rr - hp_r) * a_hp;
            rl = bump(0, rl - hp_l);
            rr = bump(1, rr - hp_r);
            lp1_l += (rl - lp1_l) * a_lp;
            lp2_l += (lp1_l - lp2_l) * a_lp;
            lp1_r += (rr - lp1_r) * a_lpd;
            lp2_r += (lp1_r - lp2_r) * a_lpd;
            az_r += (lp2_r - az_r) * a_az;
            rl = lp2_l;
            rr = az_r;
            if (drop_t > 0.0f) {
                float x2 = drop_t / drop_len;
                drop_t -= 1.0f;
                drop_g += ((1.0f - drop_depth * sin01(0.5f * x2)) - drop_g) * 0.01f;
            } else {
                drop_g += (1.0f - drop_g) * 0.004f;
            }
            tp_drop = 1.0f - drop_g;
            wander_l += (wander_tl - wander_l) * 0.00003f;
            wander_r += (wander_tr - wander_r) * 0.00003f;
            rl *= drop_g * (1.0f + wander_l) * 2.0f;
            rr *= drop_g * (1.0f + wander_r) * 2.0f;

            /* the tape's own hiss */
            for (k = 0; k < 2; k++) {
                float h = rnds();
                hiss_hp[k] += (h - hiss_hp[k]) * 0.11f;
                hiss_lp[k] += ((h - hiss_hp[k]) - hiss_lp[k]) * a_hiss;
            }
            rl += hiss_lp[0] * dk.hiss * 3.0f;
            rr += hiss_lp[1] * dk.hiss * 3.0f;

            /* the width */
            f0 = 0.5f * (rl + rr);
            f1 = 0.5f * (rl - rr) * dk.width;
            l = f0 + f1;
            r = f0 - f1;

            gain_now += (gain - gain_now) * 0.004f;
            l *= gain_now;
            r *= gain_now;
            if (fm_fabsf(l) > pk)
                pk = fm_fabsf(l);
            if (fm_fabsf(r) > pk)
                pk = fm_fabsf(r);
            lr[2 * i] = (int32_t)(fm_clampf(l, -1.0f, 1.0f) * 8388000.0f);
            lr[2 * i + 1] = (int32_t)(fm_clampf(r, -1.0f, 1.0f) * 8388000.0f);
        }
    }
    tp_reel_turns += dt * 0.6f * tp_speed * (tp_frozen ? tp_loop_speed : 1.0f);   /* the reels: the loop's speed */
    if (pk > tp_peak)
        tp_peak = pk;
}
