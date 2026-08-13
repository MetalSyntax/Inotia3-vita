/*
 * @file  splash.c
 * @brief See splash.h.
 *
 * Corrida 91 (PORTING_PLAN.md): a sibling investigation traced the "Com2uS
 * splash" the user expected between the Vita's own boot splash and this
 * port's loading screen to `WrapperActivity.initializeBackgroundImage()`/
 * `ProgressBarStatus(int,int)`/`dismissBackgroundImage()` -- plain Java
 * callbacks the real `.so` fires via JNI (confirmed call site,
 * `WrapperJinterface.java:410`) while `MainActivity` itself is still doing
 * synchronous asset-loading work, well before its `GLSurfaceView`/the game's
 * own native rendering ever starts. This is NOT part of the `.so`'s own
 * `STATE_Set` state machine (a separate, exhaustive investigation ruled that
 * out with real hardware-log evidence) -- it's Android/Java UI this port
 * never had a Java layer to run in the first place, so it's implemented here
 * directly in the loader instead.
 *
 * `MainActivity.initializeBackgroundImage()` (decompiled_apk/sources/...)
 * does exactly:
 *   InputStream is = wrapperData.getAssetManager().open(
 *       WrapperData.GAME_ASSETS_PATH + "/MainBackgroundImage" +
 *       WrapperData.RES_NAME_APPEND);
 *   Bitmap bitmap = BitmapFactory.decodeStream(is);
 * `WrapperData.GAME_ASSETS_PATH` == "common/game_res" and
 * `RES_NAME_APPEND` == ".jpg" (both already confirmed and used by this
 * port's own asset resolution in source/java.c, INOTIA3_GAME_ASSETS_PATH/
 * INOTIA3_RES_NAME_APPEND) -- giving the real on-disk name
 * "MainBackgroundImage.jpg", genuinely a JPEG (unlike most of this game's
 * OTHER assets, which hide behind a SECOND ".jpg" suffix appended on top of
 * their real extension purely to blend into an APK listing, e.g. the actual
 * "Ambience_Cave.ogg.jpg"/"MainMenu.png.jpg" pattern already confirmed
 * elsewhere in this asset tree -- a real .jpg image needs no such disguise,
 * so this filename is NOT double-suffixed).
 *
 * NOT confirmed: this exact file is present in the specific extracted-APK
 * copy this project's decompiled_so/decompiled_apk trees were built from
 * (inotia3-1-3-2/assets/common/game_res/ has no MainBackgroundImage.* at
 * all, checked directly). This may be an APK-version difference (this repo's
 * own .gitignore also references an inotia3-1-4-5/ extraction that was never
 * used for this session's decompile work) or simply a gap in that specific
 * local extraction. Since this port already treats a missing loose asset as
 * a non-fatal, logged condition everywhere else (GetFileDescriptor_stub),
 * the same convention applies here: if the file isn't there, this just logs
 * a warning and skips drawing anything, rather than blocking boot.
 *
 * stb_image.h (lib/stb/, vendored verbatim from ../Zenonia3-vita/lib/stb/ --
 * same already-hardware-validated dependency, same reason it's needed there:
 * this port has no PNG/JPEG decoder of its own outside the .so's internal,
 * not-externally-callable libpng) does the actual decode. The full-screen
 * quad + GL state setup below mirrors ../Zenonia3-vita/loader/androidui.c's
 * androidui_draw_quad()/androidui_draw() -- the same "Android drew this
 * overlay, not the .so, so this loader must replicate it by hand with a raw
 * GL texture" problem, solved there first.
 */

#include "splash.h"

#include "utils/glutil.h"
#include "utils/logger.h"

#include <vitaGL.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include <stb/stb_image.h>

// Same convention as source/java.c's INOTIA3_LOOSE_ASSETS_PATH/
// INOTIA3_GAME_ASSETS_PATH/INOTIA3_RES_NAME_APPEND -- duplicated as a literal
// here rather than shared across a header, since it's one string used in
// exactly one place on each side and not worth a cross-file dependency for.
#define SPLASH_IMAGE_PATH "ux0:data/inotia3/assets/common/game_res/MainBackgroundImage.jpg"

#define SCREEN_W 960
#define SCREEN_H 544

static GLuint g_splash_tex = 0;
static int g_splash_w = 0, g_splash_h = 0;
static int g_splash_loaded = 0; // 0 = never attempted or failed; 1 = have a real texture

// Sets up an orthographic screen-space (0..960, 0..544, y-down) draw state
// matching this port's real panel resolution -- mirrors
// ../Zenonia3-vita/loader/androidui.c's androidui_draw()'s own setup for the
// exact same class of problem (a raw GL overlay drawn outside the .so's own
// projection/state).
static void splash_begin_draw(void) {
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0.0f, (float) SCREEN_W, (float) SCREEN_H, 0.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

static void splash_end_draw(void) {
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

static void splash_draw_background(void) {
    if (!g_splash_loaded) return;

    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glBindTexture(GL_TEXTURE_2D, g_splash_tex);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    // Stretched to fill the real 960x544 panel, matching Android's ImageView
    // default scaleType (FIT_XY-ish full-bleed background) rather than
    // preserving the source JPEG's own aspect ratio -- same call this port
    // already makes for other full-screen backgrounds.
    const float verts[] = { 0, 0,  (float) SCREEN_W, 0,  0, (float) SCREEN_H,  (float) SCREEN_W, (float) SCREEN_H };
    const float uvs[]   = { 0, 0,  1, 0,               0, 1,                   1, 1 };
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
}

// A plain filled-rect progress bar (no source asset for this -- the real
// Android ProgressBar is a system widget with its own theme, not an asset
// this game ships) -- flat color, bottom-center, simple and legible rather
// than trying to fake Android's default widget styling exactly.
static void splash_draw_progress_bar(int cur, int total) {
    if (total <= 0) return;
    float frac = (float) cur / (float) total;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    const float bar_w = 400.0f, bar_h = 12.0f;
    const float bar_x = (SCREEN_W - bar_w) / 2.0f;
    const float bar_y = SCREEN_H - 40.0f;

    // Track (dim outline) -- always full width, so partial progress is
    // visible against something.
    const float track[] = { bar_x, bar_y,  bar_x + bar_w, bar_y,
                             bar_x, bar_y + bar_h,  bar_x + bar_w, bar_y + bar_h };
    glColor4f(0.0f, 0.0f, 0.0f, 0.5f);
    glVertexPointer(2, GL_FLOAT, 0, track);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Fill, proportional to cur/total.
    float fill_w = bar_w * frac;
    const float fill[] = { bar_x, bar_y,  bar_x + fill_w, bar_y,
                            bar_x, bar_y + bar_h,  bar_x + fill_w, bar_y + bar_h };
    glColor4f(1.0f, 1.0f, 1.0f, 0.9f);
    glVertexPointer(2, GL_FLOAT, 0, fill);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

void splash_show(void) {
    int channels;
    unsigned char *pixels = stbi_load(SPLASH_IMAGE_PATH, &g_splash_w, &g_splash_h, &channels, 4);
    if (!pixels) {
        l_warn("[Splash] \"%s\" not found or failed to decode (%s) -- skipping splash image",
               SPLASH_IMAGE_PATH, stbi_failure_reason());
        g_splash_loaded = 0;
        return;
    }

    glGenTextures(1, &g_splash_tex);
    glBindTexture(GL_TEXTURE_2D, g_splash_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_splash_w, g_splash_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    stbi_image_free(pixels);
    g_splash_loaded = 1;

    l_success("[Splash] loaded \"%s\" (%dx%d) -> tex=%u", SPLASH_IMAGE_PATH,
              g_splash_w, g_splash_h, (unsigned int) g_splash_tex);

    // Draw immediately into BOTH vitaGL buffers (two draw+swap passes) so
    // the image survives on screen regardless of which buffer is next
    // presented -- same reasoning as main.c's own initial double
    // glClear()+gl_swap() before the .so ever draws anything.
    splash_begin_draw();
    splash_draw_background();
    splash_end_draw();
    gl_swap();
    splash_begin_draw();
    splash_draw_background();
    splash_end_draw();
    gl_swap();
}

void splash_update_progress(int cur, int total) {
    // Redraw background (if any) + the bar every call and present once --
    // real Android calls this several times while loading, each one a fresh
    // frame, not an accumulating draw.
    splash_begin_draw();
    splash_draw_background();
    splash_draw_progress_bar(cur, total);
    splash_end_draw();
    gl_swap();
    splash_begin_draw();
    splash_draw_background();
    splash_draw_progress_bar(cur, total);
    splash_end_draw();
    gl_swap();
}

void splash_hide(void) {
    if (g_splash_loaded) {
        glDeleteTextures(1, &g_splash_tex);
        g_splash_tex = 0;
        g_splash_loaded = 0;
    }
}
