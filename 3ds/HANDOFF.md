# Handoff: 3DS optimization session, stopped 2026-10-06

## Why this file exists
The user stopped this session because the agent appeared to hang repeatedly — on live-device
network calls, and at least once on a plain local `grep` with no network involved. The cause of
the local-command hang is **not diagnosed**. Read "What was failing" below before doing anything
else, and work in smaller, single-step increments until that's understood.

## What was failing (read this first)
- Confirmed pattern: chaining 3-4 tool calls in one turn with no text in between, especially
  around Docker (`docker run ...`, ~5-15s cold start each) and live-device network calls (FTP,
  3dslink, UDP broadcast discovery), produced long silent gaps the user read as a hang requiring
  manual intervention.
- Also happened at least once on a **local-only `grep`** with no network/Docker involved. This
  means the live-device theory does not fully explain it. Root cause unknown — could be
  terminal/harness rendering, could be something else. Do not assume backgrounding +
  short timeouts alone fixes it; it fixed the live-device pattern specifically, not necessarily
  everything.
- A memory file is already saved from this session:
  `C:\Users\Andrew\.claude\projects\c--Users-Andrew-Documents-GitHub-ACGC-3DS-Port\memory\never-block-on-live-device.md`
  — rule: any command touching the 3DS over network (FTP, 3dslink, livelog, UDP discovery) must
  be `run_in_background: true`, every socket gets its own short timeout (5-10s), one attempt then
  report, never chain a wait after starting a background call.
- **New rule for the next agent, not yet saved to memory — do this first:** work in single steps.
  One tool call, then a short text message, every time, even for local/offline commands. Do not
  chain multiple tool calls in one turn while investigating. If the user says something looks
  stuck, stop immediately and report exactly what the last command was and whether it returned,
  rather than issuing another command to "check."
- Known Git-Bash gotcha hit twice this session: `docker run ... /opt/devkitpro/...` paths get
  mangled by MSYS path conversion unless the command is prefixed with `MSYS_NO_PATHCONV=1`.
  Forgetting this produces a confusing "no such file or directory" for a path that does exist
  inside the container.

## Current state of the repo
Working tree has uncommitted changes (per [[commit-only-when-told]] memory — **do not commit
anything unless the user explicitly says so**). Changed/added so far this session, all already
build-checked in Azahar (see "Verified so far" below), **none pushed to hardware after the last
batch, none committed**:

- `3ds/src/n3ds_sys.c` — log output moved off the game thread: a lock-free-producer/ring design,
  one log worker thread on core 1 (deadlock rules documented at the top of that section), core-1
  CPU time limit (`APT_SetAppCpuTimeLimit`, tries 79/69/30 on O3DS, 80/79/69/30 on N3DS per user's
  numbers, drops to 30 around HOME via the APT hook), network buffer cut from 1MB to 128KB and
  only reserved on a netload start, `[BOOT]`/`[CORE]`/`[FPSCR]` logging, `n3ds_boot_mark()` and
  `n3ds_log_core()` helpers, a `[LOCK]` watchdog line if the log lock is held >500ms.
- `3ds/src/n3ds_gl.c` — texture upload: removed the per-texel divide (column lookup table +
  power-of-two fast path). `[WORK]` per-frame counters (vtx bytes, draws, texture cache
  lookups/scan steps/uploads). `[CALLS]` report call wired into the 600-frame `[PERF]` window.
- `3ds/src/n3ds_calls.c` (new) — `--wrap` counters for `__aeabi_idiv/uidiv`, `sinf/cosf/sqrtf/powf`,
  `sin/cos/sqrt/pow`, each tracking up to 8 distinct callers by return address. Gated by debug
  switch `calls` in debug3ds.txt.
- `3ds/src/n3ds_tev.c` — added debug switches `locktest`, `profile`, `calls` (bits 512/1024/2048).
- `3ds/CMakeLists.txt` — added `n3ds_calls.c`, linker `--wrap` flags, and (big one) **un-excluded
  and explicitly added `src/static/libultra/gu/sins.c` and `coss.c`** — the real N64 fixed-point
  sine table (1024 x s16, 2KB), replacing `pc_misc.c`'s double-precision `sin()`/`cos()`
  fallback that an Azahar `[CALLS]` profile showed running ~723 times/frame combined.
- `pc/src/pc_misc.c` — removed the double-math `sins`/`coss` (now provided by the real table, see
  above). **Mirrored into `pc/CMakeLists.txt` too (PC build), but the PC build could not be
  verified — no MinGW32 toolchain is available in this environment (`MSYSTEM=MINGW64`, no `gcc`
  on PATH).** If you have a working PC build, check this compiles/links first.
- `pc/src/pc_gx_texture.c` — texture cache: added a 256-bucket hash index on `data_ptr` in front
  of the old linear scan (was O(n) up to 2048 entries, now ~1.4 steps/lookup measured). Added
  `pc_gx_texture_work_stats()` for the `[WORK]` counters above.
- `pc/src/pc_gx.c` — replaced several divide-by-constant (255.0f/32767.0f/127.0f) with
  multiply-by-reciprocal (`GXNormal3s16/3s8`, TEV/fog color unpack, `GXSetCopyClear`).
- `pc/src/pc_disc.c`, `pc/src/pc_assets.c`, `pc/src/pc_main.c` — `PC_BOOT_MARK()` timeline calls
  (defined in `pc/include/pc_platform.h`, 3DS-only, no-op on PC) at each boot phase.
- `pc/src/pc_dvd.c` — times big disc reads (>512KB) into `[BOOT]`.
- `pc/src/pc_texture_pack.c` — XXH64 self-test skipped on 3DS boot (`#ifndef TARGET_3DS`).
- `pc/src/pc_settings.c`, `pc/src/pc_settings_menu.c` — on 3DS: default `window_width/height` to
  400x240 (was 640x480, cosmetic only — the real output is always 400x240 via
  `SDL_GL_GetDrawableSize` regardless of this setting), `msaa` default 0 (not implemented on
  3DS). Hid the Display/VSync/MSAA/Resolution menu items on 3DS (`#ifndef TARGET_3DS`) since
  they're no-ops there (SDL window calls are stubs) and Resolution specifically could trigger a
  pointless 15s confirm dialog.
- `3ds/push_3ds.sh` — now calls `3ds/tools/livelog.py` instead of its old inline Python client
  (which crashed on Japanese log text, cp1252 encoding error).
- `3ds/tools/livelog.py` (rewritten) — raw-byte output, reconnect on drop, sequence-number gap
  detection.
- `3ds/tools/checklog.py` (new) — checks a log for expected tags (`[BOOT] [PERF] [PROFILE] [CORE]
  [FPSCR] [APT] [TEX] [WORK] [CALLS]`), flags gaps/problems, compares two logs line-for-line. Has
  a `--selftest`.
- `3ds/PLAN.md` — added a hardware-shape rules table, N3DS speedup TODO (deliberately NOT enabled
  — target is 60fps on O3DS at 268MHz without it), and two "future TODO, not yet investigated"
  known issues (see below).
- `C:\Users\Andrew\.claude\plans\snappy-sparking-elephant.md` — the full optimization plan (see
  "The plan" below for a summary; read the file for the complete version with the hardware-shape
  table, LUT section, and threading rules).
- `build3ds/crash_dumps/crash_dump_0000002{1,2}.dmp` — pulled via FTP (port 5000,
  `/luma/dumps/arm11/`), 244 bytes each. Confirmed by hex: Luma3DS exception-dump magic
  (`DEADC0DE DEADCAFE`), process name "3dsx_app" readable in the bytes (our game). **Register
  values not decoded yet** — needs Luma3DS's actual `ExceptionDump` struct layout fetched from
  its source before trusting any specific PC/LR/FAR offset; do not eyeball-guess the byte offsets.

## Verified so far (Azahar, via `3ds/run_azahar.ps1` + `3ds/tools/checklog.py`)
- All of the above builds clean (`sh 3ds/build.sh`).
- Logs cross-checked clean: SD log and Wi-Fi log match line-for-line, 0 sequence gaps, all
  expected tags present except `[TEX]` (never wired up) and transiently `[CALLS]` (only prints
  every 600 frames, needs a ~45s+ run to appear).
- `[CALLS]` confirmed the sine-table fix: before, `sin`/`cos` (double) showed ~216-218k calls per
  600 frames (~360 each/frame, ~723 combined) from two call sites resolved to `pc_misc.c:180/185`
  (now replaced). After the fix, rebuilt and reran: those two lines are gone entirely from
  `[CALLS]` output, all other counts (`uidiv`, `idiv`, `sinf`, `cosf`, `powf`) unchanged.
- Texture cache index confirmed via `[WORK]`: scan_steps/lookup dropped to ~1.3-1.4 (was up to
  2048 worst case before).
- **Pushed to hardware once after the sine-table fix** (O3DS, 192.168.1.168). Result: **no
  measurable fps change** (17.3fps/proc 55.6ms vs previous 17.5fps/proc 55.7ms — within noise).
  User confirmed this plainly. Reasoning given and accepted: Azahar's CPU is dynarmic (JIT to
  host x86), so removing 723 double-trig calls/frame saves maybe 50-100 cycles each on a 268MHz
  ARM11 (~0.3-0.8ms/frame) — real but tiny next to the ~55ms/frame total. **This was expected,
  not a failure**; the sine-table fix was correctness/cache-footprint work, not the fps fix.

## The actual bottleneck (not yet touched)
From Azahar's own `[PROFILE]` breakdown (every run, consistent):
```
cpu_other=~28ms gx_flush=~9-11ms ... emu64_task=~22-27ms ... jw_frame=~22-27ms ... efb_copy=0.000
```
GPU draw time on real hardware is 6.3-6.6ms (confirmed via `[PERF]` on the O3DS push)
and the GPU is otherwise idle. **The CPU-side `emu64_task` (N64 display-list interpretation) is
the dominant cost, not divides/trig/texture work.** This matches the plan's step 3
("CPU work into the GPU") and is the next real target, but it's bigger and riskier than anything
done so far.

### What was found mid-investigation (not yet acted on)
- `emu64.c`'s `set_position()` (~line 2680) does a CPU-side matrix transform
  (`guMtxXFM1F_dol`/`MTXMultVec`) on vertex position/normal, but **only once per unique "nonshared"
  vertex** (shared vertices across a triangle strip/fan are transformed once and reused) — this
  is a real GC-era optimization (avoid GX hardware matrix-switch cost), not an obviously wasteful
  per-triangle cost. The PICA has no equivalent matrix-switch cost (uniform upload is cheap,
  draw-call granularity), so architecturally the CPU pre-transform *might* be unnecessary on our
  GPU — but changing this means restructuring how `pc_gx.c`/`gl_draw_elements` batches draws
  around matrix changes, and risks breaking shared/nonshared vertex-index assumptions. **Do not
  attempt this blind.** Get data first (see below).
- Checked the `idiv` hot callers via `[CALLS]` (addr2line via
  `MSYS_NO_PATHCONV=1 docker run --rm -v ac3ds-work:/work devkitpro/devkitarm:latest
  /opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line -f -e /work/build/ac_3ds.elf <addr>...`):
  resolved to `emu64.c:4188` (`dl_G_FILLRECTEv`), `jaudio_NES/internal/driver.c:135`
  (`Nas_CpuFX`), `jaudio_NES/internal/rspsim.c:621` (`RspStart`), `m_rcp.c:205/208`
  (`texture_z_light_prim_xlu_disp`). All low call counts (3.75-24/frame) — **not the bottleneck
  either**, confirmed negligible, same conclusion as the trig fix: real but tiny.
- **Found, but not yet used:** `emu64.c`'s own dispatch loop (`emu64_taskstart_r`, ~line 5763)
  already has per-opcode timing built in from the original decompiled game code —
  `EMU64_TIMED_SEGMENT_BEGIN()`/`END(command_info[cmd_index].time)` wraps every single opcode
  handler call (`dl_G_VTX`, `dl_G_TRI1`, `dl_G_DL`, etc.), and there are already
  `pc_emu64_frame_vtx_cmds`/`pc_emu64_frame_tri_cmds`/`pc_emu64_frame_dl_cmds`/
  `pc_emu64_frame_noop_cmds` counters (`#ifdef TARGET_PC`). **This was the next concrete step when
  the session was stopped: check whether `command_info[].time` and these counters are already
  printed anywhere (grep turned up nothing obvious yet), and if not, add a periodic print of the
  top entries by time.** This is additive-only (no logic change, just reading existing
  instrumentation), low risk, and would show which opcode group (vertex transform vs triangle
  submit vs display-list jump vs texture/tile setup) actually dominates the ~22ms — real data
  before any structural change to emu64's vertex pipeline.

## The plan
Full version at `C:\Users\Andrew\.claude\plans\snappy-sparking-elephant.md`. Summary of remaining
steps (0 and 1 are done, see "Verified so far" above):
- **Step 2** (not started): vertex path in a 32-byte cache-line-aligned layout under
  `TARGET_3DS`, writing directly into the linear arena (removes a 96-byte struct + memset +
  copy). Needs checking every `texcoord[i]` write for i>0 in `pc_gx.c` first.
- **Step 3** (investigation started, see above): CPU work into the GPU — the real fps target.
  Get the `command_info[].time` breakdown first, then decide what's actually worth moving.
- **Step 4**: one-pass texture path in native PICA formats (I4→L4, I8→L8, etc.), merging 3
  redundant passes in `pc_gx_texture.c`/`n3ds_gl.c`'s texture upload into one. Only matters for
  load-time/memory, not steady fps (uploads are rare in steady play per `[WORK]`).
  Step 5: boot time (SD read 2.7s + Yaz0 decode 0.8s on real hardware, confirmed via `[BOOT]`).
- Plan also has: a hardware-shape table (cache sizes, no ARM11 divide, VFP11 non-pipelined
  divide/sqrt, etc.), a LUT section (what's worth tabling vs not), and threading rules
  (no blocking call while a lock is held, no busy waits, etc.) — read these before writing any
  new threaded or hot-path code.

## Known issues (logged as future TODOs per the user, not yet investigated)
1. Stale frame behind a text box — possibly a stale EFB copy. No reproduction steps gathered.
2. No player control after exiting the train at game start (controls work in the train itself,
   Select menu opens fine, character can't move). `m_train_control.c`'s `train_control_state` is
   a lead, not a cause. User explicitly said don't guess-fix this — needs either a hardware round
   trip with targeted logging or scripted input in Azahar to reproduce without a human driving.
3. The Nintendo logo / progressive-scan boot screen is skipped entirely: `osCreateThread2`/
   `osStartThread` in `pc/src/pc_os.c` are PC-port stubs that record a thread entry point and
   then drop it without ever calling it, so `initial_menu_init()`'s `proc()` (which draws the
   logo and waits 5-10s for input) never runs. **Left as-is deliberately** — user wants faster
   test boot for now, restore later (noted as a TODO in `3ds/PLAN.md`).
4. A `[PERF]` 600-frame summary has shown "568 stutters, worst ~14.1-14.2s" on **two separate**
   hardware pushes with nearly identical numbers both times. User confirmed they did not touch
   the settings menu either time (ruling out the first explanation given). Leading theory
   (unconfirmed): the perf counter's first delta is measured from whenever the first `n3ds_gl_swap`
   call happens, and if the engine only calls swap once incidentally early then not again until
   after ~6.5MB of RARC/ARAM blocking loads finish, that gap would show up as one huge fake
   "stutter" rather than real runtime jank. **Not verified** — would need a `[BOOT]`-style mark
   at each swap call during boot to confirm.

## Immediate next step on resume
1. Read this file.
2. Work in single steps (one tool call, then text) until confident the hang issue is understood
   or at least not being triggered.
3. Grep for whether `command_info[].time` or `pc_emu64_frame_*_cmds` are already printed anywhere
   (they may not be — this was interrupted before finishing). If not, add a periodic print
   (every 600 frames, next to `[PERF]`) of the top few `command_info[]` entries by time, as
   `[EMU64]` tag. Build, check in Azahar only (no hardware push needed for this step), read the
   result, decide the real step-3 target from there.
4. Separately: fetch Luma3DS's `ExceptionDump` struct (from its GitHub source) before decoding
   the crash dump bytes further — do not guess register offsets.
5. Do not commit anything. Do not push to hardware without the user explicitly saying "push".
