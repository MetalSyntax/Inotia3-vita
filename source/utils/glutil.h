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

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
