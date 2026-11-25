# TAPE (working name)

A generative ambient machine for the M-VAVE FM-1 about memory and media wearing out, in the spirit of
Matthewdavid's *Trust the Guide and Glide*. Tape is part of the instrument, not an effect on it.

This folder is the **probe build**: it measures the two riskiest parts on the FM-1 before the instrument is built
on them.

| Control | What it does |
| --- | --- |
| PLAY | The loop on / off (records one pass of a test pad, then only plays and wears it) |
| HOME | A fresh loop; the peaks cleared |
| PRESET | WEAR, how fast the damage grows |
| K1 RATE | The loop's rate: 22K, 14.7K, 11K (the screen shows how many seconds fit) |
| K2 FFT | The spectral layer: OFF, 256, 512, 1024 |
| K3 OVERLAP | 2X or 4X |
| K4 CHANNELS | MONO or STEREO |

The screen: CPU now and its peak (counted from a second after the last change), the longest audio block against
the 5805 µs it has, xruns (dropouts: should stay 0), the loop's length and passes, and the tape gauge.

## Build

```
python3 base/tools/build.py tape --host                     # the simulator
tape/build/host/tape_host tape/tests/probe.txt OUTDIR        # WAVs and screenshots in OUTDIR
python3 base/tools/build.py tape                             # tape/build/tape.fwsc
```

Or on GitHub: Actions, "TAPE firmware", Run workflow (or push to a `tape/*` branch).

Built on [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal and
[felucca-bsp](https://github.com/hugelton/felucca-bsp), from Felucca by Leo Kuroshita (Hügelton Instruments).
MPL-2.0.
