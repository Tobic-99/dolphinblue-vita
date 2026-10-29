# How the port runs on the PS Vita

The game renders once per 60 Hz vblank. The release shows 30 fps (each frame on screen for two refreshes,
triple buffering), which needs at most 16.7 ms of game code per vblank and 33 ms of rendering per shown frame.
Every change below that touches guest-visible state was checked bit-exact on the desktop: the 42 SH-4
semantics tests, and per-render hashes of the TA stream and guest RAM (`DB_TAHASH=1`) against the unmodified
translation.

## CPU: the translated SH-4 code

- **Function discovery** (`recomp/tools/discover.py`): entry point, call targets with constant propagation,
  tail calls, VBR handlers, code pointers in literal pools and data (validated: clean decode, closed control
  flow, no overlap with literal pools), switch tables with bounds, plus `PROLOGUE_ENTRIES` for data-referenced
  functions. 5,543 functions; every indirect jump goes through the dispatch table.
- **Register caching** (`codegen.py: cache_regs`): guest registers live in C++ locals and are written back only
  around calls, helpers, returns and the slow path of the interrupt poll. On the Vita this made the hottest
  function 22% faster per vertex.
- **Inlined FPU ops**: `fipr`/`ftrv` inlined with the helpers' exact arithmetic; `fmac` as `fmaf`.
  FPSCR modes (PR/SZ/FR) are tracked statically per function; 97 instructions keep a run-time mode test.
- **Fixed-address guest RAM** (`sh4::ram[]`), store-queue writes inlined in `wr32`, `pref` early-out.
- **Native ports** (`recomp/host/native_vtx.cpp`) override the weak generated functions of the vertex family:
  `8c31b2a0` (strip writer: TA units written straight into the TA stream), `8c31ba84` and `8c31b710`
  (3D transforms: results written straight to their destination tables), with prefetches. Each keeps the
  translation's guest-visible behaviour exactly (memory, store queues, registers at every poll point).
- **Idle loops**: the frame-wait loop in `8c320140` and the TMU0 busy-wait at `8c2f2126` run whole scheduler
  slices instead of spinning.
- **Disc reads**: one file read fetches up to 32 sectors (read-ahead), on a separate thread (`gdreader`).
- **-O3 and PGO**: profile collected on the Vita (`-DDB_PGO=gen`, autotest `pgodump N`; vitasdk's libgcov has no
  file output, `recomp/vita/pgo_stubs.cpp` writes the `.gcda` files), used with `-DDB_PGO=use`.

## Rendering

- The guest hands each STARTRENDER's TA stream to the render thread without a copy and continues; it waits
  only if the previous frame is still being built.
- **Fragment shaders specialised per state** (`#define` variants: texturing, offset colour, alpha test, shading,
  fog): no uniform branches, `discard` only in alpha-test variants. The vertex shader and all 276 fragment
  variants are compiled in advance with libshacccg (`ux0:data/dolphinblue/shaderbank` switch: compiles every
  variant at start) and packed in the VPK as `app0:shaders/<FNV-1a of the source>.gxp`. A shader not found
  there or in `ux0:data/dolphinblue/shaders/` is compiled at run time with vitaShaRK and cached, if
  `ur0:data/libshacccg.suprx` is installed.
- **Textures**: exact invalidation span per format plus a hash of the source bytes. Textures with 16-bit texels
  (ARGB1555/RGB565/ARGB4444; direct, VQ or with a 16-bit palette) are detwiddled straight into the matching
  16-bit GXM format, with no 32-bit round trip.
- `sceGxmFinish` for a frame is called only before the next frame reuses its memory, so CPU and GPU overlap.
- Guard-band clipping: triangles more than 2048 px off screen are clipped, perspective-correct.

## Sound

- **ARM7 sound driver** (`manatee.drv`): translated into blocks (`recomp/tools/arm7rec.py`) that call the
  interpreter's always-inline `exec(op, pc)` with constant arguments, so the compiler folds the decoding away
  and the behaviour stays identical. The interpreter runs anything without a block, and the blocks are
  disabled if the code in ARAM changes. An idle skip in the driver's main loop.
- **DSP programs** compiled ahead of time (`recomp/tools/dsprec.py`), selected by the FNV-1a hash of MPRO;
  output bit-identical to the DSP interpreter.
- 64-channel mixer (PCM16/PCM8/ADPCM, ADSR with the hardware rate tables) with cached channel parameters.

## Threads

Priorities stay in 0x40–0xBF, and the busy game thread runs below GXM's display-queue thread (it starved it at
the same priority).

| Core | Threads (priority) |
|---|---|
| 0 | game (0x60) |
| 1 | render (0x48), audio output (0x40), ARM7 + mixer (0x50) |
| 2 | frame build (0x48), disc reader (0x58), save-state writer (0x70) |

Core 3 is left to the system.
