/*
 * @file  wr_touch.h
 * @brief Host-side reimplementation of com.com2us.wrapper.WrapperEventHandler's
 *        MultiTouchListener (decompiled_apk's WrapperEventHandler.java).
 *
 * `WrapperActivity.IsMultiTouch` defaults to `true` and MainActivity never
 * overrides it (PORTING_PLAN.md Fase 5), so Inotia 3 always runs the
 * MultiTouchListener branch (MAX_TOUCH_POINT = 5), never the single-touch one.
 * That listener packs each event as:
 *
 *   nativeEvent(type, PointID + 1, MAKEWORD(x, y), touchCount)
 *
 * where MAKEWORD(x,y) = ((x & 0xFFFF) << 16) | (y & 0xFFFF), PointID is a
 * *stable* per-finger slot 0..4 (Android's MotionEvent pointer id -- NOT
 * SceTouchReport::id, which is an ever-growing 8-bit hardware counter; see
 * psvita-porting skill's input_handling.md for why passing that raw id
 * corrupts the engine's fixed-size per-touch arrays), and `type` is one of
 * WR_TOUCH_DOWN/DRAG/UP below. GetScreenWidth()/GetRealScreenWidth() (and
 * the Height pair) are both stubbed to 960/544 in java.c, so
 * WrapperEventHandler's ViewPortWay letterbox offset is always a no-op here
 * -- no extra coordinate transform needed beyond panel-to-screen scaling.
 */

#ifndef INOTIA3_WR_TOUCH_H
#define INOTIA3_WR_TOUCH_H

// Mirrors WrapperEventHandler.MAX_TOUCH_POINT (== 5, the MultiTouchListener branch).
#define WR_MAX_TOUCH_POINT 5

// MultiTouchListener's CS_APP_EVENT_*_EX constants (0x80000004/5/6).
#define WR_TOUCH_DOWN (-2147483644)
#define WR_TOUCH_DRAG (-2147483643)
#define WR_TOUCH_UP   (-2147483642)

// WrapperRenderer.nativeEvent(int type, int p1, int p2, int touchCount) dispatch.
typedef void (*wr_native_event_fn)(int type, int p1, int p2, int touchCount);

// Call once, before the render loop starts -- sceTouchPeek() always reports
// reportNum == 0 until the front panel's sampling is explicitly enabled
// (psvita-porting skill's input_handling.md).
void wr_touch_init(void);

// Call once per frame. Reads sceTouchPeek(), assigns stable 0..4 slots to
// each active hardware touch id, and calls native_event() for every
// begin/drag/end -- mirroring MultiTouchListener.onTouch()'s ACTION_DOWN/
// POINTER_DOWN (case 0/5), ACTION_UP/POINTER_UP (case 1/6), and
// ACTION_MOVE (case 2, gated by the same 40ms + >2px drag threshold as the
// Java original) branches.
void wr_process_touch(wr_native_event_fn native_event);

// Call once per frame, alongside wr_process_touch() -- maps the Vita's
// physical buttons to synthetic touches at the fixed screen coordinates of
// this game's on-screen button cluster (see wr_touch.c's BUTTON_MAP), so the
// game is fully playable without touching the actual (small, tightly
// packed) screen elements. Shares the same 5-slot pool wr_process_touch()
// uses, so real touch and physical buttons combined can never exceed
// WR_MAX_TOUCH_POINT concurrently -- the same ceiling the original Android
// MultiTouchListener already enforces.
void wr_process_buttons(wr_native_event_fn native_event);

// Call once per frame, right before gl_swap() -- draws a highlight outline
// around the D-pad menu-cursor's current selection (see wr_touch.c's "D-pad
// menu-cursor navigation" section). A no-op outside a qualifying menu state.
void wr_touch_draw_menu_highlight(void);

// True when wr_process_buttons()'s L+R chord (without SELECT -- see
// TOGGLE_TOUCH_UI_CHORD_MASK in wr_touch.c) has toggled the on-screen
// touch-control graphics (D-pad art + attack/skill/item circle cluster)
// hidden. Purely a draw-time flag: patch.c's VP_Draw*-family hooks are the
// only readers, and they no-op only the drawing of that cluster's gVpControl
// slots -- both wr_process_touch() (real touchscreen taps) and
// wr_process_buttons() (physical-button synthetic taps, BUTTON_MAP) keep
// working identically regardless of this flag, since neither reads it.
int wr_touch_ui_hidden(void);

#endif // INOTIA3_WR_TOUCH_H
