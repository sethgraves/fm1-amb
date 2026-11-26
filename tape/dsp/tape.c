/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Seth Graves; built on fm1-base by Charles Vestal */
/* TAPE v1's engine (tape.h): the music, the room, the tape. */
#include "tape.h"
#include "fastmath.h"

#if defined PLAT_HOST
#define TP_POOL
#else
#define TP_POOL __attribute__((section(".pool")))
#endif

volatile tp_cfg_t tp_cfg = {1, TP_CASSETTE, 0, 0.3f, 0.5f, 0.5f, 0.4f, 0.8f, 0.6f, 0.4f, 0.3f,
                            0.0f, 0.0f, 0.4f, 12.0f, 0};
volatile float tp_speed = 1.0f, tp_reel_turns, tp_drop, tp_peak;
volatile uint8_t tp_chord;
volatile uint32_t tp_notes, tp_ghosts, tp_drops, tp_drags;

const char *const TP_TYPE_NAME[TP_NTYPES] = {"CASSETTE", "MICROCASSETTE", "VHS", "REEL TO REEL"};
const char *const TP_SCENE_NAME[TP_NSCENES] = {"GLIDE", "SUNROOM", "MEMORY", "RIVER"};

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
static float mtof(float m) { return 440.0f * fm_exp2f((m - 69.0f) * (1.0f / 12.0f)); }
static float onepole(float hz) { return 1.0f - fm_exp2f(-6.2831853f * 1.442695f * hz / TP_SR); }   /* LP coefficient */
static float decay(float s) { return fm_exp2f(-1.442695f / (s * TP_SR)); }                      /* per sample, to 1/e in s */

/* --------------------------------------------------------------- scenes --- */
/* A scene is a whole arrangement, not just a key: its chords and how long each lasts, how fast the pads glide and
 * how bright they are, where the bells play and what they sound like, how busy the bells and the flute are, and how
 * far forward the stream comes. */
typedef struct {
    uint8_t root, bpm;
    uint8_t nchord, bars;              /* chords in the cycle (1: one held chord), bars of 4/4 each */
    int8_t chord[4][4], bass[4];
    uint8_t nscale;
    int8_t scale[7];
    float glide_s;                     /* the pads' glide to a new chord, seconds */
    float pad_hz;                      /* the pads' brightness (their filter, Hz) */
    float pad_lvl;                     /* the pads' level, against the defaults */
    uint8_t sixteenths;                /* 1: the bells step in sixteenths */
    uint8_t bell_lo, bell_hi;          /* the bells' register, MIDI */
    float bell_ratio, bell_index, bell_s;   /* their FM: 2 a warm bell, 1 a soft pluck; brightness; ring, seconds */
    float bell_busy;                   /* how often a bell plays a step, against DENSITY */
    uint8_t bell_random;               /* 1: any scale note, scattered (drops), not an arpeggio */
    float flute_busy;                  /* how often the flute starts a phrase */
    float nature_lift;                 /* the stream's level, against NATURE */
    int8_t pad_shift;                  /* the pads' register, semitones from the middle */
} scene_t;

static const scene_t SCENES[TP_NSCENES] = {
    /* GLIDE: D lydian, floating. The pads lead with long glides; the bells are few. */
    {50, 66, 4, 4, {{0, 4, 7, 11}, {2, 6, 9, 0}, {9, 0, 4, 7}, {5, 9, 0, 4}}, {0, 0, 9, 5}, 7, {0, 2, 4, 6, 7, 9, 11},
     4.0f, 1300.0f, 1.15f, 0, 66, 88, 2.0f, 1.0f, 2.6f, 0.45f, 0, 0.6f, 0.7f, 0},
    /* SUNROOM: F major, bright and moving. Quicker chords, the bells running high in sixteenths, the flute out
     * front. */
    {53, 84, 4, 2, {{0, 4, 7, 2}, {4, 7, 11, 2}, {5, 9, 4, 11}, {7, 9, 2, 4}}, {0, 4, 5, 7}, 7, {0, 2, 4, 5, 7, 9, 11},
     1.0f, 2600.0f, 0.85f, 1, 72, 96, 2.0f, 1.8f, 1.6f, 0.75f, 0, 2.5f, 0.6f, 4},
    /* MEMORY: A minor (dorian), low and slow. Dark pads, the bells a soft low pluck with a long ring, long chords. */
    {45, 52, 4, 8, {{0, 3, 10, 2}, {8, 0, 3, 7}, {10, 2, 5, 7}, {7, 10, 2, 5}}, {0, 8, 10, 7}, 7, {0, 2, 3, 5, 7, 9, 10},
     2.5f, 750.0f, 1.2f, 0, 52, 72, 1.0f, 0.7f, 4.5f, 0.5f, 0, 0.5f, 0.5f, -7},
    /* RIVER: Eb pentatonic, one chord held for good. The bells fall as scattered drops; the stream comes forward. */
    {51, 60, 1, 4, {{0, 4, 7, 2}}, {0}, 5, {0, 2, 4, 7, 9},
     3.0f, 1000.0f, 0.8f, 0, 70, 94, 3.0f, 0.6f, 1.2f, 1.0f, 1, 0.4f, 1.8f, 0},
};

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
    float print_s, print0, print1;     /* print-through: a wrap's time, ghost level dB new .. old */
    float mis_ms;                      /* head misalignment at AGE 1, ms of the right against the left */
} deck_t;

static const deck_t DECKS[TP_NTYPES] = {
    /* cassette */
    {4, 18, 0.55f, 1.5f, 6, 8.5f, 12000, 3500, 30, 90, 2.5f, -66, -46, 9000, 1.4f, 2.6f, 0.85f, 1.5f, 0.6f,
     0.9f, -42, -28, 0.5f},
    /* microcassette: thin, wobbly, mono, hissy */
    {10, 40, 0.8f, 4, 12, 11.0f, 6000, 2200, 250, 180, 3.0f, -56, -38, 6500, 2.0f, 3.5f, 0.12f, 4.0f, 1.4f,
     0.6f, -46, -32, 0.0f},
    /* VHS (the linear audio track): slow wobble, dull, near mono, tracking dropouts */
    {3, 14, 0.3f, 2, 8, 6.0f, 9000, 3000, 60, 110, 2.0f, -62, -44, 7500, 1.6f, 2.8f, 0.3f, 2.0f, 1.0f,
     1.2f, -48, -36, 0.15f},
    /* reel to reel: wide, steady, quiet, warm; print-through its best-known flaw */
    {1.5f, 9, 0.4f, 0.5f, 3, 7.0f, 17000, 6000, 20, 60, 3.5f, -74, -54, 12000, 1.6f, 3.0f, 1.0f, 0.6f, 0.25f,
     1.5f, -36, -24, 0.3f},
};

/* ---------------------------------------------------------------- events --- */
/* the generator plans every bell and flute note LOOK ahead, so a print-through ghost can sound before it */
#define LOOK ((uint32_t)(1.6f * TP_SR))
#define NEV 64
enum { EV_NONE, EV_BELL, EV_FLUTE };
typedef struct {
    uint32_t t;
    uint8_t kind, ghost;
    float note, vel, pan, dur;
} ev_t;
static ev_t evq[NEV];
static uint32_t now;                   /* samples since the start */

static void ev_add(uint32_t t, int kind, int ghost, float note, float vel, float pan, float dur)
{
    int i;
    for (i = 0; i < NEV; i++)
        if (evq[i].kind == EV_NONE) {
            ev_t *e = &evq[i];
            e->t = t;
            e->kind = (uint8_t)kind;
            e->ghost = (uint8_t)ghost;
            e->note = note;
            e->vel = vel;
            e->pan = pan;
            e->dur = dur;
            return;
        }
}

/* a note planned at t, with its ghosts a wrap before and after when the tape prints through */
static float print_db = -40.0f, print_s = 1.0f;
static void plan(uint32_t t, int kind, float note, float vel, float pan, float dur)
{
    uint32_t p = (uint32_t)(print_s * TP_SR);
    float g = fm_db2lin(print_db);
    ev_add(t, kind, 0, note, vel, pan, dur);
    if (g > 0.004f) {
        ev_add(t - p, kind, 1, note, vel * g * 0.7f, pan, dur);   /* the pre-echo: the layer outside, a little quieter */
        ev_add(t + p, kind, 1, note, vel * g, pan, dur);
    }
}

/* ------------------------------------------------------------------ pads --- */
#define NPAD 4
typedef struct {
    float pitch, target;               /* MIDI, gliding */
    float ph[2], drift[2], dv[2];      /* two saws, each with its own slow drift (cents) */
    float lp1, lp2, breath, bph, bhz;
} pad_t;
static pad_t pads[NPAD];
static float bass_pitch, bass_target, bass_ph, bass_lp, pad_cut = 1200.0f, pad_cut_ph;

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

static int nearest(float from, int pc, int root, int lo, int hi)   /* the pitch of class pc nearest from */
{
    int best = lo, bd = 1000, m;
    for (m = lo; m <= hi; m++)
        if (((m - root) % 12 + 12) % 12 == pc) {
            int d = m - (int)(from + 0.5f);
            d = d < 0 ? -d : d;
            if (d < bd) {
                bd = d;
                best = m;
            }
        }
    return best;
}

static void pads_to(const scene_t *s, int c)   /* the chord's tones, each voice to its nearest */
{
    int v;
    for (v = 0; v < NPAD; v++)
        pads[v].target = (float)nearest(pads[v].target, s->chord[c][v], s->root, 52 + 3 * v + s->pad_shift,
                                        66 + 3 * v + s->pad_shift);
    bass_target = (float)nearest(bass_target, s->bass[c], s->root, 36 + (s->pad_shift < 0 ? -3 : 0), 48);
}

/* ----------------------------------------------------------------- bells --- */
#define NBELL 10
typedef struct {
    float ph, inc, mph, minc, amp, dec, idx, idec, att, pan, lp;
    uint8_t on, ghost;
} bell_t;
static bell_t bells[NBELL];

static const scene_t *bell_scene;
static void bell_on(const ev_t *e)
{
    int i, pick = 0;
    float least = 10.0f, f = mtof(e->note);
    for (i = 0; i < NBELL; i++) {      /* a free voice, or the quietest */
        float a = bells[i].on ? bells[i].amp : -1.0f;
        if (a < least) {
            least = a;
            pick = i;
        }
    }
    {
        bell_t *b = &bells[pick];
        b->ph = 0.0f;
        b->mph = 0.0f;
        b->inc = f / TP_SR;
        b->minc = bell_scene->bell_ratio * f / TP_SR;   /* 2:1 a warm bell; 1:1 a soft pluck; 3:1 glassy */
        b->amp = e->vel;
        b->dec = decay(e->ghost ? 0.45f * bell_scene->bell_s : bell_scene->bell_s);
        b->idx = e->ghost ? 0.4f : bell_scene->bell_index * (1.0f + 2.5f * e->vel);
        b->idec = decay(0.35f);
        b->att = 0.0f;
        b->pan = e->pan;
        b->lp = 0.0f;
        b->on = 1;
        b->ghost = e->ghost;
    }
}

static inline void bells_tick(float *l, float *r)
{
    int i;
    for (i = 0; i < NBELL; i++) {
        bell_t *b = &bells[i];
        float s;
        if (!b->on)
            continue;
        b->att += (1.0f - b->att) * 0.02f;           /* ~1 ms: no click */
        b->mph += b->minc;
        if (b->mph >= 1.0f)
            b->mph -= 1.0f;
        b->ph += b->inc;
        if (b->ph >= 1.0f)
            b->ph -= 1.0f;
        {
            float p = b->ph + b->idx * 0.159f * psin01(b->mph);
            p -= (float)(int32_t)(p + 1.0f) - 1.0f;   /* back into [0, 1) (the index stays under a turn) */
            s = sin01(p) * b->amp * b->att;
        }
        if (b->ghost) {                              /* a ghost comes through the layers of tape: dull */
            b->lp += (s - b->lp) * 0.08f;
            s = b->lp;
        }
        b->amp *= b->dec;
        b->idx *= b->idec;
        if (b->amp < 0.0005f)
            b->on = 0;
        *l += s * (0.5f - 0.5f * b->pan);
        *r += s * (0.5f + 0.5f * b->pan);
    }
}

/* ----------------------------------------------------------------- flute --- */
/* two voices: 0 plays, 1 its print-through ghosts */
typedef struct {
    float pitch, target, ph, inc, amp, gate, vib_t, vph, chiff, n1, n2, pan, vel;
    uint32_t until;
    uint8_t ghost;
} flute_t;
static flute_t flutes[2];

static void flute_on(const ev_t *e)
{
    flute_t *f = &flutes[e->ghost ? 1 : 0];
    if (f->amp < 0.01f) {
        f->pitch = e->note;            /* from silence: start on the note */
        f->vib_t = 0.0f;
    }
    f->target = e->note;
    f->gate = 1.0f;
    f->vel = e->vel;
    f->chiff = f->amp < 0.3f ? 1.0f : 0.4f;
    f->pan = e->pan;
    f->until = now + (uint32_t)(e->dur * TP_SR);
    f->ghost = e->ghost;
}

/* once a block: the slur and the vibrato into a phase step */
static void flute_block(flute_t *f, uint32_t n)
{
    float dt = (float)n / TP_SR, vib;
    if (f->amp < 0.0002f && f->gate == 0.0f)
        return;
    f->pitch += (f->target - f->pitch) * (1.0f - fm_exp2f(-dt * 1.442695f / 0.06f));   /* a soft slur, ~60 ms */
    f->vib_t += dt;
    f->vph += 5.1f * dt;
    if (f->vph >= 1.0f)
        f->vph -= 1.0f;
    vib = fm_minf(f->vib_t * 2.0f, 1.0f) * 0.12f * sin01(f->vph);                     /* the vibrato comes late */
    f->inc = mtof(f->pitch + vib) / TP_SR;
}

static inline float flute_tick(flute_t *f)
{
    float s, s1, n;
    if (f->gate > 0.0f && (int32_t)(now - f->until) >= 0)
        f->gate = 0.0f;
    f->amp += ((f->gate * f->vel) - f->amp) * (f->gate > 0.0f ? 0.00019f : 0.00008f);   /* ~120 ms in, ~280 out */
    if (f->amp < 0.0002f && f->gate == 0.0f)
        return 0.0f;
    f->ph += f->inc;
    if (f->ph >= 1.0f)
        f->ph -= 1.0f;
    s1 = sin01(f->ph);                                 /* the second and third harmonics from the first */
    s = s1 + 0.22f * (1.0f - 2.0f * s1 * s1) + 0.06f * s1 * (3.0f - 4.0f * s1 * s1);
    n = rnds();                                        /* breath: noise round the second harmonic */
    f->n1 += (n - f->n1) * 0.35f;
    f->n2 += (f->n1 - f->n2) * 0.08f;
    f->chiff *= 0.99985f;
    s = s * 0.7f + (f->n1 - f->n2) * (0.9f + 2.5f * f->chiff);
    return s * f->amp * (f->ghost ? 0.6f : 1.0f);
}

/* ---------------------------------------------------------------- nature --- */
static float st_l[2], st_r[2], st_wave, st_ph;
typedef struct {
    float ph, f0, f1, t, len, amp;
    int left;                          /* chirps left in this song */
    uint32_t next;
    float pan;
} bird_t;
static bird_t bird;
static float drop_ph, drop_inc, drop_amp, drop_pan;

static inline void nature_tick(float lvl, float *l, float *r)
{
    float nl = rnds(), nr = rnds(), a, s;
    st_l[0] += (nl - st_l[0]) * 0.3f;                  /* the stream: noise between ~400 Hz and ~2.5 kHz */
    st_l[1] += (st_l[0] - st_l[1]) * 0.055f;
    st_r[0] += (nr - st_r[0]) * 0.3f;
    st_r[1] += (st_r[0] - st_r[1]) * 0.055f;
    st_ph += 0.13f / TP_SR;
    if (st_ph >= 1.0f)
        st_ph -= 1.0f;
    st_wave = 0.7f + 0.3f * sin01(st_ph);
    a = lvl * 0.35f * st_wave;
    *l += (st_l[0] - st_l[1]) * a;
    *r += (st_r[0] - st_r[1]) * a;
    if (drop_amp > 0.0005f) {                          /* a drop on the water */
        drop_ph += drop_inc;
        drop_inc *= 0.99993f;
        s = sin01(drop_ph - (float)(int32_t)drop_ph) * drop_amp;
        drop_amp *= 0.9985f;
        *l += s * (0.5f - 0.5f * drop_pan) * lvl;
        *r += s * (0.5f + 0.5f * drop_pan) * lvl;
    } else if ((rnd() & 0xFFFFu) < (uint32_t)(lvl * 9.0f)) {
        drop_ph = 0.0f;
        drop_inc = (1400.0f + 2600.0f * rndf()) / TP_SR;
        drop_amp = 0.05f + 0.08f * rndf();
        drop_pan = rnds() * 0.8f;
    }
    /* a bird now and then: a song of a few chirps, each a quick sweep */
    if (bird.left > 0) {
        if ((int32_t)(now - bird.next) >= 0) {
            float env;
            bird.t += 1.0f / TP_SR;
            if (bird.t >= bird.len) {                  /* the chirp ends; a gap; the next */
                bird.left--;
                bird.t = 0.0f;
                bird.len = 0.05f + 0.1f * rndf();
                bird.f0 = bird.f1 * (0.8f + 0.5f * rndf());
                bird.f1 = 2200.0f + 2800.0f * rndf();
                bird.next = now + (uint32_t)((0.04f + 0.18f * rndf()) * TP_SR);
            } else {
                float x = bird.t / bird.len;
                env = sin01(0.5f * x);          /* a half sine, as a chirp swells and goes */
                bird.ph += lerp(bird.f0, bird.f1, x) / TP_SR;
                if (bird.ph >= 1.0f)
                    bird.ph -= 1.0f;
                s = sin01(bird.ph) * env * env * bird.amp * lvl;
                *l += s * (0.5f - 0.5f * bird.pan);
                *r += s * (0.5f + 0.5f * bird.pan);
            }
        }
    } else if ((int32_t)(now - bird.next) >= 0) {
        if (lvl > 0.05f && rndf() < lvl) {
            bird.left = 3 + (int)(rnd() % 5u);
            bird.t = 0.0f;
            bird.len = 0.06f + 0.08f * rndf();
            bird.f0 = 2500.0f + 2000.0f * rndf();
            bird.f1 = bird.f0 * (0.7f + 0.8f * rndf());
            bird.amp = 0.03f + 0.05f * rndf();
            bird.pan = rnds() * 0.9f;
        }
        bird.next = now + (uint32_t)((5.0f + 14.0f * rndf()) * TP_SR);
    }
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

static void deck_target(const deck_t *d, float age, float drive)
{
    float a2 = age * age, c;
    c = lerp(d->wow0, d->wow1, age) * 0.000578f;       /* cents to a speed ratio, then to samples of delay */
    dk_t.wow_a = c * TP_SR / (6.2831853f * d->wow_hz);
    dk_t.wow_hz = d->wow_hz;
    c = lerp(d->flut0, d->flut1, age) * 0.000578f;
    dk_t.flut_a = c * TP_SR / (6.2831853f * d->flut_hz);
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
    print_s = d->print_s;
    print_db = lerp(d->print0, d->print1, age);
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

/* -------------------------------------------------------------- generator --- */
static uint32_t step_next, step_n, flute_left, flute_next;
static int bell_idx = 8;
static float flute_note = 76.0f;
static uint8_t cur_scene = 255, cur_chord;

static void scene_start(int s)
{
    cur_scene = (uint8_t)s;
    cur_chord = 0;
    step_n = 0;
    pads_to(&SCENES[s], 0);
}

/* the chord's tones over two octaves from the scene's root + 12, for the bells */
static int bell_pitches(const scene_t *s, int c, int *out)
{
    int n = 0, o, i;
    for (o = -1; o < 4; o++)
        for (i = 0; i < 4; i++) {
            int m = s->root + 24 + o * 12 + s->chord[c][i];
            if (m >= s->bell_lo && m <= s->bell_hi && n < 24)
                out[n++] = m;
        }
    for (i = 1; i < n; i++) {          /* sorted, rising */
        int v = out[i], j = i;
        while (j > 0 && out[j - 1] > v) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = v;
    }
    return n;
}

static int scale_note(const scene_t *s, int deg)        /* a scale degree from the root, any octave */
{
    int o = deg / s->nscale, k = deg % s->nscale;
    if (k < 0) {
        k += s->nscale;
        o--;
    }
    return s->root + 12 * o + s->scale[k];
}

/* one eighth-note step, planned LOOK ahead */
static void gen_step(const scene_t *s, float density, float bells_on, float flute_on_lvl)
{
    uint32_t t = now + LOOK, per_bar = s->sixteenths ? 16u : 8u, per_chord = per_bar * s->bars;
    int bp[24], nb, beat = (int)(step_n % (per_bar / 2u)) == 0;
    if (step_n % per_chord == 0u && step_n && s->nchord > 1u) {   /* the next chord */
        cur_chord = (uint8_t)((cur_chord + 1u) % s->nchord);
        pads_to(s, cur_chord);
    }
    tp_chord = cur_chord;
    nb = bell_pitches(s, cur_chord, bp);
    if (s->bell_random) {                              /* drops: any scale note in the register, scattered */
        if (bells_on > 0.01f && rndf() < density * s->bell_busy * (beat ? 0.7f : 0.35f)) {
            int deg = (int)(rnd() % (uint32_t)(s->nscale * 2)), m = scale_note(s, deg) + 12;
            while (m < s->bell_lo)
                m += 12;
            while (m > s->bell_hi)
                m -= 12;
            plan(t + (rnd() & 2047u), EV_BELL, (float)m, (0.3f + 0.7f * rndf()) * 0.2f, rnds() * 0.9f, 0.0f);
        }
    } else if (bells_on > 0.01f && nb && rndf() < density * s->bell_busy * (beat ? 1.3f : 0.7f)) {
        int jump = (int)(rnd() % 5u) - 2;
        if (jump == 0)
            jump = 1;
        bell_idx += jump;
        if (bell_idx < 0)
            bell_idx = 1;
        if (bell_idx >= nb)
            bell_idx = nb > 1 ? nb - 2 : 0;
        {
            float note = (float)bp[bell_idx];
            if (rndf() < 0.15f)                        /* now and then a passing tone from the scale */
                note = (float)nearest(note + 2.0f * (float)jump, s->scale[rnd() % s->nscale], s->root, s->bell_lo,
                                      s->bell_hi);
            plan(t, EV_BELL, note, (0.35f + 0.65f * rndf()) * 0.22f, rnds() * 0.7f, 0.0f);
        }
    }
    /* the flute: a phrase of a few notes now and then, stepwise in the scale */
    if (flute_on_lvl > 0.01f) {
        if (flute_left && (int32_t)(t - flute_next) >= 0) {
            float beat_s = 60.0f / (float)s->bpm, dur = beat_s * (float)(1 + rnd() % 3u);
            int deg = 0, k;
            for (k = 0; k < 40 && scale_note(s, deg) < (int)flute_note; k++)
                deg++;
            deg += (int)(rnd() % 3u) - 1 + ((rnd() & 3u) == 0 ? ((rnd() & 1u) ? 2 : -2) : 0);
            flute_note = (float)scale_note(s, deg);
            while (flute_note > 86.0f)
                flute_note -= 12.0f;
            while (flute_note < 70.0f)
                flute_note += 12.0f;
            plan(t, EV_FLUTE, flute_note, 0.16f, rnds() * 0.4f, dur * 0.95f);
            flute_left--;
            flute_next = t + (uint32_t)(dur * TP_SR);
        } else if (!flute_left && (int32_t)(t - flute_next) >= 0
                   && rndf() < flute_on_lvl * density * 0.03f * s->flute_busy) {
            flute_left = 2u + rnd() % 4u;
            flute_next = t;
            flute_note = (float)scale_note(s, 14 + (int)(rnd() % 5u));
        }
    }
    step_n++;
}

/* -------------------------------------------------------------- the loop --- */
/* After Basinski: a loop of the machine's own output, played round and round, and every pass the oxide comes off
 * a little more, where it already was coming off. A cell's damage only grows: it dulls, quietens, then drops out,
 * and its neighbours start to go. Each pass writes in its own hiss, and the 8-bit mu-law adds its grain. */
uint8_t tp_damage[TP_NCELL];
volatile uint32_t tp_loop_pos, tp_loop_len = 176400u, tp_passes;
volatile uint8_t tp_capturing;
static uint8_t loop[TP_LOOP_BYTES] TP_POOL;
static float ulaw_dec[256];
static uint32_t lsub;
static float lacc, lold, lnew, lwear, lgen;
static float haas[512];                /* the loop's right: its left a few ms later, for some width */
static uint32_t haas_w;
static uint8_t cap_seen;

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

static void loop_capture(void)          /* a fresh loop: no damage; the first pass records the machine whole */
{
    uint32_t i;
    for (i = 0; i < TP_NCELL; i++)
        tp_damage[i] = 0;
    tp_loop_pos = 0;
    lsub = 0;
    lacc = 0.0f;
    tp_passes = 0;
    tp_capturing = 1;
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

/* one sample of the machine in; the loop's playback out (left, and the right a little later) */
static inline void loop_tick(float in, float feed, float decay, float *ol, float *or_)
{
    float t, y;
    lacc += in;
    if (++lsub >= TP_LOOP_DIV) {
        uint32_t pos = tp_loop_pos, len = tp_loop_len;
        float x, d, keep, wr;
        lsub = 0;
        x = ulaw_dec[loop[pos]];
        d = (float)tp_damage[pos / TP_CELL] * (1.0f / 255.0f);
        lwear += (x - lwear) * (1.0f - 0.85f * d);     /* worn spots go dull */
        lgen += (lwear - lgen) * (1.0f - 0.12f * decay); /* and every pass a little dull everywhere */
        keep = (1.0f - 0.3f * d * d) * (1.0f - 0.006f * decay);   /* and quiet */
        if (d > 0.55f && (rnd() & 255u) < (uint32_t)(d * 70.0f))
            keep = 0.0f;                               /* and gone */
        x = lgen * keep + rnds() * 0.0005f * (0.3f + decay);      /* the pass writes its own hiss */
        lold = lnew;
        lnew = tp_capturing ? lacc * (1.0f / (float)TP_LOOP_DIV) : x;
        wr = tp_capturing ? lnew : x * (1.0f - 0.5f * feed) + feed * lacc * (1.0f / (float)TP_LOOP_DIV);
        loop[pos] = ulaw_enc(wr);
        lacc = 0.0f;
        if (++pos >= len) {
            pos = 0;
            if (tp_capturing)
                tp_capturing = 0;
            else
                loop_wear(decay, (len + TP_CELL - 1u) / TP_CELL);
            tp_passes++;
        }
        tp_loop_pos = pos;
    }
    t = (float)lsub * (1.0f / (float)TP_LOOP_DIV);
    y = lold + (lnew - lold) * t;
    *ol = y;
    *or_ = haas[(haas_w - 300u) & 511u];              /* ~7 ms */
    haas[haas_w] = y;
    haas_w = (haas_w + 1u) & 511u;
}

/* ------------------------------------------------------------------ render --- */
static float gain_now, music, hiss_on;

void tp_init(void)
{
    uint32_t i, off = 0;
    for (i = 0; i < 4; i++) {
        rv_line[i] = rv_buf + off;
        off += RV_LEN[i];
    }
    for (i = 0; i < NPAD; i++) {
        pads[i].pitch = pads[i].target = 57.0f + 4.0f * (float)i;
        pads[i].drift[0] = rnds() * 4.0f;
        pads[i].drift[1] = rnds() * 4.0f;
        pads[i].bhz = 0.04f + 0.05f * rndf();
        pads[i].bph = rndf();
    }
    bass_pitch = bass_target = 43.0f;
    for (i = 0; i < DL; i++)
        tape_l[i] = tape_r[i] = 0;
    for (i = 0; i < NEV; i++)
        evq[i].kind = EV_NONE;
    bell_scene = &SCENES[0];
    scene_start(tp_cfg.scene);
    deck_target(&DECKS[tp_cfg.type], tp_cfg.age, tp_cfg.drive);
    glide_deck(1);
    bump_set(dk.bump_hz, dk.bump_db);
    step_next = 0;
    hiss_on = 1.0f;
    loop_init();
    cap_seen = tp_cfg.capture;
    loop_capture();
}

void tp_panic(void) { gain_now = 0.0f; }

void tp_render(int32_t *lr, uint32_t n, float gain)
{
    const tp_cfg_t c = tp_cfg;
    const scene_t *s;
    uint32_t i, k;
    float fb = 0.70f + 0.24f * c.space, wet = c.space * 0.55f, damp = onepole(lerp(5500.0f, 3200.0f, c.space)),
          beat_n = 60.0f / 2.0f * TP_SR;
    float a_lp, a_lpd, a_hp, a_az, a_hiss, pad_a = c.pads * 0.11f, bass_a = c.pads * 0.05f, pk = 0.0f;
    float inc_pad[NPAD][2], inc_bass, sat_norm, glide_k;

    if (c.scene != cur_scene && c.scene < TP_NSCENES)
        scene_start(c.scene);
    if (c.capture != cap_seen) {
        cap_seen = c.capture;
        loop_capture();
    }
    {
        uint32_t len = (uint32_t)(fm_clampf(c.length, TP_LOOP_MIN, tp_loop_max()) * TP_SR / (float)TP_LOOP_DIV);
        len = len > TP_LOOP_BYTES ? TP_LOOP_BYTES : len;
        if (tp_loop_pos >= len)
            tp_loop_pos = 0;
        tp_loop_len = len;
    }
    s = &SCENES[cur_scene];
    bell_scene = s;
    beat_n /= (float)s->bpm;                           /* samples an eighth */
    if (s->sixteenths)
        beat_n *= 0.5f;
    glide_k = 1.0f - fm_exp2f(-(float)n / TP_SR * 1.442695f / (s->glide_s * 0.45f));   /* most of it in glide_s */
    pad_a *= s->pad_lvl;
    bass_a *= s->pad_lvl;

    /* the plan: steps due now (LOOK ahead), then the events due this block */
    while ((int32_t)(now - step_next) >= 0) {
        if (c.playing)
            gen_step(s, c.density, c.bells, c.flute);
        else
            step_n++;
        step_next += (uint32_t)beat_n;
    }
    for (k = 0; k < NEV; k++) {
        ev_t *e = &evq[k];
        if (e->kind != EV_NONE && (int32_t)(e->t - (now + n)) < 0) {
            if (e->kind == EV_BELL) {
                if (c.bells > 0.01f) {
                    ev_t q = *e;
                    q.vel *= c.bells;
                    bell_on(&q);
                }
            } else if (c.flute > 0.01f) {
                ev_t q = *e;
                q.vel *= c.flute;
                flute_on(&q);
            }
            if (e->ghost)
                tp_ghosts++;
            else
                tp_notes++;
            e->kind = EV_NONE;
        }
    }

    /* the deck's figures */
    deck_target(&DECKS[c.type < TP_NTYPES ? c.type : 0], c.age, c.drive);
    glide_deck(0);
    bump_set(dk.bump_hz, dk.bump_db);
    a_lp = onepole(dk.lp * (1.0f - 0.75f * tp_drop));
    a_lpd = onepole(dk.lp * 0.8f * (1.0f - 0.75f * tp_drop));   /* the right's head, a shade off: duller */
    a_hp = onepole(dk.hp);
    a_az = onepole(fm_maxf(dk.lp * (1.0f - 0.4f * c.age), 1500.0f));
    a_hiss = onepole(dk.hiss_c);
    sat_norm = 0.5f / fm_tanhf(dk.drive * 0.5f);

    /* the pads' pitches, once a block: glides and drift */
    for (k = 0; k < NPAD; k++) {
        pad_t *p = &pads[k];
        int o;
        p->pitch += (p->target - p->pitch) * glide_k;     /* the glide: the scene's time */
        for (o = 0; o < 2; o++) {
            p->dv[o] += (rnds() * 0.05f - p->dv[o]) * 0.01f;
            p->drift[o] = fm_clampf(p->drift[o] + p->dv[o], -9.0f, 9.0f);
            inc_pad[k][o] = mtof(p->pitch + (o ? 0.07f : -0.07f) + p->drift[o] * 0.01f) / TP_SR;
        }
        p->bph += p->bhz * (float)n / TP_SR;
        if (p->bph >= 1.0f)
            p->bph -= 1.0f;
        p->breath = 0.75f + 0.25f * fm_sin_turns(p->bph);
    }
    flute_block(&flutes[0], n);
    flute_block(&flutes[1], n);
    bass_pitch += (bass_target - bass_pitch) * glide_k;
    inc_bass = mtof(bass_pitch) / TP_SR;
    pad_cut_ph += 0.021f * (float)n / TP_SR;
    if (pad_cut_ph >= 1.0f)
        pad_cut_ph -= 1.0f;
    pad_cut = s->pad_hz * (1.0f + 0.35f * fm_sin_turns(pad_cut_ph));
    {
        float a_pc = onepole(pad_cut);
        /* motor drag: now and then the motor labours and the pitch sags, then catches up */
        if (drag_left <= 0.0f && rndf() < dk.drag * (float)n / (60.0f * TP_SR)) {
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
        if (rndf() < (float)n / (3.0f * TP_SR)) {        /* the channels wander: the tape's contact on the heads */
            wander_tl = rnds() * c.age * 0.16f;
            wander_tr = rnds() * c.age * 0.16f;
        }

        for (i = 0; i < n; i++) {
            float l = 0.0f, r = 0.0f, x, d, dd, rl, rr, f0, f1;
            /* the music */
            music += ((c.playing ? 1.0f : 0.0f) - music) * 0.000008f;   /* ~3 s fade */
            for (k = 0; k < NPAD; k++) {
                pad_t *p = &pads[k];
                float sw = 0.0f, pan = (float)k * 0.5f - 0.75f;
                int o;
                for (o = 0; o < 2; o++) {
                    p->ph[o] += inc_pad[k][o];
                    if (p->ph[o] >= 1.0f)
                        p->ph[o] -= 1.0f;
                    sw += 2.0f * p->ph[o] - 1.0f - blep(p->ph[o], inc_pad[k][o]);
                }
                p->lp1 += (sw - p->lp1) * a_pc;
                p->lp2 += (p->lp1 - p->lp2) * a_pc;
                x = p->lp2 * p->breath * pad_a;
                l += x * (0.5f - 0.5f * pan);
                r += x * (0.5f + 0.5f * pan);
            }
            bass_ph += inc_bass;
            if (bass_ph >= 1.0f)
                bass_ph -= 1.0f;
            x = sin01(bass_ph) + 0.3f * (4.0f * fm_fabsf(bass_ph - 0.5f) - 1.0f);
            bass_lp += (x - bass_lp) * 0.2f;
            l += bass_lp * bass_a;
            r += bass_lp * bass_a;
            bells_tick(&l, &r);
            x = flute_tick(&flutes[0]) + flute_tick(&flutes[1]);
            l += x * (0.5f - 0.5f * flutes[0].pan);
            r += x * (0.5f + 0.5f * flutes[0].pan);
            l *= music;
            r *= music;
            if (c.nature > 0.01f) {
                float nl = 0.0f, nr = 0.0f;
                nature_tick(fm_minf(c.nature * s->nature_lift, 1.5f) * music, &nl, &nr);
                l += nl;
                r += nr;
            }
            /* the room */
            {
                float wl, wr;
                room(0.5f * (l + r), fb, damp, &wl, &wr);
                l += wl * wet;
                r += wr * wet;
            }

            /* the record head: saturation, a little bias for the even harmonics */
            l = (fm_tanhf(dk.drive * l + 0.05f) - 0.04995837f) * sat_norm;   /* tanh(0.05): the bias taken back out */
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
            wow_rw += (wow_tg - wow_rw) * 0.00004f;          /* the wow's slow wander, besides its cycle */
            flut_rw += (rnds() * 14.0f - flut_rw) * 0.002f;  /* flutter's grain: the capstan's noise */
            if (drag_left > 0.0f) {
                drag_left -= 1.0f;
                drag_off += drag_v;                    /* the tape runs slow: the delay grows */
            } else if (drag_off > 0.0f) {
                drag_off -= 0.0015f + drag_off * 0.00002f;   /* and catches up, a hair fast */
                if (drag_off < 0.0f)
                    drag_off = 0.0f;
            }
            drag_off = fm_minf(drag_off, 1500.0f);
            d = D0 + dk.wow_a * (sin01(wow_ph) + 1.5f * wow_rw)
                + dk.flut_a * (psin01(flut_ph) + 2.0f * flut_rw) + drag_off;
            d = fm_clampf(d, 4.0f, (float)DL - 40.0f);
            dd = d - d_prev;
            d_prev = d;
            tp_speed = 1.0f - dd;

            /* the play head: the right's a little off the left (misalignment) */
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
                float x2 = drop_t / drop_len;          /* 1 .. 0 through the dropout */
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

            /* the tape's own hiss, under everything, even when the music stops */
            for (k = 0; k < 2; k++) {
                float h = rnds();
                hiss_hp[k] += (h - hiss_hp[k]) * 0.11f;
                hiss_lp[k] += ((h - hiss_hp[k]) - hiss_lp[k]) * a_hiss;
            }
            rl += hiss_lp[0] * dk.hiss * 3.0f * hiss_on;
            rr += hiss_lp[1] * dk.hiss * 3.0f * hiss_on;

            /* the width: micro and VHS near mono */
            f0 = 0.5f * (rl + rr);
            f1 = 0.5f * (rl - rr) * dk.width;
            l = f0 + f1;
            r = f0 - f1;

            /* the loop: it records what came off the tape; BASINSKI turns from the machine to the loop */
            {
                float ll, lrr, b = c.basinski;
                loop_tick(0.5f * (l + r), c.feed, c.decay, &ll, &lrr);
                if (tp_capturing) {                    /* the first pass: the machine, as it is recorded */
                    ll = l;
                    lrr = r;
                }
                l = l * (1.0f - b) + ll * b;
                r = r * (1.0f - b) + lrr * b;
            }

            gain_now += (gain - gain_now) * 0.004f;
            l *= gain_now;
            r *= gain_now;
            if (fm_fabsf(l) > pk)
                pk = fm_fabsf(l);
            if (fm_fabsf(r) > pk)
                pk = fm_fabsf(r);
            lr[2 * i] = (int32_t)(fm_clampf(l, -1.0f, 1.0f) * 8388000.0f);
            lr[2 * i + 1] = (int32_t)(fm_clampf(r, -1.0f, 1.0f) * 8388000.0f);
            now++;
        }
    }
    tp_reel_turns += (float)n / TP_SR * 0.6f * tp_speed;
    if (pk > tp_peak)
        tp_peak = pk;
}
