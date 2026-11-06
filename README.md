<h1 align="center">
  <img src="extras/livearea/Pic0.png" alt="Inotia 3 PS Vita Startup Banner" width="700"><br>
  Inotia 3: Children of Carnia · PS Vita Port
</h1>

<p align="center">
  <a href="#status--playability">Status & Playability</a> •
  <a href="#setup-instructions-for-players">How to install</a> •
  <a href="#controls">Controls</a> •
  <a href="#known-issues">Known Issues</a> •
  <a href="RELEASE.md">Release notes</a> •
  <a href="#build-instructions-for-developers">How to compile</a> •
  <a href="#credits">Credits</a>
</p>

An unofficial, native **Android-to-PS Vita wrapper port** of *Inotia 3: Children of Carnia* (originally developed by Com2uS). This port utilizes **so-loader** technology, **FalsoJNI**, and **vitaGL** to load the official ARMv6 shared library (`libgame.so`) directly in native PS Vita RAM.

---

## 🎮 Status & Playability

**Status: FULLY PLAYABLE** (Offline Campaign)

The game has been thoroughly reverse-engineered and debugged across 77+ porting iterations on physical PS Vita hardware.

- [x] **Full Campaign Playable**: Story progression, party management, skills, combat, and map transitions are 100% functional.
- [x] **Native 960x544 Visuals**: Fullscreen rendering via `vitaGL` (GLES 1.1 fixed-function pipeline with `GL_FIXED` to `GL_FLOAT` conversion and `GL_LINEAR` texture filtering).
- [x] **Touch & Physical Controls**: Complete dual-input support (D-Pad, Analog Stick, and Action Buttons mapped to on-screen controls, alongside full front touchscreen multi-touch support).
- [x] **Native Audio**: BGM and sound effects decoded via `Tremor` / `libogg` portlibs.
- [x] **Save System**: Saved games persist directly as loose `.dat` files (`ux0:data/inotia3/save<N>.dat`). Every save keeps the previous file as `save<N>.dat.bak`, and starting a New Game over an occupied slot moves the old save to `save<N>.dat.del` instead of deleting it.
- [x] **Offline Bypasses**: Anti-tamper/protection routines (`excuteDirectly`, `jIsCracked`) and online ad/hub loops (`Extend_CheckAdvDisplay`, `HubInitialize`) are safely bypassed.

---

## Disclaimer

**Inotia 3: Children of Carnia** is a registered trademark of **Com2uS**. The work presented in this repository is unofficial and is not produced, endorsed, or sanctioned by Com2uS or any affiliated entities.

This software does NOT contain original game source code, compiled binaries (`libgame.so`), copyrighted graphics, or game assets. The authors do not promote or condone piracy in any way. To launch and play the game on your PS Vita, you must provide your own legally obtained copy of the Android `.apk`.

---

## Setup Instructions (For Players)

To install and run the game on your PS Vita:

1. **CFW & Plugins**: Ensure your PS Vita is running Custom Firmware (**Enso 3.60 / 3.65** recommended). Install the required kernel plugins in your `ur0:tai/config.txt` under `*KERNEL`:
   ```ini
   *KERNEL
   ur0:tai/kubridge.skprx
   ur0:tai/fd_fix.skprx
   ```
2. **Shader Compiler**: Verify that `libshacccg.suprx` is present in `ur0:data/`. If missing, extract it using **ShaRKBR33D**.
3. **Obtain Game Files**: Legally obtain a copy of the **Inotia 3: Children of Carnia** Android APK (tested version: `inotia3-1-3-2.apk`).
4. **Extract Assets & Native Library**:
   - Open the `.apk` using any ZIP archive tool (e.g., 7-Zip).
   - Copy `lib/armeabi/libgame.so` to:
     `ux0:data/inotia3/libgame.so`
   - Copy the extracted `assets/` folder contents to:
     `ux0:data/inotia3/assets/`  
     *(Alternatively, place extracted game resources under `ux0:data/inotia3/assets/common/game_res/`)*
5. **Install VPK**: Download and install `inotia3.vpk` on your console via VitaShell.

---

## Controls

The port supports both hardware buttons (mapped directly to on-screen UI positions) and direct touchscreen interaction:

| Button | Action |
| :---: | :--- |
| **D-Pad / Left Analog** | Character Movement (Up / Down / Left / Right) |
| **Cross (`✕`)** | Attack / Confirm / Action |
| **Circle (`◯`)** | Back / Close menu / Cancel pop-up |
| **Square (`▢`)** | Skill / Quick Slot |
| **Triangle (`Δ`)** | Special Skill / Quick Slot |
| **L / R Triggers** | Quick Slots / Target Toggle |
| **Start** | Pause Menu / System |
| **Select** | Map / Character Info |
| **L + R** | Show / hide the on-screen touch controls (hidden by default) |
| **Start + Select** | Cycle game speed: 15 (original) / 23 / 25 / 30 FPS |
| **L + R + Select** | Button remap menu |
| **Front Touchscreen** | Full Touch UI Navigation & Direct Movement |

---

## Known Issues

- **Online Hub / Store Disabled**: Online networking functions (Com2uS Hub, In-App Purchases, and News Banners) are disabled by design for offline stability.
- **Touch HUD hidden by default**: The on-screen D-pad and action circles are drawn at 1% opacity (still touchable). Press **L + R** to show them.
- **Main menu online buttons removed**: Tapjoy, Hub Gem and the Terms of Use scroll no longer appear on the title menu.
- **Higher FPS also speeds the game up**: Game logic advances per frame, so 23/25/30 FPS (Start + Select) makes everything run faster than the original 15 FPS. The choice is saved in `ux0:data/inotia3/config.txt`.
- **Save File Placement**: Save files are written directly to `ux0:data/inotia3/save<N>.dat` rather than subdirectories to ensure reliable persistence across sessions.

---

## Build Instructions (For Developers)

### Prerequisites

- [VitaSDK](https://vitasdk.org/) with `softfp` support configured in your system environment (`$VITASDK`).
- CMake 3.10+ and GCC 15 toolchain (or `vdpm` softfp environment).

### Compilation Commands

```bash
# Clone the repository
git clone https://github.com/your-username/Inotia3-vita.git
cd Inotia3-vita

# Create build directory
mkdir build && cd build

# Configure and compile
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

> **vitaGL**: The build compiles the vendored copy in `vendor/vitaGL` (see `vendor/vitaGL/VENDORED.md`) instead of the SDK's global `libvitaGL.a`.
>
> **Path with spaces / CMake 4**: VitaSDK tools fail when the source path contains spaces, so build from a space-free copy (e.g. `rsync` to `/tmp/inotia3-src`). With CMake 4.x, add `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.

> **GCC 15 / VitaSDK Note**: `CMakeLists.txt` automatically includes flags (`-fpermissive`, `-Wl,--no-warn-mismatch`, `-Wl,--allow-multiple-definition`) required for ABI compatibility and softfp interop.

---

## Credits & Acknowledgments

- **Inotia 3: Children of Carnia** is intellectual property of **Com2uS**.
- **Android SO Loader** framework by [TheOfficialFloW](https://github.com/TheOfficialFloW/so_loader).
- **vitaGL** graphics library by [Rinnegatamante](https://github.com/Rinnegatamante/vitaGL).
- **FalsoJNI** library by [Rinnegatamante](https://github.com/Rinnegatamante/FalsoJNI).
- **kubridge** plugin by [Bythos](https://github.com/bythos14/kubridge).
- Special thanks to the PS Vita homebrew and porting community.
