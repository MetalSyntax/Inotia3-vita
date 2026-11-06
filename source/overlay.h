/*
 * @file  overlay.h
 * @brief Loader-drawn overlays (menu selector brackets, OSD toast) in 960x544
 *        device pixels, drawn right before gl_swap() with their own GL state.
 */
#ifndef INOTIA3_OVERLAY_H
#define INOTIA3_OVERLAY_H

void overlay_begin(void);
void overlay_end(void);
// Cyan corner brackets around [x0,x1]x[y0,y1] (between begin/end).
void overlay_brackets(float x0, float y0, float x1, float y1);

// Zenonia 4-style toast at the top of the screen, fades out after ~2s.
void osd_show(const char *msg);
void osd_draw(void);

#endif
