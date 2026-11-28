/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Seth Graves; built on fm1-base by Charles Vestal */
/* TAPE: the app. You play the engine (dsp/tape.c) from the keys or MIDI; the knobs are the tape.
 *
 *   KEYS        play (F3 .. G5, moved by OCT- / OCT+); MIDI notes in from USB and the TRS jack too
 *   PRESET      the patch: M-FLUTE, M-STRINGS, M-CHOIR (Mellotron-style), PAD, BELL, VIBES
 *   ALGORITHM   the tape: CASSETTE, MICROCASSETTE, VHS, REEL TO REEL
 *   PLAY        HOLD: the chord held down when it's pressed stays (or, nothing held, the next one played), and
 *               the keys play on over it; again: it goes
 *   REC         FREEZE: the loop holds what was just played and plays it, wearing; again: let it go
 *   SELECT      the page: TAPE (AGE, WOW, FLUTTER, DRIVE), ECHO (ECHO, TIME, FEEDBACK, SPACE),
 *               LOOP (BASINSKI, LENGTH, DECAY), SETUP (ATTACK, RELEASE, TAPE END, TUNE)
 *   OCT- OCT+   the keys an octave down or up (-2 .. +2)
 *   HOME (hold) the tape's wear back to new
 *   SAVE        keeps the settings for the next power-on
 *   MASTER      volume
 *
 * Wear: the tape wears as it plays, and remembers. Every hour of playing ages it a little more, on top of AGE, up
 * to +40 after ten hours; it is written to the flash every ten minutes of play.
 *
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in scope. */
#include "fastmath.h"
#include "tape.h"

#define SET_MAGIC 0x54505634u            /* "TPV4": the played instrument */
enum { K_AGE, K_WOW, K_FLUTTER, K_DRIVE, K_ECHO, K_TIME, K_FB, K_SPACE, K_BASINSKI, K_LENGTH, K_DECAY, K_ATTACK,
       K_RELEASE, K_TAPEEND, K_TUNE, NK };
#define NPAGES 4
static const int8_t PAGE_K[NPAGES][4] = {{K_AGE, K_WOW, K_FLUTTER, K_DRIVE},
                                         {K_ECHO, K_TIME, K_FB, K_SPACE},
                                         {K_BASINSKI, K_LENGTH, K_DECAY, -1},
                                         {K_ATTACK, K_RELEASE, K_TAPEEND, K_TUNE}};
static const char *const PAGE_NAME[NPAGES] = {"TAPE", "ECHO", "LOOP", "SETUP"};
static const int8_t K_DEF[NK] = {30, 50, 50, 40, 30, 40, 40, 40, 60, 8, 40, 50, 50, 0, 32};
static const char *const KNOB_NAME[NK] = {"AGE", "WOW", "FLUTTER", "DRIVE", "ECHO", "TIME", "FEEDBACK", "SPACE",
                                          "BASINSKI", "LENGTH", "DECAY", "ATTACK", "RELEASE", "TAPE END", "TUNE"};
/* each knob's range and step a detent: LENGTH seconds; TIME tens of ms; TAPE END off/on; TUNE A less 400 Hz */
static int32_t k_lo(int k) { return k == K_LENGTH ? 2 : k == K_TIME ? 5 : k == K_TUNE ? 15 : 0; }
static int32_t k_hi(int k) { return k == K_LENGTH ? 12 : k == K_TIME ? 110 : k == K_TAPEEND ? 1 : k == K_TUNE ? 46 : 100; }
static int32_t k_step(int k)
{
    return k == K_LENGTH || k == K_TAPEEND || k == K_TUNE ? 1 : 2;
}

typedef struct {
    uint32_t magic;
    uint8_t type, patch, rsv0;
    int8_t oct;                          /* -2 .. +2 */
    int8_t k[NK];
    uint8_t rsv;
} settings_t;
static settings_t set;
static uint8_t set_dirty;

#define WEAR_MAGIC 0x57454152u           /* "WEAR" */
typedef struct {
    uint32_t magic;
    uint32_t seconds;                    /* played, all told */
} wear_t;
static wear_t wear;
static uint32_t wear_saved;

static void settings_defaults(void)
{
    int i;
    set.magic = SET_MAGIC;
    set.type = TP_CASSETTE;
    set.patch = TP_FLUTE;
    set.rsv0 = 0;
    set.oct = 0;
    for (i = 0; i < NK; i++)
        set.k[i] = K_DEF[i];
}

static int settings_ok(void)
{
    int i;
    if (set.magic != SET_MAGIC || set.type >= TP_NTYPES || set.patch >= TP_NPATCH || set.oct < -2
        || set.oct > 2)
        return 0;
    for (i = 0; i < NK; i++)
        if (set.k[i] < k_lo(i) || set.k[i] > k_hi(i))
            return 0;
    return 1;
}

/* the wear's share of the age: +0.4 at ten hours, most of it early, none past */
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

static float kf(int k) { return (float)set.k[k] * 0.01f; }

static void engine_push(void)
{
    tp_cfg.type = set.type;
    tp_cfg.patch = set.patch;
    tp_cfg.age = fm_clampf(kf(K_AGE) + wear_age(), 0.0f, 1.0f);
    tp_cfg.wow = kf(K_WOW);
    tp_cfg.flutter = kf(K_FLUTTER);
    tp_cfg.drive = kf(K_DRIVE);
    tp_cfg.echo = kf(K_ECHO);
    tp_cfg.echo_time = (float)set.k[K_TIME] * 0.01f;
    tp_cfg.echo_fb = kf(K_FB) * 0.95f;
    tp_cfg.space = kf(K_SPACE);
    tp_cfg.basinski = kf(K_BASINSKI);
    tp_cfg.length = (float)set.k[K_LENGTH];
    tp_cfg.decay = kf(K_DECAY);
    tp_cfg.attack = kf(K_ATTACK);
    tp_cfg.release = kf(K_RELEASE);
    tp_cfg.tape_end = (uint8_t)set.k[K_TAPEEND];
    tp_cfg.a4 = 400.0f + (float)set.k[K_TUNE];
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
    tp_cfg.freeze = 0;
    engine_push();
    tp_init();
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    tp_render(lr, n, (float)master_q12 * (1.0f / 4096.0f));
}

void app_panic(void) { tp_panic(); }

/* ------------------------------------------------------------------ text --- */
static struct {
    char msg[2][28];
    uint32_t msg_until;
    uint32_t btn, keys;
    uint32_t home_t;
    uint32_t sec_t;
    uint32_t cpu_peak;
    uint8_t page, redraw;
    uint8_t keynote[NKEYS];              /* the note each key sent (the octave may move while it is held) */
    uint8_t down[128];                   /* notes held now, by a key or MIDI */
    uint8_t sounding[128];               /* notes the engine has on (held, or kept by HOLD) */
    uint8_t latched[128];                /* HOLD's chord */
    uint8_t ndown;
    uint8_t hold;                        /* 0 off; 1 armed (the next chord played is held); 2 holding */
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

/* a knob's value as text */
static void value_text(int k, char *t, int words)
{
    t[0] = 0;
    if (k == K_TAPEEND) {
        text_cat(t, set.k[k] ? "8 S" : "OFF");
        return;
    }
    if (k == K_TIME) {
        int_text(set.k[k] * 10, t);
        text_cat(t, words ? " MS" : "MS");
        return;
    }
    int_text(k == K_TUNE ? 400 + set.k[k] : set.k[k], t);
    if (k == K_LENGTH)
        text_cat(t, words ? " S" : "S");
}

/* ---------------------------------------------------------------- notes --- */
/* HOLD: pressed with keys down, those notes stay; pressed with none, the next chord played stays (held as it is
 * played, kept when the last key lets go). Either way the keys then play on over it, as usual. HOLD again: it goes. */
static void note_down(int m, int vel)
{
    if (m < 0 || m > 127)
        return;
    if (!ui.down[m])
        ui.ndown++;
    ui.down[m] = 1;
    ui.sounding[m] = 1;
    if (ui.hold == 1)
        ui.latched[m] = 1;
    tp_note_on(m, vel, ui.latched[m]);
}

static void note_up(int m)
{
    if (m < 0 || m > 127 || !ui.down[m])
        return;
    ui.down[m] = 0;
    ui.ndown--;
    if (!ui.latched[m]) {
        ui.sounding[m] = 0;
        tp_note_off(m);
    }
    if (ui.hold == 1 && ui.ndown == 0)
        ui.hold = 2;                     /* the chord is in */
}

static void hold_press(void)
{
    int i, any = 0;
    if (ui.hold) {                       /* let it go: what the keys don't hold stops */
        for (i = 0; i < 128; i++) {
            if (ui.latched[i] && !ui.down[i]) {
                ui.sounding[i] = 0;
                tp_note_off(i);
            }
            ui.latched[i] = 0;
        }
        ui.hold = 0;
        app_say("HOLD", "OFF");
        return;
    }
    for (i = 0; i < 128; i++)
        if (ui.down[i]) {
            ui.latched[i] = 1;
            tp_note_hold(i);
            any = 1;
        }
    if (any) {
        ui.hold = 1;                     /* the keys still down are the chord; more added until they all let go */
        app_say("HOLD", "ON");
    } else {
        ui.hold = 1;
        app_say("HOLD", "PLAY A CHORD");
    }
}

static void keys_and_midi(void)
{
    uint32_t keys = plat_keys(), ch = keys ^ ui.keys, pkt;
    int k, port;
    for (k = 0; k < NKEYS; k++)
        if (ch & (1u << k)) {
            if (keys & (1u << k)) {
                int m = 53 + k + 12 * set.oct;            /* F3 is key 0 */
                ui.keynote[k] = (uint8_t)m;
                note_down(m, 100);
            } else {
                note_up(ui.keynote[k]);
            }
        }
    ui.keys = keys;
    while (plat_midi_in(&pkt, &port)) {                   /* notes on any channel */
        uint32_t st = (pkt >> 8) & 0xF0u, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        if (st == 0x90u && d2)
            note_down((int)d1, (int)d2);
        else if (st == 0x80u || (st == 0x90u && !d2))
            note_up((int)d1);
        else if (st == 0xB0u && (d1 == 120u || d1 == 123u)) {   /* all sound off, all notes off */
            int i;
            for (i = 0; i < 128; i++)
                ui.down[i] = ui.sounding[i] = ui.latched[i] = 0;
            ui.ndown = 0;
            ui.hold = 0;
            tp_all_off();
        }
    }
}

/* ---------------------------------------------------------------- input --- */
static void knob_turn(int k, int32_t e)
{
    char t[16];
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
    char t[12];
    keys_and_midi();
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_PLAY) {
            hold_press();
        } else if (k == B_REC) {
            tp_cfg.freeze = (uint8_t)!tp_cfg.freeze;
            app_say(tp_cfg.freeze ? "FROZEN" : "FREEZE", tp_cfg.freeze ? "" : "OFF");
        } else if (k == B_OCTDN || k == B_OCTUP) {
            int32_t o = clamp((int32_t)set.oct + (k == B_OCTUP ? 1 : -1), -2, 2);
            if (o != set.oct) {
                set.oct = (int8_t)o;
                set_dirty = 1;
            }
            int_text(set.oct, t);
            app_say("OCTAVE", t);
        } else if (k == B_SAVE) {
            if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) {
                set_dirty = 0;
                wear_save();
                app_say("SAVED", "");
            } else {
                app_say("SAVE", "FAILED");
            }
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
        int32_t p = clamp((int32_t)set.patch + e, 0, TP_NPATCH - 1);
        if (p != set.patch) {
            set.patch = (uint8_t)p;
            set_dirty = 1;
            engine_push();
        }
        app_say(TP_PATCH_NAME[set.patch], "");
    }
    if ((e = plat_enc(EN_ALGO)) != 0) {
        int32_t p = clamp((int32_t)set.type + e, 0, TP_NTYPES - 1);
        if (p != set.type) {
            set.type = (uint8_t)p;
            set_dirty = 1;
            engine_push();
        }
        app_say(TP_TYPE_NAME[set.type], "");
    }
    if ((e = plat_enc(EN_SELECT)) != 0) {
        ui.page = (uint8_t)clamp((int32_t)ui.page + (e > 0 ? 1 : -1), 0, NPAGES - 1);
        app_say(PAGE_NAME[ui.page], "");
    }
    {
        static const int EN_K[4] = {EN_K1, EN_K2, EN_K3, EN_K4};
        int i;
        for (i = 0; i < 4; i++)
            if ((e = plat_enc(EN_K[i])) != 0 && PAGE_K[ui.page][i] >= 0)
                knob_turn(PAGE_K[ui.page][i], e);
    }
    if (btn != ui.btn)
        ui.redraw = 1;
    ui.btn = btn;
    plat_glow(1);
    {
        uint32_t lit = 0;                /* the keys lit for the notes sounding in their range */
        int i;
        for (i = 0; i < NKEYS; i++) {
            int m = 53 + i + 12 * set.oct;
            if (m >= 0 && m < 128 && ui.sounding[m])
                lit |= 1u << i;
        }
        plat_leds((ui.hold ? 1u << B_PLAY : 0u) | (tp_cfg.freeze ? 1u << B_REC : 0u) | (set_dirty ? 1u << B_SAVE : 0u),
                  lit);
    }
}

void app_input_only(void) { input(); }

/* the wear: a second of sound is a second of wear */
static void wear_tick(void)
{
    uint32_t now = plat_ms();
    if (!ui.sec_t)
        ui.sec_t = now;
    while (now - ui.sec_t >= 1000u) {
        ui.sec_t += 1000u;
        if (tp_voices || tp_frozen) {
            float before = wear_age();
            wear.seconds++;
            if (wear_age() != before)
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
        text_cat(t, TP_PATCH_NAME[set.patch]);
        text_cat(t, ". ");
        text_cat(t, TP_TYPE_NAME[set.type]);
        text_cat(t, ". PAGE ");
        text_cat(t, PAGE_NAME[ui.page]);
        text_cat(t, ". WEAR ");
        int_text(wear_pct(), n);
        text_cat(t, n);
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

/* --------------------------------------------------------------- screen --- */
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

/* the tape gauge: the loop laid out flat, the head a white line. Recording: dim, all of it. Frozen: the oxide amber
 * where it holds and dark where it has come off */
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
        if (!tp_frozen)
            c = C_LINE;
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
        t[0] = 0;
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

    cv_begin(240, 156, C_BLACK);
    {
        int32_t cy = 52, sep = big ? 112 : small ? 64 : 84, pack = big ? 46 : small ? 22 : 30,
                lx = 120 - sep / 2, rx = 120 + sep / 2;
        float fill = 0.5f + 0.5f * fm_sin_turns(turns * 0.002f);
        int32_t pl = 13 + (int32_t)((float)(pack - 13) * fill), pr = 13 + (int32_t)((float)(pack - 13) * (1.0f - fill));
        if (!big) {
            int32_t w = sep + 2 * pack + 24, h = 2 * pack + 36;
            cv_rect(120 - w / 2, cy - pack - 18, w, h, C_LINE);
            cv_rect(120 - w / 2 + 2, cy - pack - 16, w - 4, h - 4, C_BLACK);
            cv_rect(120 - w / 2 + 10, cy - pack - 12, w - 20, 10, C_AMB);
        }
        reel(lx, cy, pl, turns);
        reel(rx, cy, pr, turns);
        cv_line(lx, cy + pl, rx, cy + pr, drop > 0.2f ? C_DIM : C_GRAY);
        if (drop > 0.2f)
            cv_rect(118, cy + (pl + pr) / 2 - 3, 4, 6, C_WHITE);
    }
    gauge(10, 110, 220, 12);
    x = cv_text(10, 130, &FONT_B, TP_PATCH_NAME[set.patch], C_HI);
    t[0] = 0;
    if (set.oct) {
        text_cat(t, "OCT ");
        int_text(set.oct, n);
        text_cat(t, n);
        text_cat(t, "  ");
    }
    if (ui.hold)
        text_cat(t, ui.hold == 1 ? "HOLD...  " : "HOLD  ");
    if (tp_frozen) {
        text_cat(t, "FROZEN ");
        int_text((int32_t)tp_passes, n);
        text_cat(t, n);
    } else {
        text_cat(t, "WEAR ");
        int_text(wear_pct(), n);
        text_cat(t, n);
    }
    cv_text(230 - text_w(&FONT_XS, t), 133, &FONT_XS, t, tp_frozen ? C_AMB : C_GRAY);
    (void)x;
    cv_blit(0, 30);

    cv_begin(240, 52, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    for (i = 0; i < 4; i++) {
        int k = PAGE_K[ui.page][i];
        x = 4 + i * 57;
        if (k < 0)
            continue;
        value_text(k, t, 0);
        cv_text(x, 8, &FONT_XS, KNOB_NAME[k], C_GRAY);
        cv_text(x, 24, &FONT_B, t, C_HI);
    }
    for (i = 0; i < NPAGES; i++)
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
    if (now - last_draw >= 50u)
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
    if (!strcmp(what, "type"))
        *got = set.type;
    else if (!strcmp(what, "patch"))
        *got = set.patch;
    else if (!strcmp(what, "hold"))
        *got = ui.hold;
    else if (!strcmp(what, "oct"))
        *got = set.oct;
    else if (!strcmp(what, "frozen"))
        *got = tp_frozen;
    else if (!strcmp(what, "voices"))
        *got = tp_voices;
    else if (!strcmp(what, "notes"))
        *got = (int)tp_notes;
    else if (!strcmp(what, "wear_s"))
        *got = (int)wear.seconds;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "drops"))
        *got = (int)tp_drops;
    else if (!strcmp(what, "passes"))
        *got = (int)tp_passes;
    else if (!strcmp(what, "loop_len"))
        *got = (int)tp_loop_len;
    else if (!strcmp(what, "tune"))
        *got = 400 + set.k[K_TUNE];
    else if (!strcmp(what, "damage")) {
        uint32_t i, sum = 0;
        for (i = 0; i < TP_NCELL; i++)
            sum += tp_damage[i];
        *got = (int)sum;
    } else if (!strcmp(what, "peak")) {
        *got = (int)(tp_peak * 1000.0f);
        tp_peak = 0.0f;
    } else
        return 0;
    return 1;
}
#endif
