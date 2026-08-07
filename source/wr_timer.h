/*
 * @file  wr_timer.h
 * @brief Host-side reimplementation of com.com2us.wrapper.WrapperRenderer's
 *        software timer table.
 *
 * Inotia 3's whole game loop is timer-driven, not frame-driven: the native
 * code (startClet -> CS_knlSetTimer -> TM_Set -> jSetTimer) asks the Java
 * layer to schedule a timer via WrapperJinterface.SetTimer(id, firetime,
 * interval), and the Java GLSurfaceView.Renderer fires it back by calling
 * nativeRender(id) once firetime is reached (WrapperRenderer.process()). The
 * native TM_Run(id) then dispatches the matching callback (e.g. MainProcess
 * for g_MainTimer) that actually advances the game state.
 *
 * On the Vita there is no Java timer scheduler, so this module reproduces
 * WrapperRenderer's timerList/firetimeList/intervaltimeList/losstimeList and
 * its process()/SetTimer()/UnsetTimer() logic verbatim. The clock is the same
 * `sysTime[]` array handed to nativeInit() (the .so reads it via
 * CS_knlCurrentTime() == *jsystemTimePtr), so `firetime` values arriving from
 * SetTimer are absolute in that clock and directly comparable to `now`.
 *
 * Without this, the render loop only ever called nativeRender(0) and no timer
 * with a nonzero id (i.e. every real game timer) ever fired -> black screen.
 */

#ifndef INOTIA3_WR_TIMER_H
#define INOTIA3_WR_TIMER_H

#include <stdint.h>

// Mirrors WrapperRenderer.MAX_NUM_OF_TIMER.
#define WR_MAX_NUM_OF_TIMER 32

// Reset the timer table to its constructed state (all slots empty).
void wr_timer_reset(void);

// WrapperRenderer.SetTimer(int timerID, long firetime, long intervaltime).
// `firetime` is an absolute timestamp in the sysTime[] clock.
void wr_set_timer(int timerID, int64_t firetime, int64_t intervaltime);

// WrapperRenderer.UnsetTimer(int timerID).
void wr_unset_timer(int timerID);

// The timer half of WrapperRenderer.process(): fire every due timer.
//
// `update_time` must refresh the game's sysTime[]/appUpTime[] to the current
// wall clock and return that time in ms (mirrors WrapperRenderer.UpdateCurTime()).
// It is invoked once before the scan and again after each nativeRender()
// dispatch, exactly like the Java original.
//
// `render` is the nativeRender(timerID) dispatch.
void wr_run_timers(int64_t (*update_time)(void), void (*render)(int timerID));

// --- Present-request plumbing (Corrida 31) ----------------------------------
//
// Inotia 3's MainActivity instantiates WrapperDirtyRenderer (NOT the base
// WrapperRenderer every earlier Corrida analyzed), which overrides
// onDrawFrame() to be completely empty and sets the GLSurfaceView to
// RENDERMODE_WHEN_DIRTY. Under that mode, Android never clears or swaps the
// framebuffer on its own each tick -- process()/nativeRender() (driven by
// WrapperDirtyRenderer's own background timer thread) draws directly into the
// EGL back buffer, and the buffer is only actually presented to the screen
// when the .so itself calls WrapperJinterface.Flush() -> wrapperRenderer.
// Flush() -> GLSurfaceView.requestRender() (WrapperDirtyRenderer.java), which
// schedules exactly one (empty) onDrawFrame + the implicit EGL swap that
// follows it. Between Flush() calls the screen is expected to just sit there
// unchanged -- nothing clears it.
//
// This port's main loop previously called glClear()+gl_swap() unconditionally
// every iteration, which wiped out whatever the .so had just drawn before the
// .so itself ever asked for it to be shown -- the real root cause behind the
// persistent black screen (device log confirmed the .so's GRP_Flush/Flush()
// path fires exactly once, during the loading screen, then never again in
// steady state; that's normal under this model, not a stall). java.c's
// Flush_stub (WrapperJinterface method id 41) now calls wr_request_present()
// when the .so asks for a present; main.c's loop only swaps when
// wr_take_present_request() reports one is pending.
void wr_request_present(void);

// Returns 1 and clears the pending flag if a present was requested since the
// last call, 0 otherwise.
int wr_take_present_request(void);

#endif // INOTIA3_WR_TIMER_H
