/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 */

#include <kubridge.h>
#include <so_util/so_util.h>
#include <falso_jni/FalsoJNI.h>

extern so_module so_mod;

// --- Anti-tamper bypass -----------------------------------------------------
//
// nativeInit() only bootstraps the game (startClet() -> registers g_MainTimer
// -> the whole timer-driven game loop) when BOTH excuteDirectly()==0 AND
// jIsCracked()==0. Both routines detect memory-hack / cracking tools by running
//   system("ps > ux0:data/inotia3/temp")
// and scanning the captured process list. On the Vita system() has no shell, so
// the temp file is never created, fopen() returns NULL, and both routines fall
// through to their "tampered" return value (1). That silently skips startClet()
// -> no main timer -> nothing ever renders -> black screen.
//
// jGetMemoryHackCheckType() is hardcoded to return 7 inside the .so, so the
// check can't be disabled from the JNI side either. excuteDirectly() also peeks
// at bionic FILE internals (__stream->_IO_read_base), which don't match our
// libc's layout, so satisfying the check by faking the file is unreliable.
//
// Both simply return "clean" here, which is always the correct answer for an
// offline single-player port. Addresses are the .so's static-symbol offsets
// (nm libgame.so); the low bit marks them as Thumb for hook_addr().
#define OFF_excuteDirectly 0x000ecf15
#define OFF_jIsCracked     0x000e981d

static int excuteDirectly_bypass(void) {
    return 0; // not tampered
}

static int jIsCracked_bypass(void) {
    return 0; // not cracked
}

void so_patch(void) {
    hook_addr(so_mod.text_base + OFF_excuteDirectly, (uintptr_t)&excuteDirectly_bypass);
    hook_addr(so_mod.text_base + OFF_jIsCracked, (uintptr_t)&jIsCracked_bypass);
    
    // The Com2uS library in this game relies on multiple static JavaVM pointers.
    // They are left uninitialized because JNI_OnLoad only initializes one copy.
    // We must manually initialize them to &jvm, otherwise all CS_* functions will crash.
    void **g_com2us_vm_1 = (void **)(so_mod.text_base + 0x16de24);
    *g_com2us_vm_1 = &jvm;
    
    void **g_com2us_vm_2 = (void **)(so_mod.text_base + 0x16de64);
    *g_com2us_vm_2 = &jvm;
}
