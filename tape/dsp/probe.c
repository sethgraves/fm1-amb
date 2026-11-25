/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Seth Graves; built on fm1-base by Charles Vestal */
/* The probe's engine (probe.h). Float only, no libm (fastmath.h), no 64-bit multiplies (felucca-bsp docs/CPU.md:
 * on the FM-1 they misbehave beside 16-bit loads, and a tape loop is all loads). */
#include "probe.h"
#include "fastmath.h"
#include "plat.h"

#if defined PLAT_HOST
#define PR_POOL
#else
#define PR_POOL __attribute__((section(".pool")))
#endif

volatile pr_cfg_t pr_cfg = {1, 3, 512, 4, 1, 40};
volatile uint32_t pr_passes, pr_frames, pr_render_us_max;
uint8_t pr_damage[PR_NSEG];

static uint32_t rng = 0x2545F491u;
static inline uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static inline float rndf(void) { return (float)(rnd() >> 8) * (1.0f / 16777216.0f); }   /* 0..1 */

/* ------------------------------------------------------------------ pad --- */
/* four chords, each four notes of two saws a few cents apart, through a one-pole low-pass */
static const float CHORD[4][4] = {{48, 55, 62, 64}, {45, 52, 57, 64}, {41, 48, 55, 60}, {43, 50, 57, 59}};
static float ph[8], inc[8], inc_t[8], lp[4], pad_amp;
static uint32_t pad_t, pad_chord;

static float mtof(float m) { return 440.0f * fm_exp2f((m - 69.0f) * (1.0f / 12.0f)); }

static void pad_set(uint32_t c)
{
    int i;
    for (i = 0; i < 4; i++) {
        float f = mtof(CHORD[c & 3u][i]) * (1.0f / PR_SR);
        inc_t[2 * i] = f * 0.9971f;
        inc_t[2 * i + 1] = f * 1.0029f;
    }
}

static inline float pad_tick(void)
{
    float s = 0.0f;
    int i;
    if (++pad_t >= (uint32_t)(8.0f * PR_SR)) {   /* a new chord every 8 s */
        pad_t = 0;
        pad_set(++pad_chord);
    }
    for (i = 0; i < 8; i++) {
        inc[i] += (inc_t[i] - inc[i]) * 0.0002f;   /* a slow glide between chords */
        ph[i] += inc[i];
        if (ph[i] >= 1.0f)
            ph[i] -= 1.0f;
    }
    for (i = 0; i < 4; i++) {
        float v = (ph[2 * i] + ph[2 * i + 1]) - 1.0f;
        lp[i] += (v - lp[i]) * 0.05f;
        s += lp[i];
    }
    pad_amp += (0.3f - pad_amp) * 0.00005f;     /* swells in at the start */
    return s * pad_amp;
}

/* ----------------------------------------------------------------- loop --- */
/* mu-law (G.711) bytes: 8 bits that hold about 13 bits of range, noisier when loud, which is tape enough */
static uint8_t loop[PR_LOOP_BYTES] PR_POOL;
static float ulaw_dec[256];
static uint32_t lpos, lsub;
static float lacc, lold, lnew, lwear_lp, lhiss;

static uint8_t ulaw_enc(float x)                 /* x -1..1 */
{
    int32_t s = (int32_t)(fm_clampf(x, -1.0f, 1.0f) * 32635.0f), sign = 0, e, m;
    if (s < 0) {
        s = -s;
        sign = 0x80;
    }
    s += 0x84;
    for (e = 7; e > 0 && !(s & (0x4000 >> (7 - e))); e--)
        ;
    m = (s >> (e + 3)) & 0x0F;
    return (uint8_t)~(sign | (e << 4) | m);
}

static void ulaw_table(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        int u = ~i & 0xFF, sign = u & 0x80, e = (u >> 4) & 7, m = u & 0x0F;
        int32_t s = ((m << 3) + 0x84) << e;
        s -= 0x84;
        ulaw_dec[i] = (float)(sign ? -s : s) * (1.0f / 32768.0f);
    }
}

void pr_loop_reset(void)
{
    uint32_t i;
    for (i = 0; i < PR_LOOP_BYTES; i++)
        loop[i] = 0xFF;                          /* mu-law silence */
    for (i = 0; i < PR_NSEG; i++)
        pr_damage[i] = 0;
    lpos = lsub = 0;
    lacc = lold = lnew = lwear_lp = 0.0f;
    pr_passes = 0;
}

uint32_t pr_head(void) { return lpos; }
float pr_loop_seconds(void) { return (float)PR_LOOP_BYTES * (float)pr_cfg.loop_div / PR_SR; }

/* a pass ended: the damage grows where it is, spreads a little to its neighbours, and now and then starts anew */
static void wear_pass(uint32_t wear)
{
    uint32_t i, seeds = 1u + wear / 5u;
    for (i = 0; i < PR_NSEG; i++) {
        uint32_t d = pr_damage[i];
        if (d) {
            d += 2u + (d * wear) / 800u;
            if (d > 255u)
                d = 255u;
            pr_damage[i] = (uint8_t)d;
            if (d > 40u && (rnd() & 63u) < wear / 8u) {     /* the oxide flakes outward */
                uint32_t j = (i + ((rnd() & 1u) ? 1u : PR_NSEG - 1u)) % PR_NSEG;
                if (pr_damage[j] < 8u)
                    pr_damage[j] = 8u;
            }
        }
    }
    while (seeds--)
        if ((rnd() % 100u) < wear) {
            i = rnd() % PR_NSEG;
            if (!pr_damage[i])
                pr_damage[i] = 4u;
        }
}

/* one input sample in, one sample of the loop's playback out (at the full rate, between its own samples) */
static inline float loop_tick(float in)
{
    uint32_t div = pr_cfg.loop_div;
    float t;
    lacc += in;
    if (++lsub >= div) {                         /* one sample of the loop: read, wear, write back */
        float x, d, keep, feed = (pr_passes < 1u) ? 1.0f : 0.0f;   /* the probe records one pass, then only wears */
        uint32_t seg = lpos / PR_SEG;
        lsub = 0;
        x = ulaw_dec[loop[lpos]];
        d = (float)pr_damage[seg] * (1.0f / 255.0f);
        lwear_lp += (x - lwear_lp) * (1.0f - 0.85f * d);        /* worn spots go dull */
        x = lwear_lp;
        keep = 1.0f - 0.25f * d * d;                            /* and quiet */
        if (d > 0.6f && (rnd() & 255u) < (uint32_t)(d * 60.0f))
            keep = 0.0f;                                        /* and drop out */
        lhiss = (rndf() - 0.5f) * 0.0006f;                      /* the oxide's own noise, written in every pass */
        x = x * keep + lhiss;
        lold = lnew;
        lnew = x;
        loop[lpos] = ulaw_enc(x + feed * lacc / (float)div);
        lacc = 0.0f;
        if (++lpos >= PR_LOOP_BYTES) {
            lpos = 0;
            pr_passes++;
            wear_pass(pr_cfg.wear);
        }
    }
    t = (float)lsub / (float)div;
    return lold + (lnew - lold) * t + (pr_passes < 1u ? in : 0.0f);   /* the first pass: the pad heard as it records */
}

/* ------------------------------------------------------------- spectral --- */
#define NMAX PR_FFT_MAX
static float win256[256], win512[512], win1024[1024];    /* sqrt-Hann: analysis and synthesis both */
static float tw_c[NMAX / 2], tw_s[NMAX / 2];
#define OM (2u * NMAX - 1u)
typedef struct {
    float in[NMAX];                              /* the last N inputs, a ring */
    float out[2u * NMAX];                        /* overlap-add, a ring twice as long: a frame never laps itself */
} chan_t;
static chan_t ch[2] PR_POOL;
static float fre[NMAX] PR_POOL, fim[NMAX] PR_POOL;      /* the frame being worked on, one channel at a time */
static uint32_t sp_w, sp_r, sp_hop_t, sp_n, sp_ov;   /* input index, output index (the next read), samples to the
                                                     * next hop, size and overlap in use */
static uint32_t sp_drop;                         /* frames still dropped (a "packet" lost) */

static void win_make(float *w, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        float h = 0.5f - 0.5f * fm_cosf(6.2831853f * (float)i / (float)n);
        w[i] = fm_sqrtf(h);
    }
}

static const float *win_of(uint32_t n) { return n == 256u ? win256 : n == 512u ? win512 : win1024; }

/* in place, radix 2, n a power of two up to NMAX; inv: the inverse (unscaled) */
static void fft(float *re, float *im, uint32_t n, int inv)
{
    uint32_t i, j = 0, len, k, stride;
    for (i = 1; i < n; i++) {                    /* bit reversal */
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1) {
        uint32_t half = len >> 1;
        stride = NMAX / len;
        for (i = 0; i < n; i += len)
            for (k = 0; k < half; k++) {
                float c = tw_c[k * stride], s = inv ? tw_s[k * stride] : -tw_s[k * stride];
                uint32_t a = i + k, b = a + half;
                float xr = re[b] * c - im[b] * s, xi = re[b] * s + im[b] * c;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
    }
}

/* the damage in the bins, after a low-bitrate codec: a cut-off, magnitudes in coarse steps, frames lost */
static void bins(float *re, float *im, uint32_t n, int drop)
{
    uint32_t k, cut = n * 9u / 40u;              /* ~10 kHz at any size */
    if (drop) {
        for (k = 0; k < n; k++)
            re[k] = im[k] = 0.0f;
        return;
    }
    for (k = 1; k <= n / 2u; k++) {
        float m2 = re[k] * re[k] + im[k] * im[k], g;
        if (k > cut || m2 < 1e-9f) {
            g = 0.0f;
        } else {
            float l = 0.5f * fm_log2f(m2), q = fm_floorf(l * 1.5f) * (1.0f / 1.5f);   /* 4 dB steps */
            g = fm_exp2f(q - l);
        }
        re[k] *= g;
        im[k] *= g;
        if (k < n / 2u) {                        /* the mirror bin, so the frame stays real */
            re[n - k] = re[k];
            im[n - k] = -im[k];
        }
    }
    im[0] = 0.0f;
    im[n / 2u] = 0.0f;
}

static void sp_frame(int nch)
{
    uint32_t n = sp_n, i, idx;
    const float *w = win_of(n);
    float norm = 2.0f / ((float)n * (float)sp_ov);   /* sqrt-Hann twice sums to ov/2; the IFFT is unscaled */
    int c, drop = 0;
    if (sp_drop) {
        sp_drop--;
        drop = 1;
    } else if ((rnd() & 1023u) < 6u) {
        sp_drop = 1u + (rnd() & 3u);             /* a lost packet, a few frames long */
    }
    for (c = 0; c < nch; c++) {
        chan_t *h = &ch[c];
        for (i = 0; i < n; i++) {
            idx = (sp_w + NMAX - n + i) & (NMAX - 1u);
            fre[i] = h->in[idx] * w[i];
            fim[i] = 0.0f;
        }
        fft(fre, fim, n, 0);
        bins(fre, fim, n, drop);
        fft(fre, fim, n, 1);
        for (i = 0; i < n; i++)                  /* from the next read on: the output is n samples late */
            h->out[(sp_r + i) & OM] += fre[i] * w[i] * norm;
    }
    pr_frames++;
}

static void sp_reset(uint32_t n, uint32_t ov)
{
    uint32_t i;
    int c;
    for (c = 0; c < 2; c++)
        for (i = 0; i < 2u * NMAX; i++) {
            if (i < NMAX)
                ch[c].in[i] = 0.0f;
            ch[c].out[i] = 0.0f;
        }
    sp_n = n;
    sp_ov = ov;
    sp_hop_t = n / ov;
    sp_drop = 0;
}

/* --------------------------------------------------------------- render --- */
static float gain_now;
static uint32_t cfg_n, cfg_ov;

void pr_init(void)
{
    uint32_t i;
    ulaw_table();
    win_make(win256, 256u);
    win_make(win512, 512u);
    win_make(win1024, 1024u);
    for (i = 0; i < NMAX / 2u; i++) {
        tw_c[i] = fm_cosf(6.2831853f * (float)i / (float)NMAX);
        tw_s[i] = fm_sinf(6.2831853f * (float)i / (float)NMAX);
    }
    pad_set(0);
    for (i = 0; i < 8; i++)
        inc[i] = inc_t[i];
    pr_loop_reset();
    sp_reset(512u, 4u);
}

void pr_panic(void) { gain_now = 0.0f; }

void pr_render(int32_t *lr, uint32_t n, float gain)
{
    uint32_t t0 = plat_ticks24(), i, us;
    uint32_t fn = pr_cfg.fft_n, ov = pr_cfg.overlap;
    int loop_on = pr_cfg.loop_on, nch = pr_cfg.stereo ? 2 : 1;
    if (fn != cfg_n || ov != cfg_ov) {           /* a new size: start the layer over */
        cfg_n = fn;
        cfg_ov = ov;
        if (fn)
            sp_reset(fn, ov);
    }
    for (i = 0; i < n; i++) {
        float x = pad_tick(), l, r;
        if (loop_on)
            x = loop_tick(x);
        l = x;
        r = x;
        if (fn) {
            uint32_t w = sp_w;
            ch[0].in[w] = l;
            ch[1].in[w] = 0.6f * r + 0.4f * ch[1].in[(w + NMAX - 1u) & (NMAX - 1u)];   /* the right a shade darker */
            sp_w = (w + 1u) & (NMAX - 1u);
            l = ch[0].out[sp_r];
            r = nch > 1 ? ch[1].out[sp_r] : l;
            ch[0].out[sp_r] = 0.0f;
            ch[1].out[sp_r] = 0.0f;
            sp_r = (sp_r + 1u) & OM;
            if (--sp_hop_t == 0u) {
                sp_hop_t = fn / ov;
                sp_frame(nch);
            }
        }
        gain_now += (gain - gain_now) * 0.004f;
        lr[2 * i] = (int32_t)(fm_clampf(l * gain_now, -1.0f, 1.0f) * 8388000.0f);
        lr[2 * i + 1] = (int32_t)(fm_clampf(r * gain_now, -1.0f, 1.0f) * 8388000.0f);
    }
    us = (plat_ticks24() - t0) / PLAT_TICKS_PER_US;
    if (us > pr_render_us_max)
        pr_render_us_max = us;
}
