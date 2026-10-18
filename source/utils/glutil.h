/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  glutil.h
 * @brief OpenGL API initializer, related functions.
 */

#ifndef SOLOADER_GLUTIL_H
#define SOLOADER_GLUTIL_H

#include <vitaGL.h>

#ifdef __cplusplus
extern "C" {
#endif

void gl_init();

void gl_preload();

void gl_swap();

void glCompileShader_soloader(GLuint shader);

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length);

// Fase 4 (Graficos): el .so calcula su propio glViewport a partir de una
// resolucion de diseno fija (480x320, ver PORTING_PLAN.md) combinada con el
// tamano de pantalla que le reportamos por JNI -- el resultado no cubre
// necesariamente los 960x544 reales del panel de Vita. Forzar el viewport
// real a pantalla completa (mismo fix validado en Zenonia3-vita/loader/
// dynlib.c para el mismo sintoma: "cuadro chico" con borde sin limpiar)
// deja que el hardware escale la misma proyeccion/quad del juego para
// llenar el panel, sin tocar glOrtho ni los vertices del juego.
void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height);

// Fase 4: fuerza GL_LINEAR en min/mag filter de cada textura subida --
// evita que vitaGL trate una textura sin mipmaps como "incompleta" (se
// renderiza blanco puro), mismo fix validado en Zenonia2-vita/Zenonia3-vita.
void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, const void *pixels);
void glTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                              GLsizei width, GLsizei height, GLenum format,
                              GLenum type, const void *pixels);

// Fase 4: vitaGL no consume GL_FIXED correctamente en varios puntos de su
// API GLES1 (confirmado en consola real por Zenonia3-vita/loader/dynlib.c,
// mismo toolchain/version de vitaGL) -- lee los enteros Q16.16 crudos como
// si fueran floats. Inotia 3 importa el pipeline fijo completo
// (glVertexPointer/glColorPointer/glTexCoordPointer con GL_FIXED,
// glClearColorx, glTexParameterx -- ver PORTING_PLAN.md seccion 0), asi que
// hereda el mismo riesgo. glClearColorx/glTexParameterx_soloader convierten
// directo; glVertexPointer/glColorPointer/glTexCoordPointer_soloader
// difieren la conversion Q16.16->float hasta glDrawArrays/glDrawElements
// (ahi se sabe cuantos vertices hay realmente).
void glClearColorx_soloader(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha);
void glTexParameterx_soloader(GLenum target, GLenum pname, GLfixed param);
void glVertexPointer_soloader(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void glColorPointer_soloader(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void glTexCoordPointer_soloader(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count);
void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);

void glAlphaFuncx_soloader(GLenum func, GLfixed ref);
void glClearDepthx_soloader(GLfixed depth);
void glClipPlanex_soloader(GLenum plane, const GLfixed *equation);
void glColor4x_soloader(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha);
// Scales the alpha of every .so glColor4x (and re-applies the last color now).
void gl_set_alpha_mul(float mul);
void glDepthRangex_soloader(GLfixed zNear, GLfixed zFar);
void glFogx_soloader(GLenum pname, GLfixed param);
void glFrustumx_soloader(GLfixed left, GLfixed right, GLfixed bottom, GLfixed top, GLfixed zNear, GLfixed zFar);
void glLineWidthx_soloader(GLfixed width);
void glLoadMatrixx_soloader(const GLfixed *m);
void glMaterialx_soloader(GLenum face, GLenum pname, GLfixed param);
void glMultMatrixx_soloader(const GLfixed *m);
void glOrthox_soloader(GLfixed left, GLfixed right, GLfixed bottom, GLfixed top, GLfixed zNear, GLfixed zFar);
void glPointSizex_soloader(GLfixed size);
void glPolygonOffsetx_soloader(GLfixed factor, GLfixed units);
void glRotatex_soloader(GLfixed angle, GLfixed x, GLfixed y, GLfixed z);
void glScalex_soloader(GLfixed x, GLfixed y, GLfixed z);
void glTexEnvx_soloader(GLenum target, GLenum pname, GLfixed param);
void glTranslatex_soloader(GLfixed x, GLfixed y, GLfixed z);

// Corrida 25: log every enable/disable of a short list of capabilities that
// directly affect whether a draw is visible (GL_TEXTURE_2D above all --
// Corrida 24 showed it consistently 0 at draw time) instead of only sampling
// state passively at draw time, so a transition (or the total absence of
// one) is visible in the log.
void glEnable_soloader(GLenum cap);
void glDisable_soloader(GLenum cap);

// Corrida 79: SGL_grpFillRect (source/patch.c's SGL_grpFillRect_hook) sets
// the flat draw color via glColor4x and draws a quad, but never touches
// GL_COLOR_ARRAY itself either way -- if a preceding textured/tinted sprite
// draw left it enabled, the flat color is ignored and the quad's alpha comes
// from whatever per-vertex color data happens to still be bound, instead of
// the intended translucent black. Exposed here (rather than pulling
// vitaGL.h into patch.c for one narrow use) so that hook can save/restore
// this one piece of client-array state around its real draw call.
GLboolean gl_color_array_enabled(void);
void gl_set_color_array_enabled(GLboolean enabled);

// Corrida 88: SGL_grpFillRect (source/patch.c's SGL_grpFillRect_hook) is the
// ONE draw call pattern in this whole missing-background investigation that
// never sets up its own GL_TEXTURE_COORD_ARRAY/glBindTexture state at all --
// every textured draw (sprites, portraits, icons -- all confirmed rendering
// correctly) calls glTexCoordPointer/glBindTexture itself every time, right
// before its own glDrawArrays. SGL_grpFillRect only calls glDisable(
// GL_TEXTURE_2D)/glEnable(GL_TEXTURE_2D) around its flat-color quad and
// relies on that alone to skip texturing -- if GL_TEXTURE_COORD_ARRAY client
// state and the last-bound texture are both still leftover from whatever
// textured draw ran immediately before (a real possibility: this port's own
// GL_FIXED->float deferred-conversion state, see glVertexPointer_soloader's
// own pending_fixed_* statics, persists across draws by design and is never
// touched by SGL_grpFillRect either), and if this vitaGL build's
// GL_TEXTURE_2D disable doesn't fully bypass sampling in its fixed-function
// emulation path (unconfirmed without vitaGL's own source, not vendored in
// this repo -- but consistent with this project's own established history of
// vitaGL not honoring several other enable-state/GL_FIXED assumptions taken
// for granted on desktop GL, see Corrida 24-27's glGetIntegerv() findings
// above and the whole Fase 4 GL_FIXED saga in PORTING_PLAN.md), the fragment
// could still sample a stale bound texture through stale leftover texcoords
// and multiply it into the intended flat color -- plausibly zeroing out a
// translucent black/white fill exactly as reported. Exposed here so
// SGL_grpFillRect_hook can save/restore this client-array state and force an
// explicit unbind around its real draw call, the same defensive pattern
// gl_color_array_enabled() above already established for GL_COLOR_ARRAY.
GLboolean gl_texcoord_array_enabled(void);
void gl_set_texcoord_array_enabled(GLboolean enabled);
GLint gl_get_bound_texture_2d(void);
void gl_bind_texture_2d(GLint texture);

// Corrida 89: same save/force/restore pattern, for GL_DEPTH_TEST -- the one
// enable-capability SGL_grpFillRect's own decompile never touches (it only
// manages GL_TEXTURE_2D around its flat quad), unlike every OTHER capability
// this file's Corrida 24-27 investigation already found reasons to distrust
// vitaGL's default handling of. If depth testing is active during a UI
// overlay draw and the popup's implicit Z fails against whatever's already
// in the depth buffer there, the fragment is discarded before it ever
// reaches blending -- invisible regardless of how correct every other piece
// of state (color, alpha, clip, texture) already confirmed to be.
GLboolean gl_depth_test_enabled(void);
void gl_set_depth_test_enabled(GLboolean enabled);

// Corrida 93: Solid 1x1 white texture for untextured primitive draws in vitaGL
GLuint gl_get_white_texture(void);

// Corrida 89: forces exactly ONE full diagnostic dump through this file's
// existing log_draw_call machinery (viewport/scissor, projection/modelview
// scale+translate, actual post-GL_FIXED-conversion vertex positions, every
// relevant enable state) on the very next glDrawArrays/glDrawElements,
// bypassing that machinery's own 400-call budget (long exhausted by the time
// a real play session reaches the specific draw under investigation).
// Call immediately before the draw you want dumped; the flag clears itself
// after that one draw fires.
void gl_force_log_next_draw(void);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
