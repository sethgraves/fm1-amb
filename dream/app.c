/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Seth Graves; built on fm1-base by Charles Vestal */
/* DREAM: the app. Presets of whole-number harmonics on one unit, the drone (dsp/drone.c) holding them, a screen
 * that shows each tone's harmonic, its note and how far it sits from equal temperament.
 *
 *   PLAY        the drone on / off (it swells in and fades out)
 *   PRESET      the piece
 *   K1 PITCH    the series up or down, semitones          K2 FINE     cents
 *   K3 SPREAD   the tones across the stereo field         K4 TILT     dB per octave up the series
 *   MASTER      volume                                     SAVE        keeps all of it for the next power-on
 *
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in scope. */
#include "fastmath.h"
#include "drone.h"

/* ---------------------------------------------------------------- pieces --- */
/* each: a unit frequency (Hz, before PITCH and FINE) and the harmonics that sound on it */
typedef struct {
    const char *name;                    /* on the screen (capitals) */
    float unit_hz;
    uint8_t n;
    uint8_t nbase;                       /* the first nbase harmonics are the base, under the rest */
    uint16_t harm[DR_MAXT];              /* rising */
} piece_t;

#define B3_HZ 246.941651f                /* B below middle C, A = 440 */
#define WTP_EB3 148.994709f              /* The Well-Tuned Piano's Eb (440 * 128/189, an octave down) */
static const piece_t PIECES[] = {
    /* Composition 1960 #7: B and F#, "to be held for a long time". Here as a pure fifth: harmonics 2 and 3 of
     * a unit an octave under the B, so the F# sits 2 cents above equal temperament */
    {"COMPOSITION 1960 #7", B3_HZ * 0.5f, 2, 0, {2, 3}},
    /* The Base 9:7:4 Symmetry in Prime Time When Centered above and below The Lowest Term Primes in The Range 288
     * to 224 with The Addition of 279 and 261 ... with The Addition of 119 (the Dream House), worked from its title:
     *   above, to 288: the primes 128-144 (131, 137, 139) and 64-72 (67, 71) times powers of 2: 262 274 278 268 284
     *   below, to 224: the primes 112-126 (113), 56-63 (59, 61) and 28-31.5 (29, 31) likewise: 226 236 244 232 248
     *   with 288 and 224, the additions 279 and 261, and 2 x 119 = 238
     * The base 9:7:4 as harmonics 16, 28, 36, two octaves under 36:28 = 9:7 = 288:224's own ratio.
     * The unit 4.5 Hz puts the cluster at 1008 - 1296 Hz and the base at 72, 126, 162 Hz; PITCH moves it all. */
    {"DREAM HOUSE 9:7:4", 4.5f, 18, 3,
     {16, 28, 36, 224, 226, 232, 236, 238, 244, 248, 261, 262, 268, 274, 278, 279, 284, 288}},
    /* The Well-Tuned Piano: fifths and harmonic sevenths over Eb, its A (189/128) at 440 Hz, so Eb = 297.99 Hz
     * (Gann 1993). The Opening Chord: Eb Bb C Eb F Bb, 4:6:7:8:9:12, here from the Eb an octave under that */
    {"THE OPENING CHORD", WTP_EB3 * 0.25f, 6, 0, {4, 6, 7, 8, 9, 12}},
    /* The Magic Chord: the keys E F A Bb D E G A, which sound 567/512, 9/8, 189/128, 3/2, 63/32 of Eb, then
     * the E, G (21/16) and A an octave up: 567 576 756 768 1008 1134 1344 1512 on Eb/512, or a third of that.
     * No Eb: the chord's fundamental is absent. Its top A is the 440. */
    {"THE MAGIC CHORD", WTP_EB3 * 3.0f / 512.0f, 8, 0, {189, 192, 252, 256, 336, 378, 448, 504}},
};
#define NPIECES ((int)(sizeof PIECES / sizeof PIECES[0]))

/* --------------------------------------------------------------- settings --- */
#define SET_MAGIC 0x44524D31u            /* "DRM1" */
typedef struct {
    uint32_t magic;
    uint8_t piece;
    int8_t semi;                         /* PITCH: -12 .. +12 */
    int8_t fine;                         /* FINE: cents, -50 .. +50 */
    uint8_t spread;                      /* 0 .. 100 */
    int8_t tilt;                         /* half dB per octave, -12 .. +12 (-6 .. +6 dB) */
    uint8_t playing;
    uint8_t rsv[2];
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
}

static int settings_ok(void)
{
    return set.magic == SET_MAGIC && set.piece < NPIECES && set.semi >= -12 && set.semi <= 12 && set.fine >= -50 &&
           set.fine <= 50 && set.spread <= 100 && set.tilt >= -12 && set.tilt <= 12 && set.playing <= 1;
}

/* ------------------------------------------------------------ the drone --- */
static dr_state_t st;
static uint8_t st_dirty;                 /* a change not yet taken by the render */

static float unit_hz(void)
{
    const piece_t *p = &PIECES[set.piece];
    return p->unit_hz * fm_exp2f(((float)set.semi * 100.0f + (float)set.fine) * (1.0f / 1200.0f));
}

static void drone_update(void)
{
    const piece_t *p = &PIECES[set.piece];
    float sum = 0.0f, s = (float)set.spread * 0.01f, db_oct = (float)set.tilt * 0.5f;
    uint16_t hmin = 0xFFFFu;
    int i;
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
        st.amp[i] /= sum;
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
    default: {                           /* half dB steps: "-1.5", "0", "+3" */
        int32_t a = set.tilt < 0 ? -set.tilt : set.tilt;
        text_cat(t, set.tilt < 0 ? "-" : set.tilt > 0 ? "+" : "");
        int_text(a / 2, u, 0);
        text_cat(t, u);
        if (a & 1)
            text_cat(t, ".5");
        break;
    }
    }
}
static const char *const KNOB_NAME[4] = {"PITCH", "FINE", "SPREAD", "TILT"};

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
    default:
        set.tilt = (int8_t)clamp(set.tilt + e, -12, 12);
        break;
    }
    set_dirty = 1;
    drone_update();
    knob_text(k, t);
    app_say(KNOB_NAME[k], t);
}

static void speak(void)                  /* a screen reader that has just started listening hears the screen */
{
    char t[96];
    if (ann_on() && ann_where()) {
        t[0] = 0;
        text_cat(t, APP_NAME ". ");
        text_cat(t, PIECES[set.piece].name);
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
            set_dirty = 1;
            drone_update();
        }
        app_say(PIECES[set.piece].name, "");
    }
    if ((e = plat_enc(EN_K1)) != 0)
        knob_turn(0, e);
    if ((e = plat_enc(EN_K2)) != 0)
        knob_turn(1, e);
    if ((e = plat_enc(EN_K3)) != 0)
        knob_turn(2, e);
    if ((e = plat_enc(EN_K4)) != 0)
        knob_turn(3, e);
    (void)plat_enc(EN_SELECT);
    (void)plat_enc(EN_ALGO);
    if (btn != ui.btn)
        ui.redraw = 1;
    ui.btn = btn;
    plat_glow(1);
    plat_leds((set.playing ? 1u << B_PLAY : 0u) | (set_dirty ? 1u << B_SAVE : 0u), 0);
    drone_push();
}

void app_input_only(void) { input(); }

/* the ratio between the lowest two tones, large: "3:2" (the big fonts have no colon: two dots drawn) */
static void draw_ratio(int32_t cy)
{
    const piece_t *p = &PIECES[set.piece];
    char a[8], b[8];
    int32_t wa, wb, x;
    if (p->n < 2)
        return;
    int_text(p->harm[1], a, 0);
    int_text(p->harm[0], b, 0);
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
    const piece_t *p = &PIECES[set.piece];
    int32_t i, h0, h1, x, c;
    char t[24], nm[8];
    float amax = 1e-6f;
    if (p->nbase >= p->n)
        return;
    h0 = p->harm[p->nbase];
    h1 = p->harm[p->n - 1];
    for (i = 0; i < p->n; i++)
        amax = fm_maxf(amax, st.amp[i]);

    /* "15 TONES 224 - 288", then the band in notes and Hz */
    int_text(p->n - p->nbase, t, 0);
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

    /* the cluster: a line per tone, along its harmonic number */
    cv_rect(10, 104, 220, 1, C_LINE);
    for (i = p->nbase; i < p->n; i++) {
        int32_t hgt = (int32_t)(fm_clampf(dr_level[i] / amax, 0.0f, 1.0f) * 50.0f + 0.5f);
        x = 10 + (h1 > h0 ? (p->harm[i] - h0) * 218 / (h1 - h0) : 109);
        cv_rect(x, 54, 2, 50, C_LINE);
        if (hgt)
            cv_rect(x, 104 - hgt, 2, hgt, C_HI);
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
    const piece_t *p = &PIECES[set.piece];
    char t[64], nm[8];
    const felucca_font_t *f;
    int32_t i, c, w;
    float amax = 1e-6f;
    t[0] = 0;
    for (i = 0; i < p->n; i++) {
        if (i)
            text_cat(t, ":");
        int_text(p->harm[i], nm, 0);
        text_cat(t, nm);
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
            cv_rect(x + 76, y + 6, bar, 4, C_HI);
    }
}

static void draw(void)
{
    const piece_t *p = &PIECES[set.piece];
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
    for (i = 0; i < 4; i++) {
        int32_t x = 4 + i * 59;
        knob_text(i, t);
        cv_text(x, 8, &FONT_XS, KNOB_NAME[i], C_GRAY);
        cv_text(x, 24, &FONT_B, t, C_HI);
    }
    cv_blit(0, 188);
}

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.redraw = 1;
}

void app_frame(void)
{
    static uint32_t last_draw;
    uint32_t now = plat_ms();
    input();
    speak();
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
