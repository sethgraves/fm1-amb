/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Seth Graves; built on fm1-base by Charles Vestal */
/* TAPE v1: a generative ambient machine that only exists on tape. There is no clean signal and no bypass: the
 * music is made, put through a room, and recorded onto a tape that is played back, and the tape is the instrument.
 *
 * The music (gen): warm pads of detuned saws that glide from chord to chord, soft FM bells arpeggiating the chord,
 * a breathy flute that plays a short phrase now and then, and a stream with birds. Four scenes, each a key and a
 * slow progression.
 *
 * The tape (deck): saturation and head bump on the way in; wow, flutter, motor drag and head misalignment as a
 * modulated delay; dull highs, dropouts, hiss and channel wander on the way out; print-through as faint ghosts of
 * the bells and flute a tape-wrap before and after them (the generator plans its notes ahead, so a ghost can come
 * first). Four tape types and one AGE macro set all of it.
 *
 * Threads: tp_render() runs in the audio interrupt and reads tp_cfg once a block; the main loop writes tp_cfg.
 * Float only, no libm (fastmath.h), no 64-bit multiplies (felucca-bsp docs/CPU.md). */
#pragma once
#include <stdint.h>

#define TP_SR 44100.0f

enum { TP_CASSETTE, TP_MICRO, TP_VHS, TP_REEL, TP_NTYPES };
#define TP_NSCENES 4

typedef struct {
    uint8_t playing;                   /* 0: the music fades out (the tape keeps running: hiss stays) */
    uint8_t type;                      /* TP_CASSETTE .. TP_REEL */
    uint8_t scene;                     /* 0 .. TP_NSCENES-1 */
    float age;                         /* 0 new .. 1 ruined: AGE and the tape's own wear together */
    float density;                     /* 0..1: how many notes */
    float space;                       /* 0..1: the room */
    float drive;                       /* 0..1: how hard the music hits the tape */
    float pads, bells, flute, nature;  /* 0..1 each: levels */
    /* v2: the loop. The machine records its own output onto a loop of tape that wears a little more every pass,
     * always in the same places, until only hiss and fragments are left (after William Basinski's Disintegration
     * Loops). */
    float basinski;                    /* 0..1: what you hear, from the live machine to the loop alone */
    float feed;                        /* 0..1: how much new music goes onto the loop each pass (0: it only decays) */
    float decay;                       /* 0..1: how fast it disintegrates */
    float length;                      /* the loop, seconds (TP_LOOP_MIN .. tp_loop_max()) */
    uint8_t capture;                   /* a count: each change starts a fresh loop */
    float a4;                          /* the tuning: A above middle C, Hz (432 by default) */
} tp_cfg_t;

extern volatile tp_cfg_t tp_cfg;
extern const char *const TP_TYPE_NAME[TP_NTYPES];
extern const char *const TP_SCENE_NAME[TP_NSCENES];

void tp_init(void);
void tp_render(int32_t *lr, uint32_t n, float gain);   /* n stereo frames, 24-bit; gain 0..1 */
void tp_panic(void);

/* the loop: mono, 8-bit mu-law at 14.7 kHz, in the pool; its damage in cells of TP_CELL samples */
#define TP_LOOP_BYTES 262144u
#define TP_LOOP_DIV 3u                 /* 44.1 kHz / 3 */
#define TP_CELL 256u
#define TP_NCELL (TP_LOOP_BYTES / TP_CELL)
#define TP_LOOP_MIN 2.0f
float tp_loop_max(void);               /* seconds: the whole loop */
extern uint8_t tp_damage[TP_NCELL];    /* each cell's damage, 0..255: it only grows, until a capture */
extern volatile uint32_t tp_loop_pos;  /* the head, bytes */
extern volatile uint32_t tp_loop_len;  /* the loop in use, bytes */
extern volatile uint32_t tp_passes;    /* times round since the capture */
extern volatile uint8_t tp_capturing;  /* 1: recording the first pass */

/* for the screen and the tests */
extern volatile float tp_speed;        /* the tape's speed now, 1 = nominal (wow, flutter, drag) */
extern volatile float tp_reel_turns;   /* how far the reels have turned */
extern volatile float tp_drop;         /* 0..1: a dropout now */
extern volatile float tp_peak;         /* the output's peak since last read, 0..1 */
extern volatile uint8_t tp_chord;      /* the scene's chord now */
extern volatile uint32_t tp_notes;     /* bells and flute notes played (not ghosts) */
extern volatile uint32_t tp_ghosts;    /* print-through ghosts played */
extern volatile uint32_t tp_drops;     /* dropouts */
extern volatile uint32_t tp_drags;     /* motor drags */
