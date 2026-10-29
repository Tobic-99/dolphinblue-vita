# Dolphin Blue for the PS Vita

A static recompilation of Dolphin Blue (Sammy, Atomiswave) to native ARM code. The game's SH-4 code is
translated to C++ and linked with a native runtime that replaces the hardware: the PowerVR2 tile accelerator
(rendered with GXM), the AICA sound chip (its ARM7 sound driver and DSP programs are translated as well), the
GD-ROM and the BIOS calls. There is no emulator, interpreter or JIT on the hot paths.

The repository contains no game code or data. You need your own GDI image of the Dreamcast conversion of
Dolphin Blue (`.gdi` + `track01.iso`, `track02.raw`, `track03.iso`, `track04.iso`). The translation is made for
this executable (`1ST_READ.BIN`, extracted in step 1 of [Building](#1-extract-the-disc)):

```
e2c99f4bb920d515437079a2ff13ea59e6b06c61eefdac82d583749663258d81  1ST_READ.BIN   (sha256)
47327cf4db9ace57ee2bba4948c5ccd343b44f95807fdfc9ae946e03dbb6db0a  track03.iso
003db9663a62d6d9061892759cc3b8e5bd7083c7fd852750f4a2b4aad1a92a6b  track04.iso
```

The VPK holds the translated code and nothing of the disc: textures, models, sound and the executable's data
are read from your image.

How the port runs on a 444 MHz Cortex-A9: [docs/optimizations.md](docs/optimizations.md).

## Installing on the PS Vita

Requirements: a PS Vita with HENkaku/Ensō and [VitaShell](https://github.com/TheOfficialFloW/VitaShell/releases).
No plugins are needed. The shaders are compiled in advance and packed in the VPK, so `libshacccg.suprx`
is not needed either.

1. Copy `dolphinblue.vpk` from the [release](../../releases) to the Vita and install it with VitaShell.
2. On your PC, extract `IP.BIN` and `1ST_READ.BIN` from your disc image (see [Building](#1-extract-the-disc),
   step 1; only Python 3 is needed for it).
3. Create the folder `ux0:data/dolphinblue/` on the Vita and copy into it:
   ```
   ux0:data/dolphinblue/IP.BIN
   ux0:data/dolphinblue/1ST_READ.BIN
   ux0:data/dolphinblue/disc/Dolphin Blue.gdi     (any name ending in .gdi)
   ux0:data/dolphinblue/disc/track01.iso
   ux0:data/dolphinblue/disc/track02.raw
   ux0:data/dolphinblue/disc/track03.iso
   ux0:data/dolphinblue/disc/track04.iso
   ```
4. Start Dolphin Blue from the LiveArea.

The release VPK has no LiveArea images (the system shows its default icon). The game writes its saves to
`ux0:data/dolphinblue/`: `sram.bin` (scores, settings), `hiscores.bin`, and `state.bin` (a save state taken at
the start of each mission; the game resumes from it at the next launch).

The picture fills the screen (960x544); an empty file `ux0:data/dolphinblue/aspect43` shows it in 4:3 instead.

Controls: [docs/vita-controls.md](docs/vita-controls.md).

## Building

Tested on x86-64 Linux. Every target needs Python 3, CMake 3.16+, Ninja and Git.

- PS Vita: [VitaSDK](https://vitasdk.org) (`VITASDK`) with
  [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) and
  [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) (VitaSDK packages, `vdpm`).
  The release was built with arm-vita-eabi-gcc 15.2.0, vita-toolchain `c527abce`, vita-headers `ebc8f4f7`,
  newlib `64aa7aa3`, pthread-embedded `11d2e572`; with these the build below gives the release `eboot.bin`
  (sha256 `aaa638eb2e5d55e2d7aff76a86466b5f6a764bdafcc77f69140ed07d3ee97a84`).
- Linux: clang, zlib, EGL.

### 1. Extract the disc

```sh
scripts/extract.sh /path/to/disc        # the folder with the .gdi and its tracks
```

This writes `extract/1ST_READ.BIN`, `extract/IP.BIN` and the G1ROM file system `extract/g1rom/`.

### 2. Translate

```sh
scripts/translate.sh
```

This translates the SH-4 code into `recomp/gen/` (function discovery, `tools/discover.py`; C++ generation,
`tools/codegen.py`), the sound driver `manatee.drv` into `recomp/host/arm7_blocks.inc` (`tools/arm7rec.py`,
block leaders from `recomp/data/arm_leaders.txt`) and the DSP programs of `FXPROGRAM.fpb` into
`recomp/host/aica_dsp_progs.inc` (`tools/dspextract.py`, `tools/dsprec.py`). `extract/`, `recomp/gen/` and the
two `.inc` files are not part of the repository.

### 3. Build

PS Vita (`build/vita/dolphinblue.vpk`), -O3 with the checked-in PGO profile `pgo/`:

```sh
scripts/build-vita.sh                   # DB_JOBS=N limits the number of parallel compiles
```

LiveArea images are optional: `recomp/vita/sce_sys/icon0.png` (128x128),
`recomp/vita/sce_sys/livearea/contents/bg.png` (840x500), `startup.png` (280x158) and `template.xml` are packed
when present; the PNGs must be 8-bit indexed.

Linux (`build/desktop/db_run`, `build/desktop/sh4_tests`):

```sh
scripts/build-desktop.sh
DB_BASE=$PWD build/desktop/db_run --frames 3000 --png shots --png-every 60
build/desktop/sh4_tests
```

## Credits

- [VitaSDK](https://vitasdk.org)
- [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) — Rinnegatamante (LGPL-3.0)
- [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) — bythos14 (LGPL-2.1)
- [libperf](https://github.com/bythos14/libperf) — bythos14 (MIT, `recomp/vita/third_party/libperf/LICENSE`)
- [Flycast](https://github.com/flyinghead/flycast) (GPL-2.0): hardware behaviour reference and comparison
  target (`recomp/harness/`, through its libretro core); no code taken
- [Grarak, mh3-wii](https://github.com/Grarak/mh3-wii): method and optimisation order
- [Ghidra](https://github.com/NationalSecurityAgency/ghidra), [Capstone](https://github.com/capstone-engine/capstone):
  decoder cross-checks (`recomp/tools/xcheck*.py`)
