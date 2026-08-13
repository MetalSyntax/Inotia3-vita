/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);

void gl_preload() {
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

#ifdef USE_GLSL_SHADERS
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
#endif
}

void gl_init() {
    // Corrida 78: MULTISAMPLE_4X was an unexamined leftover from this repo's
    // original SoLoBoP boilerplate -- both ../Zenonia2-vita and
    // ../Zenonia3-vita (same vitaGL, same hardware, confirmed working in
    // production) deliberately use MULTISAMPLE_NONE for this exact
    // 2D-sprite-blit rendering style, where MSAA's edge-antialiasing cost
    // buys little visible benefit. Pure GPU-load reduction, no gameplay-logic
    // risk (unlike the frame-cap/vsync tuning those two ports also carry,
    // which compensates for Gamevil's Nexus2/"Clet" engine's own frame-rate-
    // coupled game-logic timing -- NOT verified whether Inotia 3's Com2uS
    // wrapper engine has the same coupling, given it already has its own
    // explicit timer system (wr_timer.c) rather than relying on raw frame
    // rate, so that specific technique was deliberately NOT ported here).
    vglInitExtended(0, 960, 544, 6 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
}

void gl_swap() {
    vglSwapBuffers(GL_TRUE);
}

void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height) {
    static int log_count = 0;
    if (log_count < 10) {
        l_debug("[GL] glViewport requested x=%d y=%d w=%d h=%d -- forcing 0,0,960,544", x, y, width, height);
        log_count++;
    }
    glViewport(0, 0, 960, 544);
}

static void force_linear_filter(GLenum target) {
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

// Corrida 25: Corrida 24 showed pixel_sample(n=0) for the real
// MainMenu.png/font.fnt uploads (w=512 h=512 intFmt=1908 type=1401 --
// genuinely GL_RGBA/GL_UNSIGNED_BYTE, so the `type == GL_UNSIGNED_BYTE`
// check wasn't the reason) -- the likely explanation is `pixels == NULL`:
// a common pattern is glTexImage2D(..., NULL) to just allocate storage,
// with the real data uploaded right after via glTexSubImage2D (which had
// no sampling at all). Log explicitly whether `pixels` was NULL, and add
// the same sampling to glTexSubImage2D_soloader so whichever call actually
// carries the real bytes gets inspected.
//
// Corrida 28: post-loop the .so only ever draws two RGB565 (GL_UNSIGNED_
// SHORT_5_6_5, type=0x8363) textures, every frame, forever -- the real
// MainMenu.png (RGBA/GL_UNSIGNED_BYTE, bound_tex=5) is drawn exactly once
// during setup, never again once the steady-state loop starts. Whether
// that's a legitimate baked-down menu background/font-strip (fine) or
// corrupted/blank texture data (the actual black-screen cause) was
// unknowable because sample_pixels() silently skipped every non-byte
// type, including the two formats that matter most here. Decode
// GL_UNSIGNED_SHORT_5_6_5/4_4_4_4/5_5_5_1 into an approximate 0-255 luma
// per sampled pixel so min/max/avg mean the same thing as for the byte
// path -- real varied image content should show a spread with max well
// above 0, not everything pinned near 0.
static void sample_pixels(const void *pixels, GLsizei width, GLsizei height, GLenum type,
                           int *out_n, unsigned int *out_min, unsigned int *out_max, double *out_avg) {
    unsigned long sum = 0;
    unsigned int minv = 255, maxv = 0;
    int nsamples = 0;
    if (pixels && type == GL_UNSIGNED_BYTE) {
        const unsigned char *p = (const unsigned char *) pixels;
        size_t total = (size_t) width * height * 4; // upper bound; fine to overrun format-wise since we're just sampling raw bytes
        for (size_t i = 0; i < total && nsamples < 4096; i += 997) {
            unsigned int v = p[i];
            sum += v;
            if (v < minv) minv = v;
            if (v > maxv) maxv = v;
            nsamples++;
        }
    } else if (pixels && (type == GL_UNSIGNED_SHORT_5_6_5 ||
                          type == GL_UNSIGNED_SHORT_4_4_4_4 ||
                          type == GL_UNSIGNED_SHORT_5_5_5_1)) {
        const unsigned short *p = (const unsigned short *) pixels;
        size_t total_px = (size_t) width * height;
        for (size_t i = 0; i < total_px && nsamples < 4096; i += 61) {
            unsigned short px = p[i];
            unsigned int r8, g8, b8;
            if (type == GL_UNSIGNED_SHORT_5_6_5) {
                r8 = ((px >> 11) & 0x1F) * 255 / 31;
                g8 = ((px >> 5) & 0x3F) * 255 / 63;
                b8 = (px & 0x1F) * 255 / 31;
            } else if (type == GL_UNSIGNED_SHORT_4_4_4_4) {
                r8 = ((px >> 12) & 0xF) * 255 / 15;
                g8 = ((px >> 8) & 0xF) * 255 / 15;
                b8 = ((px >> 4) & 0xF) * 255 / 15;
            } else {
                r8 = ((px >> 11) & 0x1F) * 255 / 31;
                g8 = ((px >> 6) & 0x1F) * 255 / 31;
                b8 = ((px >> 1) & 0x1F) * 255 / 31;
            }
            unsigned int v = (r8 + g8 + b8) / 3;
            sum += v;
            if (v < minv) minv = v;
            if (v > maxv) maxv = v;
            nsamples++;
        }
    }
    *out_n = nsamples;
    *out_min = minv;
    *out_max = maxv;
    *out_avg = nsamples ? (double) sum / nsamples : -1.0;
}

void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, const void *pixels) {
    static int log_count = 0;
    if (log_count < 40) {
        int n; unsigned int minv, maxv; double avg;
        sample_pixels(pixels, width, height, type, &n, &minv, &maxv, &avg);
        l_debug("[GL] glTexImage2D level=%d intFmt=%x w=%d h=%d format=%x type=%x pixels=%p "
                "pixel_sample(n=%d min=%u max=%u avg=%.1f)",
                level, internalformat, width, height, format, type, pixels,
                n, minv, maxv, avg);
        log_count++;
    }
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    force_linear_filter(target);
}

void glTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                              GLsizei width, GLsizei height, GLenum format,
                              GLenum type, const void *pixels) {
    static int log_count = 0;
    if (log_count < 40) {
        int n; unsigned int minv, maxv; double avg;
        sample_pixels(pixels, width, height, type, &n, &minv, &maxv, &avg);
        l_debug("[GL] glTexSubImage2D level=%d x=%d y=%d w=%d h=%d format=%x type=%x pixels=%p "
                "pixel_sample(n=%d min=%u max=%u avg=%.1f)",
                level, xoffset, yoffset, width, height, format, type, pixels,
                n, minv, maxv, avg);
        log_count++;
    }
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
    force_linear_filter(target);
}

void glClearColorx_soloader(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha) {
    static int log_count = 0;
    if (log_count < 10) {
        l_debug("[GL] glClearColorx r=%.3f g=%.3f b=%.3f a=%.3f",
                red / 65536.0f, green / 65536.0f, blue / 65536.0f, alpha / 65536.0f);
        log_count++;
    }
    glClearColor(red / 65536.0f, green / 65536.0f, blue / 65536.0f, alpha / 65536.0f);
}

void glTexParameterx_soloader(GLenum target, GLenum pname, GLfixed param) {
    glTexParameteri(target, pname, param);
}

// Corrida 25: log transitions of the capabilities that matter most for
// "does anything show up" (GL_TEXTURE_2D first and foremost -- Corrida 24
// found it consistently disabled at every one of the first 20 draw calls).
static int is_interesting_cap(GLenum cap) {
    return cap == GL_TEXTURE_2D || cap == GL_LIGHTING || cap == GL_BLEND
           || cap == GL_ALPHA_TEST || cap == GL_CULL_FACE || cap == GL_DEPTH_TEST;
}

void glEnable_soloader(GLenum cap) {
    static int log_count = 0;
    if (is_interesting_cap(cap) && log_count < 40) {
        l_debug("[GL] glEnable(%x)", cap);
        log_count++;
    }
    glEnable(cap);
}

void glDisable_soloader(GLenum cap) {
    static int log_count = 0;
    if (is_interesting_cap(cap) && log_count < 40) {
        l_debug("[GL] glDisable(%x)", cap);
        log_count++;
    }
    glDisable(cap);
}

// Corrida 79: see the header comment on this pair (glutil.h) for why
// SGL_grpFillRect_hook (source/patch.c) needs this.
GLboolean gl_color_array_enabled(void) {
    return glIsEnabled(GL_COLOR_ARRAY);
}

void gl_set_color_array_enabled(GLboolean enabled) {
    if (enabled) {
        glEnableClientState(GL_COLOR_ARRAY);
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
    }
}

// Corrida 88: see glutil.h's comment on these four functions.
GLboolean gl_texcoord_array_enabled(void) {
    return glIsEnabled(GL_TEXTURE_COORD_ARRAY);
}

void gl_set_texcoord_array_enabled(GLboolean enabled) {
    if (enabled) {
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    } else {
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
}

GLint gl_get_bound_texture_2d(void) {
    GLint tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    return tex;
}

void gl_bind_texture_2d(GLint texture) {
    glBindTexture(GL_TEXTURE_2D, (GLuint) texture);
}

// Corrida 89: see glutil.h's comment on these.
GLboolean gl_depth_test_enabled(void) {
    return glIsEnabled(GL_DEPTH_TEST);
}

void gl_set_depth_test_enabled(GLboolean enabled) {
    if (enabled) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
}

// Corrida 89: sibling investigation to Corrida 24-27's log_draw_call below --
// that machinery already dumps everything a "draw call happened but nothing
// visible" bug needs (viewport/scissor, projection/modelview scale+translate,
// actual post-GL_FIXED-conversion vertex positions, every relevant enable
// state including GL_DEPTH_TEST) but its own 400-call cap is exhausted long
// before a real play session reaches the specific UI draws under
// investigation now (SGL_grpFillRect's own glDrawArrays). Rather than
// duplicating that machinery a second time in patch.c, let SGL_grpFillRect_hook
// force ONE full dump through the EXISTING log_draw_call, bypassing its cap,
// for exactly the draw it's about to make.
static int g_force_log_next_draw = 0;

void gl_force_log_next_draw(void) {
    g_force_log_next_draw = 1;
}

void glAlphaFuncx_soloader(GLenum func, GLfixed ref) {
    glAlphaFunc(func, ref / 65536.0f);
}

void glClearDepthx_soloader(GLfixed depth) {
    glClearDepthf(depth / 65536.0f);
}

void glClipPlanex_soloader(GLenum plane, const GLfixed *equation) {
    GLfloat eq[4] = {equation[0] / 65536.0f, equation[1] / 65536.0f, equation[2] / 65536.0f, equation[3] / 65536.0f};
    glClipPlanef(plane, eq);
}

void glColor4x_soloader(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha) {
    static int c_count = 0;
    if(c_count++ < 10) l_debug("[GL] glColor4x r=%.2f g=%.2f b=%.2f a=%.2f", red/65536.0f, green/65536.0f, blue/65536.0f, alpha/65536.0f);
    glColor4f(red / 65536.0f, green / 65536.0f, blue / 65536.0f, alpha / 65536.0f);
}

void glDepthRangex_soloader(GLfixed zNear, GLfixed zFar) {
    glDepthRangef(zNear / 65536.0f, zFar / 65536.0f);
}

void glFogx_soloader(GLenum pname, GLfixed param) {
    glFogf(pname, param / 65536.0f);
}

void glFrustumx_soloader(GLfixed left, GLfixed right, GLfixed bottom, GLfixed top, GLfixed zNear, GLfixed zFar) {
    glFrustumf(left / 65536.0f, right / 65536.0f, bottom / 65536.0f, top / 65536.0f, zNear / 65536.0f, zFar / 65536.0f);
}

void glLineWidthx_soloader(GLfixed width) {
    glLineWidth(width / 65536.0f);
}

// Corrida 27: none of the matrix-stack entry points (glLoadMatrixx/
// glMultMatrixx/glTranslatex, nor the float glLoadMatrixf/glTranslatef/
// glPushMatrix/glPopMatrix mapped straight to real GL in dynlib.c) were
// logged at all, so there was no way to tell whether the .so ever
// repositions the modelview per quad, or genuinely leaves it at identity
// and bakes absolute coordinates into vertex data instead. If this stays
// silent while glDrawArrays keeps firing, the .so isn't touching the
// modelview around these draws.
void glLoadMatrixx_soloader(const GLfixed *m) {
    static int log_count = 0;
    GLfloat mf[16];
    for (int i = 0; i < 16; i++) mf[i] = m[i] / 65536.0f;
    if (log_count < 20) {
        l_debug("[GL] glLoadMatrixx sx=%.3f sy=%.3f tx=%.3f ty=%.3f", mf[0], mf[5], mf[12], mf[13]);
        log_count++;
    }
    glLoadMatrixf(mf);
}

void glMaterialx_soloader(GLenum face, GLenum pname, GLfixed param) {
    glMaterialf(face, pname, param / 65536.0f);
}

void glMultMatrixx_soloader(const GLfixed *m) {
    static int log_count = 0;
    GLfloat mf[16];
    for (int i = 0; i < 16; i++) mf[i] = m[i] / 65536.0f;
    if (log_count < 20) {
        l_debug("[GL] glMultMatrixx sx=%.3f sy=%.3f tx=%.3f ty=%.3f", mf[0], mf[5], mf[12], mf[13]);
        log_count++;
    }
    glMultMatrixf(mf);
}

void glOrthox_soloader(GLfixed left, GLfixed right, GLfixed bottom, GLfixed top, GLfixed zNear, GLfixed zFar) {
    l_debug("[GL] glOrthox l=%.1f r=%.1f b=%.1f t=%.1f", left/65536.0f, right/65536.0f, bottom/65536.0f, top/65536.0f);
    glOrthof(left / 65536.0f, right / 65536.0f, bottom / 65536.0f, top / 65536.0f, zNear / 65536.0f, zFar / 65536.0f);
}

void glPointSizex_soloader(GLfixed size) {
    glPointSize(size / 65536.0f);
}

void glPolygonOffsetx_soloader(GLfixed factor, GLfixed units) {
    glPolygonOffset(factor / 65536.0f, units / 65536.0f);
}

void glRotatex_soloader(GLfixed angle, GLfixed x, GLfixed y, GLfixed z) {
    glRotatef(angle / 65536.0f, x / 65536.0f, y / 65536.0f, z / 65536.0f);
}

void glScalex_soloader(GLfixed x, GLfixed y, GLfixed z) {
    glScalef(x / 65536.0f, y / 65536.0f, z / 65536.0f);
}

void glTexEnvx_soloader(GLenum target, GLenum pname, GLfixed param) {
    glTexEnvf(target, pname, param / 65536.0f);
}

void glTranslatex_soloader(GLfixed x, GLfixed y, GLfixed z) {
    static int log_count = 0;
    if (log_count < 20) {
        l_debug("[GL] glTranslatex x=%.1f y=%.1f z=%.1f", x / 65536.0f, y / 65536.0f, z / 65536.0f);
        log_count++;
    }
    glTranslatef(x / 65536.0f, y / 65536.0f, z / 65536.0f);
}

// Deferred GL_FIXED -> GL_FLOAT conversion state for the vertex/color/
// texcoord arrays (see glutil.h comment). Populated by the *Pointer_soloader
// setters below, consumed and cleared by glDrawArrays_soloader/
// glDrawElements_soloader once the real vertex count is known.
static const int32_t *pending_fixed_verts = NULL;
static GLint pending_fixed_vert_size = 0;
static GLsizei pending_fixed_vert_stride = 0;
static GLfloat *fixed_vert_buf = NULL;
static int fixed_vert_buf_cap = 0;

static const int32_t *pending_fixed_colors = NULL;
static GLint pending_fixed_color_size = 0;
static GLsizei pending_fixed_color_stride = 0;
static GLfloat *fixed_color_buf = NULL;
static int fixed_color_buf_cap = 0;

static const int32_t *pending_fixed_texcoords = NULL;
static GLint pending_fixed_texcoord_size = 0;
static GLsizei pending_fixed_texcoord_stride = 0;
static GLfloat *fixed_texcoord_buf = NULL;
static int fixed_texcoord_buf_cap = 0;

void glVertexPointer_soloader(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
    if (type == GL_FIXED) {
        pending_fixed_verts = (const int32_t *) pointer;
        pending_fixed_vert_size = size;
        pending_fixed_vert_stride = stride;
        return;
    }
    pending_fixed_verts = NULL;
    glVertexPointer(size, type, stride, pointer);
}

void glColorPointer_soloader(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
    if (type == GL_FIXED) {
        pending_fixed_colors = (const int32_t *) pointer;
        pending_fixed_color_size = size;
        pending_fixed_color_stride = stride;
        return;
    }
    pending_fixed_colors = NULL;
    glColorPointer(size, type, stride, pointer);
}

void glTexCoordPointer_soloader(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
    if (type == GL_FIXED) {
        pending_fixed_texcoords = (const int32_t *) pointer;
        pending_fixed_texcoord_size = size;
        pending_fixed_texcoord_stride = stride;
        return;
    }
    pending_fixed_texcoords = NULL;
    glTexCoordPointer(size, type, stride, pointer);
}

static void convert_fixed_array(const int32_t *src, GLint size, GLsizei stride,
                                 int needed_verts, GLfloat **buf, int *buf_cap) {
    int needed_floats = needed_verts * size;
    if (needed_floats > *buf_cap) {
        *buf = (GLfloat *) realloc(*buf, needed_floats * sizeof(GLfloat));
        *buf_cap = needed_floats;
    }
    int stride_elems = stride > 0 ? stride / (int) sizeof(int32_t) : size;
    for (int i = 0; i < needed_verts; i++) {
        const int32_t *s = src + i * stride_elems;
        GLfloat *d = *buf + i * size;
        for (int c = 0; c < size; c++) {
            d[c] = s[c] / 65536.0f;
        }
    }
}

// Shared by glDrawArrays_soloader/glDrawElements_soloader: converts and
// re-binds whichever of the three arrays are currently pending GL_FIXED
// data, covering vertex indices [0, needed_verts).
static void flush_pending_fixed_arrays(int needed_verts) {
    if (pending_fixed_verts) {
        convert_fixed_array(pending_fixed_verts, pending_fixed_vert_size, pending_fixed_vert_stride,
                             needed_verts, &fixed_vert_buf, &fixed_vert_buf_cap);
        glVertexPointer(pending_fixed_vert_size, GL_FLOAT, 0, fixed_vert_buf);
    }
    if (pending_fixed_colors) {
        convert_fixed_array(pending_fixed_colors, pending_fixed_color_size, pending_fixed_color_stride,
                             needed_verts, &fixed_color_buf, &fixed_color_buf_cap);
        glColorPointer(pending_fixed_color_size, GL_FLOAT, 0, fixed_color_buf);
    }
    if (pending_fixed_texcoords) {
        convert_fixed_array(pending_fixed_texcoords, pending_fixed_texcoord_size, pending_fixed_texcoord_stride,
                             needed_verts, &fixed_texcoord_buf, &fixed_texcoord_buf_cap);
        glTexCoordPointer(pending_fixed_texcoord_size, GL_FLOAT, 0, fixed_texcoord_buf);
    }
}

// Corrida 22 (PORTING_PLAN.md Fase 9): the game reaches the render loop and
// loads textures fine (MainMenu.png/font.fnt), but the screen stays black --
// and nothing in this file logs whether the .so ever actually issues a draw
// call at all. Log the first few so a fresh device log can tell "no draw
// calls happening" apart from "draw calls happening but drawing nothing
// visible" (wrong blend/depth state, wrong viewport, transparent/black
// texture data, etc.) -- two very different next steps.
// Corrida 23: the earlier bound_tex/blend/depth dump (still below) already
// showed real draw calls with valid, nonzero bound textures -- so the
// remaining suspects are the projection/modelview matrices (is the geometry
// even inside the view frustum?) and viewport/scissor clipping. Dump both
// matrices' scale+translate terms and the viewport/scissor state, plus the
// first two vertices actually submitted (post GL_FIXED->float conversion,
// if this draw used GL_FIXED vertex data) so a single fresh log settles
// whether geometry is landing anywhere near the visible 0..960 x 0..544
// pixel range.
static void log_draw_call(const char *fn, GLenum mode, GLsizei count, const GLfloat *base_verts, int vert_size,
                           const GLfloat *base_color, int color_size, int first) {
    // Corrida 25: raised from 20 -> 400 -- Corrida 24's cap of 20 only
    // covered the very first draws of the loading screen; the actual
    // menu/background draws (the ones that matter for "is MainMenu.png
    // visible") happen later in the same run and were never reaching the log.
    static int log_count = 0;
    // Corrida 89: a forced dump (see gl_force_log_next_draw) bypasses the cap
    // and doesn't consume it -- it's investigating a specific, rare draw
    // (SGL_grpFillRect), not the high-frequency ones this cap was sized for.
    int forced = g_force_log_next_draw;
    if (forced) g_force_log_next_draw = 0;
    if (log_count < 400 || forced) {
        GLint tex = 0;
        GLint viewport[4] = {0, 0, 0, 0};
        GLint scissor_box[4] = {0, 0, 0, 0};
        GLfloat proj[16], modelview[16];
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissor_box);
        glGetFloatv(GL_PROJECTION_MATRIX, proj);
        glGetFloatv(GL_MODELVIEW_MATRIX, modelview);

        // Corrida 26: GL_TEXTURE_2D/GL_BLEND/GL_DEPTH_TEST/GL_LIGHTING/
        // GL_CULL_FACE/GL_COLOR_ARRAY are enable-capability enums -- the
        // portable way to query them is glIsEnabled(), not glGetIntegerv()
        // with the cap as pname. The prior glGetIntegerv() calls read back
        // 0 on every single one of 400 draws regardless of the glEnable()
        // calls seen moments earlier in the same trace, which means vitaGL
        // wasn't honoring that query form -- those readings were bogus, not
        // evidence that texturing/blend were actually off.
        GLboolean blend = glIsEnabled(GL_BLEND);
        GLboolean depth_test = glIsEnabled(GL_DEPTH_TEST);
        GLboolean scissor_test = glIsEnabled(GL_SCISSOR_TEST);
        GLboolean lighting = glIsEnabled(GL_LIGHTING);
        GLboolean texture_2d = glIsEnabled(GL_TEXTURE_2D);
        GLboolean cull_face = glIsEnabled(GL_CULL_FACE);
        GLboolean color_array = glIsEnabled(GL_COLOR_ARRAY);

        // Corrida 27: fixing the index-0 bug (Corrida 26) didn't change
        // anything -- v0/c0 are STILL (0,0)/(0,0,0,0) on every single one of
        // 384 draws across different textures, meaning `first` really is 0
        // every time (each draw rebinds its own small local quad, not slices
        // of one shared buffer). That alone isn't proof of a bug -- a local
        // unit-quad template positioned via the modelview matrix is a normal
        // pattern -- except mv is ALSO identity on every draw. Dump every
        // vertex of the quad (not just #0) plus its screen-space bounding
        // box so a fresh log shows whether the quad has any nonzero
        // footprint/position at all, instead of inferring it from one corner.
        char verts_buf[192] = "verts=n/a";
        if (base_verts && vert_size > 0) {
            float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
            char tmp[192];
            int off = snprintf(tmp, sizeof(tmp), "verts=[");
            for (int i = 0; i < count && i < 4; i++) {
                const GLfloat *v = base_verts + (size_t) (first + i) * vert_size;
                float x = v[0], y = vert_size > 1 ? v[1] : 0.0f;
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
                off += snprintf(tmp + off, sizeof(tmp) - off, "(%.1f,%.1f)%s", x, y, i + 1 < count && i + 1 < 4 ? "," : "");
            }
            snprintf(tmp + off, sizeof(tmp) - off, "] bbox=(%.1f,%.1f)-(%.1f,%.1f)", minx, miny, maxx, maxy);
            snprintf(verts_buf, sizeof(verts_buf), "%s", tmp);
        }
        char color_buf[64] = "c=n/a";
        if (base_color && color_size > 0) {
            const GLfloat *c0 = base_color + (size_t) first * color_size;
            snprintf(color_buf, sizeof(color_buf), "c0=(%.2f,%.2f,%.2f,%.2f)",
                     c0[0], color_size > 1 ? c0[1] : 0.0f,
                     color_size > 2 ? c0[2] : 0.0f, color_size > 3 ? c0[3] : 1.0f);
        }

        // Corrida 89: forced dumps use l_error (always compiles, per Corrida
        // 73's DEBUG_SOLOADER/Release finding) since this diagnostic must
        // survive a Release retest too; the routine budgeted l_debug calls
        // below are unchanged (this file's original, high-frequency use).
        if (forced) {
            l_error("[GL] Corrida 89 FORCED: %s mode=%x count=%d first=%d bound_tex=%d blend=%d "
                    "depth_test=%d lighting=%d texture_2d=%d cull_face=%d color_array=%d "
                    "scissor=%d(%d,%d,%d,%d) viewport=(%d,%d,%d,%d) "
                    "proj(sx=%.3f sy=%.3f tx=%.3f ty=%.3f) mv(sx=%.3f sy=%.3f tx=%.3f ty=%.3f) %s %s",
                    fn, mode, count, first, tex, blend, depth_test, lighting, texture_2d, cull_face, color_array,
                    scissor_test, scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3],
                    viewport[0], viewport[1], viewport[2], viewport[3],
                    proj[0], proj[5], proj[12], proj[13],
                    modelview[0], modelview[5], modelview[12], modelview[13],
                    verts_buf, color_buf);
        } else {
            l_debug("[GL] %s mode=%x count=%d first=%d bound_tex=%d blend=%d depth_test=%d lighting=%d "
                    "texture_2d=%d cull_face=%d color_array=%d "
                    "scissor=%d(%d,%d,%d,%d) viewport=(%d,%d,%d,%d) "
                    "proj(sx=%.3f sy=%.3f tx=%.3f ty=%.3f) mv(sx=%.3f sy=%.3f tx=%.3f ty=%.3f) %s %s",
                    fn, mode, count, first, tex, blend, depth_test, lighting, texture_2d, cull_face, color_array,
                    scissor_test, scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3],
                    viewport[0], viewport[1], viewport[2], viewport[3],
                    proj[0], proj[5], proj[12], proj[13],
                    modelview[0], modelview[5], modelview[12], modelview[13],
                    verts_buf, color_buf);
            log_count++;
        }
    }
}

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count) {
    int had_fixed_verts = pending_fixed_verts != NULL;
    int had_fixed_colors = pending_fixed_colors != NULL;
    GLint vert_size = pending_fixed_vert_size;
    GLint color_size = pending_fixed_color_size;
    flush_pending_fixed_arrays(first + count);
    log_draw_call("glDrawArrays", mode, count,
                   had_fixed_verts ? fixed_vert_buf : NULL, had_fixed_verts ? vert_size : 0,
                   had_fixed_colors ? fixed_color_buf : NULL, had_fixed_colors ? color_size : 0, first);
    glDrawArrays(mode, first, count);
}

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
    int had_fixed_verts = pending_fixed_verts != NULL;
    int had_fixed_colors = pending_fixed_colors != NULL;
    GLint vert_size = pending_fixed_vert_size;
    GLint color_size = pending_fixed_color_size;
    if (pending_fixed_verts || pending_fixed_colors || pending_fixed_texcoords) {
        int max_index = 0;
        if (type == GL_UNSIGNED_BYTE) {
            const uint8_t *idx = (const uint8_t *) indices;
            for (int i = 0; i < count; i++) if (idx[i] > max_index) max_index = idx[i];
        } else {
            const uint16_t *idx = (const uint16_t *) indices;
            for (int i = 0; i < count; i++) if (idx[i] > max_index) max_index = idx[i];
        }
        flush_pending_fixed_arrays(max_index + 1);
    }
    int first_index = 0;
    if (count > 0) {
        first_index = (type == GL_UNSIGNED_BYTE) ? ((const uint8_t *) indices)[0] : ((const uint16_t *) indices)[0];
    }
    log_draw_call("glDrawElements", mode, count,
                   had_fixed_verts ? fixed_vert_buf : NULL, had_fixed_verts ? vert_size : 0,
                   had_fixed_colors ? fixed_color_buf : NULL, had_fixed_colors ? color_size : 0, first_index);
    glDrawElements(mode, count, type, indices);
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glShaderSource<%p>(shader: %i, count: %i, string: %p, length: %p)\n", __builtin_return_address(0), shader, count, string, _length);
#endif
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            total_length += strlen(string[i]);
        } else {
            total_length += _length[i];
        }
    }

    char * str = malloc(total_length+1);
    size_t l = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            memcpy(str + l, string[i], strlen(string[i]));
            l += strlen(string[i]);
        } else {
            memcpy(str + l, string[i], _length[i]);
            l += _length[i];
        }
    }
    str[total_length] = '\0';

    load_shader(shader, str, total_length);

    free(str);
}

void glCompileShader_soloader(GLuint shader) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glCompileShader<%p>(shader: %i)\n", __builtin_return_address(0), shader);
#endif

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        glCompileShader(shader);
#ifdef DUMP_COMPILED_SHADERS
        void *bin = vglMalloc(32 * 1024);
        GLsizei len;
        vglGetShaderBinary(shader, 32 * 1024, &len, bin);
        file_save(next_shader_fname, bin, len);
        vglFree(bin);
#endif
    }
    skip_next_compile = GL_FALSE;
#endif
}

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else {
        glShaderSource(shader, 1, &string, &length);
        strcpy(next_shader_fname, gxp_path);
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    glShaderSource(shader, 1, &string, &length);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    char cg_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);
    snprintf(cg_path, sizeof(cg_path), DATA_PATH"cg/%s.cg", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else if (file_exists(cg_path)) {
        char *buffer;
        size_t size;

        file_load(cg_path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);
        strcpy(next_shader_fname, gxp_path);

        free(buffer);
        skip_next_compile = GL_FALSE;
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }

        skip_next_compile = GL_FALSE;
    }

    free(sha_name);
}
#elif defined(USE_CG_SHADERS) || defined(USE_GXP_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char path[256];
#ifdef USE_CG_SHADERS
    snprintf(path, sizeof(path), DATA_PATH"cg/%s.cg", sha_name);
#else
    snprintf(path, sizeof(path), DATA_PATH"gxp/%s.gxp", sha_name);
#endif

    if (file_exists(path)) {
#ifdef USE_CG_SHADERS
        char *buffer;
        size_t size;

        file_load(path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);

        free(buffer);
#else
        uint8_t *buffer;
        size_t size;

        file_load(path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
#endif
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#else
#error "Define one of (USE_GLSL_SHADERS, USE_CG_SHADERS, USE_GXP_SHADERS)"
#endif
