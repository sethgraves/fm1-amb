/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Seth Graves; built on fm1-base by Charles Vestal */
/* The probe: the tape machine's two riskiest parts, run on the FM-1 to see what they cost before anything else
 * is built on them.
 *
 *  1. The Basinski loop: a mono loop in the pool, 8-bit mu-law, read, worn and written back every pass, with a
 *     damage map that only grows and stays where it started. How many seconds fit, at which rate.
 *  2. The spectral layer: a short-time FFT (Hann, overlap-add) per channel, with Lossy-style damage in the bins
 *     (coarse magnitudes, a cut-off, dropped frames). What share of the audio time it takes, by size and overlap.
 *
 * The source is a plain pad (four chords of detuned saws), cheap on purpose, so the CPU figure is mostly the two
 * parts under test. The complex FFT here is of the whole frame (no real-FFT packing): a ceiling, about twice what a
 * tuned one costs.
 *
 * Threads: pr_render() runs in the audio interrupt; the main loop sets pr_cfg and reads the pr_* figures. */
#pragma once
#include <stdint.h>

#define PR_SR 44100.0f
#ifndef PR_LOOP_BYTES
#define PR_LOOP_BYTES 262144u          /* 256 KiB: the device build reports what is left of the SRAM beside it */
#endif
#define PR_SEG 256u                    /* loop samples per damage cell */
#define PR_NSEG (PR_LOOP_BYTES / PR_SEG)
#define PR_FFT_MAX 1024u

typedef struct {
    uint8_t loop_on;                   /* the loop records, plays and wears */
    uint8_t loop_div;                  /* its rate: 44100 / div (2, 3 or 4: 22.05, 14.7, 11.025 kHz) */
    uint16_t fft_n;                    /* 0 off, or 256, 512, 1024 */
    uint8_t overlap;                   /* 2 or 4 */
    uint8_t stereo;                    /* the spectral layer on both channels (1) or one, copied (0) */
    uint8_t wear;                      /* 0..100: how fast the loop wears */
} pr_cfg_t;

extern volatile pr_cfg_t pr_cfg;

void pr_init(void);
void pr_render(int32_t *lr, uint32_t n, float gain);   /* n stereo frames, 24-bit; gain 0..1 */
void pr_panic(void);
void pr_loop_reset(void);              /* a fresh loop: no damage, silence */

/* for the screen and the tests */
uint32_t pr_head(void);               /* the head's place on the loop, 0..PR_LOOP_BYTES-1 */
float pr_loop_seconds(void);           /* the loop's length at the rate now */
extern volatile uint32_t pr_passes;    /* times round the loop */
extern volatile uint32_t pr_frames;    /* spectral frames done */
extern volatile uint32_t pr_render_us_max;   /* the longest render, microseconds (block of 256: 5805 us budget) */
extern uint8_t pr_damage[PR_NSEG];     /* each cell's damage, 0..255 (only grows) */
