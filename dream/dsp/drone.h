/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Seth Graves; built on fm1-base by Charles Vestal */
/* The drone: sine tones on one harmonic series, held.
 *
 * Every tone is a whole-number harmonic h of one unit frequency (the series' "1", which need not sound).
 * Phases are 32-bit integers, and tone h steps by exactly h times the unit's step, so 3:2 is 3:2 for as
 * long as the FM-1 is on: the tones never drift against each other, whatever the unit does (a glide moves
 * them all together). No filters, no effects, no saturation: a linear sum with headroom.
 *
 * Threads: dr_render() runs in the audio interrupt. The main loop fills a dr_state_t and hands it over with
 * dr_commit(); the render takes it at the start of its next block. Float only, no libm (fastmath.h). */
#pragma once
#include <stdint.h>

#define DR_SR 44100.0f
#define DR_MAXT 32                     /* tones, at most (the chip's real limit is measured, not assumed) */

typedef struct {
    float unit_hz;                     /* the series' unit: tone h sounds at h * unit_hz */
    uint8_t n;                         /* tones */
    uint8_t playing;                   /* 0: every tone fades out (and in again on 1) */
    uint16_t harm[DR_MAXT];            /* each tone's harmonic number, 1.. */
    float amp[DR_MAXT];                /* its level, linear; their sum is kept under 1 by the caller */
    float pan[DR_MAXT];                /* -1 left .. +1 right (equal power) */
    float glide_s;                     /* seconds for a change of unit to arrive */
    float fade_s;                      /* seconds for a tone to swell in or fade out */
} dr_state_t;

void dr_init(void);
int dr_commit(const dr_state_t *s);    /* 1 taken; 0 the last is not yet taken (try again next frame) */
void dr_render(int32_t *lr, uint32_t n, float gain);   /* n stereo frames, 24-bit; gain 0..1 */
void dr_panic(void);

/* for the screen and the tests */
extern volatile float dr_level[DR_MAXT];   /* each tone's level now */
extern volatile uint32_t dr_unit_inc;      /* the unit's phase step now (2^32 a turn) */
extern volatile float dr_peak;             /* the output's peak since last read, 0..1 of full scale */
