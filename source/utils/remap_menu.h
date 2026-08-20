/*
 * @file  remap_menu.h
 * @brief Blocking, dialog-driven physical-button remap flow.
 */

#ifndef INOTIA3_REMAP_MENU_H
#define INOTIA3_REMAP_MENU_H

// Walks the user through reassigning each of wr_touch.c's BUTTON_MAP actions
// to a different physical Vita button, using SceMsgDialog message boxes
// (dialog.c) -- no custom font rendering needed. Blocks the calling thread
// until the flow finishes or is cancelled, exactly like dialog.c's own
// fatal_error() already does for its message dialog. Persists the result via
// settings_save() before returning. Meant to be invoked from the main loop
// when wr_touch.c detects the button-remap-menu chord.
void remap_menu_run(void);

#endif // INOTIA3_REMAP_MENU_H
