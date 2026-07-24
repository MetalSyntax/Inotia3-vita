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

#include "utils/logger.h"
#include "wr_timer.h"
#include "reimpl/apk_zip.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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
    // keeps and passes to the others. Stubbed as no-ops for now (Fase 7 audio),
    // but SoundCreate hands back a unique nonzero handle so the engine treats
    // every sound as "loaded" and doesn't retry loading it every frame.
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

void Flush_stub(jmethodID id, va_list args) {
    // do nothing
}

// --- Audio no-op stubs -------------------------------------------------------
// Fase 7's real Tremor-backed playback (source/audio.c/.h) is temporarily
// disconnected (not deleted) to eliminate the audio subsystem/mixer thread
// as a variable while triaging the Fase 9 stack-protector crash in
// png_create_read_struct_2 -- see PORTING_PLAN.md. Re-wire these back to
// audio_sound_*() once that crash is resolved.
static jint g_next_sound_handle = 1;

jint SoundCreate_stub(jmethodID id, va_list args) {
    jstring name_obj = va_arg(args, jstring);
    if (name_obj) {
        const char *name = jni->GetStringUTFChars(&jni, name_obj, NULL);
        game_log("[Audio] SoundCreate(\"%s\") -> handle %d (no-op)", name, g_next_sound_handle);
        jni->ReleaseStringUTFChars(&jni, name_obj, (char *)name);
    }
    return g_next_sound_handle++;
}

jint Sound_NoopOk(jmethodID id, va_list args) {
    return 0;
}

jint SoundGetVolume_stub(jmethodID id, va_list args) {
    return 100;
}

MethodsInt methodsInt[] = {
    { 1, HubInitialize_stub },
    { 11, GetRealScreenWidth_stub },
    { 12, GetRealScreenHeight_stub },
    { 16, JSON_getInt_stub },
    { 18, JSON_length_stub },
    { 27, SoundCreate_stub },
    { 28, Sound_NoopOk },    // SoundDestroy
    { 29, Sound_NoopOk },    // SoundPlay
    { 30, Sound_NoopOk },    // SoundPause
    { 31, Sound_NoopOk },    // SoundResume
    { 32, Sound_NoopOk },    // SoundStop
    { 33, SoundGetVolume_stub },
    { 34, Sound_NoopOk },    // SoundSetVolume
    { 35, Sound_NoopOk },    // SoundPrepare
    { 36, GetScreenWidth_stub },
    { 37, GetScreenHeight_stub },
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

jobject GetAppProperty_stub(jmethodID id, va_list args) {
    jbyteArray arr = jni->NewByteArray(&jni, 12);
    jbyte data[12] = "123456789012";
    jni->SetByteArrayRegion(&jni, arr, 0, 12, data);
    return arr;
}

jobject GetUDID_stub(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "00000000-0000-0000-0000-000000000000");
}

jobject GetPhoneNumber_stub(jmethodID id, va_list args) {
    return jni->NewStringUTF(&jni, "0000000000");
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
