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
    vglInitExtended(0, 960, 544, 6 * 1024 * 1024, SCE_GXM_MULTISAMPLE_4X);
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

void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, const void *pixels) {
    static int log_count = 0;
    if (log_count < 10) {
        l_debug("[GL] glTexImage2D level=%d intFmt=%x w=%d h=%d format=%x type=%x",
                level, internalformat, width, height, format, type);
        log_count++;
    }
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    force_linear_filter(target);
}

void glTexSubImage2D_soloader(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                              GLsizei width, GLsizei height, GLenum format,
                              GLenum type, const void *pixels) {
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
    force_linear_filter(target);
}

void glClearColorx_soloader(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha) {
    glClearColor(red / 65536.0f, green / 65536.0f, blue / 65536.0f, alpha / 65536.0f);
}

void glTexParameterx_soloader(GLenum target, GLenum pname, GLfixed param) {
    glTexParameteri(target, pname, param);
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

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count) {
    flush_pending_fixed_arrays(first + count);
    glDrawArrays(mode, first, count);
}

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
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
