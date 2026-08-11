/*
 * @file  wr_touch.c
 * @brief See wr_touch.h. Faithful port of WrapperEventHandler.MultiTouchListener.
 */

#include "wr_touch.h"
#include "utils/utils.h"
#include "utils/logger.h"

#include <psp2/touch.h>
#include <psp2/ctrl.h>

#include <stdlib.h>

#define WR_MAX_HW_REPORTS 8

// Floor for wr_process_buttons()'s synthetic per-button ids (BUTTON_MAP's
// virtual_hw_id below) -- real SceTouchReport::id is an 8-bit hardware
// counter, so anything at/above this is guaranteed to never collide with one.
// wr_process_touch()'s own end-of-frame release loop uses this same floor to
// tell real hardware slots apart from virtual button slots (see the comment
// there for why that distinction matters).
#define WR_VIRTUAL_HW_ID_BASE 1000

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
    //
    // Slots occupied by wr_process_buttons()'s virtual button ids
    // (slotHwId >= WR_VIRTUAL_HW_ID_BASE) must be skipped here: they never
    // appear in touch.report (this is real sceTouchPeek() hardware data, a
    // button press never shows up in it), so without this guard every held
    // button got force-released the very next frame after its DOWN -- a
    // physical button could never register as "still held", only ever as a
    // single tap, regardless of how long it was actually pressed. Those
    // slots are exclusively owned and released by wr_process_buttons()'s own
    // press/release transition logic below.
    for (int s = 0; s < WR_MAX_TOUCH_POINT; s++) {
        if (slotHwId[s] != -1 && slotHwId[s] < WR_VIRTUAL_HW_ID_BASE && !seen[s]) {
            int touchCount = active_slot_count();
            native_event(WR_TOUCH_UP, s + 1, makeword(slotX[s], slotY[s]), touchCount);
            slotHwId[s] = -1;
        }
    }
}

// --- Physical-button-to-touch mapping ---------------------------------------
// This game's on-screen controls (movement d-pad + a tightly packed cluster
// of attack/skill/item circles, all quite small) are awkward to hit
// precisely on a handheld's actual touchscreen -- mapping the Vita's real
// buttons to synthetic touches at those same fixed screen coordinates makes
// the game fully playable without ever touching the panel.
//
// Corrida 76: the coordinates below were originally measured off a
// reference screenshot of the Android APK (960x640) and edge-anchor
// converted to this port's 960x544 canvas -- reasonable in principle, but
// the user reported physical D-Pad UP doing nothing (confirmed: every other
// direction worked), and provided a REAL screenshot taken directly from
// this port running on actual hardware
// (2026-08-11-184634.jpg, confirmed via `sips` to be exactly 960x544, this
// port's own native canvas -- no scaling/conversion needed at all, unlike
// the old reference image). Brightness-centroid measurement (Python/PIL)
// directly against that screenshot found the ENTIRE bottom-anchored
// cluster (d-pad, attack, skills, items) sitting measurably lower on
// screen than the old reference-derived values assumed -- and the
// top-anchored pause button sitting measurably higher -- with the error
// growing the further an element sits from its anchor edge (UP, the
// tallest element in the d-pad, had the single largest miss at ~47px;
// items already close to the bottom edge were only off by single-digit
// pixels and needed no correction). This matches the reported symptom
// exactly: UP's old y=294 missed the real up-arrow's hit region entirely,
// while LEFT/RIGHT/DOWN's smaller misses (18-28px) still landed inside
// their targets by luck. Re-measured coordinates below (UP/DOWN/LEFT/
// RIGHT/START/CROSS/CIRCLE/SQUARE/TRIANGLE) come directly from that
// screenshot; LTRIGGER/RTRIGGER/SELECT measured within single digits of
// their existing values (near the bottom edge, low accumulated error) and
// were left unchanged. CIRCLE/SQUARE/TRIANGLE's real on-screen slots were
// all unequipped/empty in the screenshot (no skill/item icon lit) --
// matched to their existing SCE_CTRL_* assignment by closest X coordinate
// (X isn't affected by the vertical edge-anchor error at all, so it's the
// more reliable signal for telling the 3 otherwise-identical empty circles
// apart) rather than by icon appearance.
typedef struct {
    unsigned int button;   // one SCE_CTRL_* bit
    int x, y;               // target screen coordinate, 960x544 space
    int virtual_hw_id;      // fake SceTouchReport::id -- must never collide
                             // with a real one (an 8-bit hardware counter),
                             // and must stay >= WR_VIRTUAL_HW_ID_BASE so
                             // wr_process_touch() knows to leave it alone
} wr_button_map_t;

static const wr_button_map_t BUTTON_MAP[] = {
    // Movement d-pad (left side of screen).
    { SCE_CTRL_UP,        140, 341, WR_VIRTUAL_HW_ID_BASE + 0 },
    { SCE_CTRL_DOWN,       145, 507, WR_VIRTUAL_HW_ID_BASE + 1 },
    { SCE_CTRL_LEFT,        42, 417, WR_VIRTUAL_HW_ID_BASE + 2 },
    { SCE_CTRL_RIGHT,      245, 417, WR_VIRTUAL_HW_ID_BASE + 3 },
    // Main attack (the large reticle circle, bottom-right corner).
    { SCE_CTRL_CROSS,      880, 470, WR_VIRTUAL_HW_ID_BASE + 4 },
    // Skill circles (top-right cluster: gold cross icon, dark/spiky icon).
    { SCE_CTRL_CIRCLE,     917, 365, WR_VIRTUAL_HW_ID_BASE + 5 },
    { SCE_CTRL_SQUARE,     757, 427, WR_VIRTUAL_HW_ID_BASE + 6 },
    // Item/shield circle (the diamond icon, above the main attack button).
    { SCE_CTRL_TRIANGLE,   827, 365, WR_VIRTUAL_HW_ID_BASE + 7 },
    // Item quick-slots (bottom row: potion, then two more slots).
    { SCE_CTRL_LTRIGGER,   540, 499, WR_VIRTUAL_HW_ID_BASE + 8 },
    { SCE_CTRL_RTRIGGER,   640, 499, WR_VIRTUAL_HW_ID_BASE + 9 },
    { SCE_CTRL_SELECT,     735, 499, WR_VIRTUAL_HW_ID_BASE + 10 },
    // Pause (top-left, below the party portraits).
    { SCE_CTRL_START,       42, 272, WR_VIRTUAL_HW_ID_BASE + 11 },
};
#define WR_BUTTON_MAP_COUNT (sizeof(BUTTON_MAP) / sizeof(BUTTON_MAP[0]))

static unsigned int g_prev_buttons = 0;

void wr_process_buttons(wr_native_event_fn native_event) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);

    for (size_t i = 0; i < WR_BUTTON_MAP_COUNT; i++) {
        const wr_button_map_t *m = &BUTTON_MAP[i];
        int now = (pad.buttons & m->button) != 0;
        int before = (g_prev_buttons & m->button) != 0;
        if (now == before) continue; // no state change this frame

        if (now) {
            // Press -- ACTION_DOWN, same slot pool wr_process_touch() uses.
            int slot = -1;
            for (int s = 0; s < WR_MAX_TOUCH_POINT; s++) {
                if (slotHwId[s] == -1) { slot = s; break; }
            }
            if (slot == -1) {
                l_warn("wr_process_buttons: all %d touch slots busy -- dropping button 0x%x",
                       WR_MAX_TOUCH_POINT, m->button);
                continue;
            }
            slotHwId[slot] = m->virtual_hw_id;
            slotX[slot] = m->x;
            slotY[slot] = m->y;
            slotDragTime[slot] = 0;
            native_event(WR_TOUCH_DOWN, slot + 1, makeword(m->x, m->y), active_slot_count());
        } else {
            // Release -- ACTION_UP, find whichever slot this button occupies.
            for (int s = 0; s < WR_MAX_TOUCH_POINT; s++) {
                if (slotHwId[s] == m->virtual_hw_id) {
                    int touchCount = active_slot_count();
                    native_event(WR_TOUCH_UP, s + 1, makeword(slotX[s], slotY[s]), touchCount);
                    slotHwId[s] = -1;
                    break;
                }
            }
        }
    }

    g_prev_buttons = pad.buttons;
}
