/*
 * @file  wr_touch.c
 * @brief See wr_touch.h. Faithful port of WrapperEventHandler.MultiTouchListener.
 */

#include "wr_touch.h"
#include "utils/utils.h"
#include "utils/logger.h"
#include "utils/settings.h"
#include "utils/remap_menu.h"
#include "overlay.h"

#include <psp2/touch.h>
#include <psp2/ctrl.h>
#include <vitaGL.h>
#include <so_util/so_util.h>

#include <stdlib.h>

extern so_module so_mod;

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

static inline int makeword(int x, int y) {
    return ((x & 0xFFFF) << 16) | (y & 0xFFFF);
}

// --- D-pad menu-cursor navigation --------------------------------------------
//
// User request: navigate menus with the D-pad (X confirms) instead of only
// via the touchscreen. Investigation ruled out reviving the engine's own
// dormant CONTROL2_navigateKeyProc cursor system (out_ghidra.c ~19187,
// ~25384): its input queue (g_lnkInputQueue) is only ever fed by
// TOUCH_HandleCletEvent, which has ZERO callers anywhere in the decompile --
// that whole path is unreachable dead code in this build, and reviving it
// would mean reinstating an orphaned call site, materially riskier than this.
//
// Instead: `UI_listControl` (confirmed via `nm -D libgame.so`, a real exported
// data symbol, present in both 1-3-2 and 1-4-5) is a LIVE array of UICONTROL
// entries (44 bytes/0x2c stride) the engine itself populates for whichever
// screen currently has touchable widgets, and hit-tests by simple center-rect
// (confirmed by reading UICONTROLLIST_Event/UICONTROL_IsInEventArea,
// out_ghidra.c ~129838-129990). This reads that array to get REAL button
// coordinates (no guessing/hardcoding), and drives selection by synthesizing
// an ordinary DOWN+UP touch tap at the chosen entry's center through the SAME
// slot pool and native_event() path BUTTON_MAP already uses -- indistinguishable
// from a real finger tap to the engine, no internal function called directly.
//
// Activation is gated purely on `ui_list_count() > 0` -- NOT also on
// GAMESTATE_nState==3 as a first attempt did: that state number turned out
// not to correspond to the screens users actually call "the main menu"
// (D-pad did nothing there), and the live control count is the only signal
// that actually matters ("are there touchable widgets right now"), so the
// state check was a wrong, overly-restrictive assumption -- removed rather
// than guessing at the right state number without a way to verify it.
//
// Coverage caveat: some menu sub-screens (Shop/SkillEquip/SystemSetting, per
// TouchUIView_Touch<Screen> ad-hoc handlers) don't populate UI_listControl at
// all -- on those, ui_list_count() is 0 and this silently does nothing,
// leaving the D-pad's normal BUTTON_MAP behavior active there instead (see
// wr_process_buttons). Extending coverage to those screens would need
// per-screen hardcoded coordinates (same style as BUTTON_MAP), not attempted
// here.
#define UICTRL_LIST_OFF_COUNT   0xc
#define UICTRL_LIST_OFF_ENTRIES 0x10
#define UICTRL_ENTRY_STRIDE     0x2c
#define UICTRL_OFF_CENTER_X     0xc
#define UICTRL_OFF_CENTER_Y     0x10
#define UICTRL_OFF_WIDTH        0x14
#define UICTRL_OFF_HEIGHT       0x18
#define UICTRL_OFF_CLICK_CB     0x28

#define WR_MENU_CURSOR_VIRTUAL_HW_ID (WR_VIRTUAL_HW_ID_BASE + 100) // distinct from BUTTON_MAP's 0..11

static void *g_ui_list_control = NULL;  // &UI_listControl (a struct instance, not a pointer-to-struct)
// CIRCLE = back/close (wr_circle_back) -- engine globals, resolved in wr_touch_init.
static int16_t *g_state_nState;
static int32_t *g_gamestate_nState;
static uint8_t *g_popupmsg_bOn;
static int32_t *g_keyset_nData;
static int (*g_popupmsg_presskey)(int);
// handleCletEvent (out_ghidra.c ~25243) routes a touch to, in order: the
// POPUPMSG / NPC / choice / wipe-out popups, then UI_listControl only while
// IsIfreeEvent(), then per-GAMESTATE ad-hoc handlers (pause menu, shops...).
static uint8_t *g_npc_popup, *g_choice_popup, *g_wipe_popup;
static int (*g_is_ifree_event)(void);
static int g_menu_cursor_index = -1;
static int g_was_active = 0;

static int popup_active(void) {
    return (g_popupmsg_bOn && *g_popupmsg_bOn) || (g_npc_popup && *g_npc_popup) ||
           (g_choice_popup && *g_choice_popup) || (g_wipe_popup && *g_wipe_popup);
}

static int ui_list_count(void) {
    if (!g_ui_list_control) return 0;
    return *(int32_t *) ((uint8_t *) g_ui_list_control + UICTRL_LIST_OFF_COUNT);
}

static uint8_t *ui_entry(int idx) {
    if (!g_ui_list_control) return NULL;
    uint8_t *entries = *(uint8_t **) ((uint8_t *) g_ui_list_control + UICTRL_LIST_OFF_ENTRIES);
    return entries ? entries + (size_t) idx * UICTRL_ENTRY_STRIDE : NULL;
}

static int ui_entry_clickable(int idx) {
    uint8_t *e = ui_entry(idx);
    return e && *(void **) (e + UICTRL_OFF_CLICK_CB) != NULL;
}

static void ui_entry_rect(int idx, int *cx, int *cy, int *w, int *h) {
    uint8_t *e = ui_entry(idx);
    *cx = e ? *(int32_t *) (e + UICTRL_OFF_CENTER_X) : 0;
    *cy = e ? *(int32_t *) (e + UICTRL_OFF_CENTER_Y) : 0;
    *w  = e ? *(int32_t *) (e + UICTRL_OFF_WIDTH)   : 0;
    *h  = e ? *(int32_t *) (e + UICTRL_OFF_HEIGHT)  : 0;
}

// Re-evaluates cursor validity/entry each call (cheap: a handful of struct
// reads) and returns whether menu-cursor mode should currently intercept the
// D-pad/Cross. Also owns (re)picking a starting selection whenever this just
// became active, or the current index falls off a resized/changed control list.
static int wr_touch_menu_cursor_active(void) {
    int now_active = ui_list_count() > 0 && !popup_active() && (!g_is_ifree_event || (g_is_ifree_event() & 0xff)); // C bool return: low byte only
    if (!now_active) {
        g_menu_cursor_index = -1;
    } else if (!g_was_active || g_menu_cursor_index < 0 ||
               g_menu_cursor_index >= ui_list_count() || !ui_entry_clickable(g_menu_cursor_index)) {
        g_menu_cursor_index = -1;
        for (int i = 0; i < ui_list_count(); i++) {
            if (ui_entry_clickable(i)) { g_menu_cursor_index = i; break; }
        }
    }
    g_was_active = now_active;
    return now_active && g_menu_cursor_index >= 0;
}

// dir: 0=up, 1=down, 2=left, 3=right. Simple weighted directional-nav
// heuristic: candidates must lie (strictly) on the pressed side along the
// primary axis; score favors small primary-axis distance, penalizing
// secondary-axis offset more heavily so a straight neighbor wins over a
// diagonal one.
static int find_nearest_in_direction(int current_idx, int dir) {
    int cx, cy, cw, ch;
    ui_entry_rect(current_idx, &cx, &cy, &cw, &ch);
    int best = -1;
    long best_score = -1;
    int count = ui_list_count();
    for (int i = 0; i < count; i++) {
        if (i == current_idx || !ui_entry_clickable(i)) continue;
        int ex, ey, ew, eh;
        ui_entry_rect(i, &ex, &ey, &ew, &eh);
        int dx = ex - cx, dy = ey - cy;
        int primary, secondary;
        switch (dir) {
            case 0: if (dy >= 0) continue; primary = -dy; secondary = dx; break;
            case 1: if (dy <= 0) continue; primary = dy;  secondary = dx; break;
            case 2: if (dx >= 0) continue; primary = -dx; secondary = dy; break;
            default: if (dx <= 0) continue; primary = dx; secondary = dy; break;
        }
        long score = (long) primary + (long) (secondary < 0 ? -secondary : secondary) * 2;
        if (best < 0 || score < best_score) { best = i; best_score = score; }
    }
    return best;
}

// Moves the selection (D-pad edges) and, on a fresh Cross press, synthesizes
// an ordinary tap (DOWN immediately followed by UP, same slot pool BUTTON_MAP
// uses) at the selected entry's real on-screen center.
static void wr_touch_process_menu_cursor(unsigned int now_buttons, unsigned int prev_buttons,
                                          wr_native_event_fn native_event) {
    int up    = (now_buttons & SCE_CTRL_UP)    && !(prev_buttons & SCE_CTRL_UP);
    int down  = (now_buttons & SCE_CTRL_DOWN)  && !(prev_buttons & SCE_CTRL_DOWN);
    int left  = (now_buttons & SCE_CTRL_LEFT)  && !(prev_buttons & SCE_CTRL_LEFT);
    int right = (now_buttons & SCE_CTRL_RIGHT) && !(prev_buttons & SCE_CTRL_RIGHT);
    int confirm = (now_buttons & SCE_CTRL_CROSS) && !(prev_buttons & SCE_CTRL_CROSS);

    int dir = up ? 0 : down ? 1 : left ? 2 : right ? 3 : -1;
    if (dir >= 0) {
        int next = find_nearest_in_direction(g_menu_cursor_index, dir);
        if (next >= 0) g_menu_cursor_index = next;
    }

    if (confirm && g_menu_cursor_index >= 0) {
        int cx, cy, w, h;
        ui_entry_rect(g_menu_cursor_index, &cx, &cy, &w, &h);
        int slot = -1;
        for (int s = 0; s < WR_MAX_TOUCH_POINT; s++) if (slotHwId[s] == -1) { slot = s; break; }
        if (slot >= 0) {
            slotHwId[slot] = WR_MENU_CURSOR_VIRTUAL_HW_ID;
            slotX[slot] = cx;
            slotY[slot] = cy;
            native_event(WR_TOUCH_DOWN, slot + 1, makeword(cx, cy), active_slot_count());
            native_event(WR_TOUCH_UP, slot + 1, makeword(cx, cy), active_slot_count());
            slotHwId[slot] = -1;
        }
    }
}

// Call once per frame, right before gl_swap() -- draws a simple highlight
// outline around the currently-selected control so D-pad navigation has
// visible feedback (the engine's own "pressed" cursor visuals are driven by
// touch events we deliberately don't synthesize for mere hovering, only for
// the actual confirm tap -- see wr_touch_process_menu_cursor()).
void wr_touch_draw_menu_highlight(void) {
    if (!wr_touch_menu_cursor_active()) return;
    int cx, cy, w, h;
    ui_entry_rect(g_menu_cursor_index, &cx, &cy, &w, &h);
    if (w <= 0) w = 40;
    if (h <= 0) h = 40;
    // Corner brackets only (Carnivores-vita input.c draw_overlay style): pad 2,
    // thickness 1.5, arms 25% of the side capped at 10 -- all in 480x320 logical units.
    float x0 = cx - w / 2.0f - 2.0f, x1 = cx + w / 2.0f + 2.0f;
    float y0 = cy - h / 2.0f - 2.0f, y1 = cy + h / 2.0f + 2.0f;
    const float t = 1.5f;
    float lx = (x1 - x0) * 0.25f, ly = (y1 - y0) * 0.25f;
    if (lx > 10.0f) lx = 10.0f;
    if (ly > 10.0f) ly = 10.0f;
    const float r[8][4] = {
        { x0 - t, y0 - t, x0 + lx, y0 }, { x0 - t, y0, x0, y0 + ly },          // top-left
        { x1 - lx, y0 - t, x1 + t, y0 }, { x1, y0, x1 + t, y0 + ly },          // top-right
        { x0 - t, y1, x0 + lx, y1 + t }, { x0 - t, y1 - ly, x0, y1 },          // bottom-left
        { x1 - lx, y1, x1 + t, y1 + t }, { x1, y1 - ly, x1 + t, y1 },          // bottom-right
    };
    static GLfloat verts[8 * 12]; // static: vitaGL may read client arrays after we return

    GLboolean tex2d = glIsEnabled(GL_TEXTURE_2D);
    GLboolean blend = glIsEnabled(GL_BLEND);
    GLboolean cull = glIsEnabled(GL_CULL_FACE);   // quads can be culled under the y-down ortho (Corrida 92)
    GLboolean carr = glIsEnabled(GL_COLOR_ARRAY); // a live color array would override glColor4f
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);
    glDisableClientState(GL_COLOR_ARRAY);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    // Pass 0: dark teal outline (#0E3A40) 1 unit wider so it reads on parchment buttons;
    // pass 1: Inotia HUD cyan core (#5FF0FF).
    for (int p = 0; p < 2; p++) {
        float g = p == 0 ? 1.0f : 0.0f;
        for (int i = 0; i < 8; i++) {
            float a0 = r[i][0] - g, b0 = r[i][1] - g, a1 = r[i][2] + g, b1 = r[i][3] + g;
            GLfloat *v = &verts[i * 12];
            v[0] = a0; v[1] = b0; v[2] = a1; v[3]  = b0; v[4]  = a1; v[5]  = b1;
            v[6] = a0; v[7] = b0; v[8] = a1; v[9]  = b1; v[10] = a0; v[11] = b1;
        }
        if (p == 0) glColor4f(0x0e / 255.0f, 0x3a / 255.0f, 0x40 / 255.0f, 0.8f);
        else        glColor4f(0x5f / 255.0f, 0xf0 / 255.0f, 0xff / 255.0f, 0.9f);
        glDrawArrays(GL_TRIANGLES, 0, 8 * 6);
    }
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f); // restore a sane default before the next frame's game draws
    if (tex2d) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
    if (!blend) glDisable(GL_BLEND);
    if (cull) glEnable(GL_CULL_FACE);
    if (carr) glEnableClientState(GL_COLOR_ARRAY);
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
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG); // read_pad()'s left stick

    g_ui_list_control = (void *) so_symbol(&so_mod, "UI_listControl");
    if (!g_ui_list_control) {
        l_warn("wr_touch_init: UI_listControl not found -- "
               "D-pad menu navigation disabled, touch/BUTTON_MAP unaffected");
    }
    g_state_nState      = (int16_t *) so_symbol(&so_mod, "STATE_nState");
    g_gamestate_nState  = (int32_t *) so_symbol(&so_mod, "GAMESTATE_nState");
    g_popupmsg_bOn      = (uint8_t *) so_symbol(&so_mod, "POPUPMSG_bOn");
    g_keyset_nData      = (int32_t *) so_symbol(&so_mod, "KEYSET_nData");
    g_popupmsg_presskey = (void *) so_symbol(&so_mod, "POPUPMSG_PressKey");
    g_npc_popup         = (uint8_t *) so_symbol(&so_mod, "NPCPopup");
    g_choice_popup      = (uint8_t *) so_symbol(&so_mod, "isChoicePopupActive");
    g_wipe_popup        = (uint8_t *) so_symbol(&so_mod, "WipeOutPopup");
    g_is_ifree_event    = (void *) so_symbol(&so_mod, "IsIfreeEvent");
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

// Every physical button already drives some BUTTON_MAP tap, so this hotkey
// chord unavoidably overlaps some of them -- detected on the exact frame all
// three buttons first become simultaneously held, and consumed (skipped in
// the normal per-BUTTON_MAP dispatch below) so the chord itself doesn't also
// fire a spurious in-game tap.
#define REMAP_MENU_CHORD_MASK (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT)

// L+R together, WITHOUT SELECT, toggles hiding the on-screen touch-control
// graphics (D-pad art + attack/skill/item circle cluster the game draws
// every frame) -- a purely visual toggle, see patch.c's VP_Draw*-family
// hooks (the only code that reads g_touch_ui_hidden, via wr_touch_ui_hidden()
// below). Deliberately excludes SELECT so this can never fire on the same
// frame as REMAP_MENU_CHORD_MASK above (checked first, and returns before
// this is ever reached on that frame); requiring SELECT to be UP here too
// means holding L+R while SELECT joins in later never double-fires this
// chord either. LTRIGGER/RTRIGGER are ALSO each individually mapped to an
// item quick-slot tap in BUTTON_MAP -- consumed_mask below suppresses just
// those two for the frame the chord fires, same as REMAP_MENU_CHORD_MASK's
// own comment describes, so toggling never ALSO fires a spurious item tap.
#define TOGGLE_TOUCH_UI_CHORD_MASK (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)
// START+SELECT cycles the FPS setting (fps_menu_cycle, remap_menu.c).
#define FPS_CHORD_MASK (SCE_CTRL_START | SCE_CTRL_SELECT)

static int g_touch_ui_hidden = 1; // default: HUD cluster at 1% opacity (patch.c wr_vp_dimmed)

int wr_touch_ui_hidden(void) {
    return g_touch_ui_hidden;
}

// CIRCLE = back/close. Uses the engine's own Android BACK: nativeEvent(2, 4 /*KEYCODE_BACK*/)
// -> handleKeyEvent (out_ghidra.c ~25042), which closes in-game menus (STATE 7, GAMESTATE 3..12)
// but opens the quit Yes/No in the field (GAMESTATE 2) / title (STATE 5) -- skipped there -- and
// is ignored while a POPUPMSG box is up, which we cancel the way the engine does (KEYSET_nData[18]).
static void wr_circle_back(wr_native_event_fn native_event) {
    if (!g_state_nState || !g_gamestate_nState || !g_popupmsg_bOn || !g_keyset_nData || !g_popupmsg_presskey) return;
    if (*g_popupmsg_bOn) { g_popupmsg_presskey(g_keyset_nData[18]); return; }
    if (*g_state_nState == 5) return;
    if (*g_state_nState == 7 && (*g_gamestate_nState < 3 || *g_gamestate_nState > 12)) return;
    native_event(2, 4, -1, -1); // CS_KEY_PRESSEVENT
    native_event(3, 4, -1, -1); // CS_KEY_RELEASEEVENT
}

// Sends UP for every slot held by a physical button. A slot that never gets
// its UP stays "pressed" in the engine forever: dialogue taps stop advancing,
// drags become multi-touch, and once all slots leak no input works at all.
static void release_virtual_slots(wr_native_event_fn native_event) {
    for (int s = 0; s < WR_MAX_TOUCH_POINT; s++) {
        if (slotHwId[s] >= WR_VIRTUAL_HW_ID_BASE) {
            native_event(WR_TOUCH_UP, s + 1, makeword(slotX[s], slotY[s]), active_slot_count());
            slotHwId[s] = -1;
        }
    }
}

// Left stick -> D-pad bits (same deadzone idea as the Zenonia ports).
static unsigned int read_pad(void) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned int b = pad.buttons;
    if (pad.lx < 128 - 64) b |= SCE_CTRL_LEFT;
    else if (pad.lx > 128 + 64) b |= SCE_CTRL_RIGHT;
    if (pad.ly < 128 - 64) b |= SCE_CTRL_UP;
    else if (pad.ly > 128 + 64) b |= SCE_CTRL_DOWN;
    return b;
}

void wr_process_buttons(wr_native_event_fn native_event) {
    unsigned int buttons = read_pad();

    unsigned int consumed_mask = 0;

    int remap = (buttons & REMAP_MENU_CHORD_MASK) == REMAP_MENU_CHORD_MASK &&
                (g_prev_buttons & REMAP_MENU_CHORD_MASK) != REMAP_MENU_CHORD_MASK;
    int fps = (buttons & FPS_CHORD_MASK) == FPS_CHORD_MASK &&
              (g_prev_buttons & FPS_CHORD_MASK) != FPS_CHORD_MASK;
    if (remap || fps) {
        release_virtual_slots(native_event);
        if (remap) remap_menu_run(); // blocks (own gl_swap() loop) until the flow finishes
        else fps_menu_cycle();
        g_prev_buttons = read_pad(); // re-sync so held chord buttons don't fire taps
        return; // skip this frame's normal dispatch; next frame resumes as usual
    }

    // L+R without SELECT -- toggle hiding the on-screen touch-control
    // graphics (see TOGGLE_TOUCH_UI_CHORD_MASK above). Unlike the remap
    // chord above, this does NOT return early: it only needs to consume
    // LTRIGGER/RTRIGGER's own individual BUTTON_MAP taps for this frame, via
    // consumed_mask below, same as every other BUTTON_MAP entry already
    // checks.
    if ((buttons & TOGGLE_TOUCH_UI_CHORD_MASK) == TOGGLE_TOUCH_UI_CHORD_MASK &&
        (buttons & SCE_CTRL_SELECT) == 0 &&
        (g_prev_buttons & TOGGLE_TOUCH_UI_CHORD_MASK) != TOGGLE_TOUCH_UI_CHORD_MASK) {
        g_touch_ui_hidden = !g_touch_ui_hidden;
        osd_show(g_touch_ui_hidden ? "TOUCH CONTROLS: HIDDEN" : "TOUCH CONTROLS: SHOWN");
        consumed_mask |= TOGGLE_TOUCH_UI_CHORD_MASK;
    }

    if ((buttons & SCE_CTRL_CIRCLE) && !(g_prev_buttons & SCE_CTRL_CIRCLE))
        wr_circle_back(native_event);
    consumed_mask |= SCE_CTRL_CIRCLE; // CIRCLE is back-only, never a BUTTON_MAP tap

    int in_menu_cursor_mode = wr_touch_menu_cursor_active();

    for (size_t i = 0; i < WR_BUTTON_MAP_COUNT; i++) {
        const wr_button_map_t *m = &BUTTON_MAP[i];
        // setting_button_remap[i] == 0 means "use the compiled default";
        // m->button itself always identifies the ACTION slot (unaffected by
        // remapping), used below to tell movement/confirm apart regardless
        // of which physical button currently triggers them.
        unsigned int mapped_button = setting_button_remap[i] != 0 ? setting_button_remap[i] : m->button;

        // In menus the D-pad/X drive the selector, and the field
        // HUD taps (fixed coordinates) would only hit random menu items.
        int blocked = (mapped_button & consumed_mask) || in_menu_cursor_mode;

        int held = -1;
        for (int s = 0; s < WR_MAX_TOUCH_POINT; s++)
            if (slotHwId[s] == m->virtual_hw_id) { held = s; break; }
        int now = !blocked && (buttons & mapped_button) != 0;
        int before = (g_prev_buttons & mapped_button) != 0;

        // Release is level-based: a held slot is freed whenever its button is
        // up OR blocked (e.g. a dialogue opened menu-cursor mode while the
        // D-pad was held). Edge-only release used to leak the slot forever.
        if (held >= 0 && !now) {
            native_event(WR_TOUCH_UP, held + 1, makeword(slotX[held], slotY[held]), active_slot_count());
            slotHwId[held] = -1;
            continue;
        }
        if (!now || before || held >= 0) continue; // press only on a fresh edge

        {
            // Press -- ACTION_DOWN, same slot pool wr_process_touch() uses.
            int slot = -1;
            for (int s = 0; s < WR_MAX_TOUCH_POINT; s++) {
                if (slotHwId[s] == -1) { slot = s; break; }
            }
            if (slot == -1) {
                l_warn("wr_process_buttons: all %d touch slots busy -- dropping button 0x%x",
                       WR_MAX_TOUCH_POINT, mapped_button);
                continue;
            }
            slotHwId[slot] = m->virtual_hw_id;
            slotX[slot] = m->x;
            slotY[slot] = m->y;
            slotDragTime[slot] = 0;
            native_event(WR_TOUCH_DOWN, slot + 1, makeword(m->x, m->y), active_slot_count());
        }
    }

    if (in_menu_cursor_mode) {
        wr_touch_process_menu_cursor(buttons, g_prev_buttons, native_event);
    }

    g_prev_buttons = buttons;
}
