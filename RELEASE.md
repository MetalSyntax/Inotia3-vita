# Inotia 3: Children of Carnia v1.0.3 — PS Vita Port

Bug-fix and quality-of-life update for the native Android-to-PS Vita wrapper port of **Inotia 3: Children of Carnia** (originally by Com2uS). Powered by so-loader, FalsoJNI, and vitaGL.

This release builds on **v1.0.2**. It fixes input issues reported while playing on physical PS Vita hardware and adds a game speed option.

## 🛠️ Fixes & Improvements

1. **Dialogues advance again**: Cutscene dialogues could stop responding to both touch and buttons, forcing a restart. A button press could leave a virtual finger "stuck" on the screen; every press is now always released.
2. **Dragging works again**: Quest descriptions scroll again, and items can be moved in the bag again, so equipping weapons and armour works.
3. **Buttons no longer press hidden menu items**: While a pop-up, NPC window or dialogue choice is open, the D-pad and Cross no longer trigger the menu behind it.
4. **Left stick works as a D-pad** everywhere, including in menus.
5. **Game speed option (Start + Select)**: Cycles between 15 FPS (original), 23, 25 and 30 FPS. A short notice at the top of the screen shows the new value without pausing the game. The game's logic runs per frame, so a higher FPS also makes the game run faster. The choice is saved.
6. **Skill quick-slots stay visible**: With the touch controls hidden, quick-slots that have a skill assigned (including its cooldown) are now shown, not only item slots.
7. **L + R shows or hides the touch controls**, with an on-screen notice. Start + Select no longer does this; it now changes the game speed.

## 📋 Prerequisites & Requirements

Before installing, ensure your PS Vita has:

- Custom Firmware (3.60 / 3.65 Enso recommended).
- Plugins installed in `ur0:tai/config.txt` under `*KERNEL`:
  - `kubridge.skprx` (v0.3 or newer)
  - `fd_fix.skprx` (or rePatch)
- `libshacccg.suprx` installed in `ur0:data/` (can be extracted using ShaRKBR33D).

## 📦 Installation Guide

**Updating from v1.0.0 / v1.0.1 / v1.0.2:** just install the new `inotia3.vpk`. Your game files and saves in `ux0:data/inotia3/` are kept.

**Fresh install:**

1. Install `inotia3.vpk` on your PS Vita using VitaShell.
2. Legally obtain a copy of the Inotia 3: Children of Carnia Android APK (v1.3.2).
3. Open the `.apk` using any zip extractor (like 7-Zip).
4. Copy `lib/armeabi/libgame.so` to:
   `ux0:data/inotia3/libgame.so`
5. Extract the `assets/` folder from the `.apk` and copy its contents to:
   `ux0:data/inotia3/assets/`
   *(Or copy extracted game resources into `ux0:data/inotia3/assets/common/game_res/`)*
6. Launch the game from LiveArea!

## 🎮 Controls

| Button | Action |
| :--- | :--- |
| D-Pad / Left Analog | Move Character / Navigate menus |
| Cross (✕) | Attack / Confirm / Action |
| Circle (◯) | Back / Close menu / Cancel pop-up |
| Square (▢) | Skill / Quick Slot |
| Triangle (Δ) | Special Skill / Quick Slot |
| L / R Triggers | Quick Slots / Target Toggle |
| Start | Pause Menu |
| Select | Map / Character Info |
| L + R | Show / hide on-screen touch controls |
| **Start + Select** | **Game speed: 15 / 23 / 25 / 30 FPS** *(new)* |
| L + R + Select | Button remap menu |
| Touchscreen | Full Touch UI Navigation |

## 📜 Credits

- Original Game by **Com2uS**.
- **TheOfficialFloW** for so_loader.
- **Rinnegatamante** for vitaGL & FalsoJNI.
- **Bythos** for kubridge.
