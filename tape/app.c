/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Seth Graves; built on fm1-base by Charles Vestal */
/* TAPE: the app. The engine (dsp/tape.c), its few controls, and the tape's own wear, kept between sessions.
 *
 *   PLAY        the music on / off (it fades; the tape keeps running, so the hiss stays)
 *   PRESET      the tape: CASSETTE, MICROCASSETTE, VHS, REEL TO REEL
 *   ALGORITHM   the scene: GLIDE, SUNROOM, MEMORY, RIVER
 *   K1 AGE      new .. ruined: wow, flutter, hiss, dull highs, dropouts, drag, print-through, all at once
 *   K2 DENSITY  how many notes                  K3 SPACE   the room           K4 DRIVE   how hard it hits the tape
 *   SELECT      the second page: K1 PADS, K2 BELLS, K3 FLUTE, K4 NATURE (levels)
 *               the third, the loop: K1 BASINSKI (from the machine to the loop), K2 FEED (new music onto it each
 *               pass), K3 LENGTH (seconds), K4 DECAY (how fast it disintegrates)
 *   REC         CAPTURE: a fresh loop, recorded from the machine (lit while it records the first pass)
 *               the fourth, SETUP: K1 KEY (SCENE: each scene its own; or a flute's key, C .. B: every scene moves
 *               so the flute's minor pentatonic sits in it), K2 TUNE (A = 415 .. 446 Hz, 432 by default)
 *   OCT- OCT+   the tuning too, a hertz at a time
 *   HOME (hold) the tape's wear back to new
 *   SAVE        keeps the settings for the next power-on
 *   MASTER      volume
 *
 * Wear: the tape wears as it plays, and it remembers. Every hour of playing ages it a little more, on top of AGE,
 * up to +40 after ten hours; it is written to the flash every ten minutes of play and when the music stops.
 *
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in scope. */
#include "fastmath.h"
#include "tape.h"

#define SET_MAGIC 0x54505633u            /* "TPV3" */
enum { K_AGE, K_DENSITY, K_SPACE, K_DRIVE, K_PADS, K_BELLS, K_FLUTE, K_NATURE, K_BASINSKI, K_FEED, K_LENGTH,
       K_DECAY, K_KEY, K_TUNE, NK };
#define NPAGES 4
typedef struct {
    uint32_t magic;
    uint8_t playing, type, scene, rsv;
    int8_t k[NK];                        /* 0..100 each; LENGTH seconds; KEY 0 (the scene's) .. 12; TUNE A less 400 Hz */
    uint8_t rsv2[2];
} settings_t;
static settings_t set;
static uint8_t set_dirty;

#define WEAR_MAGIC 0x57454152u           /* "WEAR" */
typedef struct {
    uint32_t magic;
    uint32_t seconds;                    /* played, all told */
} wear_t;
static wear_t wear;
static uint32_t wear_saved;              /* the seconds the flash has */

static const int8_t K_DEF[NK] = {30, 50, 50, 40, 80, 60, 40, 30, 0, 0, 12, 40, 0, 32};
static const char *const KNOB_NAME[NK] = {"AGE", "DENSITY", "SPACE", "DRIVE", "PADS", "BELLS", "FLUTE", "NATURE",
                                          "BASINSKI", "FEED", "LENGTH", "DECAY", "KEY", "TUNE"};
static const char *const KEY_NAME[13] = {"SCENE", "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
/* each knob's range and step a detent: LENGTH in seconds, the rest 0..100 */
static int32_t k_lo(int k) { return k == K_LENGTH ? 2 : k == K_TUNE ? 15 : 0; }
static int32_t k_hi(int k) { return k == K_LENGTH ? 17 : k == K_KEY ? 12 : k == K_TUNE ? 46 : 100; }
static int32_t k_step(int k) { return k == K_LENGTH || k == K_KEY || k == K_TUNE ? 1 : 2; }

static void settings_defaults(void)
{
    int i;
    set.magic = SET_MAGIC;
    set.playing = 1;
    set.type = TP_CASSETTE;
    set.scene = 0;
    for (i = 0; i < NK; i++)
        set.k[i] = K_DEF[i];
}

static int settings_ok(void)
{
    int i;
    if (set.magic != SET_MAGIC || set.playing > 1u || set.type >= TP_NTYPES || set.scene >= TP_NSCENES
)
        return 0;
    for (i = 0; i < NK; i++)
        if (set.k[i] < k_lo(i) || set.k[i] > k_hi(i))
            return 0;
    return 1;
}

/* the wear's share of the age: +0.4 at ten hours, most of it early (a square root), none past */
static float wear_age(void)
{
    float h = (float)wear.seconds * (1.0f / 3600.0f);
    return 0.4f * fm_sqrtf(fm_minf(h * 0.1f, 1.0f));
}

static int wear_pct(void) { return (int)(wear_age() * 100.0f + 0.5f); }

static void wear_save(void)
{
    if (wear.seconds != wear_saved && plat_store_save(OBJ_WEAR, &wear, sizeof wear) == 0)
        wear_saved = wear.seconds;
}

static void engine_push(void)
{
    tp_cfg.playing = set.playing;
    tp_cfg.type = set.type;
    tp_cfg.scene = set.scene;
    tp_cfg.age = fm_clampf((float)set.k[K_AGE] * 0.01f + wear_age(), 0.0f, 1.0f);
    tp_cfg.density = (float)set.k[K_DENSITY] * 0.01f;
    tp_cfg.space = (float)set.k[K_SPACE] * 0.01f;
    tp_cfg.drive = (float)set.k[K_DRIVE] * 0.01f;
    tp_cfg.pads = (float)set.k[K_PADS] * 0.01f;
    tp_cfg.bells = (float)set.k[K_BELLS] * 0.01f;
    tp_cfg.flute = (float)set.k[K_FLUTE] * 0.01f;
    tp_cfg.nature = (float)set.k[K_NATURE] * 0.01f;
    tp_cfg.basinski = (float)set.k[K_BASINSKI] * 0.01f;
    tp_cfg.feed = (float)set.k[K_FEED] * 0.01f;
    tp_cfg.length = (float)set.k[K_LENGTH];
    tp_cfg.decay = (float)set.k[K_DECAY] * 0.01f;
    tp_cfg.a4 = 400.0f + (float)set.k[K_TUNE];
    tp_cfg.key = (uint8_t)set.k[K_KEY];
}

static void text_cat(char *d, const char *s);
static void int_text(int32_t v, char *b);
static void tune_text(char *t)           /* "A = 432" */
{
    char n[8];
    t[0] = 0;
    text_cat(t, "A = ");
    int_text(400 + set.k[K_TUNE], n);
    text_cat(t, n);
}

void app_init(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || !settings_ok())
        settings_defaults();
    if (plat_store_load(OBJ_WEAR, &wear, sizeof wear) != (int)sizeof wear || wear.magic != WEAR_MAGIC) {
        wear.magic = WEAR_MAGIC;
        wear.seconds = 0;
    }
    wear_saved = wear.seconds;
    engine_push();
    tp_init();
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port))
        ;
    tp_render(lr, n, (float)master_q12 * (1.0f / 4096.0f));
}

void app_panic(void) { tp_panic(); }

/* ------------------------------------------------------------------ text --- */
static struct {
    char msg[2][28];
    uint32_t msg_until;
    uint32_t btn;
    uint32_t home_t;                     /* HOME went down (0: not held, or its hold already acted) */
    uint32_t sec_t;                      /* the last second of play counted */
    uint32_t cpu_peak;                   /* the CPU's peak, counted from 3 s after power-on */
    uint8_t page, redraw;
} ui;

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

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

/* a knob's value as text: LENGTH in seconds, KEY a note (or SCENE), TUNE in hertz */
static void value_text(int k, char *t, int words)
{
    t[0] = 0;
    if (k == K_KEY) {
        text_cat(t, KEY_NAME[set.k[k] % 13]);
        if (words && set.k[k])
            text_cat(t, " FLUTE");
        return;
    }
    int_text(k == K_TUNE ? 400 + set.k[k] : set.k[k], t);
    if (k == K_LENGTH)
        text_cat(t, words ? " S" : "S");
}

/* ---------------------------------------------------------------- input --- */
static void knob_turn(int k, int32_t e)
{
    char t[12];
    int32_t v = clamp((int32_t)set.k[k] + k_step(k) * e, k_lo(k), k_hi(k));
    if (v != set.k[k]) {
        set.k[k] = (int8_t)v;
        set_dirty = 1;
        engine_push();
    }
    value_text(k, t, 1);
    app_say(KNOB_NAME[k], t);
}

static void wear_reset(void)
{
    wear.seconds = 0;
    wear_save();
    engine_push();
    app_say("WEAR", "NEW TAPE");
}

static void input(void)
{
    uint32_t btn = plat_buttons(), k, now = plat_ms();
    int32_t e;
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_PLAY) {
            set.playing = (uint8_t)!set.playing;
            set_dirty = 1;
            engine_push();
            if (!set.playing)
                wear_save();
            app_say(set.playing ? "PLAY" : "STOP", "");
        } else if (k == B_SAVE) {
            if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) {
                set_dirty = 0;
                wear_save();
                app_say("SAVED", "");
            } else {
                app_say("SAVE", "FAILED");
            }
        } else if (k == B_OCTDN || k == B_OCTUP) {
            char t[12];
            int32_t v = clamp((int32_t)set.k[K_TUNE] + (k == B_OCTUP ? 1 : -1), 15, 46);
            if (v != set.k[K_TUNE]) {
                set.k[K_TUNE] = (int8_t)v;
                set_dirty = 1;
                engine_push();
            }
            tune_text(t);
            app_say("TUNING", t);
        } else if (k == B_REC) {
            tp_cfg.capture++;
            app_say("CAPTURE", "A FRESH LOOP");
        } else if (k == B_HOME) {
            ui.home_t = now | 1u;
            app_say("HOLD HOME", "TO RESET WEAR");
        }
    }
    if (ui.home_t && !(btn & (1u << B_HOME)))
        ui.home_t = 0;
    else if (ui.home_t && now - ui.home_t >= 2000u) {
        ui.home_t = 0;
        wear_reset();
    }
    if ((e = plat_enc(EN_PRESET)) != 0) {
        int32_t p = clamp((int32_t)set.type + e, 0, TP_NTYPES - 1);
        if (p != set.type) {
            set.type = (uint8_t)p;
            set_dirty = 1;
            engine_push();
        }
        app_say(TP_TYPE_NAME[set.type], "");
    }
    if ((e = plat_enc(EN_ALGO)) != 0) {
        int32_t p = clamp((int32_t)set.scene + e, 0, TP_NSCENES - 1);
        if (p != set.scene) {
            set.scene = (uint8_t)p;
            set_dirty = 1;
            engine_push();
        }
        app_say(TP_SCENE_NAME[set.scene], "");
    }
    if ((e = plat_enc(EN_SELECT)) != 0) {
        static const char *const PAGE[NPAGES] = {"PAGE 1", "PAGE 2", "LOOP", "SETUP"};
        ui.page = (uint8_t)clamp((int32_t)ui.page + (e > 0 ? 1 : -1), 0, NPAGES - 1);
        app_say(PAGE[ui.page], "");
    }
    if ((e = plat_enc(EN_K1)) != 0)
        knob_turn(ui.page * 4 + 0, e);
    if ((e = plat_enc(EN_K2)) != 0)
        knob_turn(ui.page * 4 + 1, e);
    if ((e = plat_enc(EN_K3)) != 0 && ui.page * 4 + 2 < NK)
        knob_turn(ui.page * 4 + 2, e);
    if ((e = plat_enc(EN_K4)) != 0 && ui.page * 4 + 3 < NK)
        knob_turn(ui.page * 4 + 3, e);
    if (btn != ui.btn)
        ui.redraw = 1;
    ui.btn = btn;
    plat_glow(1);
    plat_leds((set.playing ? 1u << B_PLAY : 0u) | (set_dirty ? 1u << B_SAVE : 0u) | (tp_capturing ? 1u << B_REC : 0u),
              0);
}

void app_input_only(void) { input(); }

/* the wear: a second of play is a second of wear; to the flash every ten minutes of it */
static void wear_tick(void)
{
    uint32_t now = plat_ms();
    if (!ui.sec_t)
        ui.sec_t = now;
    while (now - ui.sec_t >= 1000u) {
        ui.sec_t += 1000u;
        if (set.playing) {
            float before = wear_age();
            wear.seconds++;
            if (wear_age() - before > 0.0f)
                engine_push();
            if (wear.seconds - wear_saved >= 600u)
                wear_save();
        }
    }
}

static void speak(void)
{
    char t[96], n[12];
    if (ann_on() && ann_where()) {
        t[0] = 0;
        text_cat(t, "TAPE. ");
        text_cat(t, TP_TYPE_NAME[set.type]);
        text_cat(t, ". ");
        text_cat(t, TP_SCENE_NAME[set.scene]);
        text_cat(t, ". AGE ");
        int_text(set.k[K_AGE], n);
        text_cat(t, n);
        text_cat(t, ". WEAR ");
        int_text(wear_pct(), n);
        text_cat(t, n);
        text_cat(t, ". ");
        tune_text(n);
        text_cat(t, n);
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

/* --------------------------------------------------------------- screen --- */
/* warm browns and amber, as old tape and its label */
static const uint16_t TPAL[5] = {RGB(58, 44, 34), RGB(116, 92, 70), RGB(196, 174, 150), RGB(226, 176, 112),
                                 RGB(255, 200, 120)};

static void circle(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t x = r, y = 0, err = 1 - r;
    while (x >= y) {
        cv_pset(cx + x, cy + y, c);
        cv_pset(cx + y, cy + x, c);
        cv_pset(cx - y, cy + x, c);
        cv_pset(cx - x, cy + y, c);
        cv_pset(cx - x, cy - y, c);
        cv_pset(cx - y, cy - x, c);
        cv_pset(cx + y, cy - x, c);
        cv_pset(cx + x, cy - y, c);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

static void disc(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t y;
    for (y = -r; y <= r; y++) {
        int32_t w = (int32_t)fm_sqrtf((float)(r * r - y * y));
        cv_rect(cx - w, cy + y, 2 * w + 1, 1, c);
    }
}

/* a reel: its pack of tape, the hub, three spokes turned to a */
static void reel(int32_t cx, int32_t cy, int32_t pack, float a)
{
    int32_t i;
    disc(cx, cy, pack, C_LINE);
    circle(cx, cy, pack, C_DIM);
    disc(cx, cy, 12, C_BLACK);
    circle(cx, cy, 12, C_GRAY);
    for (i = 0; i < 3; i++) {
        float t = a + (float)i * (1.0f / 3.0f);
        int32_t x = (int32_t)(10.0f * fm_cosf(t * 6.2831853f)), y = (int32_t)(10.0f * fm_sinf(t * 6.2831853f));
        cv_line(cx + x / 3, cy + y / 3, cx + x, cy + y, C_HI);
    }
}

/* the tape gauge: the loop laid out flat, its oxide amber where it holds and dark where it has come off, the head a
 * white line; while capturing, the recorded part fills in behind the head */
static void gauge(int32_t gx, int32_t gy, int32_t gw, int32_t gh)
{
    uint32_t len = tp_loop_len, ncell = (len + TP_CELL - 1u) / TP_CELL, pos = tp_loop_pos;
    int32_t i, head = gx + (int32_t)(pos * (uint32_t)gw / (len ? len : 1u))   /* < 2^26: 32 bits do */;
    cv_rect(gx - 1, gy - 1, gw + 2, gh + 2, C_LINE);
    for (i = 0; i < gw; i++) {
        uint32_t a = (uint32_t)i * ncell / (uint32_t)gw, b = (uint32_t)(i + 1) * ncell / (uint32_t)gw, d = 0;
        uint16_t c;
        if (b <= a)
            b = a + 1u;
        for (; a < b && a < TP_NCELL; a++)
            if (tp_damage[a] > d)
                d = tp_damage[a];
        if (tp_capturing && gx + i > head)
            c = C_BLACK;                 /* not yet recorded */
        else
            c = d > 150u ? C_BLACK : d > 60u ? C_LINE : d > 8u ? C_DIM : C_AMB;
        cv_rect(gx + i, gy, 1, gh, c);
    }
    cv_rect(head, gy - 3, 1, gh + 6, C_WHITE);
}

static void draw(void)
{
    char t[40], n[12];
    int32_t i, x;
    uint32_t now = plat_ms();
    float turns = tp_reel_turns, drop = tp_drop;
    int big = set.type == TP_REEL, small = set.type == TP_MICRO;

    cv_begin(240, 30, C_BLACK);
    if (now < ui.msg_until) {
        x = cv_text(8, 6, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], C_HI);
    } else {
        x = cv_text(8, 6, &FONT_B, "TAPE", C_WHITE);
        t[0] = 0;                        /* the audio's share of the CPU, and its peak since power-on */
        text_cat(t, "CPU ");
        int_text((int32_t)plat_cpu_pct(), n);
        text_cat(t, n);
        text_cat(t, "% / ");
        int_text((int32_t)ui.cpu_peak, n);
        text_cat(t, n);
        text_cat(t, "%");
        cv_text(x + 10, 9, &FONT_XS, t, C_DIM);
        cv_text(232 - text_w(&FONT_XS, TP_TYPE_NAME[set.type]), 9, &FONT_XS, TP_TYPE_NAME[set.type], C_AMB);
    }
    cv_blit(0, 0);

    /* the tape: a shell (or two open reels), the reels turning with the transport's wobble */
    cv_begin(240, 156, C_BLACK);
    {
        int32_t cy = 52, sep = big ? 112 : small ? 64 : 84, pack = big ? 46 : small ? 22 : 30,
                lx = 120 - sep / 2, rx = 120 + sep / 2;
        float fill = 0.5f + 0.5f * fm_sin_turns(turns * 0.002f);   /* the tape winding from one side to the other */
        int32_t pl = 13 + (int32_t)((float)(pack - 13) * fill), pr = 13 + (int32_t)((float)(pack - 13) * (1.0f - fill));
        if (!big) {
            int32_t w = sep + 2 * pack + 24, h = 2 * pack + 36;
            cv_rect(120 - w / 2, cy - pack - 18, w, h, C_LINE);
            cv_rect(120 - w / 2 + 2, cy - pack - 16, w - 4, h - 4, C_BLACK);
            cv_rect(120 - w / 2 + 10, cy - pack - 12, w - 20, 10, C_AMB);    /* the label */
        }
        reel(lx, cy, pl, turns);
        reel(rx, cy, pr, turns);
        cv_line(lx, cy + pl, rx, cy + pr, drop > 0.2f ? C_DIM : C_GRAY);   /* the tape between them */
        (void)n;
        if (drop > 0.2f)
            cv_rect(118, cy + (pl + pr) / 2 - 3, 4, 6, C_WHITE);             /* a dropout, as it passes */
    }
    gauge(10, 110, 220, 12);
    t[0] = 0;
    text_cat(t, TP_SCENE_NAME[set.scene]);
    x = cv_text(10, 130, &FONT_B, t, C_HI);
    if (set.k[K_KEY]) {                  /* for a flute: its key */
        t[0] = 0;
        text_cat(t, KEY_NAME[set.k[K_KEY] % 13]);
        cv_text(x + 6, 133, &FONT_XS, t, C_AMB);
    }
    t[0] = 0;
    if (tp_capturing) {
        text_cat(t, "CAPTURING");
    } else {
        int_text((int32_t)tp_passes, n);
        text_cat(t, n);
        text_cat(t, tp_passes == 1u ? " PASS" : " PASSES");
    }
    text_cat(t, "  WEAR ");
    int_text(wear_pct(), n);
    text_cat(t, n);
    if (!set.playing)
        text_cat(t, "  STOP");
    cv_text(230 - text_w(&FONT_XS, t), 133, &FONT_XS, t, C_GRAY);
    cv_blit(0, 30);

    cv_begin(240, 52, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    for (i = 0; i < 4; i++) {
        int32_t k = ui.page * 4 + i;
        x = 4 + i * 57;
        if (k >= NK)
            break;
        value_text(k, t, 0);
        cv_text(x, 8, &FONT_XS, KNOB_NAME[k], C_GRAY);
        cv_text(x, 24, &FONT_B, t, C_HI);
    }
    for (i = 0; i < NPAGES; i++)          /* which page: three dots, SELECT turns */
        cv_rect(228, 4 + 11 * i, 5, 5, ui.page == i ? C_GRAY : C_LINE);
    cv_blit(0, 188);
}

void app_ui_init(void)
{
    int k;
    for (k = 0; k < 5; k++)
        pal[k] = TPAL[k];
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.redraw = 1;
}

void app_frame(void)
{
    static uint32_t last_draw;
    uint32_t now = plat_ms();
    input();
    wear_tick();
    if (now > 3000u && plat_cpu_pct() > ui.cpu_peak)
        ui.cpu_peak = plat_cpu_pct();
    speak();
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (now - last_draw >= 50u)          /* the reels turn */
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
    else if (!strcmp(what, "type"))
        *got = set.type;
    else if (!strcmp(what, "scene"))
        *got = set.scene;
    else if (!strcmp(what, "age"))
        *got = set.k[K_AGE];
    else if (!strcmp(what, "wear_s"))
        *got = (int)wear.seconds;
    else if (!strcmp(what, "wear_pct"))
        *got = wear_pct();
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "notes"))
        *got = (int)tp_notes;
    else if (!strcmp(what, "ghosts"))
        *got = (int)tp_ghosts;
    else if (!strcmp(what, "drops"))
        *got = (int)tp_drops;
    else if (!strcmp(what, "drags"))
        *got = (int)tp_drags;
    else if (!strcmp(what, "tune"))
        *got = 400 + set.k[K_TUNE];
    else if (!strcmp(what, "key"))
        *got = set.k[K_KEY];
    else if (!strcmp(what, "passes"))
        *got = (int)tp_passes;
    else if (!strcmp(what, "capturing"))
        *got = tp_capturing;
    else if (!strcmp(what, "loop_len"))
        *got = (int)tp_loop_len;
    else if (!strcmp(what, "damage")) {   /* the damage map's sum */
        uint32_t i, sum = 0;
        for (i = 0; i < TP_NCELL; i++)
            sum += tp_damage[i];
        *got = (int)sum;
    } else if (!strcmp(what, "chord"))
        *got = tp_chord;
    else if (!strcmp(what, "peak")) {    /* the output's peak since last read, thousandths */
        *got = (int)(tp_peak * 1000.0f);
        tp_peak = 0.0f;
    } else
        return 0;
    return 1;
}
#endif
