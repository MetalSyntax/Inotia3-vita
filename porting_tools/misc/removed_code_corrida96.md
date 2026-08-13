# Código removido tras Corrida 96 (limpieza para aislar el fix de fondos negros transparentes)

Contexto: el fix real y confirmado para los fondos/paneles/tooltips translúcidos
invisibles es la reescritura de `SGL_grpFillRect_hook` (Corrida 92-95, `source/patch.c`)
más la textura blanca 1x1 `gl_get_white_texture()` (`source/utils/glutil.c/.h`). Todo lo
que sigue se agregó junto con ese fix en el mismo diff sin commitear, pero **no es
necesario para arreglar los fondos** -- se removió para descartarlo como causa del
glitch visual al aparecer el nombre del stage y del bug de movimiento cerca de NPCs en
el pueblo (reportados en `logs/log_029.txt`).

Si tras esta limpieza el glitch del banner de stage o el bug de movimiento
**desaparecen**, el código de abajo era la causa (probablemente la sección 2,
`SGL_grpDrawRect_hook`, nunca antes hookeada). Si **persisten**, no eran la causa y el
problema sigue estando en `SGL_grpFillRect_hook` o en otro lado.

## 1. `source/main.c` -- filtro de logs `[JNI]` (Corrida 94)

No tiene relación con el fix de fondos -- era una optimización de I/O/FPS para dejar de
escribir a disco los logs verbosos con prefijo `[JNI]`.

```c
#include <string.h>
...
void game_log(const char *fmt, ...) {
    // Corrida 94: Filter out verbose JNI logs to eliminate disk I/O overhead and FPS drops
    if (fmt && strncmp(fmt, "[JNI]", 5) == 0) return;
    char buf[512];
    ...
```

## 2. `source/main.c` -- creación temprana (eager) de la textura blanca (Corrida 96)

Intento mío (turno anterior) de mitigar un posible freeze creando la textura blanca al
boot en vez de en el primer draw call. No es necesario para el fix de fondos en sí (la
textura ya se crea de forma perezosa dentro de `gl_get_white_texture()`); se agregó
para investigar un bug de freeze distinto (congelamiento al cargar partida), no
confirmado con evidencia de logs. Se remueve para reducir el diff a lo mínimo.

```c
    // Corrida 96: gl_get_white_texture() (glutil.c) used to be created lazily
    // on its first caller's first invocation -- SGL_grpFillRect_hook/
    // SGL_grpDrawRect_hook (patch.c), which fire mid-frame the first time a
    // translucent panel/banner/tooltip is drawn (exactly what happens right
    // when a saved game loads or a stage transition starts). Allocating a GL
    // texture (glGenTextures+glTexImage2D) at an unpredictable point inside
    // the hot render path is a known hang/stall risk on vitaGL. so_patch()
    // (utils/init.c) runs before this point, so the GL context didn't exist
    // yet to do this any earlier -- this is the earliest safe spot, right
    // after vglInitExtended() and well before nativeInit()/the game loop
    // ever issues a real draw call.
    gl_get_white_texture();
```

## 3. `source/patch.c` -- diagnóstico de `MainProcess_hook` (Corrida 45/46) + cap subido (Corrida 96)

No tiene relación con el fix de fondos -- es instrumentación para OTRO bug (freeze con
`INITSTATE_nState` atascado). Ya estaba en el HEAD commiteado con cap=200; el diff sin
commitear lo había borrado, y yo lo reinstalé el turno pasado subiendo el cap a 6000.
Se revierte todo el bloque a exactamente lo que hay en HEAD (cap=200, sin el comentario
Corrida 96) para no mezclar instrumentación de un bug distinto con el fix de fondos.

```c
// Corrida 96: MainProcess ticks every 100ms (Corrida 32) -- the old cap of
// 200 (~20s) is exhausted long before a real test session reaches a save
// load or stage change deep into gameplay, same blind spot Corrida 83 found
// and fixed for FILLRECT_LOG_MAX by raising it, not by making the cap
// smarter. Raised to cover a full multi-minute test session instead.
#define MAINPROCESS_LOG_MAX 6000
```
(revertido a `#define MAINPROCESS_LOG_MAX 200`, igual que HEAD)

```c
static void MainProcess_hook(uint32_t param_1, uint32_t param_2) {
    // Corrida 96: this diagnostic block was dropped while chasing the
    // SGL_grpFillRect/DrawRect background-visibility bug (Corrida 92-95) --
    // but it's exactly the tool Corrida 45/46 used to confirm the LAST time
    // this project saw "render keeps ticking, game logic doesn't": a stuck
    // INITSTATE_nState. Reinstated (same 200-call cap as before) so the next
    // save-load/stage-change freeze report comes with real INITSTATE_nState/
    // MEM_pool numbers instead of just the glOrthox spam this bug currently
    // leaves in the log.
    if (g_mainprocess_log_count < MAINPROCESS_LOG_MAX) {
    ...
```
(el cuerpo del `if` en sí queda igual que HEAD, solo se remueve el comentario Corrida 96
agregado por mí)

## 4. `source/patch.c` -- `SGL_grpDrawRect_hook` completo (nunca antes hookeado)

Este es el candidato más fuerte para el glitch del banner de nombre de stage y el bug
de movimiento cerca de NPCs: `SGL_grpDrawRect` (dibuja el CONTORNO/borde de un
rectángulo, no el relleno) nunca había sido hookeado en este proyecto -- corría el
código nativo original sin modificar. El fix de fondos negros transparentes es
enteramente sobre `SGL_grpFillRect` (el relleno); `DrawRect` se agregó "de yapa" en el
mismo diff sin evidencia de que estuviera roto. Se remueve el hook completo (la función
nativa vuelve a ejecutarse sin modificar).

```c
static so_hook h_SGL_grpDrawRect;   // (declaración, también removida)
```

```c
static void SGL_grpDrawRect_hook(int x, int y, int w, int h, uint32_t color) {
    if (g_ptr_SGL_CLIP && ((int32_t)g_ptr_SGL_CLIP[0] >= 0) && ((int32_t)g_ptr_SGL_CLIP[1] >= 0)) {
        int cx1 = (int32_t)g_ptr_SGL_CLIP[0];
        int cy1 = (int32_t)g_ptr_SGL_CLIP[1];
        int cx2 = cx1 + (int32_t)g_ptr_SGL_CLIP[2];
        int cy2 = cy1 + (int32_t)g_ptr_SGL_CLIP[3];
        int x2 = x + w;
        int y2 = y + h;

        if (cx2 <= x || cy2 <= y || x2 <= cx1 || y2 <= cy1) return;
        if (x < cx1) { w -= (cx1 - x); x = cx1; }
        if (y < cy1) { h -= (cy1 - y); y = cy1; }
        if (x + w > cx2) w = cx2 - x;
        if (y + h > cy2) h = cy2 - y;
    }
    if (w <= 0 || h <= 0) return;

    GLboolean color_array_before = glIsEnabled(GL_COLOR_ARRAY);
    GLboolean texcoord_array_before = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
    GLboolean texture_2d_before = glIsEnabled(GL_TEXTURE_2D);
    GLboolean cull_face_before = glIsEnabled(GL_CULL_FACE);
    GLboolean depth_test_before = glIsEnabled(GL_DEPTH_TEST);
    GLboolean scissor_test_before = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean blend_before = glIsEnabled(GL_BLEND);
    GLint bound_tex_before = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound_tex_before);

    int32_t *vbuf = (int32_t *)(so_mod.text_base + 0x1778a8);
    vbuf[0] = x << 16;
    vbuf[1] = y << 16;
    vbuf[2] = (x + w) << 16;
    vbuf[3] = y << 16;
    vbuf[4] = (x + w) << 16;
    vbuf[5] = (y + h) << 16;
    vbuf[6] = x << 16;
    vbuf[7] = (y + h) << 16;

    glVertexPointer_soloader(2, 0x140c /* GL_FIXED */, 0, vbuf);

    float r = (color & 0xff) / 255.0f;
    float g = ((color >> 8) & 0xff) / 255.0f;
    float b = ((color >> 16) & 0xff) / 255.0f;
    float a = ((color >> 24) & 0xff) / 255.0f;

    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, gl_get_white_texture());
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glColor4f(r, g, b, a);

    glDrawArrays_soloader(GL_LINE_LOOP, 0, 4);

    if (color_array_before) glEnableClientState(GL_COLOR_ARRAY);
    if (texcoord_array_before) glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    if (!texture_2d_before) glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, (GLuint) bound_tex_before);

    if (cull_face_before) glEnable(GL_CULL_FACE);
    if (depth_test_before) glEnable(GL_DEPTH_TEST);
    if (scissor_test_before) glEnable(GL_SCISSOR_TEST);
    if (!blend_before) glDisable(GL_BLEND);
}
```

Y su registro en `so_patch()`:

```c
    uintptr_t addr_SGL_grpDrawRect = so_symbol(&so_mod, "SGL_grpDrawRect");
    if (addr_SGL_grpDrawRect) {
        h_SGL_grpDrawRect = hook_addr(addr_SGL_grpDrawRect, (uintptr_t)&SGL_grpDrawRect_hook);
    }
```

## 5. `source/utils/glutil.c` y `.h` -- helpers de estado GL sin uso (Corrida 92)

`gl_cull_face_enabled`/`gl_set_cull_face_enabled`, `gl_scissor_test_enabled`/
`gl_set_scissor_test_enabled`, `gl_blend_enabled`/`gl_set_blend_enabled`. Se
agregaron pensando en usarlos desde `SGL_grpFillRect_hook`, pero la implementación
final de ese hook llama a `glIsEnabled`/`glEnable`/`glDisable` directamente -- estos 6
wrappers quedaron sin ningún caller. Código muerto, no afecta comportamiento removerlo.

```c
// glutil.c
GLboolean gl_cull_face_enabled(void) {
    return glIsEnabled(GL_CULL_FACE);
}

void gl_set_cull_face_enabled(GLboolean enabled) {
    if (enabled) {
        glEnable(GL_CULL_FACE);
    } else {
        glDisable(GL_CULL_FACE);
    }
}

GLboolean gl_scissor_test_enabled(void) {
    return glIsEnabled(GL_SCISSOR_TEST);
}

void gl_set_scissor_test_enabled(GLboolean enabled) {
    if (enabled) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

GLboolean gl_blend_enabled(void) {
    return glIsEnabled(GL_BLEND);
}

void gl_set_blend_enabled(GLboolean enabled) {
    if (enabled) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
}
```

```c
// glutil.h
GLboolean gl_cull_face_enabled(void);
void gl_set_cull_face_enabled(GLboolean enabled);
GLboolean gl_scissor_test_enabled(void);
void gl_set_scissor_test_enabled(GLboolean enabled);
GLboolean gl_blend_enabled(void);
void gl_set_blend_enabled(GLboolean enabled);
```

## Lo que se mantuvo (el fix real de fondos)

- `source/patch.c`: `SGL_grpFillRect_hook` completo (clip contra `SGL_CLIP`, re-bind de
  `glVertexPointer` al buffer de vértices del propio juego, disable de
  `CULL_FACE`/`DEPTH_TEST`/`SCISSOR_TEST`, enable de `BLEND` con
  `GL_SRC_ALPHA`/`GL_ONE_MINUS_SRC_ALPHA`, bind de la textura blanca, draw
  `GL_TRIANGLE_FAN`, restauración de estado previo) y su registro en `so_patch()`.
- `source/utils/glutil.c` / `.h`: `gl_get_white_texture()` (usada por el fillrect hook).
