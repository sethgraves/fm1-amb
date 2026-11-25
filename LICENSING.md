# DREAM licensing

DREAM is free software under the **Mozilla Public License 2.0** (`MPL-2.0`, full text in `LICENSE`), file by file.
If you change one of its files and share the result, share that file's source under the MPL.

| What | Origin | Licence |
| --- | --- | --- |
| `base/`: the platform (boot, USB, storage, the update path, screen, panel, the screen reader, the host simulator, the browser emulator, the build and install tools) | [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal and [felucca-bsp](https://github.com/hugelton/felucca-bsp), from Felucca by Leo Kuroshita (Hügelton Instruments) | MPL-2.0 |
| `app/`: the drone engine, the pieces, the screen | Seth Graves, from fm1-base's app template | MPL-2.0 |

This repository began as a fork of [AMB-1](https://github.com/charlesvestal/fm1-amb); none of AMB-1's own code
remains.

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Fonts: Barlow Semi Condensed (The Barlow Project Authors), Terminus (Dimitar Toshkov Zhekov) | SIL OFL 1.1 | `base/assets/fonts/` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin`, read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## Names

DREAM is an independent tribute to La Monte Young and Marian Zazeela, not affiliated with or endorsed by them or the
MELA Foundation. Its pieces are named for the works whose tunings they follow. "M-VAVE" and "FM-1" are trademarks
of their owners, used only to say what hardware DREAM runs on.
