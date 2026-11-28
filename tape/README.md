# TAPE (working name)

A Mellotron meets an ambient synth, for the M-VAVE FM-1, with the tape built in. You play it from the keys or
over MIDI; the tape is part of the instrument, with no clean signal and no bypass.

**The voices.** Three Mellotron-style patches, synthesised in that spirit: M-FLUTE (breathy, a chiff, a late
vibrato), M-STRINGS (a bowed section of three saws), M-CHOIR (an "aah" through a vowel's formants). Each key is
its own strip of tape: it comes up to speed a little flat, wobbles on its own, and with TAPE END on runs out after
eight seconds, as a Mellotron's does. Two synths on the same tape, PAD (warm detuned saws) and BELL (soft FM), and
VIBES: a vibraphone's bar (its first, fourth and tenth partials, the mallet in the upper two), its motor turning.
Eight notes at once.

**The tape.** A tape echo (its own loop of tape: the repeats darker each time round, the pitch bending when TIME
moves), a room, then the deck: saturation and head bump on the way in; wow, flutter, motor drag and head
misalignment in the transport; dull highs, dropouts, hiss and channels that wander on the way out. Four tape types
and AGE over all of it.

**FREEZE and BASINSKI.** The loop records what you play, the last LENGTH seconds, all the time, unheard. FREEZE
holds it: what you just played goes round and round, and every pass the oxide comes off a little more, always where
it was already coming off, until only hiss and fragments are left (after William Basinski's *Disintegration
Loops*). It plays back into the echo, the room and the tape along with what you play now, so every knob still works
on it; BASINSKI is the balance between the two. FREEZE again lets it go. The gauge under the tape is the loop laid
flat: amber where the oxide holds, dark where it has gone.

| Control | What it does |
| --- | --- |
| Keys | Play (F3 to G5); MIDI notes in from USB and the TRS jack too |
| PRESET | The patch: M-FLUTE, M-STRINGS, M-CHOIR, PAD, BELL, VIBES |
| ALGORITHM | The tape: CASSETTE, MICROCASSETTE, VHS, REEL TO REEL |
| PLAY | HOLD: the chord held down when you press it stays (or, with nothing held, the next chord you play), and the keys play on over it; again: it goes |
| REC | FREEZE |
| OCT- / OCT+ | The keys down or up an octave (-2 to +2) |
| SELECT | The page |
| Page TAPE | AGE, WOW, FLUTTER, DRIVE (WOW and FLUTTER: the tape's own at 50) |
| Page ECHO | ECHO (level), TIME (50 to 1100 ms), FEEDBACK, SPACE (the room) |
| Page LOOP | BASINSKI, LENGTH (2 to 12 s), DECAY |
| Page SETUP | ATTACK, RELEASE (the patch's own at 50), TAPE END (OFF or 8 S), TUNE (A = 415 to 446 Hz, 432 by default) |
| HOME (hold 2 s) | The tape's wear back to new |
| SAVE | Keeps the settings for the next power-on |

**Wear.** The tape wears as it plays, and remembers: each hour of sound ages it a little more on top of AGE, up to
+40 after ten hours, most of it early. It is written to the flash every ten minutes of play.

| Tape | Character |
| --- | --- |
| Cassette | The middle: some wobble, some hiss, highs go with age |
| Microcassette | Thin (no lows), wobbly, mono, hissy, dull |
| VHS | The linear audio track: slow wobble, dull, near mono, dropouts |
| Reel to reel | Wide, steady, quiet, warm head bump |

At most about two thirds of the FM-1's audio time (eight choir notes with everything up), measured against the
device's own readings of an earlier build.

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
