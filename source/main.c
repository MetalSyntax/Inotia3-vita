#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/utils.h"
#include "utils/logger.h"
#include "wr_timer.h"
#include "wr_touch.h"
#include "audio.h"

#include <psp2/kernel/threadmgr.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

// patch.c (Corrida 33): fires the UserAgree module callback startClet()
// registers but nothing in this port would otherwise ever invoke, which is
// what actually kicks off the STATE_* game state machine.
extern void patch_fire_user_agree_callback(void);

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 16 * 1024 * 1024;
#endif

// Corrida 15 (PORTING_PLAN.md): vitasdk's default main-thread stack (256KB)
// is sized for simple homebrews, not a deeply-nested Android engine call
// chain (JNI dispatch -> asset loader -> DecodePNG -> libpng ->
// inflateInit_ -> zlib). A __stack_chk_fail kept firing inside libpng's
// png_create_read_struct_2 with no local buffer overflow to account for it
// -- the leading suspect left once heap size and jmp_shim were ruled out is
// simply running the call stack too deep for the default reservation.
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;

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

// Render-loop plumbing shared with the WrapperRenderer timer emulation
// (wr_timer.c). The .so drives its game loop by asking WrapperJinterface for
// timers (java.c -> wr_set_timer) and expects nativeRender(timerID) called back
// when each is due; wr_run_timers() below does that dispatch.
static jlong *g_sysTime_ptr = NULL;   // == the .so's CS_knlCurrentTime() source
static jint  *g_upTime_ptr = NULL;    // == the .so's CS_knlUpTime() source
static uint64_t g_start_time = 0;

// DrawTextInRect_stub (java.c) renders glyphs directly into this SAME buffer:
// the .so caches ONE raw pointer to it (via jGetPixelBufferPointer(), fed by
// nativeInit()'s GetIntArrayElements(pixel) call below) and reads it back
// row-major after every DrawTextInRect call (SGL_RenderFontToAlphaImageInRect,
// out_ghidra.c:162636) -- writing here IS the render, no separate JNI needed.
jint *g_textview_pixel_buffer = NULL;
const int g_textview_width = TEXTVIEW_WIDTH;
const int g_textview_height = TEXTVIEW_HEIGHT;
static void (*g_nativeRender)(void *env, void *clazz, int timerID) = NULL;

// WrapperRenderer.UpdateCurTime(): refresh the shared clock arrays and return
// the current wall-clock time in ms.
static int64_t wr_update_time(void) {
    uint64_t now = current_timestamp_ms();
    g_sysTime_ptr[0] = (jlong) now;
    g_upTime_ptr[0] = (jint) (now - g_start_time);
    return (int64_t) now;
}

// WrapperRenderer.nativeRender(int) dispatch.
static void wr_render(int timerID) {
    g_nativeRender(&jni, NULL, timerID);
}

// WrapperRenderer.nativeEvent(int type, int p1, int p2, int touchCount)
// dispatch -- see wr_touch.c for the touch-state machine that produces these
// calls (Fase 5, Input).
static void (*g_nativeEvent)(void *env, void *clazz, int type, int p1, int p2, int touchCount) = NULL;
static void wr_dispatch_event(int type, int p1, int p2, int touchCount) {
    g_nativeEvent(&jni, NULL, type, p1, p2, touchCount);
}

int main() {
    // Corrida 67: started before anything else so it can flush the log ring
    // independently of the main thread's health for the whole process
    // lifetime, including a hang inside soloader_init_all()/nativeInit()
    // itself, not just the render loop below.
    logger_start_watchdog();

    soloader_init_all();

    int (* JNI_OnLoad)(void *jvm) = (void *) so_symbol(&so_mod, "JNI_OnLoad");
    JNI_OnLoad(&jvm);

    gl_init();

    void (* nativePreInit)(void *env, void *clazz, int *parameter, int screenW, int screenH) =
        (void *) so_symbol(&so_mod, "Java_com_com2us_wrapper_WrapperRenderer_nativePreInit");
    void (* nativeInit)(void *env, void *clazz, long long *sysTime, int *upTime, int *pixel, float *gyro, void *activity) =
        (void *) so_symbol(&so_mod, "Java_com_com2us_wrapper_WrapperRenderer_nativeInit");
    // nativePreInit() writes into `parameter` (WrapperRenderer.java passes an
    // uninitialized `int[3]`) -- allocate zeroed, don't assume the .so
    // ignores it just because Java never reads it back itself.
    jintArray parameter = jni->NewIntArray(&jni, 3);
    nativePreInit(&jni, NULL, parameter, SCREEN_W, SCREEN_H);

    // WrapperRenderer.UpdateCurTime() (Java) computes these two *before*
    // calling nativeInit() -- systemCurrentTime is wall-clock ms + a fixed
    // GMT offset, appUpTime is elapsed ms since first UpdateCurTime() call
    // (~0 here). Mirrored with current_timestamp_ms() rather than passing
    // zeros, since it's cheap to get right and unknown whether the .so reads
    // these once at init or relies on them being sane immediately.
    jlongArray systemCurrentTime = jni->NewLongArray(&jni, 1);
    jlong *sysTime_ptr = jni->GetLongArrayElements(&jni, systemCurrentTime, NULL);
    sysTime_ptr[0] = (jlong) current_timestamp_ms();

    jintArray appUpTime = jni->NewIntArray(&jni, 1);

    // pixel: TEXTVIEW_HEIGHT * TEXTVIEW_WIDTH ints (~4MB) -- too big for the
    // stack, and it must stay alive for the lifetime of the .so (native code
    // keeps writing/reading it every frame via the pointer handed here once).
    jintArray pixel = jni->NewIntArray(&jni, TEXTVIEW_WIDTH * TEXTVIEW_HEIGHT);
    g_textview_pixel_buffer = jni->GetIntArrayElements(&jni, pixel, NULL);

    // GyroManager.gyro is a real `float[3]` (x/y/z) -- confirmed in
    // decompiled_apk's GyroManager.java. The Vita has no Android-style gyro
    // sensor API; zeroed is a safe placeholder until/unless a real run shows
    // the engine depends on nonzero motion values to boot.
    jfloatArray gyro = jni->NewFloatArray(&jni, 3);

    void (* nativeResume)(void *env, void *clazz) =
        (void *) so_symbol(&so_mod, "Java_com_com2us_wrapper_WrapperRenderer_nativeResume");
    void (* nativeResumeClet)(void *env, void *clazz) =
        (void *) so_symbol(&so_mod, "Java_com_com2us_wrapper_WrapperRenderer_nativeResumeClet");
    void (* nativeRender)(void *env, void *clazz, int timerID) =
        (void *) so_symbol(&so_mod, "Java_com_com2us_wrapper_WrapperRenderer_nativeRender");
    g_nativeEvent = (void *) so_symbol(&so_mod, "Java_com_com2us_wrapper_WrapperRenderer_nativeEvent");

    // Audio mixer must be up before nativeInit(): startClet() (which runs
    // *inside* nativeInit()) can reach SOUNDSYSTEM_*/SOUNDPLAYER_* code that
    // calls SoundCreate/SoundPlay via JNI immediately (Corrida 63, see
    // PORTING_PLAN.md -- audio_init() was never called anywhere before this).
    audio_init();

    // Timer table must be in its constructed (empty, maxIndex == -1) state
    // before nativeInit(): startClet() runs *inside* nativeInit() and
    // immediately registers g_MainTimer via WrapperJinterface.SetTimer ->
    // wr_set_timer().
    wr_timer_reset();
    g_nativeRender = nativeRender;

    nativeInit(&jni, NULL, systemCurrentTime, appUpTime, pixel, gyro, &fake_activity_obj);

    nativeResume(&jni, NULL);
    nativeResumeClet(&jni, NULL);

    // See patch.c (Corrida 33): must run after startClet() (inside
    // nativeInit()) has fully finished registering everything else, not
    // synchronously from inside CS_UserAgreeSetCallback's hook -- otherwise
    // the state machine's first Enter callback would run before
    // CS_ActiveUserStart()/news banner/g_MainTimer's own registration do.
    patch_fire_user_agree_callback();

    l_success("nativeInit() and resume returned -- entering render loop.");

    jint *upTime_ptr = jni->GetIntArrayElements(&jni, appUpTime, NULL);

    g_sysTime_ptr = sysTime_ptr;
    g_upTime_ptr = upTime_ptr;
    g_start_time = current_timestamp_ms();

    wr_touch_init();

    // Initial buffer clear: real Android relies on the EGL context's own
    // guarantee that a freshly created surface starts blank, since nothing
    // in the Java call chain ever clears it explicitly (see wr_timer.h
    // Corrida 31). One clear+swap here covers both vitaGL buffers before the
    // .so draws anything, matching that same one-time guarantee.
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl_swap();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl_swap();

    // Mirror of WrapperDirtyRenderer's actual execution model (Corrida 31,
    // see wr_timer.h) -- NOT WrapperRenderer.onDrawFrame(), which every
    // earlier Corrida assumed applies here but does not: Inotia 3's
    // MainActivity instantiates WrapperDirtyRenderer, whose onDrawFrame() is
    // empty and whose GLSurfaceView runs RENDERMODE_WHEN_DIRTY. Every tick
    // just drains touch events and fires due timers (nativeRender -- where
    // the .so does its own drawing AND its own clearing, if any); the buffer
    // is only ever presented when the .so itself calls Flush() (java.c's
    // Flush_stub -> wr_request_present()), exactly like requestRender().
    while (1) {
        wr_process_touch(wr_dispatch_event);
        wr_process_buttons(wr_dispatch_event);
        wr_run_timers(wr_update_time, wr_render);
        if (wr_take_present_request()) {
            gl_swap();
        }
    }

    sceKernelExitDeleteThread(0);
}
