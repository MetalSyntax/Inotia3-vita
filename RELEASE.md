# Release notes

## 1.0.2 (2026-10-18)

### Fixed / attempted fixes

| # | Issue | Status | Fix |
|---|-------|--------|-----|
| 1 | **Tall skill description panels lose their dark background.** On long skill descriptions (e.g. Fireball, Missile), the text appeared over the skill list with no background behind it. | Fixed (pending console confirmation) | When the popup is taller than the screen allows, the game stores a negative Y position in an unsigned 16-bit value. The original GL code converts it in a way that wraps back to the right negative value, but our `SGL_grpFillRect` hook converted it straight to float and drew the background off-screen at y≈65545. The hook now truncates coordinates to `int16_t` the same way the original does (`source/patch.c`). |
| 2 | **Text cut off: letters with descenders (g, j, p, q, y) lost their bottom row, and some lines wrapped one word too early.** | Fixed | The glyph renderer now draws all 10 font rows (row 10 holds the descenders). A trailing space no longer forces a line break (`source/java.c`). |
| 3 | **Circle didn't work as "back".** | Fixed | Circle now sends the engine's own Android BACK key: it closes in-game menus and cancels pop-up messages. It is ignored on the field and title screen, where BACK would open the quit dialog (`source/wr_touch.c`). |
| 4 | **The on-screen touch controls cover the game when playing with physical buttons.** | Fixed | The D-pad art and action/skill/item circles are now drawn at 1% opacity by default and stay touchable. **L + R** or **Start + Select** toggles them. Skill quick-slots with an assigned skill stay visible (`source/patch.c`, `source/wr_touch.c`). |
| 5 | **Online-only buttons on the main menu** (Tapjoy, Hub Gem, Terms of Use scroll) did nothing offline. | Fixed | These controls are no longer added to the menu, and the CPI banner flag is cleared (`source/patch.c`). |
| 6 | **Risk of losing a save.** The game overwrites `save<N>.dat` in place and deletes it as soon as "New Game" is chosen on an occupied slot. | Fixed | Each save keeps the previous file as `save<N>.dat.bak`. Deleting a slot moves it to `save<N>.dat.del` instead (`source/patch.c`). To recover, rename the file back to `save<N>.dat`. |

### Other changes

- **vitaGL vendored** in `vendor/vitaGL`: the SDK's global `libvitaGL.a` had been rebuilt with unknown flags and caused a black screen. See `vendor/vitaGL/VENDORED.md`.
- **Character portrait flicker (attempt):** triple buffering turned back on, because the flicker appeared after the switch to double buffering.
- **Menu cursor (D-pad navigation):** the highlight is now drawn as cyan corner brackets that stay readable on parchment buttons.
- Translucent fills now draw on texture unit 0 with a white texture, so they no longer depend on texture unit 1's leftover state.
- Logs are now named `ux0:data/inotia3/logs/log_NNN.log`.
- CPU/GPU clock requests no longer abort startup if the firmware rejects a frequency.

## 1.0.1 (2026-08-13)

- Fixed the missing top-left HUD character portrait (multi-texture rendering).

## 1.0.0

- First public release: offline campaign fully playable.
