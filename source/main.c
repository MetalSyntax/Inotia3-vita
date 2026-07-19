#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/utils.h"
#include "utils/logger.h"

#include <psp2/kernel/threadmgr.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

// Vita's native panel resolution. WrapperRenderer.onSurfaceCreated() feeds
// whatever WrapperJinterface.GetRealScreenWidth/Height() return straight into
// nativePreInit() as the device's real screen size (PORTING_PLAN.md Fase 2).
// Using 960x544 here is a deliberate first guess to see whether Inotia 3's UI
// tolerates a 16:9 target instead of a phone aspect ratio -- if HUD/menu
// layout breaks on a real run, this is the first thing to revisit (same
// situation Zenonia 3 flagged for its own configurable internal resolution).
#define SCREEN_W 960
#define SCREEN_H 544

// WrapperJinterface.setTextViewHeight(1024, 1024, false) is called during
// onSurfaceCreated() right before setPixel() allocates `pixel = new
// int[TEXTVIEW_HEIGHT * TEXTVIEW_WIDTH]` (confirmed in decompiled_apk's
// WrapperJinterface.java) -- this is a font/text glyph scratch buffer that
// Android renders via Bitmap/Canvas and the .so textures from directly, the
// same architecture documented in the psvita-porting skill's
// text_rendering.md. Faking this buffer's *size* right is required even
// before real text rendering is implemented, since nativeInit()'s pointer
// arithmetic against it depends on this exact dimension.
#define TEXTVIEW_WIDTH  1024
#define TEXTVIEW_HEIGHT 1024

// Opaque placeholder passed as the `Object activity` argument of
// nativeInit() (Java passes `this.activity`, the WrapperActivity instance).
// Not a real jobject -- the .so is only confirmed so far to hold this
// pointer, not necessarily call back into it (PORTING_PLAN.md Fase 2/§4). If
// a real run shows a JNI call against this handle, the requested method name
// needs a handler registered in java.c, and this may need to become a real
// FalsoJNI object reference instead of a bare sentinel.
static int fake_activity_obj = 0;

// FalsoJNI_Logger.c (vendored) routes its own [JNI]/[JNI WARN]/[JNI ERR]
// traces through this project-supplied game_log() instead of a private
// logger, so those traces share this project's own logging destination
// (see utils/logger.h's l_info) rather than needing a second log sink.
void game_log(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    l_info("%s", buf);
}

int main() {
    soloader_init_all();

    int (* JNI_OnLoad)(void *jvm) = (void *) so_symbol(&so_mod, "JNI_OnLoad");
    JNI_OnLoad(&jvm);

    gl_init();

    void (* nativePreInit)(void *env, void *clazz, int *parameter, int screenW, int screenH) =
        (void *) so_symbol(&so_mod, "Java_com_com2us_wrapper_WrapperRenderer_nativePreInit");
    void (* nativeInit)(void *env, void *clazz, long long *sysTime, int *upTime, int *pixel, float *gyro, void *activity) =
        (void *) so_symbol(&so_mod, "Java_com_com2us_wrapper_WrapperRenderer_nativeInit");
    void (* nativeReDraw)(void *env, void *clazz) =
        (void *) so_symbol(&so_mod, "Java_com_com2us_wrapper_WrapperRenderer_nativeReDraw");

    // nativePreInit() writes into `parameter` (WrapperRenderer.java passes an
    // uninitialized `int[3]`) -- allocate zeroed, don't assume the .so
    // ignores it just because Java never reads it back itself.
    int parameter[3] = {0, 0, 0};
    nativePreInit(&jni, NULL, parameter, SCREEN_W, SCREEN_H);

    // WrapperRenderer.UpdateCurTime() (Java) computes these two *before*
    // calling nativeInit() -- systemCurrentTime is wall-clock ms + a fixed
    // GMT offset, appUpTime is elapsed ms since first UpdateCurTime() call
    // (~0 here). Mirrored with current_timestamp_ms() rather than passing
    // zeros, since it's cheap to get right and unknown whether the .so reads
    // these once at init or relies on them being sane immediately.
    long long systemCurrentTime[1] = { (long long) current_timestamp_ms() };
    int appUpTime[1] = { 0 };

    // pixel: TEXTVIEW_HEIGHT * TEXTVIEW_WIDTH ints (~4MB) -- too big for the
    // stack, and it must stay alive for the lifetime of the .so (native code
    // keeps writing/reading it every frame via the pointer handed here once).
    int *pixel = calloc((size_t) TEXTVIEW_WIDTH * TEXTVIEW_HEIGHT, sizeof(int));

    // GyroManager.gyro is a real `float[3]` (x/y/z) -- confirmed in
    // decompiled_apk's GyroManager.java. The Vita has no Android-style gyro
    // sensor API; zeroed is a safe placeholder until/unless a real run shows
    // the engine depends on nonzero motion values to boot.
    float gyro[3] = { 0.0f, 0.0f, 0.0f };

    nativeInit(&jni, NULL, systemCurrentTime, appUpTime, pixel, gyro, &fake_activity_obj);

    l_success("nativeInit() returned -- entering render loop.");

    // Touch/button -> nativeEvent() and timer -> nativeRender() wiring is
    // Fase 5 (Input) work and not implemented yet (see PORTING_PLAN.md) --
    // this loop only drives the per-frame redraw call so far, which is the
    // minimum needed to get anything on screen at all for the first
    // Vita3K/hardware run.
    while (1) {
        nativeReDraw(&jni, NULL);
        gl_swap();
    }

    sceKernelExitDeleteThread(0);
}
