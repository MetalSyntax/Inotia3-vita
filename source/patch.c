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

#include "utils/logger.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

// --- Corrida 17 checkpoint: png_create_struct_2 -----------------------------
//
// __stack_chk_fail_soloader (source/reimpl/sys.c) proved the canary at
// png_create_read_struct_2's own sp+132 holds THIS invocation's png_ptr
// heap address by the time the function returns -- an isolated 4-byte
// overwrite with no adjacent collateral damage and no in-function code path
// that could produce it (verified against the full disassembly). This hook
// replaces png_create_struct_2 (the allocator png_create_read_struct_2 calls
// for both its own png_struct AND, elsewhere, the info_struct) with a
// faithful reimplementation -- verified against the real disassembly at
// 0x0010e75c: type==1 -> 844 bytes, type==2 -> 288 bytes, malloc() (the
// custom-allocator/mem_ptr!=0 branch is unused in every call site we've
// observed and just falls back to malloc with a warning) -- and adds ONE
// checkpoint read of the CALLER's sp+132 at the earliest possible point:
// `bl png_create_struct_2` is literally 2 instructions after the canary is
// stored, so if it's already wrong here the corruption is near-instant (and
// almost certainly external -- another thread, or a wild write from
// elsewhere); if it's fine here, the corruption happens later, somewhere in
// zbuf alloc / inflateInit_ / png_set_read_fn.
#define OFF_png_create_struct_2 0x0010e75d

extern void *__stack_chk_guard;

__attribute__((noinline, used))
static void *png_create_struct_2_impl(int type, void *mem_ptr, void *malloc_fn, uint32_t *caller_sp) {
    l_info("png_create_struct_2 checkpoint: caller_sp=%p canary_now=%08x guard_now=%08x type=%d mem_ptr=%p",
           caller_sp, caller_sp[33], (unsigned int)(uintptr_t)__stack_chk_guard, type, mem_ptr);

    size_t size;
    if (type == 2) size = 288;
    else if (type == 1) size = 844;
    else return 0;

    if (mem_ptr != 0) {
        l_warn("png_create_struct_2: unexpected custom mem_ptr=%p malloc_fn=%p, falling back to malloc",
               mem_ptr, malloc_fn);
    }

    void *result = malloc(size);
    if (result) memset(result, 0, size);
    return result;
}

// r3 is unused by png_create_struct_2's own (type, mem_ptr, malloc_fn) args,
// so stashing the caller's sp there costs nothing -- `bl` doesn't touch sp,
// so at entry here sp IS still exactly png_create_read_struct_2's own frame.
__attribute__((naked))
void png_create_struct_2_hook(void) {
    asm volatile(
        "mov r3, sp\n"
        "b png_create_struct_2_impl\n"
    );
}

void so_patch(void) {
    hook_addr(so_mod.text_base + OFF_excuteDirectly, (uintptr_t)&excuteDirectly_bypass);
    hook_addr(so_mod.text_base + OFF_jIsCracked, (uintptr_t)&jIsCracked_bypass);
    hook_addr(so_mod.text_base + OFF_png_create_struct_2, (uintptr_t)&png_create_struct_2_hook);

    // The Com2uS library in this game relies on multiple static JavaVM pointers.
    // They are left uninitialized because JNI_OnLoad only initializes one copy.
    // We must manually initialize them to &jvm, otherwise all CS_* functions will crash.
    void **g_com2us_vm_1 = (void **)(so_mod.text_base + 0x16de24);
    *g_com2us_vm_1 = &jvm;

    void **g_com2us_vm_2 = (void **)(so_mod.text_base + 0x16de64);
    *g_com2us_vm_2 = &jvm;
}
