/*
 * @file  wr_touch.c
 * @brief See wr_touch.h. Faithful port of WrapperEventHandler.MultiTouchListener.
 */

#include "wr_touch.h"
#include "utils/utils.h"

#include <psp2/touch.h>

#include <stdlib.h>

#define WR_MAX_HW_REPORTS 8

// Vita's front touch panel reports in a fixed 1920x1088 coordinate space --
// exactly 2x the 960x544 panel resolution nativePreInit() reports to the
// .so (main.c's SCREEN_W/SCREEN_H) -- regardless of what's actually plugged
// in, so a flat /2 is always the right panel->screen scale (same constant
// Zenonia3-vita's loader/main.c uses for its own touch code).
#define TOUCH_PANEL_SCALE 2

// Per-slot state, mirroring WrapperEventHandler's prevPoint[]/dragTime[]
// arrays (Point[]/prevStatus[] are folded into slotHwId/active below).
static int slotHwId[WR_MAX_TOUCH_POINT];   // raw SceTouchReport::id occupying this slot, -1 = free
static int slotX[WR_MAX_TOUCH_POINT];      // last position sent to the engine (screen px)
static int slotY[WR_MAX_TOUCH_POINT];
static uint64_t slotDragTime[WR_MAX_TOUCH_POINT];

static int active_slot_count(void) {
    int n = 0;
    for (int i = 0; i < WR_MAX_TOUCH_POINT; i++) if (slotHwId[i] != -1) n++;
    return n;
}

void wr_touch_init(void) {
    for (int i = 0; i < WR_MAX_TOUCH_POINT; i++) {
        slotHwId[i] = -1;
        slotX[i] = 0;
        slotY[i] = 0;
        slotDragTime[i] = 0;
    }
    // psvita-porting skill's input_handling.md: sceTouchPeek() always
    // reports reportNum == 0 until sampling is explicitly turned on.
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
}

static inline int makeword(int x, int y) {
    return ((x & 0xFFFF) << 16) | (y & 0xFFFF);
}

void wr_process_touch(wr_native_event_fn native_event) {
    SceTouchData touch;
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

    int seen[WR_MAX_TOUCH_POINT] = {0};

    for (int r = 0; r < touch.reportNum && r < WR_MAX_HW_REPORTS; r++) {
        int hwId = touch.report[r].id;
        int x = touch.report[r].x / TOUCH_PANEL_SCALE;
        int y = touch.report[r].y / TOUCH_PANEL_SCALE;
        if (x < 0) x = 0; else if (x > 960) x = 960;
        if (y < 0) y = 0; else if (y > 544) y = 544;

        int slot = -1;
        for (int s = 0; s < WR_MAX_TOUCH_POINT; s++) {
            if (slotHwId[s] == hwId) { slot = s; break; }
        }

        if (slot == -1) {
            // New finger -- ACTION_DOWN/POINTER_DOWN (case 0/5).
            for (int s = 0; s < WR_MAX_TOUCH_POINT; s++) {
                if (slotHwId[s] == -1) { slot = s; break; }
            }
            if (slot == -1) continue; // more than 5 simultaneous touches -- drop, matches MAX_TOUCH_POINT

            slotHwId[slot] = hwId;
            slotX[slot] = x;
            slotY[slot] = y;
            slotDragTime[slot] = 0;
            seen[slot] = 1;

            int touchCount = active_slot_count();
            native_event(WR_TOUCH_DOWN, slot + 1, makeword(x, y), touchCount);
            continue;
        }

        seen[slot] = 1;

        // Already-tracked finger -- ACTION_MOVE (case 2), gated by the same
        // 40ms + >2px drag threshold WrapperEventHandler applies before
        // forwarding a move (avoids flooding the engine with sub-pixel jitter).
        uint64_t now = current_timestamp_ms();
        int dx = x - slotX[slot];
        int dy = y - slotY[slot];
        if (now - slotDragTime[slot] > 40 && (abs(dx) > 2 || abs(dy) > 2)) {
            slotDragTime[slot] = now;
            slotX[slot] = x;
            slotY[slot] = y;
            native_event(WR_TOUCH_DRAG, slot + 1, makeword(x, y), active_slot_count());
        }
    }

    // Any slot that was active but didn't show up in this frame's reports
    // lifted -- ACTION_UP/POINTER_UP (case 1/6). touchCount is reported
    // *before* freeing the slot, matching WrapperEventHandler (decrements
    // PointerCountInGame only after queuing the release event).
    for (int s = 0; s < WR_MAX_TOUCH_POINT; s++) {
        if (slotHwId[s] != -1 && !seen[s]) {
            int touchCount = active_slot_count();
            native_event(WR_TOUCH_UP, s + 1, makeword(slotX[s], slotY[s]), touchCount);
            slotHwId[s] = -1;
        }
    }
}
