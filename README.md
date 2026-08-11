<h1 align="center">Inotia 3: Children of Carnia — PS Vita Port</h1>

<p align="center">
  <img src="extras/livearea/Pic0.png" alt="Inotia 3 PS Vita Startup Banner" width="700">
</p>

<p align="center">
  <b>An unofficial Android-to-PS Vita wrapper port</b> of <i>Inotia 3: Children of Carnia</i> (originally by Com2uS) utilizing <b>so-loader</b> technology, <b>FalsoJNI</b>, and <b>vitaGL</b>.
</p>

---

## 📖 Project Overview

This repository contains a native wrapper port for the **PlayStation Vita** of the classic ARPG **Inotia 3: Children of Carnia** (Com2uS).

Instead of re-implementing the game engine or relying on heavy Java/Android emulation, this project uses **so-loader** (pioneered by TheOfficialFloW) to load the ARMv6 shared library (`libgame.so`) directly into PS Vita RAM. It bridges system calls, Android JNI calls, graphics, and audio through **FalsoJNI**, **vitaGL**, and custom C/C++ shims.

---

## 📊 Current Port Progress & Status

The port has progressed through extensive reverse-engineering of Com2uS's proprietary engine (`com.com2us.wrapper.*`) and hardware debugging on both physical Vita hardware and Vita3K.

### Completed Milestones

- [x] **Phase 1: Engine Analysis & Reverse-Engineering**  
      Decompiled `.so` binary using Ghidra/angr. Mapped all 44 exported JNI symbols, dynamic dependencies, and understood the custom Com2uS 3-step native lifecycle (`nativePreInit` → `nativeInit` → native loop).
- [x] **Phase 2: Loader & Bootstrap**  
      Implemented `so_loader` memory relocation and JNI initialization. Configured full build system for VitaSDK (supporting GCC 15 toolchain).
- [x] **Phase 3: Anti-Tamper Bypass & Initialization**  
      Successfully bypassed internal anti-tamper security routines (`isCracked`, `excuteDirectly`, protection checks) via memory patching.
- [x] **Phase 4: Graphics & Renderer (OpenGL ES 1.1 / vitaGL)**  
      Mapped coordinates to Vita's native **960x544** panel. Implemented deferred `GL_FIXED` to `GL_FLOAT` array conversions (`glVertexPointer`, `glColorPointer`, `glTexCoordPointer`, `glClearColorx`, `glTexParameterx`) required for `vitaGL`. Enforced `GL_LINEAR` texture filtering for smooth visual output.
- [x] **Phase 5: Input System**  
      Wired PS Vita hardware touch interface (`sceTouchPeek`) to `nativeEvent()` with multi-touch slot tracking (supporting 0–4 active touch slots), matching `WrapperEventHandler.MultiTouchListener`.
- [x] **Phase 6: Asset Pipeline & File I/O**  
      Hooked path resolution (`GetFullPathName`, `GetUserDir`, `GetPackageName`) to load extracted assets directly from `ux0:data/inotia3/` as loose files (or fallback to `.apk` offset parsing). Fixed memory-mapping issues for resource packages.
- [x] **Phase 7: Memory & Memory-Allocator Fixes**  
      Resolved critical `__stack_chk_fail` canary triggers and Out-Of-Memory crashes by tuning `sceLibcHeapSize` (16MB), `_newlib_heap_size_user` (256MB), and main thread stack size (4MB), as well as fixing signedness pointer checks (`RES_LoadToHeap` / `RES_MoveToFilePack`).
- [x] **Phase 8: Text Wrapping & String Metrics**  
      Implemented native C fallback for JNI string dimension checks (`GetStringWidthHeightInRect`), resolving lore text wrapping and font loading bottlenecks (`FONT_Load`).
- [x] **Phase 9: Audio Pipeline**  
      Implemented native OGG audio decoding (`libogg` / `Tremor` portlibs) mapped directly to `sceAudioOut`.

---

## 🛠️ Building from Source

### Prerequisites

- [VitaSDK](https://vitasdk.org/) installed and set up in your system environment (`$VITASDK`).

### Compilation Instructions

```bash
# Clone the repository
git clone https://github.com/your-username/Inotia3-vita.git
cd Inotia3-vita

# Create build directory
mkdir build && cd build

# Configure and compile
cmake ..
make
```

> **GCC 15 Compatibility Note**: `CMakeLists.txt` automatically applies `-fpermissive`, `-Wl,--no-warn-mismatch`, and `-Wl,--allow-multiple-definition` flags to handle strictness and ABI compatibility in modern VitaSDK toolchains.

---

## ⚙️ Installation & Setup (Legal Notice)

> **IMPORTANT**: This repository **does NOT contain any copyrighted game assets or the proprietary `libgame.so` binary file**. You must provide your own legally acquired copy of the Android APK.

### Setup Instructions

1. Obtain a legally purchased copy of **Inotia 3: Children of Carnia** Android APK (tested version: `inotia3-1-3-2.apk`).
2. Extract `libgame.so` from `lib/armeabi/libgame.so` within the APK and copy it to your PS Vita at:  
   `ux0:data/inotia3/libgame.so`
3. Extract the `assets/` directory from the APK and place its contents into:  
   `ux0:data/inotia3/assets/`  
   _(Or place extracted assets under `ux0:data/inotia3/assets/common/game_res/`)_
4. Build or download `inotia3.vpk` and install it on your PS Vita using VitaShell (or load it in Vita3K).

---

## 📜 Credits & Acknowledgments

- **Inotia 3: Children of Carnia** is intellectual property of **Com2uS**.
- **so_loader** framework by [TheOfficialFloW](https://github.com/TheOfficialFloW/so_loader).
- **vitaGL** graphics library by [Rinnegatamante](https://github.com/Rinnegatamante/vitaGL).
- **FalsoJNI** library by [Rinnegatamante](https://github.com/Rinnegatamante/FalsoJNI).
- Special thanks to the PS Vita homebrew and porting community for tools, guides, and testing support.
