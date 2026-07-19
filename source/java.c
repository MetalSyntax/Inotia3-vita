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

#include <falso_jni/FalsoJNI_Impl.h>

#include "utils/logger.h"

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

MethodsInt methodsInt[] = {
    { 1, HubInitialize_stub },
};
MethodsLong methodsLong[] = {};
MethodsObject methodsObject[] = {};
MethodsShort methodsShort[] = {};

void Inotia3_VoidNoop(jmethodID id, va_list args) {
}

MethodsVoid methodsVoid[] = {
    { 2, Inotia3_VoidNoop }, // alertPopup
    { 3, Inotia3_VoidNoop }, // setGameResolution
    { 4, Inotia3_VoidNoop }, // adCreate
    { 5, Inotia3_VoidNoop }, // setAdProperty
    { 6, Inotia3_VoidNoop }, // newsBannerInit
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
