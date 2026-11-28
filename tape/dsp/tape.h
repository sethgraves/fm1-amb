/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Seth Graves; built on fm1-base by Charles Vestal */
/* TAPE: a Mellotron meets an ambient synth, on tape. You play it; the tape is part of the instrument, with no clean
 * signal and no bypass.
 *
 * The voices: three Mellotron-style patches (flute, strings, choir), each key its own strip of tape that comes up to
 * speed, wobbles on its own and can run out after eight seconds; and two synths (a pad, a bell). Eight at once.
 * Then a tape echo, a room, and the deck: saturation and head bump on the way in; wow, flutter, motor drag and head
 * misalignment as a modulated delay; dull highs, dropouts, hiss and channel wander on the way out. Four tape types,
 * and AGE over all of it.
 *
 * The loop (after William Basinski's Disintegration Loops) records the voices, the last LENGTH seconds, all the
 * time. FREEZE holds it: what was just played goes round, wearing a little more every pass where it already wears,
 * and plays back into the echo, the room and the tape with what is played now (BASINSKI: how much of each), so
 * every knob still works on it.
 *
 * Threads: tp_render() runs in the audio interrupt and reads tp_cfg once a block; the main loop writes tp_cfg and
 * sends notes with tp_note_on/off (a queue). Float only, no libm (fastmath.h), no 64-bit multiplies (felucca-bsp
 * docs/CPU.md). */
#pragma once
#include <stdint.h>

#define TP_SR 44100.0f

enum { TP_CASSETTE, TP_MICRO, TP_VHS, TP_REEL, TP_NTYPES };
enum { TP_FLUTE, TP_STRINGS, TP_CHOIR, TP_PAD, TP_BELL, TP_NPATCH };

typedef struct {
    uint8_t type;                      /* TP_CASSETTE .. TP_REEL */
    uint8_t patch;                     /* for the next notes: TP_FLUTE .. TP_BELL */
    uint8_t freeze;                    /* 1: the loop holds what was just played and plays it, wearing */
    uint8_t tape_end;                  /* 1: the Mellotron patches run out after eight seconds */
    float age;                         /* 0 new .. 1 ruined: AGE and the tape's own wear together */
    float wow, flutter;                /* 0..1: the tape's own at 0.5 */
    float drive;                       /* 0..1: how hard it hits the tape */
    float space;                       /* 0..1: the room */
    float echo, echo_time, echo_fb;    /* the tape echo: level 0..1, time s (0.05 .. 1.1), feedback 0..1 */
    float basinski;                    /* 0..1: how much of the frozen loop is heard */
    float decay;                       /* 0..1: how fast the frozen loop disintegrates */
    float length;                      /* the loop, seconds (TP_LOOP_MIN .. tp_loop_max()) */
    float attack, release;             /* 0..1: the patch's own at 0.5 */
    float a4;                          /* the tuning: A above middle C, Hz (432 by default) */
} tp_cfg_t;

extern volatile tp_cfg_t tp_cfg;
extern const char *const TP_TYPE_NAME[TP_NTYPES];
extern const char *const TP_PATCH_NAME[TP_NPATCH];

void tp_init(void);
void tp_render(int32_t *lr, uint32_t n, float gain);   /* n stereo frames, 24-bit; gain 0..1 */
void tp_panic(void);
void tp_note_on(int note, int vel, int held);   /* MIDI note, velocity 1..127; held: HOLD's (never stolen while
                                                  * there is another voice); from the main loop */
void tp_note_hold(int note);           /* a note already sounding becomes HOLD's */
void tp_note_off(int note);
void tp_all_off(void);

/* the loop: mono, 8-bit mu-law at 14.7 kHz, in the pool; its damage in cells of TP_CELL samples */
#define TP_LOOP_BYTES 176640u          /* 12 s at 14.7 kHz, in whole cells */
#define TP_LOOP_DIV 3u                 /* 44.1 kHz / 3 */
#define TP_CELL 256u
#define TP_NCELL (TP_LOOP_BYTES / TP_CELL)
#define TP_LOOP_MIN 2.0f
float tp_loop_max(void);               /* seconds: the whole loop */
extern uint8_t tp_damage[TP_NCELL];    /* each cell's damage, 0..255: it only grows, while frozen */
extern volatile uint32_t tp_loop_pos;  /* the head, bytes */
extern volatile uint32_t tp_loop_len;  /* the loop in use, bytes */
extern volatile uint32_t tp_passes;    /* times round since FREEZE */
extern volatile uint8_t tp_frozen;     /* the render's FREEZE */

/* for the screen and the tests */
extern volatile float tp_speed;        /* the tape's speed now, 1 = nominal (wow, flutter, drag) */
extern volatile float tp_reel_turns;   /* how far the reels have turned */
extern volatile float tp_drop;         /* 0..1: a dropout now */
extern volatile float tp_peak;         /* the output's peak since last read, 0..1 */
extern volatile uint8_t tp_voices;     /* voices sounding */
extern volatile uint32_t tp_notes;     /* notes started */
extern volatile uint32_t tp_drops;     /* dropouts */
extern volatile uint32_t tp_drags;     /* motor drags */
