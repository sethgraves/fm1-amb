/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Seth Graves; built on fm1-base by Charles Vestal */
/* DREAM: the app. Presets of whole-number harmonics on one unit, the drone (dsp/drone.c) holding them, a screen
 * that shows each tone's harmonic, its note and how far it sits from equal temperament.
 *
 *   PLAY        the drone on / off (it swells in and fades out)
 *   PRESET      the piece
 *   K1 PITCH    the series up or down, semitones          K2 FINE     cents
 *   K3 SPREAD   the tones across the stereo field         K4 TILT     dB per octave up the series
 *   SELECT      the second page of knobs:
 *   K1 DENSITY  how many of the tones sound, the simplest kept longest   K2 BALANCE   the base against the rest, dB
 *   K3 DRIFT    0 holds the tones phase-locked; up, their phases slowly drift against each other
 *   K4 EVOLVE   minutes for the density to breathe down to one tone and back; OFF holds it
 *   MASTER      volume                                     SAVE        keeps all of it for the next power-on
 *
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in scope. */
#include "fastmath.h"
#include "drone.h"

/* ---------------------------------------------------------------- pieces --- */
/* A piece (PRESET) is a unit frequency (Hz, before PITCH and FINE) and its versions (ALGORITHM): each version a
 * set of harmonics on that unit. piece_t is one version; work_t the piece. */
typedef struct {
    const char *name;                    /* on the screen (capitals) */
    uint8_t n;
    uint8_t nbase;                       /* the first nbase harmonics are the base, under the rest */
    uint16_t harm[DR_MAXT];              /* rising */
} piece_t;

typedef struct {
    float unit_hz;
    uint8_t drift;                       /* DRIFT when the piece is chosen, 0 .. 100 */
    uint8_t nvar;                        /* its versions */
    uint8_t var0;                        /* the one it starts on */
    const piece_t *var;
} work_t;

#define B3_HZ 246.941651f                /* B below middle C, A = 440 */
#define WTP_EB3 148.994709f              /* The Well-Tuned Piano's Eb (440 * 128/189, an octave down) */

/* Composition 1960 #7: B and F#, "to be held for a long time". Here as a pure fifth, 2:3, so the F# sits 2 cents
 * above equal temperament; in three octaves, on a unit two octaves under B3 */
static const piece_t V1960[] = {
    {"COMPOSITION 1960 #7  B2", 2, 0, {2, 3}},
    {"COMPOSITION 1960 #7  B3", 2, 0, {4, 6}},
    {"COMPOSITION 1960 #7  B4", 2, 0, {8, 12}},
};

/* The Base 9:7:4 Symmetry in Prime Time When Centered above and below The Lowest Term Primes in The Range 288
 * to 224 with The Addition of 279 and 261 ... with The Addition of 119 (the Dream House), worked from its title:
 *   above, to 288: the primes 128-144 (131, 137, 139) and 64-72 (67, 71) times powers of 2: 262 274 278 268 284
 *   below, to 224: the primes 112-126 (113), 56-63 (59, 61) and 28-31.5 (29, 31) likewise: 226 236 244 232 248
 *   with 288 and 224, the additions 279 and 261, and 2 x 119 = 238
 * Its inclusory optional bases, as the title lists them: 7; 8; 14:8; 18:14:8; 18:16:14; 18:16:14:8; 9:7:4; or the
 * empty base. Each at four times its numbers (9:7:4 as 36:28:16, two octaves under 9:7 = 288:224's own ratio).
 * The unit 4.5 Hz puts the cluster at 1008 - 1296 Hz; PITCH moves it all. */
static const piece_t VDREAM[] = {
    {"DREAM HOUSE  BASE 7", 16, 1, {28, 224, 226, 232, 236, 238, 244, 248, 261, 262, 268, 274, 278, 279, 284, 288}},
    {"DREAM HOUSE  BASE 8", 16, 1, {32, 224, 226, 232, 236, 238, 244, 248, 261, 262, 268, 274, 278, 279, 284, 288}},
    {"DREAM HOUSE  BASE 14:8", 17, 2, {32, 56, 224, 226, 232, 236, 238, 244, 248, 261, 262, 268, 274, 278, 279, 284, 288}},
    {"DREAM HOUSE  BASE 18:14:8", 18, 3, {32, 56, 72, 224, 226, 232, 236, 238, 244, 248, 261, 262, 268, 274, 278, 279, 284, 288}},
    {"DREAM HOUSE  BASE 18:16:14", 18, 3, {56, 64, 72, 224, 226, 232, 236, 238, 244, 248, 261, 262, 268, 274, 278, 279, 284, 288}},
    {"DREAM HOUSE  BASE 18:16:14:8", 19, 4, {32, 56, 64, 72, 224, 226, 232, 236, 238, 244, 248, 261, 262, 268, 274, 278, 279, 284, 288}},
    {"DREAM HOUSE  BASE 9:7:4", 18, 3, {16, 28, 36, 224, 226, 232, 236, 238, 244, 248, 261, 262, 268, 274, 278, 279, 284, 288}},
    {"DREAM HOUSE  EMPTY BASE", 15, 0, {224, 226, 232, 236, 238, 244, 248, 261, 262, 268, 274, 278, 279, 284, 288}},
};

/* The Well-Tuned Piano: fifths and harmonic sevenths over Eb, its A (189/128) at 440 Hz, so Eb = 297.99 Hz
 * (Gann 1993). The Opening Chord: Eb Bb C Eb F Bb, 4:6:7:8:9:12, here from the Eb an octave under that */
static const piece_t VOPEN[] = {
    {"THE OPENING CHORD", 6, 0, {4, 6, 7, 8, 9, 12}},
};

/* The Magic Chord: the keys E F A Bb D E G A, which sound 567/512, 9/8, 189/128, 3/2, 63/32 of Eb, then the E,
 * G (21/16) and A an octave up: 567 576 756 768 1008 1134 1344 1512 on Eb/512. No Eb: the chord's fundamental
 * is absent. Its top A is the 440. The Magic Opening Chord: the Opening Chord (on Eb3, 512) sounding with it,
 * which adds its C and Eb */
static const piece_t VMAGIC[] = {
    {"THE MAGIC CHORD", 8, 0, {567, 576, 756, 768, 1008, 1134, 1344, 1512}},
    {"THE MAGIC OPENING CHORD", 13, 0, {512, 567, 576, 756, 768, 896, 1008, 1024, 1134, 1152, 1344, 1512, 1536}},
};

/* A drift study: "two or more sine waves, the frequencies of which are tuned to relate to each other as
 * rational frequency ratios" (Young, 1968), their phase angles let drift. The tones of 14 VII 73 aren't
 * published (three sine waves); these are a choice: 6:7:8 on 1960 #7's B series, F#5 A5 B5, the B at 988 Hz */
static const piece_t VDRIFT[] = {
    {"DRIFT STUDY", 3, 0, {6, 7, 8}},
};

/* Map of 49's Dream: "sine waves sustained in various relationships over one continuous sine wave drone", the
 * ratios without 5. Its own sets aren't published; this is a choice: a 60 Hz drone (8) under 49:56:63:72, all
 * of 2, 3 and 7, led by 49 = 7 x 7 */
static const piece_t VMAP[] = {
    {"MAP OF 49'S DREAM", 5, 1, {8, 49, 56, 63, 72}},
};

/* The Well-Tuned Piano's tuning entire: its twelve pitches (Eb 1/1, E 567/512, F 9/8, F# 147/128, G 21/16,
 * G# 1323/1024, A 189/128, Bb 3/2, B 49/32, C 7/4, C# 441/256, D 63/32) in three octaves from the Eb, 36 tones
 * on Eb/1024. Not a chord Young plays; the lattice his chords come from, all at once. DENSITY thins it toward
 * Eb and Bb; EVOLVE breathes it */
static const piece_t VWTP[] = {
    {"WELL-TUNED PIANO TUNING", 36, 0,
     {1024, 1134, 1152, 1176, 1323, 1344, 1512, 1536, 1568, 1764, 1792, 2016,
      2048, 2268, 2304, 2352, 2646, 2688, 3024, 3072, 3136, 3528, 3584, 4032,
      4096, 4536, 4608, 4704, 5292, 5376, 6048, 6144, 6272, 7056, 7168, 8064}},
};

#define NV(v) ((uint8_t)(sizeof v / sizeof v[0]))
static const work_t PIECES[] = {
    {B3_HZ * 0.25f, 0, NV(V1960), 1, V1960},
    {4.5f, 0, NV(VDREAM), 6, VDREAM},
    {WTP_EB3 * 0.25f, 0, NV(VOPEN), 0, VOPEN},
    {WTP_EB3 / 512.0f, 0, NV(VMAGIC), 0, VMAGIC},
    {B3_HZ * 0.5f, 40, NV(VDRIFT), 0, VDRIFT},
    {7.5f, 0, NV(VMAP), 0, VMAP},
    {WTP_EB3 / 1024.0f, 0, NV(VWTP), 0, VWTP},
};
#define NPIECES ((int)(sizeof PIECES / sizeof PIECES[0]))

/* --------------------------------------------------------------- settings --- */
#define SET_MAGIC 0x44524D32u            /* "DRM2" */
typedef struct {
    uint32_t magic;
    uint8_t piece;
    int8_t semi;                         /* PITCH: -12 .. +12 */
    int8_t fine;                         /* FINE: cents, -50 .. +50 */
    uint8_t spread;                      /* 0 .. 100 */
    int8_t tilt;                         /* half dB per octave, -12 .. +12 (-6 .. +6 dB) */
    uint8_t playing;
    uint8_t drift;                       /* DRIFT: 0 (locked) .. 100 */
    uint8_t density;                     /* DENSITY: % of the tones above the base that sound, 0 .. 100 */
    int8_t balance;                      /* BALANCE: the base, dB, -12 .. +12 */
    uint8_t evolve;                      /* EVOLVE: minutes a breath, 0 (off) .. 60 */
    uint8_t variant;                     /* ALGORITHM: the piece's version */
    uint8_t rsv;
} settings_t;
static settings_t set;
static uint8_t set_dirty;

static void settings_defaults(void)
{
    set.magic = SET_MAGIC;
    set.piece = 0;
    set.semi = 0;
    set.fine = 0;
    set.spread = 40;
    set.tilt = 0;
    set.playing = 1;
    set.drift = 0;
    set.density = 100;
    set.balance = 0;
    set.evolve = 0;
    set.variant = PIECES[0].var0;
}

static int settings_ok(void)
{
    return set.magic == SET_MAGIC && set.piece < NPIECES && set.semi >= -12 && set.semi <= 12 && set.fine >= -50 &&
           set.fine <= 50 && set.spread <= 100 && set.tilt >= -12 && set.tilt <= 12 && set.playing <= 1 && set.drift <= 100 &&
           set.density <= 100 && set.balance >= -12 && set.balance <= 12 && set.evolve <= 60 &&
           set.variant < PIECES[set.piece].nvar;
}

/* the piece's version now */
static const piece_t *cur(void) { return &PIECES[set.piece].var[set.variant]; }

/* ------------------------------------------------------------ the drone --- */
static dr_state_t st;
static uint8_t st_dirty;                 /* a change not yet taken by the render */

static float unit_hz(void)
{
    const work_t *p = &PIECES[set.piece];
    return p->unit_hz * fm_exp2f(((float)set.semi * 100.0f + (float)set.fine) * (1.0f / 1200.0f));
}

/* DENSITY's order: the tones above the base, simplest first, by their odd part (a harmonic's number with the
 * factors of 2 taken out: 224 = 7 x 32 before 288 = 9 x 32 before 238 = 119 x 2), then the lower. The base
 * always sounds. rank[i]: tone i's place (0 the simplest); the base's -1. */
static int8_t rank[DR_MAXT];
static uint8_t n_upper;                  /* the tones above the base */
static uint8_t n_sounding;               /* of those, how many DENSITY (and EVOLVE) let sound */
static float evolve_now = 1.0f;          /* EVOLVE's share of DENSITY now: 1 .. down to one tone */

static uint32_t odd_part(uint32_t h)
{
    while (h && !(h & 1u))
        h >>= 1;
    return h;
}

static void rank_tones(void)
{
    const piece_t *p = cur();
    int i, j;
    n_upper = (uint8_t)(p->n - p->nbase);
    for (i = 0; i < p->n; i++) {
        int r = 0;
        if (i < p->nbase) {
            rank[i] = -1;
            continue;
        }
        for (j = p->nbase; j < p->n; j++) {
            uint32_t oi = odd_part(p->harm[i]), oj = odd_part(p->harm[j]);
            if (oj < oi || (oj == oi && p->harm[j] < p->harm[i]))
                r++;
        }
        rank[i] = (int8_t)r;
    }
}

static int sounding_count(void)          /* DENSITY times EVOLVE's breath, at least one */
{
    float d = (float)set.density * 0.01f * evolve_now;
    int k = (int)(d * (float)n_upper + 0.5f);
    return k < 1 ? 1 : k > n_upper ? n_upper : k;
}

static void drone_update(void)
{
    const piece_t *p = cur();
    float sum = 0.0f, s = (float)set.spread * 0.01f, db_oct = (float)set.tilt * 0.5f;
    float base_g = fm_db2lin((float)set.balance);
    uint16_t hmin = 0xFFFFu;
    int i;
    rank_tones();
    n_sounding = (uint8_t)sounding_count();
    st.unit_hz = unit_hz();
    st.n = p->n;
    st.playing = set.playing;
    st.glide_s = 0.3f;
    st.fade_s = 6.0f;
    for (i = 0; i < p->n; i++)
        if (p->harm[i] < hmin)
            hmin = p->harm[i];
    for (i = 0; i < p->n; i++) {
        float a = fm_db2lin(db_oct * fm_log2f((float)p->harm[i] / (float)hmin));
        if (rank[i] < 0)
            a *= base_g;                 /* BALANCE */
        else if (rank[i] >= n_sounding)
            a = 0.0f;                    /* DENSITY: thinned out (it fades) */
        st.harm[i] = p->harm[i];
        st.amp[i] = a;
        if (p->n <= 1)
            st.pan[i] = 0.0f;
        else if (p->n == 2)              /* the two at either side */
            st.pan[i] = s * (2.0f * (float)i - 1.0f);
        else                             /* neighbours on opposite sides, the higher further out */
            st.pan[i] = s * ((i & 1) ? 1.0f : -1.0f) * (0.3f + 0.7f * (float)i / (float)(p->n - 1));
        sum += a;
    }
    for (i = 0; i < p->n; i++)           /* their sum 1: in phase, 0.9 of full scale at most */
        st.amp[i] /= fm_maxf(sum, 1e-6f);
    {   /* DRIFT: up to 0.1 Hz (squared, so the low end is fine), each tone its own share and direction; the
         * lowest stays put. 97391.5 phase steps a hertz */
        float r = (float)set.drift * 0.01f;
        r = r * r * 0.1f * (4294967296.0f / DR_SR);
        for (i = 0; i < p->n; i++) {
            float k = (float)i * 0.618034f;
            k -= fm_floorf(k);
            st.drift[i] = i ? (int32_t)(((i & 1) ? 1.0f : -1.0f) * (0.4f + 0.6f * k) * r) : 0;
        }
    }
    st_dirty = 1;
}

static void drone_push(void)
{
    if (st_dirty && dr_commit(&st))
        st_dirty = 0;
}

void app_init(void)
{
    dr_init();
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || !settings_ok())
        settings_defaults();
    drone_update();
    drone_push();
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port))    /* nothing plays from MIDI yet: the queue drained */
        ;
    dr_render(lr, n, (float)master_q12 * (1.0f / 4096.0f));
}

void app_panic(void) { dr_panic(); }

/* -------------------------------------------------------------- text --- */
static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b, int plus)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    else if (plus && v > 0)
        *b++ = '+';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

static void hz_text(float hz, char *b)   /* "370.41" */
{
    int32_t c = (int32_t)(hz * 100.0f + 0.5f);
    char t[4];
    int_text(c / 100, b, 0);
    t[0] = '.';
    t[1] = (char)('0' + (c / 10) % 10);
    t[2] = (char)('0' + c % 10);
    t[3] = 0;
    text_cat(b, t);
}

/* a frequency as the nearest equal-tempered note and its offset in cents: "F#4", +2 */
static const char *const NOTE[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static void note_of(float hz, char *name, int32_t *cents)
{
    float m = 69.0f + 12.0f * fm_log2f(hz * (1.0f / 440.0f));
    int32_t k = (int32_t)fm_floorf(m + 0.5f);
    char o[8];
    *cents = (int32_t)fm_floorf((m - (float)k) * 100.0f + 0.5f);
    name[0] = 0;
    text_cat(name, NOTE[((k % 12) + 12) % 12]);
    int_text(k / 12 - 1, o, 0);
    text_cat(name, o);
}

/* --------------------------------------------------------------------- UI --- */
static struct {
    char msg[2][28];
    uint32_t msg_until;
    uint32_t btn;
    uint8_t redraw;
    uint8_t page;                        /* the knobs' page: 0 or 1 */
    uint8_t opening;                     /* the opening: the glow swelling with the first tones */
    uint32_t open_t0;
} ui;

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[64];
    for (i = 0; i < 27u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 27u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
    ui.redraw = 1;
    if (ui.opening) {                    /* a control touched: straight to the screen */
        ui.opening = 0;
        lcd_fill(0, 0, 240, 240, C_BLACK);
    }
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

/* the knobs, two pages of four (SELECT turns the page): 0-3 PITCH FINE SPREAD TILT, 4-7 DENSITY BALANCE DRIFT
 * EVOLVE */
static const char *const KNOB_NAME[8] = {"PITCH", "FINE", "SPREAD", "TILT", "DENSITY", "BALANCE", "DRIFT", "EVOLVE"};

static void knob_text(int k, char *t)
{
    char u[12];
    t[0] = 0;
    switch (k) {
    case 0:
        int_text(set.semi, t, 1);
        break;
    case 1:
        int_text(set.fine, t, 1);
        break;
    case 2:
        int_text(set.spread, t, 0);
        break;
    case 3: {                            /* half dB steps: "-1.5", "0", "+3" */
        int32_t a = set.tilt < 0 ? -set.tilt : set.tilt;
        text_cat(t, set.tilt < 0 ? "-" : set.tilt > 0 ? "+" : "");
        int_text(a / 2, u, 0);
        text_cat(t, u);
        if (a & 1)
            text_cat(t, ".5");
        break;
    }
    case 4:
        int_text(set.density, t, 0);
        text_cat(t, "%");
        break;
    case 5:
        int_text(set.balance, t, 1);
        break;
    case 6:
        int_text(set.drift, t, 0);
        break;
    default:
        if (set.evolve) {
            int_text(set.evolve, t, 0);
            text_cat(t, "M");
        } else {
            text_cat(t, "OFF");
        }
        break;
    }
}

static void knob_turn(int k, int32_t e)
{
    char t[16];
    switch (k) {
    case 0:
        set.semi = (int8_t)clamp(set.semi + e, -12, 12);
        break;
    case 1:
        set.fine = (int8_t)clamp(set.fine + e, -50, 50);
        break;
    case 2:
        set.spread = (uint8_t)clamp((int32_t)set.spread + e * 2, 0, 100);
        break;
    case 3:
        set.tilt = (int8_t)clamp(set.tilt + e, -12, 12);
        break;
    case 4:
        set.density = (uint8_t)clamp((int32_t)set.density + e * 2, 0, 100);
        break;
    case 5:
        set.balance = (int8_t)clamp(set.balance + e, -12, 12);
        break;
    case 6:
        set.drift = (uint8_t)clamp((int32_t)set.drift + e * 2, 0, 100);
        break;
    default:
        set.evolve = (uint8_t)clamp((int32_t)set.evolve + e, 0, 60);
        if (!set.evolve)
            evolve_now = 1.0f;
        break;
    }
    set_dirty = 1;
    drone_update();
    knob_text(k, t);
    app_say(KNOB_NAME[k], t);
}

/* EVOLVE: the density breathes, a raised cosine from DENSITY down to one tone and back, once in set.evolve
 * minutes. Checked twice a second; the drone hears of it only when a tone comes or goes */
static void evolve_tick(void)
{
    static uint32_t last, t0;
    uint32_t now = plat_ms();
    float ph;
    if (now - last < 500u)
        return;
    last = now;
    if (!set.evolve) {
        t0 = now;
        return;
    }
    ph = (float)((now - t0) % ((uint32_t)set.evolve * 60000u)) / ((float)set.evolve * 60000.0f);
    evolve_now = 0.5f + 0.5f * fm_cosf(ph * FM_TWO_PI);
    if (sounding_count() != n_sounding)
        drone_update();
}

static void speak(void)                  /* a screen reader that has just started listening hears the screen */
{
    char t[96];
    if (ann_on() && ann_where()) {
        t[0] = 0;
        text_cat(t, APP_NAME ". ");
        text_cat(t, cur()->name);
        text_cat(t, set.playing ? ". PLAYING" : ". STOPPED");
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

static void input(void)
{
    uint32_t btn = plat_buttons(), k;
    int32_t e;
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_PLAY) {
            set.playing = (uint8_t)!set.playing;
            set_dirty = 1;
            drone_update();
            app_say(set.playing ? "PLAY" : "STOP", "");
        } else if (k == B_SAVE) {
            if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) {
                set_dirty = 0;
                app_say("SAVED", "");
            } else {
                app_say("SAVE", "FAILED");
            }
        }
    }
    if ((e = plat_enc(EN_PRESET)) != 0) {
        int32_t p = clamp((int32_t)set.piece + e, 0, NPIECES - 1);
        if (p != set.piece) {
            set.piece = (uint8_t)p;
            set.drift = PIECES[p].drift;     /* the second page is the piece's own: a new piece starts it over */
            set.density = 100;
            set.balance = 0;
            set.evolve = 0;
            evolve_now = 1.0f;
            set.variant = PIECES[p].var0;
            set_dirty = 1;
            drone_update();
        }
        app_say(cur()->name, "");
    }
    if ((e = plat_enc(EN_SELECT)) != 0) {
        ui.page = (uint8_t)(e > 0);
        app_say(ui.page ? "PAGE 2" : "PAGE 1", "");
    }
    if ((e = plat_enc(EN_K1)) != 0)
        knob_turn(ui.page * 4 + 0, e);
    if ((e = plat_enc(EN_K2)) != 0)
        knob_turn(ui.page * 4 + 1, e);
    if ((e = plat_enc(EN_K3)) != 0)
        knob_turn(ui.page * 4 + 2, e);
    if ((e = plat_enc(EN_K4)) != 0)
        knob_turn(ui.page * 4 + 3, e);
    if ((e = plat_enc(EN_ALGO)) != 0) {    /* ALGORITHM: the piece's versions */
        int32_t v = clamp((int32_t)set.variant + e, 0, PIECES[set.piece].nvar - 1);
        if (v != set.variant) {
            set.variant = (uint8_t)v;
            set_dirty = 1;
            drone_update();
        }
        {   /* the version's own part of the name, after the piece's: "BASE 18:16:14:8" */
            const char *n = cur()->name, *q = n;
            while (*q && !(q[0] == ' ' && q[1] == ' '))
                q++;
            app_say(*q ? q + 2 : n, "");
        }
    }
    if (btn != ui.btn)
        ui.redraw = 1;
    ui.btn = btn;
    plat_glow(1);
    plat_leds((set.playing ? 1u << B_PLAY : 0u) | (set_dirty ? 1u << B_SAVE : 0u), 0);
    drone_push();
}

void app_input_only(void) { input(); }

/* ------------------------------------------------------------- the look --- */
/* After Marian Zazeela's light in the Dream House: magenta and violet on black. The platform's five steps
 * (gfx.c: rules, inactive, labels, secondary, values), with the hue in the two brightest, so colour still
 * means something: a value, a level. */
static const uint16_t ZPAL[5] = {RGB(46, 30, 60), RGB(100, 76, 122), RGB(184, 166, 206), RGB(170, 130, 255),
                                 RGB(255, 72, 200)};
static void palette_dream(void)
{
    int k;
    for (k = 0; k < 5; k++)
        pal[k] = ZPAL[k];
}

/* violet (0) to magenta (1), scaled by k (0..1): the tones low to high, the glow edge to centre */
static uint16_t glow_rgb(float t, float k)
{
    float r = (150.0f + 105.0f * t) * k, g = (80.0f - 10.0f * t) * k, b = (255.0f - 55.0f * t) * k;
    return RGB((uint32_t)r, (uint32_t)g, (uint32_t)b);
}

/* the glow: magenta light from the middle of the screen fading to violet and black, brightness k; with the
 * name over it. Drawn in two bands of 120 rows */
static void draw_glow(float k)
{
    int32_t band, x, y;
    for (band = 0; band < 2; band++) {
        cv_begin(240, 120, C_BLACK);
        for (y = 0; y < 120; y++) {
            float dy = (float)(y + band * 120 - 100);
            for (x = 0; x < 240; x++) {
                float dx = (float)(x - 120), d2 = dx * dx + dy * dy * 1.6f;
                float f = k / (1.0f + d2 * (1.0f / 3600.0f));   /* bright core, long soft edge */
                float t = fm_clampf(1.0f - d2 * (1.0f / 16000.0f), 0.0f, 1.0f);
                if (f > 0.01f) {             /* dithered (4 x 4 Bayer): the 16-bit screen's steps don't ring */
                    static const uint8_t BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
                    float dd = ((float)BAYER[((y & 3) << 2) | (x & 3)] + 0.5f) * (1.0f / 16.0f);
                    float kk = fm_minf(f, 1.0f);
                    uint32_t r = (uint32_t)fm_minf((150.0f + 105.0f * t) * kk + dd * 8.0f, 255.0f);
                    uint32_t g = (uint32_t)fm_minf((80.0f - 10.0f * t) * kk + dd * 4.0f, 255.0f);
                    uint32_t bl = (uint32_t)fm_minf((255.0f - 55.0f * t) * kk + dd * 8.0f, 255.0f);
                    cv_pset(x, y, RGB(r, g, bl));
                }
            }
        }
        if (band == 0) {
            cv_text(120 - text_w(&FONT_L, APP_NAME) / 2, 78, &FONT_L, APP_NAME, C_WHITE);
        } else {
            static const char *const L1 = "AFTER LA MONTE YOUNG", *const L2 = "AND MARIAN ZAZEELA";
            cv_text(120 - text_w(&FONT_XS, L1) / 2, 30, &FONT_XS, L1, RGB(236, 220, 250));
            cv_text(120 - text_w(&FONT_XS, L2) / 2, 46, &FONT_XS, L2, RGB(236, 220, 250));
        }
        cv_blit(0, (uint32_t)(band * 120));
    }
}

void app_splash(void)                    /* the boot screen (APP_SPLASH): the glow, low */
{
    palette_dream();
    draw_glow(0.3f);
}

/* the ratio between the lowest two tones, large: "3:2" (the big fonts have no colon: two dots drawn) */
static void draw_ratio(int32_t cy)
{
    const piece_t *p = cur();
    char a[8], b[8];
    int32_t wa, wb, x;
    if (p->n < 2)
        return;
    {
        uint32_t x = p->harm[1], y = p->harm[0];
        while (y) {
            uint32_t r = x % y;
            x = y;
            y = r;
        }
        int_text((int32_t)(p->harm[1] / x), a, 0);
        int_text((int32_t)(p->harm[0] / x), b, 0);
    }
    wa = text_w(&FONT_L, a);
    wb = text_w(&FONT_L, b);
    x = 120 - (wa + 14 + wb) / 2;
    cv_text(x, cy, &FONT_L, a, C_HI);
    cv_rect(x + wa + 5, cy + 9, 4, 4, C_HI);
    cv_rect(x + wa + 5, cy + 21, 4, 4, C_HI);
    cv_text(x + wa + 14, cy, &FONT_L, b, C_HI);
}

/* a piece of many tones: the cluster as lines along its harmonic numbers (each as tall as its level), the base
 * as a row of notes under it. Drawn into the middle canvas (240 x 150), under the piece's name. */
static void draw_cluster(float u)
{
    const piece_t *p = cur();
    int32_t i, h0, h1, x, c;
    char t[24], nm[8];
    float amax = 1e-6f;
    if (p->nbase >= p->n)
        return;
    h0 = p->harm[p->nbase];
    h1 = p->harm[p->n - 1];
    for (i = 0; i < p->n; i++)
        amax = fm_maxf(amax, st.amp[i]);

    /* "15 TONES 224 - 288" (or "9/15" while DENSITY thins them), then the band in notes */
    t[0] = 0;
    if (n_sounding < n_upper) {
        int_text(n_sounding, t, 0);
        text_cat(t, "/");
    }
    int_text(p->n - p->nbase, nm, 0);
    text_cat(t, nm);
    text_cat(t, " TONES  ");
    int_text(h0, nm, 0);
    text_cat(t, nm);
    text_cat(t, " - ");
    int_text(h1, nm, 0);
    text_cat(t, nm);
    cv_text(120 - text_w(&FONT_B, t) / 2, 18, &FONT_B, t, C_WHITE);
    note_of(u * (float)h0, t, &c);
    text_cat(t, " - ");
    note_of(u * (float)h1, nm, &c);
    text_cat(t, nm);
    cv_text(120 - text_w(&FONT_XS, t) / 2, 38, &FONT_XS, t, C_GRAY);

    /* the cluster: a line per tone, placed by pitch (a log scale: an octave is an octave wherever it is) */
    cv_rect(10, 104, 220, 1, C_LINE);
    for (i = p->nbase; i < p->n; i++) {
        int32_t hgt = (int32_t)(fm_clampf(dr_level[i] / amax, 0.0f, 1.0f) * 50.0f + 0.5f);
        x = 10 + (h1 > h0 ? (int32_t)(fm_log2f((float)p->harm[i] / (float)h0) /
                                      fm_log2f((float)h1 / (float)h0) * 218.0f + 0.5f) : 109);
        cv_rect(x, 54, 2, 50, C_LINE);
        if (hgt)
            cv_rect(x, 104 - hgt, 2, hgt, glow_rgb((float)(x - 10) / 218.0f, 1.0f));
    }
    int_text(h0, t, 0);
    cv_text(10, 107, &FONT_XS, t, C_DIM);
    int_text(h1, t, 0);
    cv_text(230 - text_w(&FONT_XS, t), 107, &FONT_XS, t, C_DIM);

    /* the base */
    if (p->nbase) {
        x = cv_text(10, 126, &FONT_XS, "BASE", C_GRAY) + 8;
        for (i = 0; i < p->nbase && x < 220; i++) {
            note_of(u * (float)p->harm[i], nm, &c);
            x = cv_text(x, 126, &FONT_XS, nm, dr_level[i] > 0.5f * st.amp[i] ? C_WHITE : C_DIM) + 10;
        }
    }
}

/* a chord of 3 to 8 tones: its ratio across the top, then the tones in two columns of four, rising down each:
 * the note, its cents from equal temperament, a bar for its level */
static void draw_chord(float u)
{
    const piece_t *p = cur();
    char t[64], nm[8];
    const felucca_font_t *f;
    int32_t i, c, w;
    float amax = 1e-6f;
    {   /* the ratio in lowest terms */
        uint32_t g = 0, a, b;
        for (i = 0; i < p->n; i++) {
            for (a = g, b = p->harm[i]; b;) {
                uint32_t r = a % b;
                a = b;
                b = r;
            }
            g = a;
        }
        t[0] = 0;
        for (i = 0; i < p->n; i++) {
            if (i)
                text_cat(t, ":");
            int_text((int32_t)(p->harm[i] / (g ? g : 1u)), nm, 0);
            text_cat(t, nm);
        }
    }
    f = text_w(&FONT_B, t) <= 224 ? &FONT_B : &FONT_XS;
    w = text_w(f, t);
    cv_text(120 - w / 2, 20, f, t, C_HI);
    for (i = 0; i < p->n; i++)
        amax = fm_maxf(amax, st.amp[i]);
    for (i = 0; i < p->n; i++) {
        int32_t x = i < 4 ? 10 : 126, y = 50 + (i & 3) * 24, bar;
        note_of(u * (float)p->harm[i], nm, &c);
        cv_text(x, y, &FONT_B, nm, C_WHITE);
        int_text(c, t, 1);
        text_cat(t, "C");
        cv_text(x + 42, y + 3, &FONT_XS, t, c ? C_AMB : C_DIM);
        bar = (int32_t)(fm_clampf(dr_level[i] / amax, 0.0f, 1.0f) * 26.0f + 0.5f);
        cv_rect(x + 76, y + 6, 26, 4, C_LINE);
        if (bar)
            cv_rect(x + 76, y + 6, bar, 4, glow_rgb((float)i / (float)(p->n - 1), 1.0f));
    }
}

static void draw(void)
{
    const piece_t *p = cur();
    float u = unit_hz();
    char t[32], nm[8];
    int32_t i, y;
    uint32_t now = plat_ms();

    /* header: name and state, or a message for a moment */
    cv_begin(240, 30, C_BLACK);
    if (now < ui.msg_until) {
        int32_t x = cv_text(8, 6, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], C_HI);
    } else {
        int32_t x = cv_text(8, 6, &FONT_B, APP_NAME, C_WHITE);
        t[0] = 0;                        /* the audio's share of the CPU: how many tones the chip can hold */
        text_cat(t, "CPU ");
        int_text((int32_t)plat_cpu_pct(), nm, 0);
        text_cat(t, nm);
        text_cat(t, "%");
        cv_text(x + 12, 9, &FONT_XS, t, C_DIM);
        cv_text(232 - text_w(&FONT_XS, set.playing ? "PLAYING" : "STOPPED"), 9, &FONT_XS,
                set.playing ? "PLAYING" : "STOPPED", set.playing ? C_HI : C_DIM);
    }
    cv_blit(0, 0);

    /* the piece, the ratio, the tones */
    cv_begin(240, 150, C_BLACK);
    cv_text(120 - text_w(&FONT_XS, p->name) / 2, 2, &FONT_XS, p->name, C_GRAY);
    if (p->n > 2) {
        if (p->n > 8)
            draw_cluster(u);
        else
            draw_chord(u);
        cv_blit(0, 34);
        goto knobs;
    }
    draw_ratio(22);
    for (i = 0, y = 66; i < p->n && y < 146; i++, y += 20) {
        int32_t c, bar;
        float hz = u * (float)p->harm[i];
        int_text(p->harm[i], t, 0);
        cv_text(10, y, &FONT_XS, t, C_GRAY);
        note_of(hz, nm, &c);
        cv_text(40, y, &FONT_B, nm, C_WHITE);
        int_text(c, t, 1);
        text_cat(t, "C");
        cv_text(84, y + 3, &FONT_XS, t, c ? C_AMB : C_DIM);
        hz_text(hz, t);
        cv_text(124, y + 3, &FONT_XS, t, C_GRAY);
        bar = (int32_t)(fm_clampf(dr_level[i] / fm_maxf(st.amp[i], 1e-6f), 0.0f, 1.0f) * 40.0f + 0.5f);
        cv_rect(190, y + 6, 40, 4, C_LINE);
        if (bar)
            cv_rect(190, y + 6, bar, 4, C_HI);
    }
    cv_blit(0, 34);

knobs:
    cv_begin(240, 50, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    for (i = 0; i < 4; i++) {            /* this page's four */
        int32_t x = 4 + i * 57, k = ui.page * 4 + i;
        knob_text(k, t);
        cv_text(x, 8, &FONT_XS, KNOB_NAME[k], C_GRAY);
        cv_text(x, 24, &FONT_B, t, C_HI);
    }
    cv_rect(226, 12, 6, 6, ui.page ? C_LINE : C_GRAY);    /* which page: two dots, SELECT turns */
    cv_rect(226, 26, 6, 6, ui.page ? C_GRAY : C_LINE);
    cv_blit(0, 188);
}

void app_ui_init(void)
{
    static uint8_t once;
    palette_dream();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.redraw = 1;
    if (!once) {                         /* the first time only: the opening */
        once = 1;
        ui.opening = 1;
        ui.open_t0 = plat_ms();
    }
}

/* the opening: the glow swells from the boot screen's 0.3 to full over 3 s, as the drone swells in, holds,
 * then gives way to the screen at 4.5 s (or at the first touch of a control) */
static void opening_frame(void)
{
    static uint32_t last;
    uint32_t now = plat_ms(), e = now - ui.open_t0;
    float a;
    if (e >= 4500u || ui.btn) {
        ui.opening = 0;
        lcd_fill(0, 0, 240, 240, C_BLACK);
        ui.redraw = 1;
        return;
    }
    if (now - last < 50u)
        return;
    last = now;
    a = fm_minf((float)e / 3000.0f, 1.0f);
    a = a * a * (3.0f - 2.0f * a);       /* eased */
    draw_glow(0.3f + 0.7f * a);
}

void app_frame(void)
{
    static uint32_t last_draw;
    uint32_t now = plat_ms();
    input();
    evolve_tick();
    speak();
    if (ui.opening) {
        opening_frame();
        if (ui.opening)
            return;
    }
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (now - last_draw >= 100u)         /* the level bars move as tones swell and fade */
        ui.redraw = 1;
    if (ui.redraw) {
        ui.redraw = 0;
        last_draw = now;
        draw();
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "playing"))
        *got = set.playing;
    else if (!strcmp(what, "piece"))
        *got = set.piece;
    else if (!strcmp(what, "semi"))
        *got = set.semi;
    else if (!strcmp(what, "fine"))
        *got = set.fine;
    else if (!strcmp(what, "variant"))
        *got = set.variant;
    else if (!strcmp(what, "ntones"))
        *got = cur()->n;
    else if (!strcmp(what, "density"))
        *got = set.density;
    else if (!strcmp(what, "balance"))
        *got = set.balance;
    else if (!strcmp(what, "sounding"))
        *got = n_sounding;
    else if (!strcmp(what, "drift"))
        *got = set.drift;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "unit_inc"))
        *got = (int)dr_unit_inc;
    else if (!strncmp(what, "level", 5))    /* levelN: tone N's level, thousandths */
        *got = (int)(dr_level[atoi(what + 5) % DR_MAXT] * 1000.0f + 0.5f);
    else
        return 0;
    return 1;
}
#endif
