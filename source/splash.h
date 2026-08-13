/*
 * @file  splash.h
 * @brief Corrida 91: WrapperActivity.initializeBackgroundImage()/
 *        ProgressBarStatus()/dismissBackgroundImage() -- see splash.c for the
 *        full writeup. Draws the "common/game_res/MainBackgroundImage.jpg"
 *        splash the real Android app shows (as a plain Java ImageView, no
 *        native .so involvement) while the .so's own nativeInit() is still
 *        doing synchronous asset-loading work, before the main render loop
 *        (and thus the .so's own drawing) ever starts.
 */

#ifndef INOTIA3_SPLASH_H
#define INOTIA3_SPLASH_H

// Loads and displays the splash image full-screen (one immediate draw+swap
// of both vitaGL buffers, so it survives until something else draws over
// it). Safe to call only after gl_init() has run (main.c calls it before
// nativeInit(), which is the only caller of this function via java.c's
// InitializeBackgroundImage_stub). No-op (logs a warning, does not draw
// anything) if the asset file isn't present -- matches this port's existing
// "missing loose asset" convention (GetFileDescriptor_stub) rather than
// crashing.
void splash_show(void);

// Redraws the splash background plus a simple filled-rect progress bar
// showing cur/total, then presents. Safe to call even if splash_show()
// hasn't loaded a texture (draws just the bar on whatever's already on
// screen) -- real Android updates this several times during loading.
void splash_update_progress(int cur, int total);

// Frees the splash texture. Does not clear the screen -- whatever the .so
// draws next (or main.c's own initial glClear) naturally covers it.
void splash_hide(void);

#endif // INOTIA3_SPLASH_H
