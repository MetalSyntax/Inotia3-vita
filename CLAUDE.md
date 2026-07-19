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

## Build

`CMakeLists.txt` is configured for this project: `VITA_APP_NAME "Inotia 3"`, `VITA_TITLEID "INOTIA3PV"`,
`VITA_VPKNAME "inotia3"`, `DATA_PATH "ux0:data/inotia3/"`, `SO_PATH "${DATA_PATH}libgame.so"`,
`project(inotia3 ...)`. Build via `porting_tools/build/build_and_install.sh`, which `rsync`s the repo to
`/tmp/inotia3-src`/`/tmp/inotia3-build` first — **required**, the real project path has a space
(`PSVITA Develop`) that breaks VitaSDK's toolchain/`vita-mksfoex`.

## Deploy / debug loop

`porting_tools/manage_vita.py` is an interactive FTP menu against VitaShell on a real console: upload the
VPK, pull the latest `.psp2dmp`/log, disconnect Proton VPN (LAN routing conflict), run the build/decompile/
test scripts under `porting_tools/{build,tests,misc}/`. `VITA_IP`/`VITA_LOGS_DIR`/`LOCAL_VPK_PATH` are
hardcoded at the top of the file for this project already.

No emulator/CI test suite exists yet for gameplay logic. `porting_tools/tests/run_tests.sh` is a
placeholder — it was inherited from an unrelated cocos2d-x port and doesn't apply to this engine's audio
system; write a real host-side test once the `.ogg`/`.mmf` decode path is implemented. Verification happens
by installing the VPK on Vita3K/real hardware and reading the log (`ux0:data/inotia3/logs/`) and/or
`.psp2dmp`. Use the `so-crash-triage` skill to cross-reference log → dump → `.so` symbol → `out_ghidra.c`
pseudo-C on any crash.

## Repository layout and what's actually tracked

`.gitignore` excludes the original game's copyrighted material: `inotia3-1-3-2/` (extracted APK, including
`lib/armeabi/libgame.so`), `*.apk`/`*.zip`, `decompiled_apk/` (jadx output), `decompiled_so/` (Ghidra
output), and build artifacts/logs/dumps. `source/`, `lib/` (vendored FalsoJNI/so_util/kubridge/fios — the
loader's own code, **not** proprietary), `extras/`, and `porting_tools/` are tracked. A fresh clone is not
buildable as-is — `inotia3-1-3-2.apk` must be supplied locally (a legally-obtained copy) and re-decompiled
(`porting_tools/build/decompile_all.sh`) before building.

## Skills

- `psvita-porting` (also copied to `.claude/skills/psvita-porting/` for a self-contained checkout) — generic
  SoLoader porting patterns (JNI stubs, asset packaging, input, VPK packaging, hardware debugging).
- `so-crash-triage` (global) — cross-reference a crash log + `.psp2dmp` against the real `.so` and its
  decompiled sources.
- `psvita-port-init` (global) — the checklist this repo's initialization followed; reuse it verbatim for
  the *next* port.
