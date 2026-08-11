/*
 * java.c
 *
 * "Java-side" native method handlers that libgame.so calls back into via
 * FalsoJNI (GetMethodID/GetStaticMethodID by name -- FalsoJNI matches purely
 * on method name string, not class or signature, see getMethodIdByName() in
 * FalsoJNI_ImplBridge.c).
 *
 * Analysis (PORTING_PLAN.md Fase 2): a full sweep of decompiled_so's
 * out_ghidra.c found ~160 distinct JNIEnv vtable calls the .so makes, but
 * NONE of them belong to WrapperRenderer (the core render/lifecycle class
 * main.c drives directly) -- they're all Com2uS's optional subsystems (Hub
 * login/social, ads, IAP, Tapjoy, news banner, memory-stat HUD, user-defined
 * dialogs), which PORTING_PLAN.md already flags as no-op-until-proven-
 * necessary for an offline port. Only entries with a CONFIRMED literal JNI
 * signature (not Ghidra's inferred/unresolved DAT_ pointers) are registered
 * below -- for everything else, leaving the name unregistered is the safer
 * default: GetStaticMethodID/GetMethodID returns NULL (FalsoJNI's normal
 * "not found", logged, non-fatal), which lets whatever null-check the
 * calling engine code has (if any) skip the feature cleanly. Registering a
 * method under a *guessed* return type risks a worse outcome -- FalsoJNI
 * returns a type-specific fallback value (e.g. methodIntCall returns -1 for
 * an unregistered id if some *other* mismatched call site reaches it) that
 * the engine could misread as a real result. The remaining ~150 names are
 * intentionally left out until a real Vita3K/hardware run's log confirms
 * which ones actually get called and need a real handler (see
 * so-crash-triage skill / PORTING_PLAN.md Fase 3 methodology).
 */

#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <so_util/so_util.h>

#include "utils/logger.h"
#include "wr_timer.h"
#include "reimpl/apk_zip.h"
#include "audio.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

extern so_module so_mod;

// main.c: the SAME buffer nativeInit() handed the .so as its `pixel[]`
// scratch array -- DrawTextInRect_stub (Corrida 59, below) renders glyphs
// straight into it instead of going through a second JNI round trip.
extern jint *g_textview_pixel_buffer;
extern const int g_textview_width;
extern const int g_textview_height;

// WrapperData.java: getApkFilePath()/GAME_ASSETS_PATH/RES_NAME_APPEND.
// GetFileDescriptor_stub below prefers a pre-extracted LOOSE copy of the
// asset (see INOTIA3_LOOSE_ASSETS_PATH) -- no real .apk needed on the
// console at all, matching how ../Zenonia2-vita/../Zenonia3-vita stage
// their assets (plain files under ux0:data/..., never touching the .apk).
// Inotia 3's own Android code just happens to route this one call through
// AssetManager.openFd() (raw offset into the APK zip) rather than a
// simple streaming readAssets()-style method like Zenonia's engine uses --
// an engine difference, not a choice we made -- so falling back to parsing
// the real .apk (apk_zip.c) only kicks in if the loose file isn't there.
#define INOTIA3_APK_PATH "ux0:/data/inotia3/inotia3.apk"
#define INOTIA3_GAME_ASSETS_PATH "common/game_res"
#define INOTIA3_RES_NAME_APPEND ".jpg"
#define INOTIA3_LOOSE_ASSETS_PATH "ux0:data/inotia3/assets/" INOTIA3_GAME_ASSETS_PATH "/"

// Defined in main.c; also used by FalsoJNI_Logger.c. Routes engine/JNI traces
// into this port's logger.
extern void game_log(const char *fmt, ...);

/*
 * JNI Methods
 */

NameToMethodID nameToMethodId[] = {
    // HubConstant.HubInitialize(int, int): int -- confirmed literal
    // signature "(II)I" in out_ghidra.c. Return 0 (assumed success/no-error)
    // until a real run shows what the engine does with a nonzero result.
    { 1, "HubInitialize", METHOD_TYPE_INT },

    // WrapperUserDefined.alertPopup(byte[], byte[]): void -- confirmed
    // literal signature "([B[B)V". No-op: the byte arrays are presumably a
    // title+message pair for a native alert dialog we don't render.
    { 2, "alertPopup", METHOD_TYPE_VOID },

    // Ad/news-banner module init calls -- confirmed literal signatures
    // ("(II)V", "(IIIIIII)V", "(IIII)V"). No-ops: no ad/news-banner UI in
    // this port.
    { 3, "setGameResolution", METHOD_TYPE_VOID },
    { 4, "adCreate", METHOD_TYPE_VOID },
    { 5, "setAdProperty", METHOD_TYPE_VOID },
    { 6, "newsBannerInit", METHOD_TYPE_VOID },

    // File path getters
    { 7, "GetUserDir", METHOD_TYPE_OBJECT },
    { 8, "getUserDir", METHOD_TYPE_OBJECT },
    { 9, "GetPackageName", METHOD_TYPE_OBJECT },
    { 10, "getPackageName", METHOD_TYPE_OBJECT },

    // Screen and Viewport
    { 11, "GetRealScreenWidth", METHOD_TYPE_INT },
    { 12, "GetRealScreenHeight", METHOD_TYPE_INT },
    { 13, "SetSourceSize", METHOD_TYPE_VOID },
    { 14, "SetViewPort", METHOD_TYPE_VOID },

    // JSON and Utils
    { 15, "org/json/JSONObject/<init>", METHOD_TYPE_OBJECT },
    { 16, "getInt", METHOD_TYPE_INT },
    { 17, "getJSONArray", METHOD_TYPE_OBJECT },
    { 18, "length", METHOD_TYPE_INT },
    { 19, "getString", METHOD_TYPE_OBJECT },

    // Hardware Info (returns strings or objects)
    { 20, "GetCom2usUDID", METHOD_TYPE_OBJECT },
    { 21, "GetCountry", METHOD_TYPE_OBJECT },
    { 22, "GetLanguage", METHOD_TYPE_OBJECT },
    { 23, "CC_MD5", METHOD_TYPE_OBJECT },

    // HTTP
    { 24, "org/apache/http/client/methods/HttpPost/<init>", METHOD_TYPE_OBJECT },

    // WrapperRenderer software timers. The .so drives its entire game loop
    // through these: jSetTimer()/jUnsetTimer() call WrapperJinterface.SetTimer
    // /UnsetTimer (static methods), and the Java layer fires nativeRender(id)
    // back when due. Reimplemented host-side in wr_timer.c (see main.c loop).
    { 25, "SetTimer", METHOD_TYPE_VOID },
    { 26, "UnsetTimer", METHOD_TYPE_VOID },

    // Com2uS audio surface. The .so's SOUNDSYSTEM_*/SOUNDPLAYER_* code delegates
    // playback to Android via these static WrapperJinterface methods (jSound*
    // wrappers). All return int; SoundCreate returns a sound handle the engine
    // keeps and passes to the others. Real Tremor-backed playback (source/
    // audio.c, wired in below at Corrida 63 -- see PORTING_PLAN.md).
    { 27, "SoundCreate", METHOD_TYPE_INT },     // (Ljava/lang/String;Z)I
    { 28, "SoundDestroy", METHOD_TYPE_INT },    // (I)I
    { 29, "SoundPlay", METHOD_TYPE_INT },       // (IZ)I
    { 30, "SoundPause", METHOD_TYPE_INT },      // (I)I
    { 31, "SoundResume", METHOD_TYPE_INT },     // (I)I
    { 32, "SoundStop", METHOD_TYPE_INT },       // (I)I
    { 33, "SoundGetVolume", METHOD_TYPE_INT },  // (I)I
    { 34, "SoundSetVolume", METHOD_TYPE_INT },  // (II)I
    { 35, "SoundPrepare", METHOD_TYPE_INT },    // (I)I

    // Missing methods found in crash log
    { 36, "GetScreenWidth", METHOD_TYPE_INT },
    { 37, "GetScreenHeight", METHOD_TYPE_INT },
    { 38, "getMaxMemory", METHOD_TYPE_LONG },
    { 39, "GetFileDescriptor", METHOD_TYPE_OBJECT },
    { 40, "GetApkFilePath", METHOD_TYPE_OBJECT },
    { 41, "Flush", METHOD_TYPE_VOID },
    { 42, "GetAppProperty", METHOD_TYPE_OBJECT },
    { 43, "GetUDID", METHOD_TYPE_OBJECT },
    { 44, "GetPhoneNumber", METHOD_TYPE_OBJECT },

    // Corrida 40: WrapperJinterface.GetNoticeHttp(String): Object -- fetches
    // the "notice board" message from http://s.com2us.net/game/inotia3/
    // notice.php (a 2013-era Com2uS server, long gone). Extend_RequestNotice
    // (out_ghidra.c:117731) does `strcpy(buf, (char*)jRequestNotice(url))`
    // with NO null check on the result -- confirmed by disassembly/dump
    // (Corrida 40): an unregistered Object-returning method defaults to NULL
    // in FalsoJNI, and `strcpy(dst, NULL)` crashes immediately (R1==0 in the
    // dump). Must return a real (non-NULL) string.
    { 45, "GetNoticeHttp", METHOD_TYPE_OBJECT },

    // Corrida 44: WrapperJinterface.GetStringWidthHeightInRect(String, int,
    // int, int, int, int[]): int -- confirmed literal signature
    // "(Ljava/lang/String;IIII[I)I". Left unregistered, this always returns
    // 0 ("everything fits") from FalsoJNI's default -- and the game's own
    // native word-wrap loop (used to lay out long text blocks like
    // memorytext_en.dat, a ~109KB lore document) never finds a real line
    // break, spinning hundreds of calls per invocation without ever making
    // progress -- observed on device as an apparent infinite loop stuck on
    // the loading screen (766 calls logged in a single run before the
    // console's log buffer was cut short). Must return a real width so the
    // caller's wrap loop actually terminates.
    { 46, "GetStringWidthHeightInRect", METHOD_TYPE_INT },

    // Corrida 50: game now reaches Title but crashes entering the Hub screen
    // (STATE_EnterHub -> HUB_InitString, out_ghidra.c:111434-111466) with a
    // Data Abort in our own loader's strcpy, R1 (source) == 0 -- same crash
    // shape as Corrida 40, different call site. HUB_InitString does
    // `pcVar2 = GetCharStringFromPlist(&DAT_001340b0,"Title",lang);
    // strcpy(acStack_42c, pcVar2);` (and again for "Message") with no NULL
    // check. GetCharStringFromPlist (out_ghidra.c:117575) is a thin wrapper
    // over jGetStringFromXmlEx (out_ghidra.c:2882) -- which, UNLIKE the
    // GetStaticMethodID-by-id methods above, resolves via
    // `GetMethodID(g_userDefinedClass, "GetStringFromXmlEx",
    // "(Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/Object;")` -- an
    // INSTANCE method. FalsoJNI's GetMethodID matches by name through this
    // same nameToMethodId[] table (see FalsoJNI.c's GetMethodID ->
    // getMethodIdByName), so it was simply never registered and always
    // returned NULL -- jGetStringFromXmlEx itself already null-checks that
    // (out_ghidra.c:2901-2903), but NONE of its ~15 call sites in the .so
    // (out_ghidra.c:16444/26539/26541/26556/26558/111451/111456/111687/
    // 112076/112078, all feeding straight into strcpy/strtok) check the
    // result before using it. Registering it to always return a real
    // (non-NULL, empty) string fixes every one of those call sites at once --
    // the same "faithful no-op, never crashes, never fabricates" fix as
    // Corrida 40, just at the root instead of per call site.
    { 47, "GetStringFromXmlEx", METHOD_TYPE_OBJECT },

    // Same risk, same fix, no confirmed call site yet (jGetStringFromXml,
    // out_ghidra.c:2917-2948, the 2-arg sibling of GetStringFromXmlEx above)
    // -- registered defensively since it shares the exact same
    // GetMethodID-returns-NULL-unchecked-by-caller shape.
    { 48, "GetStringFromXml", METHOD_TYPE_OBJECT },

    // Corrida 59: see DrawTextInRect_stub's own comment -- this is the
    // engine's universal text-draw entry point (every menu/HP/item/dialogue
    // string in the game funnels through it), left unregistered until now.
    { 49, "DrawTextInRect", METHOD_TYPE_INT },

    // Corrida 60: WrapperJinterface.GetStringWidth(String, int, int): int --
    // confirmed literal signature "(Ljava/lang/String;II)I" via the native
    // wrapper jGetStringWidth (out_ghidra.c:143919). Investigated for a user
    // report of "some texts have no background panel and show as plain white
    // letters" (readability bug). This is a SEPARATE text-measurement entry
    // point from GetStringWidthHeightInRect (id 46, fixed in Corrida 44):
    // jGetStringWidth is called by __utilGetStringWidth (out_ghidra.c:151938,
    // backing CSOGL_grpGetStringWidth/ByUTF8) and by SGL_GetAsciiTextSize
    // (out_ghidra.c:162608-162632) -- a sibling of SGL_GetTextSizeInRect/
    // SGL_GetTextSize (:162570-162604), which correctly go through the
    // already-working id 46 (jGetStringWidthHeightInRect) instead. Left
    // unregistered, GetMethodID returns NULL -- but unlike Corrida 40/50's
    // crash-prone strcpy(NULL) call sites, jGetStringWidth is written
    // defensively enough not to crash on that: it unconditionally calls
    // CallIntMethod(env, obj, NULL, ...) anyway, and FalsoJNI's
    // methodIntCall() (lib/falso_jni/FalsoJNI_ImplBridge.c) falls through its
    // "not found" loop to `return -1;` (NOT 0 -- confirmed by reading that
    // function directly, unlike methodObjectCall/methodVoidCall which have
    // different fallbacks). That -1 propagates straight back out as a
    // "measured text width" of -1 to whichever caller uses it to size a
    // background/highlight rect BEFORE the actual text glyphs are drawn.
    // Since the glyphs themselves are sized and drawn through the OTHER,
    // correctly-registered id 46/49 pair (SGL_grpCreateStringImageInRect's
    // own string-image object, which never touches this value), the net
    // effect for any UI element that sizes its background via THIS call path
    // instead of SGL_GetTextSizeInRect's is exactly "text renders fine, its
    // background box does not" -- the most concrete, decompile-supported
    // candidate found for the "some texts have no background" report. NOT
    // confirmed against a real hardware log yet -- see GetStringWidth_stub's
    // own comment and PORTING_PLAN.md's Corrida 60 entry for the full
    // writeup and what's still open.
    { 50, "GetStringWidth", METHOD_TYPE_INT },
};

MethodsBoolean methodsBoolean[] = {};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};

jint HubInitialize_stub(jmethodID id, va_list args) {
    l_info("[Java] HubInitialize called -- no-op (no Com2uS Hub in this port), returning 0.");
    return 0;
}

jint GetRealScreenWidth_stub(jmethodID id, va_list args) {
    return 960;
}

jint GetRealScreenHeight_stub(jmethodID id, va_list args) {
    return 544;
}

jint JSON_getInt_stub(jmethodID id, va_list args) {
    return 0;
}

jint JSON_length_stub(jmethodID id, va_list args) {
    return 0;
}

jint GetScreenWidth_stub(jmethodID id, va_list args) {
    return 960;
}

jint GetScreenHeight_stub(jmethodID id, va_list args) {
    return 544;
}

jlong GetMaxMemory_stub(jmethodID id, va_list args) {
    return 256 * 1024 * 1024;
}

// Corrida 44: per-byte width table replicated from this port's OWN native
// bitmap-font metric, FONT_GetSubStringWidth (out_ghidra.c:19780-19828) --
// space=4px, a high-bit-set/multibyte lead byte=9px (consumes 2 bytes), any
// other byte=6px. Copied directly rather than calling into the .so: it's a
// trivial, self-contained per-byte cost table with no dependency on
// FONT_pFontData or any other .so-internal state.
static int inotia3_char_width(const unsigned char *s, size_t i, size_t len, size_t *advance) {
    if (s[i] == ' ') {
        *advance = 1;
        return 4;
    }
    if (s[i] & 0x80) {
        *advance = (i + 1 < len) ? 2 : 1;
        return 9;
    }
    *advance = 1;
    return 6;
}

// Corrida 44/59: shared greedy word-wrap, O(n) single pass, preferring to
// break at the last whitespace seen on the current line (mirrors the real
// Java implementation's own word-boundary preference) when one is available.
// Originally private to GetStringWidthHeightInRect_stub; factored out in
// Corrida 59 so DrawTextInRect_stub's actual glyph rendering walks EXACTLY
// the same lines GetStringWidthHeightInRect_stub already measured -- the
// engine calls the two back to back and assumes they agree (SGL_Font_Android
// ::RenderToTextureInRect sizes its texture from the first call's result,
// then hands that same rect to the second).
static const int INOTIA3_LINE_HEIGHT = 16;

static void inotia3_wrap_lines(const unsigned char *str, size_t len, int rectWidth,
                                void (*on_line)(void *ctx, size_t start, size_t end, int width),
                                void *ctx) {
    if (rectWidth <= 0) rectWidth = 1;
    size_t i = 0;
    size_t line_start = 0;
    int line_width = 0;
    size_t last_space = (size_t) -1;
    int width_at_last_space = 0;

    while (i < len) {
        size_t advance;
        int cw = inotia3_char_width(str, i, len, &advance);

        if (str[i] == '\n') {
            on_line(ctx, line_start, i, line_width);
            i++;
            line_start = i;
            line_width = 0;
            last_space = (size_t) -1;
            continue;
        }

        if (line_width + cw > rectWidth && i > line_start) {
            int have_space = (last_space != (size_t) -1 && last_space > line_start);
            size_t break_at = have_space ? last_space : i;
            int width_used = have_space ? width_at_last_space : line_width;
            on_line(ctx, line_start, break_at, width_used);
            i = break_at;
            while (i < len && str[i] == ' ') i++;
            line_start = i;
            line_width = 0;
            last_space = (size_t) -1;
            continue;
        }

        if (str[i] == ' ') {
            last_space = i;
            width_at_last_space = line_width;
        }
        line_width += cw;
        i += advance;
    }
    if (i > line_start) {
        on_line(ctx, line_start, i, line_width);
    }
}

typedef struct {
    int max_width;
    int total_height;
} inotia3_measure_ctx_t;

static void inotia3_measure_line_cb(void *ctx_, size_t start, size_t end, int width) {
    (void) start;
    (void) end;
    inotia3_measure_ctx_t *ctx = (inotia3_measure_ctx_t *) ctx_;
    if (width > ctx->max_width) ctx->max_width = width;
    ctx->total_height += INOTIA3_LINE_HEIGHT;
}

// WrapperJinterface.GetStringWidthHeightInRect(String, int, int, int, int,
// int[]): int -- see the NameToMethodID comment (Corrida 44) for why this
// needs a real implementation, not just a safe default. fontSize/fontStyle/
// isBreak are ignored -- this port's bitmap font has one fixed per-byte
// metric, not a scalable one, and both of the real isBreak modes end up
// wrapping on width with a whitespace preference anyway.
jint GetStringWidthHeightInRect_stub(jmethodID id, va_list args) {
    jstring str_obj = va_arg(args, jstring);
    va_arg(args, jint); // fontSize
    va_arg(args, jint); // fontStyle
    jint rectWidth = va_arg(args, jint);
    va_arg(args, jint); // isBreak
    jintArray value_arr = va_arg(args, jintArray);

    if (!str_obj || !value_arr) return 0;

    const unsigned char *str = (const unsigned char *) jni->GetStringUTFChars(&jni, str_obj, NULL);
    jint *value = jni->GetIntArrayElements(&jni, value_arr, NULL);

    size_t len = strlen((const char *) str);
    if (rectWidth <= 0) rectWidth = 1;

    inotia3_measure_ctx_t ctx = {0, 0};
    inotia3_wrap_lines(str, len, rectWidth, inotia3_measure_line_cb, &ctx);

    int max_width = ctx.max_width > rectWidth ? rectWidth : ctx.max_width;
    value[0] = max_width;
    value[1] = ctx.total_height;

    jni->ReleaseStringUTFChars(&jni, str_obj, (char *) str);
    return max_width;
}

// Corrida 60: WrapperJinterface.GetStringWidth(String, int, int): int -- see
// the NameToMethodID comment above (id 50) for the full call-chain writeup.
// A flat, single-line measurement (the real Java implementation is one
// paint.measureText() call, no wrapping) using the SAME per-byte metric table
// as GetStringWidthHeightInRect_stub/DrawTextInRect_stub (inotia3_char_width)
// for the same reason those do: this port's bitmap font has one fixed
// per-byte metric, not a scalable one, so fontSize/fontStyle are accepted
// (to match the real signature) but otherwise ignored.
jint GetStringWidth_stub(jmethodID id, va_list args) {
    jstring str_obj = va_arg(args, jstring);
    va_arg(args, jint); // fontSize
    va_arg(args, jint); // fontStyle

    if (!str_obj) return 0;

    const unsigned char *str = (const unsigned char *) jni->GetStringUTFChars(&jni, str_obj, NULL);
    size_t len = strlen((const char *) str);

    int width = 0;
    for (size_t i = 0; i < len;) {
        size_t advance;
        width += inotia3_char_width(str, i, len, &advance);
        i += advance;
    }

    jni->ReleaseStringUTFChars(&jni, str_obj, (char *) str);
    return width;
}

// Corrida 59: DrawTextInRect never being registered was the root cause of
// EVERY piece of dynamically-drawn text in this port being invisible (menus,
// HP, item names, save-slot text, dialogues) -- not just Hub-style rich text
// as first suspected. Confirmed by tracing the real call chain in
// out_ghidra.c: GRPX_DrawStringWithFont/GRPX_DrawString (the engine's ONLY
// general-purpose text draw, used everywhere) -> SGL_grpDrawStringWithFont/
// SGL_grpDrawStringInRectWithFontEx -> SGL_grpCreateStringImageInRect ->
// SGL_Texture_Text::SetTextInRect -> SGL_Font_Android::RenderToTextureInRect
// (:161897) -> SGL_RenderFontToAlphaImageInRect (:162636), which does:
//   jDrawTextInRect(text, len, fontSize, 0, align, rectWidth, isBreak);
//   puVar2 = jGetPixelBufferPointer();  // == DAT_0017dea4, cached ONCE
//   for each row/col: *(alpha_dest) = (byte)(*puVar2++ >> 24);  // ARGB->A
// jGetPixelBufferPointer() (out_ghidra.c:144072) is NOT a JNI call -- it just
// returns a raw pointer the .so cached during nativeInit() from
// GetIntArrayElements() on the `pixel[]` array main.c passes in (confirmed:
// nativeInit's 3rd array arg lands in that exact global). That means
// DrawTextInRect doesn't need to hand back a Bitmap/int[] through any new
// JNI plumbing at all -- writing glyphs directly into
// g_textview_pixel_buffer (main.c's own live pointer to that SAME array) IS
// the render; the .so reads it back moments later through the pointer it
// already has.
//
// Renders with THIS port's own bitmap font (FONT_pFontData, already loaded
// by FONT_Load -- see Corrida 42) rather than a real TrueType rasterizer:
// same glyph lookup GRP_PutAsciiChar uses (out_ghidra.c:20971-20999) -- 94
// printable ASCII glyphs (0x21-0x7E) at `FONT_pFontData + 0xb10 +
// (ch-0x21)*0xc`, 12 bytes/glyph, rows 1-9 of each record (row 0 and 10/11
// are metrics this port doesn't need -- GRP_PutAsciiChar itself never reads
// them either), 5 columns per row in bits 7-3, bit CLEAR ("0") means ink
// (confirmed: the default/space glyph is memset to 0xFF, all bits set, and
// draws nothing). Writes full-white ARGB (0xFFFFFFFF) for ink pixels --
// SGL_RenderFontToAlphaImageInRect only ever reads the alpha byte (bits
// 31-24), and the engine's real Paint-based renderer draws pure white too
// (WrapperJinterface.DrawTextInRect's real Java sets ARGB 255/255/255/255
// before drawing) -- the actual on-screen color comes from GRPX_SetFontColor/
// SGL_Texture_Text's OWN tinting downstream, unaffected by this change.
// Multi-byte/high-bit bytes are skipped (this font has no glyphs for them,
// same as the real GRP_PutAsciiChar) rather than guessed at.
//
// The write stride is `rectWidth` (this call's own parameter), NOT
// g_textview_width -- confirmed from the read side: SGL_RenderFontToAlphaImageInRect
// advances its read pointer by exactly the width it was given
// (`puVar2 = puVar2 + param_6`), and that width is the SAME value
// SGL_Font_Android::RenderToTextureInRect computed (measured text width, or
// the original rect width if isBreak) and threaded through unchanged as
// jDrawTextInRect's own rectWidth argument -- so writing with any other
// stride would desync from what gets read back. Bounds-checked against the
// buffer's real capacity (g_textview_width * g_textview_height ints)
// regardless, since nothing enforces that assumption from our side.
typedef struct {
    const unsigned char *str;
    size_t len;
    int rect_width;
    int align; // 0 = left, 1 = center, 2 = right
    int y;
} inotia3_draw_ctx_t;

static void inotia3_draw_glyph(unsigned char ch, int x, int y, int stride) {
    if (!g_textview_pixel_buffer) return;
    if (ch < 0x21 || ch > 0x7E) return; // space and non-ASCII: no glyph, draw nothing
    unsigned glyph_idx = (unsigned) (ch - 0x21);
    if (glyph_idx >= 0x5E) return;

    static uintptr_t font_data_slot = 0;
    if (!font_data_slot) font_data_slot = so_symbol(&so_mod, "FONT_pFontData");
    if (!font_data_slot) return;
    uint32_t font_base = *(uint32_t *) font_data_slot;
    if (!font_base) return;

    const uint8_t *glyph = (const uint8_t *) (uintptr_t) font_base + 0xb10 + glyph_idx * 0xc;
    long buffer_capacity = (long) g_textview_width * (long) g_textview_height;

    for (int row = 0; row < 9; row++) {
        int py = y + row;
        if (py < 0) continue;
        uint8_t bits = glyph[1 + row];
        for (int col = 0; col < 5; col++) {
            if (((bits >> (7 - col)) & 1) != 0) continue; // bit set = background
            int px = x + col;
            if (px < 0 || px >= stride) continue;
            long idx = (long) py * (long) stride + (long) px;
            if (idx < 0 || idx >= buffer_capacity) continue;
            g_textview_pixel_buffer[idx] = (jint) 0xFFFFFFFFu;
        }
    }
}

static void inotia3_draw_line_cb(void *ctx_, size_t start, size_t end, int width) {
    inotia3_draw_ctx_t *ctx = (inotia3_draw_ctx_t *) ctx_;

    int x0 = 0;
    if (ctx->align == 1) x0 = (ctx->rect_width - width) / 2;
    else if (ctx->align == 2) x0 = ctx->rect_width - width;
    if (x0 < 0) x0 = 0;

    int x = x0;
    for (size_t i = start; i < end;) {
        size_t advance;
        int cw = inotia3_char_width(ctx->str, i, end, &advance);
        inotia3_draw_glyph(ctx->str[i], x, ctx->y + 3, ctx->rect_width);
        x += cw;
        i += advance;
    }
    ctx->y += INOTIA3_LINE_HEIGHT;
}

jint DrawTextInRect_stub(jmethodID id, va_list args) {
    jbyteArray s_obj = va_arg(args, jbyteArray);
    va_arg(args, jint); // fontSize
    va_arg(args, jint); // fontStyle
    jint align = va_arg(args, jint);
    jint rectWidth = va_arg(args, jint);
    va_arg(args, jint); // isBreak

    if (!s_obj || !g_textview_pixel_buffer) return 0;
    if (rectWidth <= 0) rectWidth = 1;

    jsize len = jni->GetArrayLength(&jni, s_obj);
    jbyte *bytes = jni->GetByteArrayElements(&jni, s_obj, NULL);
    if (!bytes) return 0;

    // g_textview_pixel_buffer is one shared, reused-every-call scratch area
    // (always written starting at index 0, row-major) -- without clearing
    // it first, ink pixels left behind by a PREVIOUS, longer/taller string
    // would bleed into whatever the caller reads back for THIS shorter one,
    // since both draws start at the same buffer offsets. The real Android
    // Canvas has the same shared-bitmap shape but starts fully transparent
    // each time implicitly (a fresh region of an ARGB_8888 bitmap Android
    // never explicitly repaints outside of drawText's own glyph coverage) --
    // measure first (same wrap, so the cleared region always matches what
    // this call is about to draw) to know how tall a region to zero.
    inotia3_measure_ctx_t measure_ctx = {0, 0};
    inotia3_wrap_lines((const unsigned char *) bytes, (size_t) len, rectWidth, inotia3_measure_line_cb,
                        &measure_ctx);
    long clear_pixels = (long) rectWidth * (long) measure_ctx.total_height;
    long buffer_capacity = (long) g_textview_width * (long) g_textview_height;
    if (clear_pixels > buffer_capacity) clear_pixels = buffer_capacity;
    if (clear_pixels > 0) memset(g_textview_pixel_buffer, 0, (size_t) clear_pixels * sizeof(jint));

    inotia3_draw_ctx_t ctx;
    ctx.str = (const unsigned char *) bytes;
    ctx.len = (size_t) len;
    ctx.rect_width = rectWidth;
    ctx.align = align;
    ctx.y = 0;
    inotia3_wrap_lines((const unsigned char *) bytes, (size_t) len, rectWidth, inotia3_draw_line_cb, &ctx);

    jni->ReleaseByteArrayElements(&jni, s_obj, bytes, 0);
    return 0;
}

// WrapperJinterface.Flush() -> wrapperRenderer.Flush() -> GLSurfaceView.
// requestRender() under WrapperDirtyRenderer (see wr_timer.h Corrida 31): the
// .so calling this is the ONLY signal that the frame it just drew should
// actually be presented. Was a pure no-op until Corrida 31 -- see wr_timer.h.
void Flush_stub(jmethodID id, va_list args) {
    wr_request_present();
}

// --- Audio wrappers -----------------------------------------------------
// Corrida 63 (PORTING_PLAN.md): re-wired to the real Tremor-backed playback
// in source/audio.c/.h. These 9 ids were deliberately disconnected (wired to
// no-ops instead) starting around Corrida 33 to rule the audio/mixer thread
// out while triaging the Fase 9 stack-protector crash in
// png_create_read_struct_2 -- that crash was root-caused in Corrida 47 to an
// unrelated sign-bit bug in RES_MoveToFilePack (already fixed), so there is
// no longer a known reason to keep audio disconnected. See PORTING_PLAN.md
// Corrida 63 for the full note and remaining verification caveats.

// Resolves the logical name libgame.so passes to SoundCreate into a real
// on-disk path, mirroring GetFileDescriptor_stub's loose-asset convention
// above: assets are staged loose under INOTIA3_LOOSE_ASSETS_PATH keeping
// their original APK "disguise" suffix (INOTIA3_RES_NAME_APPEND, ".jpg" on
// what is actually a plain OggS/Vorbis file -- see audio.h), and the code
// re-appends that suffix itself rather than requiring any asset-staging
// script to strip it (audio.h's own header comment says stripping is an
// "asset-preparation concern" -- that turned out to be unnecessary and,
// worse, inconsistent with how the already-working image path above
// resolves names: it also keeps the ".jpg" suffix on disk and re-appends it
// in code, never strips it). Tries multiple candidates and logs which one
// hit (if any), since the exact string libgame.so passes here hasn't been
// confirmed against out_ghidra.c in this environment (no decompiled_so
// checkout available) -- see PORTING_PLAN.md Corrida 63.
static int inotia3_resolve_sound_path(const char *name, char *out, size_t out_size) {
    if (!name || !name[0] || out_size == 0) return 0;

    struct stat st;

    // Candidate 1: verbatim -- already absolute, or already fully resolved
    // by whatever built the string on the .so side.
    if (stat(name, &st) == 0) {
        strncpy(out, name, out_size - 1);
        out[out_size - 1] = '\0';
        return 1;
    }

    // Candidate 2: loose-assets dir + name as-is (name may already carry
    // ".ogg" or even the full disguise ".ogg.jpg" suffix).
    snprintf(out, out_size, "%s%s", INOTIA3_LOOSE_ASSETS_PATH, name);
    if (stat(out, &st) == 0) return 1;

    // Candidate 3: loose-assets dir + name + disguise suffix -- same
    // convention GetFileDescriptor_stub uses for images.
    snprintf(out, out_size, "%s%s%s", INOTIA3_LOOSE_ASSETS_PATH, name, INOTIA3_RES_NAME_APPEND);
    if (stat(out, &st) == 0) return 1;

    out[0] = '\0';
    return 0;
}

// SoundCreate(String path, boolean loop): int -- (Ljava/lang/String;Z)I
jint SoundCreate_wrapper(jmethodID id, va_list args) {
    jstring name_obj = va_arg(args, jstring);
    jint loop = va_arg(args, jint); // jboolean is promoted to int in varargs

    if (!name_obj) return 0;
    const char *name = jni->GetStringUTFChars(&jni, name_obj, NULL);

    char resolved[256];
    jint handle = 0;
    if (inotia3_resolve_sound_path(name, resolved, sizeof(resolved))) {
        handle = audio_sound_create(resolved, loop);
        game_log("[Audio] SoundCreate(\"%s\") -> \"%s\", handle %d", name, resolved, handle);
    } else {
        game_log("[Audio] SoundCreate(\"%s\"): could not resolve to a real file under %s "
                  "(tried verbatim, %s<name>, %s<name>%s)",
                  name, INOTIA3_LOOSE_ASSETS_PATH, INOTIA3_LOOSE_ASSETS_PATH,
                  INOTIA3_LOOSE_ASSETS_PATH, INOTIA3_RES_NAME_APPEND);
    }

    jni->ReleaseStringUTFChars(&jni, name_obj, (char *) name);
    return handle;
}

// SoundDestroy(int handle): int -- (I)I
jint SoundDestroy_wrapper(jmethodID id, va_list args) {
    jint handle = va_arg(args, jint);
    return audio_sound_destroy(handle);
}

// SoundPlay(int handle, boolean loop): int -- (IZ)I
jint SoundPlay_wrapper(jmethodID id, va_list args) {
    jint handle = va_arg(args, jint);
    jint loop = va_arg(args, jint);
    return audio_sound_play(handle, loop);
}

// SoundPause(int handle): int -- (I)I
jint SoundPause_wrapper(jmethodID id, va_list args) {
    jint handle = va_arg(args, jint);
    return audio_sound_pause(handle);
}

// SoundResume(int handle): int -- (I)I
jint SoundResume_wrapper(jmethodID id, va_list args) {
    jint handle = va_arg(args, jint);
    return audio_sound_resume(handle);
}

// SoundStop(int handle): int -- (I)I
jint SoundStop_wrapper(jmethodID id, va_list args) {
    jint handle = va_arg(args, jint);
    return audio_sound_stop(handle);
}

// SoundGetVolume(int handle): int -- (I)I
jint SoundGetVolume_wrapper(jmethodID id, va_list args) {
    jint handle = va_arg(args, jint);
    return audio_sound_get_volume(handle);
}

// SoundSetVolume(int handle, int volume): int -- (II)I
jint SoundSetVolume_wrapper(jmethodID id, va_list args) {
    jint handle = va_arg(args, jint);
    jint volume = va_arg(args, jint);
    return audio_sound_set_volume(handle, volume);
}

// SoundPrepare(int handle): int -- (I)I
jint SoundPrepare_wrapper(jmethodID id, va_list args) {
    jint handle = va_arg(args, jint);
    return audio_sound_prepare(handle);
}

MethodsInt methodsInt[] = {
    { 1, HubInitialize_stub },
    { 11, GetRealScreenWidth_stub },
    { 12, GetRealScreenHeight_stub },
    { 16, JSON_getInt_stub },
    { 18, JSON_length_stub },
    { 27, SoundCreate_wrapper },
    { 28, SoundDestroy_wrapper },
    { 29, SoundPlay_wrapper },
    { 30, SoundPause_wrapper },
    { 31, SoundResume_wrapper },
    { 32, SoundStop_wrapper },
    { 33, SoundGetVolume_wrapper },
    { 34, SoundSetVolume_wrapper },
    { 35, SoundPrepare_wrapper },
    { 36, GetScreenWidth_stub },
    { 37, GetScreenHeight_stub },
    { 46, GetStringWidthHeightInRect_stub },
    { 49, DrawTextInRect_stub },
    { 50, GetStringWidth_stub },
};
MethodsLong methodsLong[] = {
    { 38, GetMaxMemory_stub },
};

jobject GetUserDir_stub(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "ux0:data/");
}

jobject GetPackageName_stub(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "inotia3");
}

jobject GetJSONArray_stub(jmethodID id, va_list args) {
    return (jobject)0x42424242; // Dummy object
}

jobject GetString_stub(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "");
}

jobject GetCom2usUDID_stub(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "00000000-0000-0000-0000-000000000000");
}

jobject GetCountry_stub(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "US");
}

jobject GetLanguage_stub(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "en");
}

jobject CC_MD5_stub(jmethodID id, va_list args) {
    jbyteArray arr = jni->NewByteArray(&jni, 16);
    return arr;
}

jobject GetJSONObject_stub(jmethodID id, va_list args) {
    return (jobject)0x42424242; // Dummy object for JSONObject
}

jobject HttpPost_init_stub(jmethodID id, va_list args) {
    jstring url_obj = va_arg(args, jstring);
    if (url_obj) {
        const char *url = jni->GetStringUTFChars(&jni, url_obj, NULL);
        game_log("[HTTP] HttpPost requested URL: %s", url);
        jni->ReleaseStringUTFChars(&jni, url_obj, (char *)url);
    }
    return (jobject)0x42424242; // Dummy object
}

// GetFileDescriptor_stub (below) resolves an asset to either a loose file
// (offset 0, no .apk involved) or a real offset into INOTIA3_APK_PATH. Since
// GetApkFilePath() is called separately, with no argument tying it to which
// asset was just resolved, this one-shot flag is how the two calls stay in
// sync -- set right before returning from GetFileDescriptor_stub, consumed
// (and cleared) by the very next GetApkFilePath_stub call, matching the
// call order confirmed in every real device log so far.
static char g_pending_asset_path[256];
static int g_pending_asset_is_loose = 0;

jobject GetApkFilePath_stub(jmethodID id, va_list args) {
    if (g_pending_asset_is_loose) {
        g_pending_asset_is_loose = 0;
        return jni->NewStringUTF(&jni, g_pending_asset_path);
    }
    return jni->NewStringUTF(&jni, INOTIA3_APK_PATH);
}

// Corrida 37: jGetApkFilePath() (the real .so function, not this JNI stub)
// caches its own return value the FIRST time it's ever called and never asks
// Java again (device log confirmed: the "GetApkFilePath" JNI call fires
// exactly once, for the very first asset resolved -- every later asset's
// call into jGetApkFilePath_real() just replays that first cached string).
// File_GetFullPathName_hook's mode==2 branch (source/patch.c) used to build
// its "@offset|length|path" string from that call -- which meant every asset
// after the first got the FIRST asset's path baked into its own permanently
// cached ResourceInformation struct, truncating/misreading it later. This
// exposes the SAME decision GetApkFilePath_stub above makes -- without the
// one-shot consumption -- so the hook can read the CURRENT asset's real path
// directly (fresh every call, since it's set synchronously by
// GetFileDescriptor_stub just above, for the same `name`, moments earlier in
// the same File::GetFullPathName(mode=2) call) instead of trusting the .so's
// stale cache.
const char *java_peek_current_asset_path(void) {
    return g_pending_asset_is_loose ? g_pending_asset_path : INOTIA3_APK_PATH;
}

// WrapperJinterface.GetFileDescriptor(String filename, int[] value, boolean
// closeFDdirectly): Object -- decompiled_so's jGetFileDescriptorEx() confirms
// the reachable call site (CS_fsGetFileDescriptor) always passes
// closeFDdirectly=1 and only reads the offset/length pair back out of
// `value`, never the returned object -- so filling that array correctly and
// returning NULL is exactly what the real Java implementation does too in
// that path (WrapperJinterface.java: fd=null when closeFDdirectly). See
// apk_zip.c for why the actual zip entry name needs ".jpg" appended.
jobject GetFileDescriptor_stub(jmethodID id, va_list args) {
    jstring name_obj = va_arg(args, jstring);
    jintArray value_arr = va_arg(args, jintArray);
    va_arg(args, jint); // closeFDdirectly -- always true at the only reachable call site

    if (!name_obj || !value_arr) return 0;

    const char *name = jni->GetStringUTFChars(&jni, name_obj, NULL);
    jint *value = jni->GetIntArrayElements(&jni, value_arr, NULL);

    char loose_path[256];
    snprintf(loose_path, sizeof(loose_path), "%s%s%s",
             INOTIA3_LOOSE_ASSETS_PATH, name, INOTIA3_RES_NAME_APPEND);

    struct stat st;
    if (stat(loose_path, &st) == 0) {
        value[0] = 0;
        value[1] = (jint) st.st_size;
        strncpy(g_pending_asset_path, loose_path, sizeof(g_pending_asset_path) - 1);
        g_pending_asset_path[sizeof(g_pending_asset_path) - 1] = '\0';
        g_pending_asset_is_loose = 1;
        l_debug("[Assets] GetFileDescriptor(\"%s\") -> loose file %s (length=%ld)",
                name, loose_path, (long) st.st_size);
    } else {
        char entry_path[256];
        snprintf(entry_path, sizeof(entry_path), "assets/%s/%s%s",
                 INOTIA3_GAME_ASSETS_PATH, name, INOTIA3_RES_NAME_APPEND);

        uint32_t offset = 0, length = 0;
        int found = apk_zip_find_stored_entry(INOTIA3_APK_PATH, entry_path, &offset, &length);
        g_pending_asset_is_loose = 0;

        if (found == 0) {
            value[0] = (jint) offset;
            value[1] = (jint) length;
            l_debug("[Assets] GetFileDescriptor(\"%s\") -> apk offset=%u length=%u", name, offset, length);
        } else {
            value[0] = -1;
            value[1] = -1;
            l_warn("[Assets] GetFileDescriptor(\"%s\") -> \"%s\" not found as a loose file (%s) or in the apk (%s)",
                   name, loose_path, entry_path, INOTIA3_APK_PATH);
        }
    }

    jni->ReleaseStringUTFChars(&jni, name_obj, (char *) name);
    return 0;
}

// Corrida 69 (so-crash-triage, save-load crash) / Corrida 72 (reverted --
// so-crash-triage, save disappeared): WrapperJinterface.GetAppProperty
// (confirmed via jGetAppProperty, out_ghidra.c:146811, real signature
// "([B)[B", byte[] in/out) is called with exactly 3 keys (grepped every
// CS_hlpGetAppProperty call site): "uniqueUserId", "uniqueDeviceId",
// "sessionKey". Corrida 69 changed this from a fixed non-empty 12-byte
// payload ("123456789012") to an empty array, on the hypothesis that
// DownloadGameData (out_ghidra.c:16260) gating a Hub cloud-download call on
// `buf[0] != '\0'` was the cause of a save-load crash. Corrida 72 found the
// REAL, byte-exact-confirmed cause of that crash was unrelated (SAVE_Load's
// own PLAYER_pActivePlayer NULL-deref, fixed via SAVE_Load_hook below/
// elsewhere in this file's history) -- the DownloadGameData angle was never
// more than a plausible-looking stack-scan artifact. Worse, this same
// "uniqueUserId" value is what HubSave_LoadKey() (out_ghidra.c:124296) uses
// to compute the on-disk save subdirectory (HubSave_GetFolderName ->
// MD5(HubSave_key)) -- making it empty changed that subdirectory's hash from
// MD5("123456789012") to MD5(""), silently orphaning every save written
// under the old value. Reverted to the original stable non-empty payload:
// the save-path stability this is actually relied on for matters more than
// the unconfirmed Hub-download hypothesis, and that path is closed properly
// now anyway via a direct no-op hook on CS_hubDownloadGameData/
// CS_hubUploadCompleteGameData/CS_hubUploadGameData in source/patch.c
// (Corrida 72) -- the right layer to neutralize it without depending on
// what this shared, multi-consumer JNI property call returns.
jobject GetAppProperty_stub(jmethodID id, va_list args) {
    jbyteArray arr = jni->NewByteArray(&jni, 12);
    if (arr) {
        jni->SetByteArrayRegion(&jni, arr, 0, 12, (jbyte *) "123456789012");
    }
    return arr;
}

jobject GetUDID_stub(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "00000000-0000-0000-0000-000000000000");
}

jobject GetPhoneNumber_stub(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "0000000000");
}

// Corrida 40: an empty string makes Extend_RequestNotice's own
// `strtok(acStack_f2c, "|")` return NULL immediately (nothing to tokenize),
// which short-circuits its `pcVar3 != NULL && strcmp(pcVar3,"OK")==0` check
// to false -- exactly the same "no notice today" path the real server
// produces whenever it doesn't reply with "OK|...". Faithful no-op, not a
// guess: never crashes, never fabricates a notice popup.
jobject GetNoticeHttp_stub(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "");
}

// Corrida 50: see nameToMethodId[]'s entries 47/48 above -- HUB_InitString
// and its ~15 sibling call sites strcpy/strtok this result with no NULL
// check, so it must never be NULL. An empty string renders as blank
// Title/Message text on the Hub screen -- faithful no-op, not a guess.
jobject GetStringFromXmlEx_stub(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "");
}

jobject GetStringFromXml_stub(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "");
}

MethodsObject methodsObject[] = {
    { 7, GetUserDir_stub },
    { 8, GetUserDir_stub },
    { 9, GetPackageName_stub },
    { 10, GetPackageName_stub },
    { 15, GetJSONObject_stub },
    { 17, GetJSONArray_stub },
    { 19, GetString_stub },
    { 20, GetCom2usUDID_stub },
    { 21, GetCountry_stub },
    { 22, GetLanguage_stub },
    { 23, CC_MD5_stub },
    { 24, HttpPost_init_stub },
    { 39, GetFileDescriptor_stub },
    { 40, GetApkFilePath_stub },
    { 42, GetAppProperty_stub },
    { 43, GetUDID_stub },
    { 44, GetPhoneNumber_stub },
    { 45, GetNoticeHttp_stub },
    { 47, GetStringFromXmlEx_stub },
    { 48, GetStringFromXml_stub },
};
MethodsShort methodsShort[] = {};

void Inotia3_VoidNoop(jmethodID id, va_list args) {
}

// WrapperJinterface.SetTimer(int timerID, long firetime, long intervaltime).
// Signature "(IJJ)V": read one jint then two jlongs off the varargs.
void WrapperJinterface_SetTimer(jmethodID id, va_list args) {
    jint timerID = va_arg(args, jint);
    jlong firetime = va_arg(args, jlong);
    jlong intervaltime = va_arg(args, jlong);
    wr_set_timer(timerID, firetime, intervaltime);
}

// WrapperJinterface.UnsetTimer(int timerID). Signature "(I)V".
void WrapperJinterface_UnsetTimer(jmethodID id, va_list args) {
    jint timerID = va_arg(args, jint);
    wr_unset_timer(timerID);
}

MethodsVoid methodsVoid[] = {
    { 2, Inotia3_VoidNoop }, // alertPopup
    { 3, Inotia3_VoidNoop }, // setGameResolution
    { 41, Flush_stub },
    { 4, Inotia3_VoidNoop }, // adCreate
    { 5, Inotia3_VoidNoop }, // setAdProperty
    { 6, Inotia3_VoidNoop }, // newsBannerInit
    { 13, Inotia3_VoidNoop }, // SetSourceSize
    { 14, Inotia3_VoidNoop }, // SetViewPort
    { 25, WrapperJinterface_SetTimer },
    { 26, WrapperJinterface_UnsetTimer },
};

/*
 * JNI Fields
 */

// System-wide constant that applications sometimes request
// https://developer.android.com/reference/android/content/Context.html#WINDOW_SERVICE
char WINDOW_SERVICE[] = "window";

// System-wide constant that's often used to determine Android version
// https://developer.android.com/reference/android/os/Build.VERSION.html#SDK_INT
// Possible values: https://developer.android.com/reference/android/os/Build.VERSION_CODES
const int SDK_INT = 19; // Android 4.4 / KitKat

NameToFieldID nameToFieldId[] = {
        { 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
        { 1, "SDK_INT", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
        { 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
        { 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
