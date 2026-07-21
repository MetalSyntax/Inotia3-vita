/*
 * @file  wr_timer.c
 * @brief See wr_timer.h. Faithful port of WrapperRenderer's timer bookkeeping.
 */

#include "wr_timer.h"

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
    timerList[idx2] = timerID;
    intervaltimeList[idx2] = intervaltime;
    firetimeList[idx2] = firetime;
    losstimeList[idx2] = 0;
}

void wr_unset_timer(int timerID) {
    for (int i = 0; i <= maxIndex; i++) {
        if (timerList[i] == timerID) {
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

void wr_run_timers(int64_t (*update_time)(void), void (*render)(int timerID)) {
    int64_t now = update_time(); // WrapperRenderer.process() -> UpdateCurTime()
    for (int i = 0; i <= maxIndex; i++) {
        int64_t losstime = (now - firetimeList[i]) + losstimeList[i];
        if (timerList[i] != 0 && losstime >= 0) {
            firetimeList[i] = 0;
            render(timerList[i]);
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
