/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Seth Graves; built on fm1-base by Charles Vestal */
/* TAPE PROBE: the app. It runs dsp/probe.c and shows what it costs.
 *
 *   PLAY        the loop on / off (on: it records one pass of the pad, then only plays and wears)
 *   HOME        a fresh loop, and the peaks cleared
 *   PRESET      WEAR, 0..100: how fast the damage grows
 *   K1 RATE     the loop's rate: 22.05, 14.7 or 11.025 kHz (the screen shows how many seconds that holds)
 *   K2 FFT      the spectral layer: OFF, 256, 512 or 1024 points
 *   K3 OVERLAP  2x or 4x (4x: twice the frames)
 *   K4 CHANNELS MONO (one FFT, copied) or STEREO (two)
 *   MASTER      volume
 *
 * The screen: CPU now and its peak since the last change, the longest render against the 5.8 ms a block has,
 * xruns, the loop's length and passes, and the tape gauge (the damage map along the loop, the head on it).
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in scope. */
#include "fastmath.h"
#include "probe.h"

static struct {
    char msg[2][28];
    uint32_t msg_until;
    uint32_t btn;
    uint32_t peak, peak_from;            /* CPU's peak, counted from peak_from (ms) on: a change settles first */
    uint8_t redraw;
} ui;

static const uint8_t DIVS[3] = {2, 3, 4};
static const uint16_t FFTS[4] = {0, 256, 512, 1024};
static int8_t k_rate = 1, k_fft = 2, k_ov = 1, k_ch = 1;

void app_init(void) { pr_init(); }

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port))
        ;
    pr_render(lr, n, (float)master_q12 * (1.0f / 4096.0f));
}

void app_panic(void) { pr_panic(); }

/* ------------------------------------------------------------------ text --- */
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

static void tenths_text(float v, char *b)        /* "17.8" */
{
    int32_t t = (int32_t)(v * 10.0f + 0.5f);
    char d[4];
    int_text(t / 10, b);
    text_cat(b, ".");
    d[0] = (char)('0' + t % 10);
    d[1] = 0;
    text_cat(b, d);
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

/* ---------------------------------------------------------------- knobs --- */
static const char *const KNOB_NAME[4] = {"RATE", "FFT", "OVERLAP", "CHANNELS"};

static void knob_text(int k, char *t)
{
    static const char *const RATE[3] = {"22K", "14.7K", "11K"};
    t[0] = 0;
    if (k == 0)
        text_cat(t, RATE[k_rate]);
    else if (k == 1) {
        if (k_fft)
            int_text(FFTS[k_fft], t);
        else
            text_cat(t, "OFF");
    } else if (k == 2)
        text_cat(t, k_ov ? "4X" : "2X");
    else
        text_cat(t, k_ch ? "STEREO" : "MONO");
}

static void changed(void)                /* the engine's settings, and the peak counted again a second later */
{
    pr_cfg.loop_div = DIVS[k_rate];
    pr_cfg.fft_n = FFTS[k_fft];
    pr_cfg.overlap = k_ov ? 4u : 2u;
    pr_cfg.stereo = (uint8_t)k_ch;
    ui.peak = 0;
    ui.peak_from = plat_ms() + 1000u;
    pr_render_us_max = 0;
}

static void knob_turn(int k, int32_t e)
{
    char t[16];
    int8_t *v = k == 0 ? &k_rate : k == 1 ? &k_fft : k == 2 ? &k_ov : &k_ch;
    int32_t hi = k == 0 ? 2 : k == 1 ? 3 : 1;
    *v = (int8_t)clamp(*v + e, 0, hi);
    if (k == 0)
        pr_loop_reset();                 /* a new rate is a new tape */
    changed();
    knob_text(k, t);
    app_say(KNOB_NAME[k], t);
}

static void input(void)
{
    uint32_t btn = plat_buttons(), k;
    int32_t e;
    char t[12];
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_PLAY) {
            pr_cfg.loop_on = (uint8_t)!pr_cfg.loop_on;
            changed();
            app_say("LOOP", pr_cfg.loop_on ? "ON" : "OFF");
        } else if (k == B_HOME) {
            pr_loop_reset();
            changed();
            app_say("FRESH LOOP", "");
        }
    }
    if ((e = plat_enc(EN_PRESET)) != 0) {
        pr_cfg.wear = (uint8_t)clamp((int32_t)pr_cfg.wear + e * 5, 0, 100);
        int_text(pr_cfg.wear, t);
        app_say("WEAR", t);
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
    ui.btn = btn;
    plat_glow(1);
    plat_leds(pr_cfg.loop_on ? 1u << B_PLAY : 0u, 0);
}

void app_input_only(void) { input(); }

static void speak(void)
{
    char t[80], n[12];
    if (ann_on() && ann_where()) {
        t[0] = 0;
        text_cat(t, "TAPE PROBE. CPU ");
        int_text((int32_t)plat_cpu_pct(), n);
        text_cat(t, n);
        text_cat(t, ". PEAK ");
        int_text((int32_t)ui.peak, n);
        text_cat(t, n);
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

/* --------------------------------------------------------------- screen --- */
/* worn tape: a brown-amber palette, the platform's five steps (gfx.c) */
static const uint16_t TPAL[5] = {RGB(52, 40, 30), RGB(110, 88, 66), RGB(190, 170, 146), RGB(220, 170, 110),
                                 RGB(255, 196, 110)};

static void row(int32_t y, const char *label, const char *val, uint16_t c)
{
    cv_text(10, y, &FONT_XS, label, C_GRAY);
    cv_text(96, y - 2, &FONT_B, val, c);
}

static void draw(void)
{
    char t[32], n[12];
    int32_t i, x;
    uint32_t now = plat_ms(), us = pr_render_us_max, pct = plat_cpu_pct();
    float secs = pr_loop_seconds();

    cv_begin(240, 30, C_BLACK);
    if (now < ui.msg_until) {
        x = cv_text(8, 6, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], C_HI);
    } else {
        cv_text(8, 6, &FONT_B, APP_NAME, C_WHITE);
        cv_text(232 - text_w(&FONT_XS, pr_cfg.loop_on ? "LOOP ON" : "LOOP OFF"), 9, &FONT_XS,
                pr_cfg.loop_on ? "LOOP ON" : "LOOP OFF", pr_cfg.loop_on ? C_HI : C_DIM);
    }
    cv_blit(0, 0);

    cv_begin(240, 158, C_BLACK);
    int_text((int32_t)pct, t);
    text_cat(t, "%");
    row(4, "CPU", t, C_HI);
    int_text((int32_t)ui.peak, t);
    text_cat(t, "%");
    row(24, "PEAK", t, ui.peak > 80u ? C_WHITE : C_HI);
    int_text((int32_t)us, t);
    text_cat(t, " US / 5805");
    row(44, "LONGEST BLOCK", t, us > 5000u ? C_WHITE : C_AMB);
    int_text((int32_t)plat_xruns(), t);
    row(64, "XRUNS", t, C_AMB);
    tenths_text(secs, t);
    text_cat(t, " S   ");
    int_text((int32_t)pr_passes, n);
    text_cat(t, n);
    text_cat(t, " PASSES");
    row(84, "LOOP", t, C_AMB);
    int_text(pr_cfg.wear, t);
    row(104, "WEAR", t, C_AMB);
    /* the tape gauge: the damage along the loop, 1024 cells in 220 columns, the worst of each; the head a line */
    {
        int32_t gx = 10, gy = 128, gw = 220, gh = 18, head;
        cv_rect(gx - 1, gy - 1, gw + 2, gh + 2, C_LINE);   /* the tape: an outline, dark inside */
        cv_rect(gx, gy, gw, gh, C_BLACK);
        for (i = 0; i < gw; i++) {
            uint32_t a = (uint32_t)i * PR_NSEG / (uint32_t)gw, b = (uint32_t)(i + 1) * PR_NSEG / (uint32_t)gw, d = 0;
            for (; a < b; a++)
                if (pr_damage[a] > d)
                    d = pr_damage[a];
            if (d) {
                int32_t h = 3 + (int32_t)(d * (uint32_t)(gh - 4) / 255u);   /* any damage at all shows */
                cv_rect(gx + i, gy + gh - 1 - h, 1, h, d > 150u ? C_WHITE : C_AMB);
            }
        }
        head = gx + (int32_t)(pr_head() * (uint32_t)gw / PR_LOOP_BYTES);
        cv_rect(head, gy - 3, 1, gh + 6, C_HI);
    }
    cv_blit(0, 32);

    cv_begin(240, 50, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    for (i = 0; i < 4; i++) {
        x = 4 + i * 59;
        knob_text(i, t);
        cv_text(x, 8, &FONT_XS, KNOB_NAME[i], C_GRAY);
        cv_text(x, 24, &FONT_B, t, C_HI);
    }
    cv_blit(0, 190);
}

void app_ui_init(void)
{
    int k;
    for (k = 0; k < 5; k++)
        pal[k] = TPAL[k];
    lcd_fill(0, 0, 240, 240, C_BLACK);
    changed();
    ui.redraw = 1;
}

void app_frame(void)
{
    static uint32_t last_draw;
    uint32_t now = plat_ms(), pct = plat_cpu_pct();
    input();
    speak();
    if ((int32_t)(now - ui.peak_from) >= 0 && pct > ui.peak)
        ui.peak = pct;
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (now - last_draw >= 250u)
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
    uint32_t i, s = 0;
    if (!strcmp(what, "loop_on"))
        *got = pr_cfg.loop_on;
    else if (!strcmp(what, "fft"))
        *got = pr_cfg.fft_n;
    else if (!strcmp(what, "passes"))
        *got = (int)pr_passes;
    else if (!strcmp(what, "frames"))
        *got = (int)pr_frames;
    else if (!strcmp(what, "loop_ms"))
        *got = (int)(pr_loop_seconds() * 1000.0f);
    else if (!strcmp(what, "damage")) {      /* the damage map's sum */
        for (i = 0; i < PR_NSEG; i++)
            s += pr_damage[i];
        *got = (int)s;
    } else
        return 0;
    return 1;
}
#endif
