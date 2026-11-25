# DREAM

Firmware for the M-VAVE FM-1: pure sine tones, tuned as whole-number harmonics of one unit frequency, held for
as long as the FM-1 is on. After La Monte Young's Dream House and Composition 1960 #7. No filters, no effects,
no saturation.

Each tone's phase is a 32-bit integer stepping by exactly its harmonic number times the unit's step, so the
ratios are exact and the tones never drift against each other, even while the pitch glides.

| Control | What it does |
| --- | --- |
| PLAY | The drone on / off (swells in, fades out) |
| PRESET | The piece |
| K1 PITCH | The whole series up or down, semitones |
| K2 FINE | Cents |
| K3 SPREAD | The tones across the stereo field |
| K4 TILT | Level slope up the series, dB per octave |
| MASTER | Volume |
| SAVE | Keeps the settings for the next power-on |

## Build

From the repository root (fm1-base in `base/`, with felucca-bsp inside it):

```
python3 base/tools/build.py dream --host                       # the simulator
dream/build/host/dream_host dream/tests/basic.txt OUTDIR       # run it: WAVs and screenshots in OUTDIR
python3 base/tools/build.py dream                              # dream/build/dream.fwsc (JieLi toolchain, AC79 SDK)
```

Or on GitHub: Actions, "DREAM firmware", Run workflow. The package is in the run's `packages` artifact.

Built on [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal and
[felucca-bsp](https://github.com/hugelton/felucca-bsp), from Felucca by Leo Kuroshita (Hügelton Instruments).
MPL-2.0.
