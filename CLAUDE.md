# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A PS Vita "soloader" port of **Inotia 3** (Android, ARMv6, **Com2uS** proprietary wrapper engine —
`com.com2us.wrapper.*`). This is not a reimplementation of the game: `source/`/`lib/` build a native Vita
executable that loads the original `libgame.so` in memory, relocates it, and feeds it a hand-built JNI
environment (FalsoJNI, vendored under `lib/falso_jni/`) plus GLES1/libc shims so the unmodified ARM game
code runs directly on the console. There is no Android/Dalvik layer at all.

This repo started from the generic **SoLoBoP** (so-loader boilerplate) template — unlike `../Zenonia3-vita`
(which forks a working `../Zenonia2-vita` codebase for the *same* engine), there is **no sibling port to
fork from here**: Inotia 3's engine is unrelated to Zenonia's Gamevil Nexus2/"Clet" engine, confirmed by JNI
symbol analysis (see `PORTING_PLAN.md` §0 — read this before assuming any code/ABI is portable from the
Zenonia ports). The generic SoLoader/FalsoJNI/vitaGL *architecture* documented in the `psvita-porting` skill
still applies; the concrete implementation does not.

## Engine findings (see PORTING_PLAN.md §0 for full detail)

- Native lifecycle is `nativePreInit(screenW,screenH)` → `nativeInit(sysTime[],upTime[],pixels[],gyro[],
  activity)` → per-frame `nativeEvent(type,p1,p2,touchCount)` + `nativeRender(timerID)` + `nativeReDraw()`
  → 3-step pause/resume (`nativePause`→`nativeResume`→`nativeResumeClet`, not a single event) → `nativeDone`.
- GLES1 fixed-function pipeline only (`glVertexPointer`/`glDrawArrays`/`glClearColorx`/etc., all the
  fixed-point `x`-suffixed calls) — **no `glCreateShader`/`glShaderSource` anywhere in the `.so`**. There is
  nothing to translate GLSL→Cg for this game; don't reintroduce that workflow.
- File I/O is standard libc (`fopen`/`fread`/`fstat`/`opendir`) — no custom JNI asset-manager call like
  Zenonia's `readAssets`. Likely loose files on disk, not APK/OBB-packaged — confirm before implementing
  hooks (PORTING_PLAN.md §Fase 6).
- Audio goes through a Com2uS-proprietary `SOUNDSYSTEM_*`/`SOUNDPLAYER_*` import surface, not
  cocosdenshion/OpenAL. Formats confirmed via `strings`: `.mmf` and `.ogg`.
- 44 exported `Java_com_com2us_*` symbols span `WrapperRenderer` (core lifecycle), `WrapperJinterface`,
  Com2uS Hub (social/login), `InApp`, ads (Tapjoy/Mopub), `ActiveUser`, `WrapperProtection`. Only
  `WrapperRenderer`'s lifecycle is required for offline gameplay — everything else is a no-op-until-proven-
  necessary candidate.
- `nativeRender()` (the timer-driven callback, `source/wr_timer.c`) fuses per-tick game logic AND its own
  GLES1 drawing into one call — there is no separate "advance simulation" vs. "draw the frame" split to
  intercept independently. Confirmed via on-console profiling (PORTING_PLAN.md Corrida 104): GL draw calls
  (~0.02-0.1ms each) and texture uploads (~2-4ms each) are cheap and not the bottleneck in busy scenes (up
  to ~125ms/frame with several `CHAR_AddAction`'d enemies alive) — that cost lives inside the .so's own
  logic, not anything this loader controls. This is *why* a Zenonia-4-style frameskip (which relies on
  logic and drawing being separate calls, so it can skip only the expensive draw) doesn't transplant here:
  tried once (Corrida 105), it measurably slowed the simulated game down and was reverted.
- Menu/UI hit-testing is split across at least 4 independent mechanisms selected by game/menu state — there
  is **no single universal touch-target registry**. The one confirmed live and hookable is `UI_listControl`
  (a real exported data symbol, `nm -D libgame.so` confirms it in both 1-3-2/1-4-5 APKs): a flat array of
  44-byte `UICONTROL` structs (state flag @+0x0, centerX/Y @+0xc/+0x10, width/height @+0x14/+0x18, click
  callback @+0x28) hit-tested by simple center-rect (`UICONTROLLIST_Event`/`UICONTROL_IsInEventArea`,
  `out_ghidra.c` ~129838-129990) — `source/wr_touch.c`'s D-pad menu-cursor navigation reads this directly
  for real coordinates. Other screens (Shop/SkillEquip/SystemSetting) use ad-hoc `TouchUIView_Touch<Screen>`
  handlers instead and aren't covered. Separately, the engine also carries a **dead** cursor/key-navigation
  system (`CONTROL_GetCursor`/`CONTROL2_navigateKeyProc`, `out_ghidra.c` ~17480-17600/~19187/~25384) —
  likely leftover from a feature-phone/physical-keypad build — whose only feeder
  (`TOUCH_HandleCletEvent` → `INPUTQUEUE_Put`) has zero callers anywhere in the decompile; don't try to
  revive it without also reinstating that orphaned call site (materially riskier than reading
  `UI_listControl` directly, which is what this repo does instead).

## Build

`CMakeLists.txt` is configured for this project: `VITA_APP_NAME "Inotia 3"`, `VITA_TITLEID "INOTIA3PV"`,
`VITA_VPKNAME "inotia3"`, `DATA_PATH "ux0:data/inotia3/"`, `SO_PATH "${DATA_PATH}libgame.so"`,
`project(inotia3 ...)`. The real project path has a space (`PSVITA Develop`) that breaks VitaSDK's
toolchain/`vita-mksfoex` — building in place fails; `rsync` the tree to a space-free path (e.g.
`/tmp/inotia3-src`) and build there (`cmake <path> -DCMAKE_BUILD_TYPE=Release && make`) before copying
`inotia3.vpk`/`eboot.bin` back.

**2026-08-19: `porting_tools/` (the old `build_and_install.sh`/`manage_vita.py`/decompile scripts) and the
`psvita-port-toolkit/` skill copy were removed from this repo.** The user now drives build/deploy/decompile
with an external tool kept outside this repo; `.psvita-toolkit.json` (repo root, tracked) is that tool's
per-project config (IPs, paths, titleid) — not something this repo's own scripts read. If you need to
build/deploy/pull logs and no external tool is available in the current environment, fall back to the
manual `rsync` + `cmake`/`make` steps above, and FTP directly (VitaShell, port 1337 per
`.psvita-toolkit.json`) for install/log pulls; don't assume `porting_tools/*` still exists.

## Deploy / debug loop

No emulator/CI test suite exists yet for gameplay logic. Verification happens by installing the VPK on
Vita3K/real hardware and reading the log (`ux0:data/inotia3/logs/`, per `.psvita-toolkit.json`'s
`vita_logs_dir`) and/or a `.psp2dmp` crash dump. Use the `so-crash-triage` skill to cross-reference log →
dump → `.so` symbol → `out_ghidra.c` pseudo-C on any crash.

## Repository layout and what's actually tracked

`.gitignore` excludes the original game's copyrighted material: `inotia3-1-3-2/` (extracted APK, including
`lib/armeabi/libgame.so`), `*.apk`/`*.zip`, `decompiled_apk/` (jadx output), `decompiled_so/` (Ghidra
output), and build artifacts/logs/dumps. `source/`, `lib/` (vendored FalsoJNI/so_util/kubridge/fios — the
loader's own code, **not** proprietary), and `extras/` are tracked. A fresh clone is not buildable as-is —
`inotia3-1-3-2.apk` must be supplied locally (a legally-obtained copy) and re-decompiled (jadx for
`decompiled_apk/`, Ghidra for `decompiled_so/` — see the `psvita-port-init` skill for the general recipe;
this repo's own decompile scripts were removed, see Build section above) before building.

## Skills

- `psvita-porting` (also copied to `.claude/skills/psvita-porting/` for a self-contained checkout) — generic
  SoLoader porting patterns (JNI stubs, asset packaging, input, VPK packaging, hardware debugging).
- `so-crash-triage` (global) — cross-reference a crash log + `.psp2dmp` against the real `.so` and its
  decompiled sources.
- `psvita-port-init` (global) — the checklist this repo's initialization followed; reuse it verbatim for
  the *next* port.
