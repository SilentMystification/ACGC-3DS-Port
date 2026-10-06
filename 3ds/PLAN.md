# Animal Crossing GC → Nintendo 3DS: port plan

Native 3DS homebrew port of Animal Crossing (GAFE01, USA Rev 0), built on the
[ac-decomp](https://github.com/ACreTeam/ac-decomp) decompilation and the
[ACGC-PC-Port](https://github.com/flyngmt/ACGC-PC-Port) platform layer.

## Goals

- Run on Old 3DS (O3DS) and New 3DS (N3DS).
- Target 60 fps on both. If O3DS cannot hold 60 after optimization, a 30 fps lock is a fallback setting only.
- Shape hot code for the ARM11 instead of the GameCube's Gekko.
- Top screen: game. Bottom screen: map/inventory UI, with a toggleable debug page (frame time, memory).

## Status

| Phase | State |
|---|---|
| 0. Decomp baseline | Done. `ninja` in ac-decomp matches `build.sha1` (static.dol `2ae8f56e…`, foresta.rel `c59d278a…`). |
| 1. 3DS skeleton | Done. Boots to the main loop in Azahar (O3DS and N3DS modes), no crashes. Rendering is stubbed: the top screen is black. |
| 2. Renderer | In progress. citro3d backend draws the logo, text and NPCs on the title screen. World is black, player body is solid green, EFB copies read black. |
| 3. Input, audio, saves | Input and audio output work through the shim. Audio command queue overflows on O3DS (see Known issues). |
| 4. ARM performance | Not started. |
| 5. Polish | Not started. |

## Hardware constraints that drive the design

| | GameCube | O3DS | N3DS |
|---|---|---|---|
| CPU | Gekko PPC 486 MHz, paired singles | ARM11 MPCore 268 MHz | ARM11 804 MHz + 2 MB L2 |
| Cores for the app | 1 | core 0 + time-limited core 1 | core 0, core 1, core 2 |
| RAM for the app | 24 MB + 16 MB ARAM | ~57 MB heap measured (Azahar) | ~85 MB heap measured (Azahar) |
| GPU | Flipper, TEV up to 16 stages | PICA200, 6 combiner stages, no fragment shaders | same |
| Screen | 640×480 | top 400×240, bottom 320×240 touch | same |

- ARM11 integer multiply is fast (1–3 cycles). Integer **divide** has no hardware instruction
  (`__aeabi_idiv`, ~20–80 cycles). VFP float divide and sqrt are also slow (~15–30 cycles).
  Hot paths: replace divides with reciprocal multiplies, use the game's s16-angle sin/cos tables, fast rsqrt.
- The original game runs one logic tick per 60 Hz retrace (`GAME_FRAME` = 1 in `include/game.h`).
  No interpolation should be needed for 60 fps; the whole frame must fit in 16.7 ms.

## Architecture

```
game code → N64 display lists → emu64 (DL interpreter) → GX API (pc/src/pc_gx*.c) → backend
```

The PC port replaces only the backend (OpenGL 3.3). The 3DS port reuses `pc/` unchanged where it can:

- `3ds/include/SDL.h` + `3ds/src/n3ds_sdl.c`: the SDL2 subset that `pc/` uses, on libctru
  (timers, threads, LightLock mutexes, ndsp audio, hid input as a game controller, APT events).
- `3ds/src/n3ds_gl.c`: the GL subset that `pc_gx*.c` calls, on citro3d: frame, fixed-function state,
  rotated viewport/scissor, textures (RGBA8 -> tiled LA8/RGB565/RGBA5551/RGBA8), 36-byte vertex arena, draws.
- `3ds/src/n3ds_tev.c` + `3ds/shaders/gx.v.pica`: replaces `pc_gx_tev.c`. TEV stages -> PICA combiners
  (constant-folded, up to 3 PICA stages per GX stage), GX lighting and texgen per vertex, GX fog via the PICA fog LUT.
- `3ds/src/n3ds_sys.c`: memory layout, screens, game thread, logging.

### Memory layout (important)

emu64's `seg2k0` treats addresses in 0x03000000–0x0FFFFFFF as N64 segment addresses. On 3DS the
default heap (0x08000000) and the main thread stack (below 0x10000000) are in that range. So:

- `__system_allocateHeaps` is overridden: one LINEAR allocation (0x30000000+) holds a 12 MB
  `linearAlloc` pool followed by the newlib malloc heap.
- The game runs on a thread with a 2 MB stack allocated from that heap.

### Logging

stdout/stderr go to the bottom-screen console, `svcOutputDebugString`, and
`sdmc:/3ds/AnimalCrossing/log.txt` (unbuffered, survives a crash or force-close).

## Build and test

`sh 3ds/build.sh` is incremental (the `ac3ds-work` volume keeps the ninja build dir).
`powershell -File 3ds/run_azahar.ps1 [-Until <regex>] [-Timeout <s>] [-Capture]` copies the build to the
Azahar SD card, runs it, and always force-kills Azahar. It stops on a matching game-log line, an Azahar
error dialog, 10 s of game-log silence, or the timeout. Output: `build3ds/last_log.txt`,
`build3ds/last_shot.png`, `build3ds/last_exception.txt`, and crash addresses resolved with addr2line.
On hardware, the game's own exception handler writes `[CRASH]` lines to `log.txt`.


```sh
sh 3ds/build.sh          # Docker devkitpro/devkitarm → build3ds/ac_3ds.3dsx
```

The script rsyncs sources into the Docker volume `ac3ds-work` and builds there. Building directly from
the Windows bind mount is I/O-bound (~10% CPU); from the volume it uses all cores
(clean build ~2 min, incremental ~1 min).

SD card layout (hardware, or `%APPDATA%\Azahar\sdmc\` for Azahar):

```
sdmc:/3ds/dspfirm.cdc                          (dump once with the DSP1 homebrew; needed for audio)
sdmc:/3ds/AnimalCrossing/ac_3ds.3dsx
sdmc:/3ds/AnimalCrossing/rom/<disc image>.iso  (ISO/GCM/CISO)
sdmc:/3ds/AnimalCrossing/shaders/              (copy of pc/shaders; required by pc_gx_tev until Phase 2)
```

Azahar does not model real 3DS CPU timing. Use it for correctness; measure performance on hardware.
Memory faults logged at the moment Azahar is force-closed are shutdown artifacts.

## Phase 2: renderer (in progress)

Next: the world is black and the player body is solid green (check textures/TLUT and combiner output),
EFB copies (step 6), GDB stub mode in `run_azahar.ps1`, framebuffer screenshots from the game.

Replace the GL draw path in `pc_gx.c` / `pc_gx_tev.c` / `pc_gx_texture.c` with citro3d. Keep the GX
state tracking front half (the `GX*` API functions); swap the back half (`pc_gx_flush_vertices`,
`pc_gx_draw_pending`, init, texture upload, EFB copy).

1. Frame setup: citro3d render target on the top screen, swap in `pc_platform_swap_buffers`.
   Viewport from 640×480 to 400×240 (4:3 pillarbox first; crop/stretch as a setting later).
2. Vertex path: one picasso vertex shader (position transform + fog factor). Vertices go into a
   per-frame linear ring buffer.
3. TEV → PICA texture combiners, cached by a TEV-state hash. Most AC materials use ≤ 3 stages
   (the PC shader handles up to 3), so 6 PICA stages should fit. Log materials that do not fit.
4. Lighting: PICA fixed-function lighting where it matches; otherwise per-vertex in the shader.
5. Textures: reuse the CPU decoders in `pc_gx_texture.c`, then convert to PICA tiled formats.
   Cache by address + TLUT hash. Consider ETC1 for large textures on O3DS.
6. EFB copies (used for some effects and the inventory background): render-to-texture.
7. Remove the runtime shader-file requirement once the GL path is gone.

Reference: the Vita port's `vita/src/vita_gx_cmdbuf.c` shows a GX command-buffer approach on a weak
GPU. On Vita, emu64 was ~92% of frame time, so profile emu64 early.

## Phase 3: input, audio, saves

- Input mapping today: A/B/X/Y → A/B/X/Y, L/R → L/R triggers, ZR → Z, Start → Start,
  Select → pause menu, circle pad → stick, N3DS C-stick → C-stick. O3DS needs a C-stick fallback (touch or button combo).
- Bottom screen UI via citro2d (map/inventory + debug page). Touch input later.
- Audio: fix the O3DS command queue overflow (see Known issues). Options: raise the core 1 time
  limit, move the mixer, reduce work in rspsim, or a lower mix rate on O3DS.
- Saves: GCI format in `sdmc:/3ds/AnimalCrossing/save/` (Dolphin-compatible, from the PC port).

## Phase 4: ARM performance

- Profile on O3DS hardware with the frame-time overlay. Fix the top hotspots first.
- Math: reciprocal multiply instead of divide in hot loops; table sin/cos; fast rsqrt in normalize;
  VFP matrix routines.
- Compiler: today `-O2 -fno-strict-aliasing -fwrapv` (the PC port's UB-safe set). Try `-O3`/LTO on
  emu64, GX, and math files (the Vita port does this). Note: the PC docs' "-O0 required" is outdated.
- N3DS: `osSetSpeedupEnable(true)` is already on (804 MHz + L2).
- Culling, LOD, and draw distance settings for O3DS.

## Phase 5: polish

Stereoscopic 3D (N3DS first), touch UI, CIA packaging (more memory via a larger system mode),
settings menu.

## Known issues

- **O3DS audio queue overflow.** `SendStart::Mesg Full Queue` (now rate-limited to one line per 600
  drops). The mixer costs ~2.1 ms per audio frame and reaches 57–60 frames/s while it runs, so CPU
  cost is not the cause; the audio thread stalls at times on the time-limited core 1. Dropped
  commands mean lost sound effects/music changes.
- **No scene logging yet.** The log shows the trademark scene starts; later scene progress is not logged.

## Changes made to shared code (outside `3ds/`)

| File | Change |
|---|---|
| `include/libc/*.h` | Guards renamed to `_AC_LIBC_*` (collided with newlib's guards) |
| `include/libc/math.h` | Removed the MinGW guard workaround (no longer needed after the rename) |
| `include/libultra/libultra.h` | Use newlib `bcmp`/`bcopy`/`bzero` on 3DS |
| `include/libultra/osContPad.h` | Include `<errno.h>` before `#undef errno` |
| `pc/include/pc_platform.h` | `<stdarg.h>`; skip POSIX mmap/dlfcn headers on 3DS |
| `pc/src/pc_main.c` | 3DS image range for seg2k0; keep stdout on 3DS |
| `pc/src/pc_audio.c` | 3DS producer throughput stats |
| `src/static/jsyswrap.cpp` | Larger system heap margin on 3DS (ARM struct layout) |
| `src/static/jaudio_NES/internal/ja_calc.c` | Include path case (`Msl`) |
| `src/static/jaudio_NES/internal/sub_sys.c` | Rate-limit the queue-full message on 3DS |
| `pc/src/pc_gx.c` | 3DS branch in `pc_gx_flush_vertices`: state upload goes to `n3ds_gx_upload` |
| `include/libc64/malloc.h`, `pc/src/pc_misc.c` | 3DS: game arena `malloc`/`free` use libc64 on the `MallocInit` block (PC maps them to system malloc and leaves the ~25 MB arena unused) |
