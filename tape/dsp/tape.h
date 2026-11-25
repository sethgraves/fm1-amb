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
} tp_cfg_t;

extern volatile tp_cfg_t tp_cfg;
extern const char *const TP_TYPE_NAME[TP_NTYPES];
extern const char *const TP_SCENE_NAME[TP_NSCENES];

void tp_init(void);
void tp_render(int32_t *lr, uint32_t n, float gain);   /* n stereo frames, 24-bit; gain 0..1 */
void tp_panic(void);

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
