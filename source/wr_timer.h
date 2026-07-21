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

#endif // INOTIA3_WR_TIMER_H
