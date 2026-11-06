/*
 * @file  remap_menu.c
 * @brief See remap_menu.h.
 */

#include "utils/remap_menu.h"
#include "utils/settings.h"
#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/utils.h"
#include "overlay.h"

#include <psp2/ctrl.h>
#include <stdio.h>
#include <vitaGL.h>

// Mirrors wr_touch.c's BUTTON_MAP order/count (SETTING_BUTTON_REMAP_COUNT) --
// kept as a separate table (not shared via a header) so this dialog-flow
// module doesn't need wr_touch.c's static BUTTON_MAP; only the human-readable
// names/compiled defaults matter here, the live mapping stays owned by
// wr_touch.c (it reads setting_button_remap[] directly).
typedef struct {
    unsigned int default_button;
    const char *name;
} remap_action_t;

static const remap_action_t ACTIONS[SETTING_BUTTON_REMAP_COUNT] = {
    { SCE_CTRL_UP,       "Movimiento: Arriba" },
    { SCE_CTRL_DOWN,     "Movimiento: Abajo" },
    { SCE_CTRL_LEFT,     "Movimiento: Izquierda" },
    { SCE_CTRL_RIGHT,    "Movimiento: Derecha" },
    { SCE_CTRL_CROSS,    "Ataque principal" },
    { SCE_CTRL_CIRCLE,   "Habilidad 1" },
    { SCE_CTRL_SQUARE,   "Habilidad 2" },
    { SCE_CTRL_TRIANGLE, "Objeto / Escudo" },
    { SCE_CTRL_LTRIGGER, "Objeto rapido 1" },
    { SCE_CTRL_RTRIGGER, "Objeto rapido 2" },
    { SCE_CTRL_SELECT,   "Objeto rapido 3" },
    { SCE_CTRL_START,    "Pausa" },
};

static const char *button_name(unsigned int b) {
    switch (b) {
        case SCE_CTRL_UP: return "ARRIBA";
        case SCE_CTRL_DOWN: return "ABAJO";
        case SCE_CTRL_LEFT: return "IZQUIERDA";
        case SCE_CTRL_RIGHT: return "DERECHA";
        case SCE_CTRL_CROSS: return "X";
        case SCE_CTRL_CIRCLE: return "CIRCULO";
        case SCE_CTRL_SQUARE: return "CUADRADO";
        case SCE_CTRL_TRIANGLE: return "TRIANGULO";
        case SCE_CTRL_LTRIGGER: return "L";
        case SCE_CTRL_RTRIGGER: return "R";
        case SCE_CTRL_SELECT: return "SELECT";
        case SCE_CTRL_START: return "START";
        default: return "?";
    }
}

// Blocks until the user dismisses the dialog. Must call vglSwapBuffers(GL_TRUE)
// directly here, NOT this project's own gl_swap() -- gl_swap() always passes
// GL_FALSE (has_commondialog) since the vitaGL swap perf fix earlier this
// session, which is correct for normal frames but means vitaGL never
// composites an active SceMsgDialog on top, so the dialog silently never
// appeared while this function still blocked waiting for a button the user
// couldn't see any prompt for (dialog.c's own fatal_error() already gets this
// right the same way -- this just missed it when the module was added).
static void show_info_dialog(const char *msg) {
    init_msg_dialog(msg);
    while (!get_msg_dialog_result()) {
        vglSwapBuffers(GL_TRUE);
    }
}

// Waits for the first NEWLY-pressed button (one that wasn't already held the
// instant this function was entered -- so releasing the chord that opened
// this flow doesn't itself count) and returns its single-bit mask, or 0 if
// START is held continuously for ~1.5s (cancel-this-entry gesture, leaving
// that action unchanged).
static unsigned int capture_next_button(void) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned int baseline = pad.buttons;
    uint64_t start_held_since = 0;

    while (1) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int newly = pad.buttons & ~baseline;
        if (newly != 0) {
            for (unsigned int b = 1; b != 0; b <<= 1) {
                if (newly & b) return b; // lowest newly-pressed bit is enough for v1
            }
        }
        if (pad.buttons & SCE_CTRL_START) {
            if (start_held_since == 0) start_held_since = current_timestamp_ms();
            else if (current_timestamp_ms() - start_held_since > 1500) return 0;
        } else {
            start_held_since = 0;
        }
        baseline &= pad.buttons; // a baseline button released mid-wait can count as "newly pressed" again
        gl_swap();
    }
}

void remap_menu_run(void) {
    show_info_dialog(
        "Reasignar botones\n\n"
        "Para cada accion se muestra su boton fisico\n"
        "actual. Presiona el NUEVO boton para esa accion,\n"
        "o mantene START ~2s para dejarla igual.\n\n"
        "Presiona OK para empezar.");

    for (int i = 0; i < SETTING_BUTTON_REMAP_COUNT; i++) {
        unsigned int current = setting_button_remap[i] != 0
            ? setting_button_remap[i] : ACTIONS[i].default_button;
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[%d/%d] %s\nBoton actual: %s\n\n"
                 "Presiona el nuevo boton,\no mantene START ~2s para no cambiar.",
                 i + 1, SETTING_BUTTON_REMAP_COUNT, ACTIONS[i].name, button_name(current));
        show_info_dialog(msg);

        unsigned int chosen = capture_next_button();
        if (chosen != 0 && chosen != SCE_CTRL_START) {
            setting_button_remap[i] = chosen;
        }
    }

    settings_save();
    show_info_dialog("Listo -- los cambios se guardaron.");
}

void fps_menu_cycle(void) {
    static const int FPS[] = { 15, 23, 25, 30 };
    int i = 0;
    while (i < 3 && FPS[i] != setting_fps) i++;
    setting_fps = FPS[(i + 1) % 4];
    settings_save();
    char msg[32];
    snprintf(msg, sizeof(msg), setting_fps == 15 ? "FPS: 15 (ORIGINAL)" : "FPS: %d", setting_fps);
    osd_show(msg);
}
