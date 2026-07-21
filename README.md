<h1 align="center">Inotia 3 PS Vita Port</h1>

<p align="center">
  An in-progress Android-to-PS Vita port of <b>Inotia 3: Children of Carnia</b> using <a href="https://github.com/TheOfficialFloW/so_loader">so-loader</a> and <b>vitaGL</b>.
</p>

## 📊 Current Port Status (July 2026)

This port is currently **in development** and not yet fully playable. The engine was successfully reverse-engineered (Com2uS custom engine, not Gamevil's Clet) and the `.so` library is booting.

### Progress Overview
- [x] **Phase 1: Engine Analysis** - Extracted symbols, mapped the 44 JNI exports, and fully understood the custom Com2uS Android lifecycle.
- [x] **Phase 2: Loader Bootstrap** - SoLoader initialization is fully implemented. The project successfully compiles with GCC 15 and generates the `inotia3.vpk`.
- [x] **Phase 3: Initial Boot & Game Loop** - The game boots on the Vita/Vita3K. Anti-tamper checks (`isCracked`, `excuteDirectly`) have been successfully bypassed. The custom timer-driven game loop is implemented and running.
- [x] **Phase 7 (Partial): Audio Stubs** - The proprietary `SOUNDSYSTEM_*` has been successfully stubbed in JNI to unblock the boot process without crashing.
- [ ] **Phase 4: Graphics** - Translating the fixed-function GLES1 pipeline to `vitaGL` and handling texture conversions.
- [ ] **Phase 5: Input** - Implementing standard Vita touch input (mapping Android's `MotionEvent` to Vita slots).
- [ ] **Phase 6: Asset Loading** - Hooking `fopen`/`stat`/`opendir` to redirect standard libc file I/O to `ux0:data/inotia3/assets/`.
- [ ] **Phase 7: Audio Decode** - Replacing the audio stubs with actual decoding for `.mmf` and `.ogg` files via `sceAudioOut`.

## 🛠️ Building

To compile the port, you need [VitaSDK](https://vitasdk.org/) installed and configured.

```bash
mkdir build
cd build
cmake ..
make
```

> **Note on GCC 15**: The `CMakeLists.txt` includes `-fpermissive`, `-Wl,--no-warn-mismatch`, and `-Wl,--allow-multiple-definition` specifically to fix strictness issues and ABI mismatches when building with modern VitaSDK environments.

## ⚠️ Legal & Setup

This repository does **not** contain any game assets or the proprietary `libgame.so` file. To run this, you will need to extract the assets from your legally obtained copy of the Android APK (specifically `inotia3-1-3-2.apk`) and place them in the `ux0:data/inotia3/` directory on your PS Vita, along with the `libgame.so` placed in `ux0:data/inotia3/libgame.so`.
