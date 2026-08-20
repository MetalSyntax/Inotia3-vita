/*
 * @file  wr_timer.c
 * @brief See wr_timer.h. Faithful port of WrapperRenderer's timer bookkeeping.
 */

#include "wr_timer.h"
#include "utils/logger.h"

// Field-for-field mirror of WrapperRenderer's private timer arrays.
static int     timerList[WR_MAX_NUM_OF_TIMER];
static int64_t intervaltimeList[WR_MAX_NUM_OF_TIMER];
static int64_t firetimeList[WR_MAX_NUM_OF_TIMER];
static int64_t losstimeList[WR_MAX_NUM_OF_TIMER];
static int     maxIndex = -1;

void wr_timer_reset(void) {
    for (int i = 0; i < WR_MAX_NUM_OF_TIMER; i++) {
        timerList[i] = 0;
        intervaltimeList[i] = 0;
        firetimeList[i] = 0;
        losstimeList[i] = 0;
    }
    maxIndex = -1;
}

// WrapperRenderer.getAvailableIndex().
static int wr_available_index(void) {
    for (int i = 0; i < WR_MAX_NUM_OF_TIMER; i++) {
        if (timerList[i] == 0) {
            if (maxIndex >= i)
                return i;
            maxIndex = i;
            return i;
        }
    }
    return -1;
}

// Corrida 32: Corrida 31 confirmed Flush()/GRP_Flush fire exactly once and
// never again in steady state -- a game-logic stall, not a presentation-
// plumbing bug. Before digging through out_ghidra.c blind, log the actual
// timer landscape: which timer IDs ever get (re)registered/unregistered, and
// which ones actually fire. If it's ALWAYS the same single ID looping
// forever, the .so's own per-frame driver never asks for anything new (dead
// end at the timer layer, look inside that one callback's state machine
// instead); if new timer IDs appear/disappear over time, that's evidence of
// attempted scene transitions worth tracing.
static int g_set_log_count = 0;
static int g_unset_log_count = 0;
#define WR_TIMER_LOG_MAX 40

void wr_set_timer(int timerID, int64_t firetime, int64_t intervaltime) {
    for (int idx = 0; idx <= maxIndex; idx++) {
        if (timerList[idx] == timerID) {
            intervaltimeList[idx] = intervaltime;
            firetimeList[idx] = firetime;
            losstimeList[idx] = 0;
            return;
        }
    }
    int idx2 = wr_available_index();
    if (idx2 < 0) // table full; matches Java's out-of-range behaviour being unreachable in practice
        return;
    if (g_set_log_count < WR_TIMER_LOG_MAX) {
        l_debug("[TIMER] new timer id=%d firetime=%lld intervaltime=%lld (#%d)",
                timerID, (long long) firetime, (long long) intervaltime, g_set_log_count + 1);
        g_set_log_count++;
    }
    timerList[idx2] = timerID;
    intervaltimeList[idx2] = intervaltime;
    firetimeList[idx2] = firetime;
    losstimeList[idx2] = 0;
}

void wr_unset_timer(int timerID) {
    for (int i = 0; i <= maxIndex; i++) {
        if (timerList[i] == timerID) {
            if (g_unset_log_count < WR_TIMER_LOG_MAX) {
                l_debug("[TIMER] unset timer id=%d (#%d)", timerID, g_unset_log_count + 1);
                g_unset_log_count++;
            }
            timerList[i] = 0;
            intervaltimeList[i] = 0;
            firetimeList[i] = 0;
            losstimeList[i] = 0;
            if (i == maxIndex) {
                do {
                    maxIndex--;
                    if (maxIndex < 0)
                        return;
                } while (timerList[maxIndex] == 0);
                return;
            }
        }
    }
}

// Corrida 32: dedup log of every DISTINCT timer id that actually fires
// (render() dispatch), not just what's registered -- see wr_set_timer above.
static int g_seen_fired_ids[WR_MAX_NUM_OF_TIMER];
static int g_seen_fired_count = 0;

static void log_fired_id_once(int timerID) {
    for (int i = 0; i < g_seen_fired_count; i++) {
        if (g_seen_fired_ids[i] == timerID)
            return;
    }
    if (g_seen_fired_count < WR_MAX_NUM_OF_TIMER) {
        g_seen_fired_ids[g_seen_fired_count++] = timerID;
        l_debug("[TIMER] first fire of distinct timer id=%d (%d distinct ids seen so far)",
                timerID, g_seen_fired_count);
    }
}

void wr_run_timers(int64_t (*update_time)(void), void (*render)(int timerID)) {
    int64_t now = update_time(); // WrapperRenderer.process() -> UpdateCurTime()
    for (int i = 0; i <= maxIndex; i++) {
        int64_t losstime = (now - firetimeList[i]) + losstimeList[i];
        if (timerList[i] != 0 && losstime >= 0) {
            int fired_id = timerList[i];
            firetimeList[i] = 0;
            log_fired_id_once(fired_id);
            render(fired_id);
            // If the native callback re-armed this timer id during render,
            // firetimeList[i] is nonzero again and the slot is kept; otherwise
            // it was a one-shot and is cleared.
            if (firetimeList[i] == 0) {
                timerList[i] = 0;
                intervaltimeList[i] = 0;
                firetimeList[i] = 0;
                losstimeList[i] = 0;
                if (i == maxIndex) {
                    do {
                        maxIndex--;
                        if (maxIndex < 0)
                            break;
                    } while (timerList[maxIndex] == 0);
                }
            } else if (intervaltimeList[i] != 0) {
                losstimeList[i] = losstime % intervaltimeList[i];
            }
            now = update_time(); // trailing UpdateCurTime()
        }
    }
}

// See wr_timer.h "Present-request plumbing" for why this exists.
static int present_requested = 0;

void wr_request_present(void) {
    present_requested = 1;
}

int wr_take_present_request(void) {
    if (!present_requested)
        return 0;
    present_requested = 0;
    return 1;
}
