# TAPE (working name)

A generative ambient machine for the M-VAVE FM-1 about memory and media wearing out, in the spirit of
Matthewdavid's *Trust the Guide and Glide*. There is no clean signal and no bypass: the music is made, put through a
room, recorded onto a tape and played back, and the tape is part of the instrument.

**The music.** Warm pads of detuned saws that glide from chord to chord, soft FM bells arpeggiating the chord, a
breathy flute that plays a short phrase now and then, and a stream with drops and birds. Four scenes, each a whole
arrangement on ALGORITHM: GLIDE (D lydian: the pads lead, long glides, few bells), SUNROOM (F major: brighter, quicker
chords, bells running high in sixteenths, the flute out front), MEMORY (A dorian: low, dark pads, the bells a soft
low pluck with a long ring, long chords), RIVER (Eb pentatonic: one held chord, the bells falling as scattered drops,
the stream forward). The flute and
the nature are synthesised, not samples.

**The tape.** Saturation and head bump on the way in. Wow, flutter, motor drag and head misalignment in the
transport. Dull highs, dropouts, hiss and channels that wander on the way out. Print-through: faint ghosts of the
bells and flute a tape-wrap before and after them (the machine plans its notes ahead, so a ghost can come first).

| Control | What it does |
| --- | --- |
| PLAY | The music on / off (it fades; the tape keeps running, so the hiss stays) |
| PRESET | The tape: CASSETTE, MICROCASSETTE, VHS, REEL TO REEL |
| ALGORITHM | The scene |
| K1 AGE | New to ruined: everything about the tape at once |
| K2 DENSITY | How many notes |
| K3 SPACE | The room |
| K4 DRIVE | How hard the music hits the tape |
| SELECT | Page 2: K1 PADS, K2 BELLS, K3 FLUTE, K4 NATURE (levels) |
| SELECT | Page 3, the loop: K1 BASINSKI, K2 FEED, K3 LENGTH, K4 DECAY |
| REC | CAPTURE: a fresh loop, recorded from the machine (lit while it records the first pass) |
| HOME (hold 2 s) | The tape's wear back to new |
| SAVE | Keeps the settings for the next power-on |
| MASTER | Volume |

**The loop (v2).** After William Basinski's *Disintegration Loops*: the machine records its own output onto a loop
(2 to 17 seconds, mono, 8-bit at 14.7 kHz) and plays it round, and every pass the oxide comes off a little more,
always where it was already coming off. Worn spots go dull, then quiet, then drop out, and spread to their
neighbours, until only hiss and fragments are left. BASINSKI turns from the live machine to the loop alone; FEED
puts new music onto it each pass (0: it only decays); DECAY sets how fast. The gauge under the tape is the loop
laid flat: amber where the oxide holds, dark where it has gone, the head a white line.

**Wear.** The tape wears as it plays, and remembers. Each hour of playing ages it a little more on top of AGE, up
to +40 after ten hours, most of it early. It is written to the flash every ten minutes of play and when the music
stops, and shown under the scene as WEAR.

| Tape | Character |
| --- | --- |
| Cassette | The middle: some wobble, some hiss, highs go with age |
| Microcassette | Thin (no lows), wobbly, mono, hissy, dull |
| VHS | The linear audio track: slow wobble, dull, near mono, tracking dropouts |
| Reel to reel | Wide, steady, quiet, warm head bump, the strongest print-through |

About a quarter of the FM-1's audio time, measured against the probe build's readings on the device.

## Build

```
python3 base/tools/build.py tape --host                     # the simulator
tape/build/host/tape_host tape/tests/basic.txt OUTDIR        # WAVs and screenshots in OUTDIR
python3 base/tools/build.py tape                             # tape/build/tape.fwsc
```

Or on GitHub: Actions, "TAPE firmware", Run workflow (or push to a `tape/*` branch).

Built on [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal and
[felucca-bsp](https://github.com/hugelton/felucca-bsp), from Felucca by Leo Kuroshita (Hügelton Instruments).
MPL-2.0.
