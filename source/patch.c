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
#include "reimpl/jmp_shim.h"

#include <psp2/kernel/processmgr.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

// --- Corrida 19 checkpoint: png_error --------------------------------------
//
// Corrida 18 proved png_create_read_struct_2 itself finishes clean (canary
// intact through BOTH its setjmp() calls) -- the "Stack collapsed" crash
// chased since Corrida 5 is a downstream symptom, not the real bug: DecodePNG
// (out_ghidra.c:179667) never calls setjmp() itself after
// png_create_read_struct/png_create_info_struct/png_set_read_fn return, so
// the ONLY jmp_buf target still registered by the time it calls
// png_read_png() is the stale one png_create_read_struct_2 set up right
// before ITS OWN return. If png_read_png() ever calls png_error() for
// ANY reason, longjmp() unwinds into a function that has already returned --
// undefined behavior on any platform, not a Vita/libc-specific bug -- and
// whatever intervening call (png_create_info_struct, header parsing, ...)
// happened to reuse that exact stack address is what actually overwrites the
// stack-canary slot before control resumes there. png_error()'s own
// disassembly (0x0010dcf0, confirmed against out_ghidra.c:190532) proves it
// ALWAYS ends in `longjmp(png_ptr, 1)` regardless of any custom error_fn
// (called first, additionally, never instead) -- so hooking it to log the
// message and replicate just that longjmp is a faithful, safe reimplication:
// it changes no control flow, it only tells us WHICH real error is firing so
// we know why libpng considers this decode broken in our environment when it
// evidently isn't on the original Android build.
#define OFF_png_error 0x0010dcf1

static void png_error_hook(void *png_ptr, const char *message) {
    l_fatal("png_error(png_ptr=%p, \"%s\") -- Corrida 19 (PORTING_PLAN.md Fase 9): "
            "about to longjmp back into png_create_read_struct_2's already-returned frame",
            png_ptr, message);
    if (png_ptr != 0) {
        longjmp_soloader(png_ptr, 1);
    }
}

// --- Corrida 21: File::GetFullPathName mode 3 dead-end ----------------------
//
// Corrida 20 traced the "Not a PNG file" failure (Corridas 19/20) past
// libpng entirely: SGL_Texture::FromResource -> DecodePNG gets its buffer via
// SGL_Resource::FromBundle -> CS_knlGetResource, which ALWAYS opens its
// backing file with MMappedFile::Open(..., mode=3) (out_ghidra.c:157400,
// hardcoded literal `3` in the call, no fallback to any other mode) ->
// File::Open -> File::GetFullPathName(name, out, size, mode=3)
// (out_ghidra.c:174929).
//
// Mode 3 unconditionally builds "/sdcard/Android/data/<pkg>/files/<name>" --
// UNLESS `name` already starts with '/', in which case it's used literally
// (out_ghidra.c:174946). On real Android, `name` here is always
// jGetApkFilePath()'s cached, resolve-ONCE value (every asset lives in the
// same .apk, so it's resolved on the first call and reused for every
// resource afterwards -- confirmed by the "only resolve if the static buffer
// is still empty" caching inside jGetApkFilePath, out_ghidra.c:145992): a
// real absolute filesystem path (e.g. "/data/app/....apk"). That leading '/'
// always shortcuts past the (irrelevant, on Android) "/sdcard/..." branch,
// and the actual read happens via a direct mmap of that same real .apk file
// at the resource's own cached offset/length.
//
// This port's GetApkFilePath_stub (source/java.c) instead returns a loose
// Vita path ("ux0:data/inotia3/assets/.../MainMenu.png.jpg") for loose
// assets -- correct, and already openable as-is (confirmed: mode 2's own
// branch of THIS SAME function already opens it successfully, moments
// earlier in the log, while registering the resource) -- but it does NOT
// start with '/', so mode 3's leading-'/' shortcut never triggers, and
// CS_knlGetResource's MMappedFile::Open falls through to a doomed
// "/sdcard/Android/data/inotia3/files/ux0:data/..." concatenation that
// always fails to open (confirmed in the log:
// `open(/sdcard/.../ux0:data/..., 0): -1`). CS_knlGetResource never checks
// that failure before returning, so SGL_Resource::FromBundle hands DecodePNG
// a buffer that was allocated (CS_knlAlloc) but never filled -- explaining
// "Not a PNG file" with no corruption anywhere: the buffer was simply never
// written.
//
// Fix: reimplement File::GetFullPathName faithfully -- calling the .so's own
// jGetUserDir/jGetPackageName/jGetApkFilePath/CS_fsGetFileDescriptor for
// modes 0/2/3, exactly like the original -- with ONE change: the "already
// resolved, use literally" passthrough also matches any Vita
// device-prefixed path (contains ':', e.g. "ux0:...") in addition to a
// leading '/'. A colon is never valid inside a short Android resource name,
// so it's an unambiguous marker that `name` is already one of our own JNI
// stubs' fully-resolved paths, not a short logical name mode 3 needs to
// prefix.
#define OFF_File_GetFullPathName   0x000ff039
#define OFF_jGetUserDir            0x000ea7d9
#define OFF_jGetPackageName        0x000ea86d
#define OFF_CS_fsGetFileDescriptor 0x000f4c25

typedef char *(*fn_str_void_t)(void);
typedef int (*fn_get_fd_t)(const char *, int *, int);

// Corrida 37: see java.c's java_peek_current_asset_path() -- jGetApkFilePath
// (the real .so function) caches its own return value forever after its
// first call, so it can't be used here to build mode 2's path for anything
// but the very first asset ever resolved. Read the current asset's real path
// straight from our own JNI stub's state instead.
extern const char *java_peek_current_asset_path(void);

static int File_GetFullPathName_hook(const char *name, char *out, size_t out_size, int mode) {
    if (out == NULL || name == NULL || (unsigned int) mode >= 6 || out_size == 0) {
        return 0;
    }

    fn_str_void_t jGetUserDir_real = (fn_str_void_t)(so_mod.text_base + OFF_jGetUserDir);
    fn_str_void_t jGetPackageName_real = (fn_str_void_t)(so_mod.text_base + OFF_jGetPackageName);
    fn_get_fd_t CS_fsGetFileDescriptor_real = (fn_get_fd_t)(so_mod.text_base + OFF_CS_fsGetFileDescriptor);

    char buf[1024];
    memset(buf, 0, sizeof(buf));

    if (name[0] == '/' || strchr(name, ':') != NULL) {
        // Corrida 34: returning `name` literally here means File::Open's own
        // path (out_ghidra.c:175020, `if (local_128[0] == '@') { ...
        // ParseOffsetAndLengthAndPath(...) ... }`) never takes its '@'
        // branch for any loose (non-apk) asset -- that branch is the ONLY
        // place File::Open's offset/length fields get written at all. Skip
        // it and those fields keep whatever was already sitting in that
        // stack slot from an unrelated earlier call at similar stack depth
        // -- confirmed via a decompile trace: the first MainMenu.png load
        // read 0 there (a fresh/never-touched slot, so MMappedFile::Open's
        // own "length==0 -> refresh via File::GetSize()" fallback saved it),
        // but reopening it later -- after Title.png's resolution left ITS
        // length (110266) sitting in that exact slot -- read that leftover
        // garbage instead, truncating the mmap and breaking libpng's CRC
        // check. On real Android every resource lives in the one .apk, so
        // every path resolves through mode 2 below and ALWAYS takes the '@'
        // branch -- this stack-uninitialized-read bug in the .so is real but
        // structurally unreachable there. Fix: format the passthrough the
        // same way mode 2 already does (offset 0, real length via stat())
        // so File::Open takes the '@' branch and refreshes both fields on
        // every single open, matching what real Android's path shape does
        // for us for free.
        struct stat st;
        long length = (stat(name, &st) == 0) ? (long) st.st_size : 0;
        snprintf(buf, sizeof(buf), "@0|%ld|%s", length, name);
    } else if (mode == 2) {
        int values[2] = {0, 0};
        CS_fsGetFileDescriptor_real(name, values, 1);
        const char *apk_path = java_peek_current_asset_path();
        snprintf(buf, sizeof(buf), "@%d|%d|%s", values[0], values[1], apk_path ? apk_path : "");
    } else if (mode == 3) {
        char *pkg = jGetPackageName_real();
        snprintf(buf, sizeof(buf), "/sdcard/Android/data/%s/files/%s", pkg ? pkg : "", name);
    } else if (mode == 0) {
        char *user_dir = jGetUserDir_real();
        char *pkg = jGetPackageName_real();
        snprintf(buf, sizeof(buf), "%s%s/%s", user_dir ? user_dir : "", pkg ? pkg : "", name);
    }
    // modes 1/4/5: no known real branch either -- leave `buf` empty, matching
    // the real function's own fall-through behavior for those.

    size_t len = strlen(buf);
    if (len >= out_size) {
        out[0] = '\0';
        return 0;
    }
    memcpy(out, buf, len + 1);
    return 1;
}

// --- Corrida 28: is the CSFB software-framebuffer ever flushed? -------------
//
// Post-loop, every frame redraws two GL_RGB/GL_UNSIGNED_SHORT_5_6_5 textures
// (via RenderCoreRedraw -> RenderCoreRedraw320, glDrawArrays only, no upload)
// that our own glTexSubImage2D_soloader logging (source/utils/glutil.c)
// already proved are uploaded exactly twice -- both times with pixel_sample
// avg=0.0 (genuinely all-zero, not a logging bug: the real MainMenu.png RGBA
// upload in the same log samples correctly, avg=118.3) -- and never uploaded
// again for the rest of the run. The only code that re-uploads real content
// is CSFB_grpFlushLcd -> RenderCoreFlush -> RenderCoreFlush320's own
// glTexSubImage2D, reached only through CS_grpFlushLcd() -> `(*__CS_grpFlushLcd)()`
// (out_ghidra.c:155178-155181). A full-text search of the 214k-line decompile
// found ZERO call sites for CS_grpFlushLcd() anywhere -- so either it's never
// invoked at all in this port's code paths, or it's reached through some
// other function pointer/vtable slot Ghidra couldn't name. Both
// CS_grpFlushLcd and its backing global `__CS_grpFlushLcd` are exported in
// .dynsym (confirmed via `objdump -T`), so this hooks the 1-instruction
// dispatcher itself (`(*__CS_grpFlushLcd)()`) to log each invocation while
// faithfully preserving its only real behavior (dereference and call) --
// a fresh device log will show directly whether this fires at all, and if
// so, how often relative to the black-screen frames.
static void (**g_ptr_CS_grpFlushLcd)(void) = NULL;

static void CS_grpFlushLcd_hook(void) {
    static int count = 0;
    if (count < 30) {
        l_debug("[CSFB] CS_grpFlushLcd() called (#%d) target=%p", count + 1,
                g_ptr_CS_grpFlushLcd ? (void *) *g_ptr_CS_grpFlushLcd : NULL);
    }
    count++;
    if (g_ptr_CS_grpFlushLcd && *g_ptr_CS_grpFlushLcd) {
        (*g_ptr_CS_grpFlushLcd)();
    }
}

// --- Corrida 30: is the real sprite-flush path (GRP/GRPX/SGL, NOT CSFB)
// ever reached in steady state? ----------------------------------------------
//
// Corrida 29's device log confirmed CS_grpFlushLcd_hook above never fires,
// even once, through a full run that reaches and stays in the render loop --
// and a decompile-wide search confirmed that's not a port bug: CS_grpFlushLcd
// has zero call sites anywhere in the real .so either, and
// `__CS_grpFlushLcd` is only ever assigned CSFB_grpFlushLcd/CSOGL_grpFlushLcd
// inside InitializeApp() depending on jGetRenderer() (hardcoded to 0, always
// selecting the CSFB branch) -- CSFB is a dead end on real Android too, not
// something this port broke.
//
// The path that actually presents sprites is entirely separate:
// GRP_Flush() (out_ghidra.c:20404) -> GRPX_GetFlag() branches to either
// GRPX_FlushAll() -> SGL_grpFlushLcd(0,0,0,0,0,0) (:123810/123813) or
// GRPX_Flush() -> SGL_FrameBufferToScreen() (:123819/123822) -- the same
// SGL_FrameBufferToScreen already implicated in Fase 4's "reduced screen with
// white border" bug. GRP_Flush() itself has 10 real call sites in game code
// (out_ghidra.c:8845/15055/51211/104173/104626/109046/109619/112040/113120/
// 124017) -- if the game is presenting anything at all in steady state, it
// goes through here.
//
// Ghidra's own guessed prototype for SGL_grpFlushLcd (`void SGL_grpFlushLcd
// (void)`, out_ghidra.c:107287) disagrees with the 6-argument call site at
// :123813 -- a common Ghidra type-inference gap, not two different
// functions (same 0x34-byte symbol, one exported name). The call site's
// argument count is what the caller's registers actually populate, so the
// hook below matches that (6 args), not the guessed declaration -- passing
// extra unused args through SO_CONTINUE is harmless on AAPCS, passing too
// few would corrupt whatever the real body reads from r0-r3/stack.
//
// All 5 symbols are exported in `.dynsym` (confirmed via `objdump -T`), so
// no fixed offsets needed. Each hook restores the original instructions,
// calls straight through, then re-patches -- transparent logging, not a
// reimplementation. `SO_CONTINUE` (so_util.h) can't be reused as-is here:
// it declares `type r = ...;` internally, which is illegal C for `type =
// void` -- so void-returning hooks use `SO_CONTINUE_VOID` below instead
// (same restore/call/re-patch sequence, no return value stored).
static so_hook h_GRP_Flush, h_GRPX_GetFlag, h_GRPX_FlushAll, h_GRPX_Flush,
        h_SGL_grpFlushLcd, h_SGL_FrameBufferToScreen;

static int g_flush_log_count = 0;
#define FLUSH_LOG_MAX 30

#define SO_CONTINUE_VOID(h, ...) do { \
    kuKernelCpuUnrestrictedMemcpy((void *)(h).addr, (h).orig_instr, sizeof((h).orig_instr)); \
    kuKernelFlushCaches((void *)(h).addr, sizeof((h).orig_instr)); \
    if ((h).thumb_addr) ((void(*)())(h).thumb_addr)(__VA_ARGS__); \
    else ((void(*)())(h).addr)(__VA_ARGS__); \
    kuKernelCpuUnrestrictedMemcpy((void *)(h).addr, (h).patch_instr, sizeof((h).patch_instr)); \
    kuKernelFlushCaches((void *)(h).addr, sizeof((h).patch_instr)); \
} while (0)

static void GRP_Flush_hook(void) {
    if (g_flush_log_count < FLUSH_LOG_MAX) {
        l_debug("[FLUSH] GRP_Flush() called (#%d)", g_flush_log_count + 1);
    }
    g_flush_log_count++;
    SO_CONTINUE_VOID(h_GRP_Flush);
}

static int GRPX_GetFlag_hook(void) {
    int r = SO_CONTINUE(int, h_GRPX_GetFlag);
    if (g_flush_log_count <= FLUSH_LOG_MAX) {
        l_debug("[FLUSH] GRPX_GetFlag() -> %d", r);
    }
    return r;
}

static void GRPX_FlushAll_hook(void) {
    if (g_flush_log_count <= FLUSH_LOG_MAX) {
        l_debug("[FLUSH] GRPX_FlushAll() called");
    }
    SO_CONTINUE_VOID(h_GRPX_FlushAll);
}

static void GRPX_Flush_hook(void) {
    if (g_flush_log_count <= FLUSH_LOG_MAX) {
        l_debug("[FLUSH] GRPX_Flush() called");
    }
    SO_CONTINUE_VOID(h_GRPX_Flush);
}

static void SGL_grpFlushLcd_hook(int a0, int a1, int a2, int a3, int a4, int a5) {
    if (g_flush_log_count <= FLUSH_LOG_MAX) {
        l_debug("[FLUSH] SGL_grpFlushLcd(%d,%d,%d,%d,%d,%d) called", a0, a1, a2, a3, a4, a5);
    }
    SO_CONTINUE_VOID(h_SGL_grpFlushLcd, a0, a1, a2, a3, a4, a5);
}

static void SGL_FrameBufferToScreen_hook(void) {
    if (g_flush_log_count <= FLUSH_LOG_MAX) {
        l_debug("[FLUSH] SGL_FrameBufferToScreen() called");
    }
    SO_CONTINUE_VOID(h_SGL_FrameBufferToScreen);
}

// --- Corrida 32: is MainProcess short-circuiting on gAppSleepInfo? ---------
//
// MainProcess (out_ghidra.c:25352, the callback g_MainTimer dispatches every
// 100ms -- confirmed firing steadily every tick in the device log) opens
// with `if (gAppSleepInfo != '\0') { CS_knlSetTimer(...100ms...); return; }`
// -- if true, it just re-arms itself and returns WITHOUT calling
// STATE_fpProcess() (the per-scene update/draw dispatcher that would
// eventually reach GRP_Flush) or SOUNDSYSTEM_Process(). `gAppSleepInfo` is
// set to 1 only by pauseClet() (:24894) and cleared by resumeClet() (:24847),
// startClet's DelegateCall_startClet() (:24990), and destroyClet() (:24762).
// On real Android, WrapperDirtyRenderer.onResume()/onResumeClet() (Java) both
// gate calling nativeResume/nativeResumeClet behind `if (this.isPaused)` --
// false on a fresh launch that was never backgrounded -- so neither is ever
// invoked on first boot; gAppSleepInfo simply stays at the 0 that
// DelegateCall_startClet() (inside nativeInit -> startClet) already set.
// This port's main.c, unlike Android, unconditionally calls nativeResume()
// then nativeResumeClet() right after nativeInit() on EVERY launch, first
// boot included -- if that path sets gAppSleepInfo to 1 anywhere before
// resumeClet() unconditionally clears it back to 0, or if resumeClet() isn't
// what nativeResumeClet actually maps to, this is exactly the kind of
// stuck-forever flag that would produce "every tick fires, nothing happens".
// Both symbols are exported in .dynsym (confirmed via objdump -T) -- hook
// MainProcess to log gAppSleepInfo's real value at the top of each call
// (capped) while calling straight through, no reimplementation.
static so_hook h_MainProcess;
static uint8_t *g_ptr_gAppSleepInfo = NULL;
static int g_mainprocess_log_count = 0;
#define MAINPROCESS_LOG_MAX 200

// Corrida 45: the user reports the game dropping to ~1 FPS and never
// reaching Title/Menu after the Corrida 44 fix. Before guessing why, get
// real numbers: how much wall-clock time each MainProcess tick actually
// takes, and exactly which INITSTATE_ProcessInit sub-state/frame-counter
// (out_ghidra.c:111798-111906) it's stuck on -- INITSTATE_nState==3 is a
// terminal "Memory error!"/GAMELOADER_pText display state with no exit
// transition anywhere in the decompile, reached if GAMELOADER_ProcessInstall
// or GAMELOADER_ProcessPreLoad (both driven by MEM_Malloc against a FIXED
// 8MB pool, MEM_nTotalPoolSize/out_ghidra.c:22319) ever returns failure --
// which would also explain repeated GetStringWidthHeightInRect/DrawTextInRect
// calls every single tick forever (redrawing that same error text each
// frame, never advancing). Raised the log cap (200, up from 20) so a longer
// stretch of ticks is visible, and added elapsed-time-since-last-tick plus
// the actual state/pool numbers -- this will show directly whether it's
// genuinely stuck in the error state (nState==3, forever) vs. progressing
// normally but slowly (nState/LoadFrameCnt advancing tick to tick, elapsed
// time revealing where the slowness actually is).
static uint64_t g_last_tick_time = 0;

static void MainProcess_hook(uint32_t param_1, uint32_t param_2) {
    if (g_mainprocess_log_count < MAINPROCESS_LOG_MAX) {
        uint64_t now = sceKernelGetProcessTimeWide();
        uint64_t delta_us = g_last_tick_time ? (now - g_last_tick_time) : 0;
        g_last_tick_time = now;

        uint8_t init_state = *(uint8_t *) (so_mod.text_base + 0x00d6a4ec); // INITSTATE_nState
        uint32_t load_frame_cnt = *(uint32_t *) (so_mod.text_base + 0x00d6a4e8); // INITSTATE_LoadFrameCnt
        uint32_t mem_used = *(uint32_t *) (so_mod.text_base + 0x00161dfc); // MEM_nUsedPoolSize
        uint32_t mem_total = *(uint32_t *) (so_mod.text_base + 0x00157264); // MEM_nTotalPoolSize
        uint32_t game_loader_ptext = *(uint32_t *) (so_mod.text_base + 0x00167078); // GAMELOADER_pText

        // Corrida 46: INITSTATE_nState==3 confirmed reached and stuck (Corrida
        // 45's log). GAMELOADER_pText is a real char[0x100] buffer
        // (GAMELOADER_ProcessInstall's own CS_knlSprintk error message,
        // out_ghidra.c:60275-60283) -- read its actual text, not just its
        // address, to see WHICH install step failed and why, instead of
        // guessing.
        const char *ptext_str = (init_state == 3 && game_loader_ptext)
            ? (const char *) (uintptr_t) game_loader_ptext : NULL;

        l_debug("[MAINPROC] MainProcess() called (#%d) gAppSleepInfo=%d delta_us=%llu "
                "INITSTATE_nState=%u INITSTATE_LoadFrameCnt=%u MEM_pool=%u/%u GAMELOADER_pText=0x%x \"%s\"",
                g_mainprocess_log_count + 1,
                g_ptr_gAppSleepInfo ? (int) *g_ptr_gAppSleepInfo : -1,
                (unsigned long long) delta_us, init_state, load_frame_cnt, mem_used, mem_total,
                game_loader_ptext, ptext_str ? ptext_str : "");
        g_mainprocess_log_count++;
    }
    SO_CONTINUE_VOID(h_MainProcess, param_1, param_2);
}

// --- Corrida 33: STATE_fpProcess is NULL forever -- nothing ever kicks off
// the state machine --------------------------------------------------------
//
// Corrida 32 proved gAppSleepInfo=0 on every MainProcess tick -- not the
// short-circuit. Reading MainProcess in full: past that check, it only ever
// calls (*STATE_fpProcess)() if non-NULL (out_ghidra.c:25388-25391).
// STATE_fpProcess/STATE_nState (out_ghidra.c's STATE_* globals) are BSS --
// zero until STATE_Set() (:24479) first runs. The ONLY call site for
// STATE_Set(0) (the Init state) anywhere in the .so is inside
// UserAgreeCallback(0) (:24932-24940) -- which is only ever invoked by
// whoever calls Java_com_com2us_module_activeuser_ActiveUser_
// jniUserAgreeNotifier (:141236), a JNI export that expects a Java-side
// Com2uS "ActiveUser"/UserAgree module (which this port has no equivalent
// of -- CLAUDE.md already flags Hub/social/module subsystems as no-op-
// until-proven-necessary) to call it back with the previously-registered
// function pointer. startClet() (:25016-25023) registers that callback via
// CS_UserAgreeSetCallback(UserAgreeCallback+1), then only shows a real UI
// (CS_UserAgreeShowUI()) for the Korean locale -- every other locale (this
// build: com.com2us.inotia3...google.global, confirmed non-Korean) relies
// entirely on that missing module system firing the callback on its own,
// with no UI. Since nothing here ever does, STATE_fpProcess stays NULL
// forever and MainProcess's per-tick check silently no-ops -- explaining
// exactly what's been observed: the main timer fires steadily every 100ms
// and nothing else ever happens.
//
// Fix: hook CS_UserAgreeSetCallback (exported in .dynsym) to stash the
// registered callback pointer (still calling through first so whatever real
// bookkeeping it does internally still happens). main.c's
// patch_fire_user_agree_callback() invokes it with argument 0 ("agreed"/no
// error) once nativeInit()/startClet() has fully returned -- deliberately
// AFTER, not from inside this hook: on real Android the module callback is
// inherently asynchronous (fired from the module system whenever IT decides,
// not synchronously mid-startClet()), and startClet() itself keeps
// initializing subsystems (CS_ActiveUserStart(), news banner, g_MainTimer's
// own registration) for several more lines after registering this callback
// -- firing it immediately, mid-hook, would run the entire state machine's
// first Enter callback before all of that finishes, an ordering real Android
// never produces.
static so_hook h_CS_UserAgreeSetCallback;
static void (*g_user_agree_callback)(int) = NULL;

static void CS_UserAgreeSetCallback_hook(uint32_t callback_ptr) {
    SO_CONTINUE_VOID(h_CS_UserAgreeSetCallback, callback_ptr);
    g_user_agree_callback = (void (*)(int))(uintptr_t) callback_ptr;
}

void patch_fire_user_agree_callback(void) {
    if (g_user_agree_callback) {
        l_debug("[STATE] firing UserAgree callback(0) -- no Hub/module layer in this "
                "port to do it for us (see Corrida 33)");
        g_user_agree_callback(0);
    } else {
        l_warn("Corrida 33: startClet() never registered a UserAgree callback -- "
               "nothing to fire, state machine will stay dead");
    }
}

// --- Corrida 35: instrument ResourceManager::Add / MMappedFile::Open -------
//
// Corrida 34's File_GetFullPathName_hook fix (forcing the "@0|<len>|<path>"
// format so File::Open always takes its '@' branch) did NOT stop the crash --
// a rebuilt binary that includes that fix (confirmed by its own on-disk size
// changing) reproduces the EXACT same "IDAT: CRC error" -> longjmp-into-a-
// dead-frame crash, same PC/LR/stack trace, with MainMenu.png.jpg's re-open
// still mmapping with another resource's length (first font.fnt's 3960, then
// Title.png's 110266). A static disassembly pass of ResourceManager::Add ->
// File::Open -> File::GetFullPathName(hook) -> ParseOffsetAndLengthAndPath ->
// File::GetOffset/GetLength shows every field SHOULD end up written
// correctly now -- Corrida 34's "uninitialized stack read" theory doesn't
// survive contact with the real disassembly. Static analysis hit its limit;
// this hooks the two functions transparently (calls through, changes nothing)
// to log name/path/offset/length actually flowing through at runtime:
// ResourceManager::Add (the 2-arg overload CS_knlGetResourceID uses to
// register/cache a resource by name) and MMappedFile::Open (what
// CS_knlGetResource actually mmaps with). This will show directly whether
// "MainMenu.png.jpg" gets CACHED with the wrong length from the start, or
// whether CS_knlGetResource hands MMappedFile::Open some OTHER resource's
// cached info (struct aliasing) when it's time to mmap it.
static so_hook h_ResourceManager_Add, h_MMappedFile_Open;

static uint32_t ResourceManager_Add_hook(uint32_t this_ptr, const char *name, unsigned long *out_length) {
    uint32_t result = SO_CONTINUE(uint32_t, h_ResourceManager_Add, this_ptr, name, out_length);
    l_debug("[RESMGR] Add(name=\"%s\") -> id=%u length=%lu", name ? name : "(null)", result,
            out_length ? *out_length : 0xdeadbeefUL);
    return result;
}

static int MMappedFile_Open_hook(uint32_t this_ptr, const char *path, unsigned long offset,
                                  unsigned long length, int a4, int a5) {
    l_debug("[MMAP] MMappedFile::Open(path=\"%s\", offset=%lu, length=%lu, %d, %d)",
            path ? path : "(null)", offset, length, a4, a5);
    int result = SO_CONTINUE(int, h_MMappedFile_Open, this_ptr, path, offset, length, a4, a5);
    // Corrida 43: does MMappedFile::Open (out_ghidra.c:175060) itself report
    // success/failure here? Its own success flag lives at `this+0x108`
    // (nulled out if the real mmap() call returns exactly (void*)-1) and the
    // adjusted data pointer GetPointer() later reads sits at `this+0x110`.
    void *raw_mmap_ptr = *(void **) (uintptr_t)(this_ptr + 0x108);
    void *data_ptr = *(void **) (uintptr_t)(this_ptr + 0x110);
    l_debug("[MMAP] MMappedFile::Open(...) -> %d, this+0x108(raw mmap)=%p, this+0x110(data ptr)=%p",
            result, raw_mmap_ptr, data_ptr);
    return result;
}

// --- Corrida 36: which resourceID is CS_knlGetResource actually asked for,
// and what does ITS OWN struct really hold at that moment? -------------------
//
// Corrida 35's logs ruled out "cached stack garbage": MainMenu.png.jpg's own
// struct (id=1, registered once with length=148229, confirmed correct at
// registration) is later reread by MMappedFile::Open with the SAME path
// (never wrong -- always "MainMenu.png.jpg") but a length that exactly
// matches whatever OTHER resource (font.fnt, then Title.png) was JUST
// registered immediately before. That's too precise to be generic memory
// corruption -- something is either asking for the wrong resourceID, or
// FindInformation is resolving the right id to the wrong struct. This hooks
// CS_knlGetResource (exported, plain C symbol) to log the resourceID it
// actually receives, independently re-runs
// ResourceManager::FindInformation(theResourceManager, id) itself, and dumps
// the raw struct pointer + path/offset/length fields BEFORE the real
// function (and MMappedFile::Open, hooked above) ever touch them --
// transparent, calls through unchanged.
typedef void *(*fn_find_info_by_id_t)(void *, uint32_t);

static so_hook h_CS_knlGetResource;

static int CS_knlGetResource_hook(uint32_t resource_id, uint32_t dest, unsigned long dest_size) {
    fn_find_info_by_id_t FindInformation_real =
        (fn_find_info_by_id_t)(uintptr_t) so_symbol(&so_mod, "_ZN15ResourceManager15FindInformationEj");
    uintptr_t mgr_slot = so_symbol(&so_mod, "theResourceManager");

    if (FindInformation_real && mgr_slot) {
        void *mgr = *(void **) mgr_slot;
        void *info = mgr ? FindInformation_real(mgr, resource_id) : NULL;
        if (info) {
            const char *path = (const char *) ((uint8_t *) info + 0x104);
            unsigned long off = *(unsigned long *) ((uint8_t *) info + 0x210);
            unsigned long len = *(unsigned long *) ((uint8_t *) info + 0x214);
            l_debug("[GETRES] CS_knlGetResource(id=%u) info=%p path=\"%s\" offset=%lu length=%lu",
                    resource_id, info, path, off, len);
        } else {
            l_debug("[GETRES] CS_knlGetResource(id=%u) FindInformation -> NULL (mgr=%p)",
                    resource_id, mgr);
        }
    } else {
        l_warn("Corrida 36: FindInformation/theResourceManager symbol missing -- can't probe id=%u",
               resource_id);
    }

    // Corrida 43: this was `SO_CONTINUE_VOID` (discarding the real return
    // value) despite RES_LoadToHeap (out_ghidra.c:23395) explicitly checking
    // `CS_knlGetResource(...) == 0` -- Ghidra typed the real function `void`
    // (out_ghidra.c:157400), but it genuinely returns 0/0xffffffee in r0. A
    // `void`-returning hook has no language guarantee that r0 still holds
    // the real call's result by the time it returns (it happened to survive
    // in practice, but that's an accident of codegen, not something to rely
    // on) -- switched to SO_CONTINUE(int, ...) so the real return value is
    // captured and passed on explicitly, removing that risk entirely.
    int result = SO_CONTINUE(int, h_CS_knlGetResource, resource_id, dest, dest_size);
    l_debug("[GETRES] CS_knlGetResource(id=%u) -> %d", resource_id, result);
    return result;
}

// --- Corrida 41: does FONT_Load() actually run, and does it succeed? -------
//
// Corrida 40's fix let the boot sequence advance past Extend_RequestNotice,
// but it crashes again shortly after: GRP_PutAsciiChar (out_ghidra.c:20971)
// does `memcpy(FONT_pFontBuffer, (void*)(FONT_pFontData + 0xb10 +
// uVar1*0xc), 0xc)` with NO null check on FONT_pFontData -- confirmed by
// disassembly/dump (R1 in the fault matches EXACTLY FONT_pFontData==NULL
// plus that computed offset). FONT_pFontData (.bss, real offset 0x56f4d0,
// starts at NULL) is only ever written by FONT_Load() (`0x6c550`,
// out_ghidra.c:19844) via `RES_LoadToPool("font.fnt", &FONT_pFontData, 0)` --
// which itself only writes its out-param if RES_LoadToHeap succeeds
// (out_ghidra.c:23378, the same CS_knlGetResourceID/CS_knlGetResource chain
// already confirmed working correctly for "font.fnt" as a resource in
// Corridas 34-40's [RESMGR]/[GETRES]/[MMAP] logs). FONT_Load() itself is only
// called from StartCletProcess() (out_ghidra.c:24944-24979), which in turn
// only runs if GRP_Initialize() (out_ghidra.c:21212) returns non-zero
// (success) -- GRP_Initialize gates on CS_grpGetScreenFrameBuffer(0) being
// non-NULL, a completely different code path than the CSOGL rendering
// already confirmed working, so it's not guaranteed to succeed just because
// sprites are drawing. Rather than guess further, this hooks FONT_Load
// (exported, `0x6c550`) transparently to log its actual return value and the
// resulting FONT_pFontData right after the real call -- this will show
// directly whether FONT_Load() runs at all, and if so, whether font.fnt's
// own resource load (already proven to work for texture use) also works for
// this raw-buffer use, or fails for a still-unknown reason.
static so_hook h_FONT_Load;

static int FONT_Load_hook(void) {
    int result = SO_CONTINUE(int, h_FONT_Load);
    void *font_data = *(void **) (so_mod.text_base + 0x56f4d0);
    l_debug("[FONT] FONT_Load() -> %d, FONT_pFontData=%p", result, font_data);
    return result;
}

// --- Corrida 42: FONT_Load() runs and fails -- root cause found and fixed --
//
// Confirmed by the Corrida 41 hook: FONT_Load() -> 0 (failure),
// FONT_pFontData=0x0 -- it DOES run, RES_LoadToPool("font.fnt", ...) DOES
// fail, even though CS_knlGetResource already works for this exact resource
// name elsewhere. Read RES_LoadToPool/RES_LoadToHeap (out_ghidra.c:23378-
// 23516) end to end: RES_LoadToHeap's real return value on SUCCESS is
// whatever CS_knlCalloc(length) returned -- and CS_knlCalloc itself
// (out_ghidra.c:157643, `void CS_knlCalloc(size_t) { calloc(1,param_1); }`)
// is a bare `calloc()` passthrough (Ghidra lost the return type, but the
// pointer rides home in r0 regardless -- confirmed by every caller treating
// it as one) -- so RES_LoadToHeap returns a REAL HEAP POINTER on success, or
// exactly -1/-2/-3 on the three known failure paths. CS_GETDPTR
// (out_ghidra.c:157108, disassembles to a bare `bx lr`) is a vestigial
// identity no-op from whatever older handle-based platform this codebase
// originated on -- it does NOT convert a handle to a pointer here, it just
// hands the same pointer back.
//
// RES_LoadToPool's own success check is `if (-1 < iVar1)` on that return
// value, interpreted as a signed int. That's correct ONLY when a real heap
// pointer's top bit happens to be 0 -- true for a typical Android userspace
// heap (well below 0x80000000), which is exactly why this 2013 code never
// tripped this on a real device. EVERY heap address this port has ever
// produced in any crash dump so far (0x81xxxxxx/0x83xxxxxx/0x98xxxxxx/...)
// has that top bit SET, making it negative as a signed int -- so this check
// fails every single time here, `RES_LoadToPool` returns that (misread as an
// error) without ever writing `*param_2`, and FONT_Load() sees "failure"
// despite the underlying resource load having actually succeeded moments
// earlier inside RES_LoadToHeap. (The exact same `-1 < iVar1`-on-
// RES_LoadToHeap idiom also appears in RES_MoveToFilePack (out_ghidra.c:
// 23424) and EXCELDATA_LoadFullResource (out_ghidra.c:58509/58510) -- not
// confirmed crashing yet, since neither is proven reached in this port's
// boot sequence so far, but flagged here since it's the exact same risk
// pattern against the exact same real function: if either ever crashes,
// apply this same fix instead of re-deriving it.)
//
// Fix: reimplement RES_LoadToPool faithfully (out_ghidra.c:23489-23516) but
// distinguish the three explicit error codes (-1/-2/-3) from "any other
// value = a real pointer" instead of trusting the sign bit. Calls straight
// through to the real RES_LoadToHeap/MEM_Malloc/CS_knlFree (all exported,
// all left completely unmodified) -- only the success/failure test changes.
// RES_LoadToPool has exactly one call site in the whole 214k-line decompile
// (FONT_Load itself), so this can't affect any other code path.
#define OFF_RES_LoadToHeap 0x0006f76d
#define OFF_MEM_Malloc     0x0006edfd
#define OFF_CS_knlFree     0x000f75f1

typedef int (*fn_res_load_to_heap_t)(const char *, unsigned long *);
typedef void *(*fn_mem_malloc_t)(unsigned long);
typedef void (*fn_cs_knlfree_t)(int);

static int RES_LoadToPool_hook(const char *name, void **out_ptr, unsigned long *out_len) {
    fn_res_load_to_heap_t RES_LoadToHeap_real = (fn_res_load_to_heap_t)(so_mod.text_base + OFF_RES_LoadToHeap);
    fn_mem_malloc_t MEM_Malloc_real = (fn_mem_malloc_t)(so_mod.text_base + OFF_MEM_Malloc);
    fn_cs_knlfree_t CS_knlFree_real = (fn_cs_knlfree_t)(so_mod.text_base + OFF_CS_knlFree);

    unsigned long length = 0;
    int handle = RES_LoadToHeap_real(name, &length);
    if (handle == -1 || handle == -2 || handle == -3) {
        l_debug("Corrida 42: RES_LoadToHeap(\"%s\") failed with real error code %d", name, handle);
        return handle;
    }

    void *dest = MEM_Malloc_real(length);
    if (!dest) {
        CS_knlFree_real(handle);
        return -3;
    }
    memcpy(dest, (void *)(uintptr_t) handle, length);
    CS_knlFree_real(handle);
    if (out_len) *out_len = length;
    *out_ptr = dest;
    l_debug("Corrida 42: RES_LoadToPool(\"%s\") recovered a valid pointer (%p) the real .so's own "
            "sign check would have discarded", name, (void *)(uintptr_t) handle);
    return 0;
}

// --- Corrida 47: the sibling bug flagged (but unconfirmed) in Corrida 42 is
// what's actually stalling INITSTATE_ProcessInit forever ------------------
//
// Corrida 46 traced INITSTATE_nState==3 (the terminal "Memory error!" state,
// out_ghidra.c:111879-111906, no exit transition anywhere) back to
// GAMELOADER_ProcessInstall (out_ghidra.c:60241-60305) treating a record's
// install as failed. GAMELOADER_Install (:60216-60237) fails immediately if
// RES_MoveToFileEx (:23471-23485) returns 0 -- which calls straight into
// RES_MoveToFilePack (:23411-23467), whose OWN success check is
// `if (-1 < iVar1)` on RES_LoadToHeap's return value -- the EXACT same
// sign-bit bug already fixed for RES_LoadToPool in Corrida 42 (a real heap
// pointer from CS_knlCalloc's calloc() always has its top bit set on this
// port's memory layout, always negative as a signed int, so this check
// always fails here even though the underlying load succeeded). Corrida 42
// flagged this exact call site as an unconfirmed risk; this confirms it and
// applies the same fix.
//
// Fix: reimplement RES_MoveToFilePack faithfully (out_ghidra.c:23411-23467),
// distinguishing RES_LoadToHeap's three explicit error codes (-1/-2/-3) from
// "any other value = a real pointer", same as Corrida 42. Calls straight
// through to the real RES_LoadToHeap/LZMA_Decode/FILE_Seek/FILE_WriteAll/
// CS_knlFree/MEM_Free (all exported, all left completely unmodified) --
// only the success/failure test changes.
#define OFF_LZMA_Decode  0x0006ebe9
#define OFF_FILE_Seek    0x0006c2e9
#define OFF_FILE_WriteAll 0x0006c335
#define OFF_MEM_Free     0x0006ef49

typedef int (*fn_lzma_decode_t)(void *, int, void **, int *);
typedef void (*fn_file_seek_t)(int, int, int);
typedef int (*fn_file_writeall_t)(int, void *, int);
typedef void (*fn_mem_free_t)(void *);

static int RES_MoveToFilePack_hook(uint32_t file_handle, const char *name, int compressed) {
    fn_res_load_to_heap_t RES_LoadToHeap_real = (fn_res_load_to_heap_t)(so_mod.text_base + OFF_RES_LoadToHeap);
    fn_lzma_decode_t LZMA_Decode_real = (fn_lzma_decode_t)(so_mod.text_base + OFF_LZMA_Decode);
    fn_file_seek_t FILE_Seek_real = (fn_file_seek_t)(so_mod.text_base + OFF_FILE_Seek);
    fn_file_writeall_t FILE_WriteAll_real = (fn_file_writeall_t)(so_mod.text_base + OFF_FILE_WriteAll);
    fn_cs_knlfree_t CS_knlFree_real = (fn_cs_knlfree_t)(so_mod.text_base + OFF_CS_knlFree);
    fn_mem_free_t MEM_Free_real = (fn_mem_free_t)(so_mod.text_base + OFF_MEM_Free);

    unsigned long length = 0;
    int handle = RES_LoadToHeap_real(name, &length);
    if (handle == -1 || handle == -2 || handle == -3) {
        l_debug("Corrida 47: RES_MoveToFilePack: RES_LoadToHeap(\"%s\") failed with real error code %d",
                name, handle);
        return 0;
    }

    unsigned char *data = (unsigned char *) (uintptr_t) handle; // CS_GETDPTR is identity
    void *write_buf;
    int write_len;
    void *lzma_buf = NULL;
    int freed_original = 0;

    if (compressed == 0) {
        write_buf = data;
        write_len = (int) length;
    } else if (data[0] == 1) {
        int ok = LZMA_Decode_real(data + 1, (int) length - 1, &write_buf, &write_len);
        CS_knlFree_real(handle);
        freed_original = 1;
        if (!ok) {
            l_debug("Corrida 47: RES_MoveToFilePack: LZMA_Decode(\"%s\") failed", name);
            return 0;
        }
        lzma_buf = write_buf;
    } else {
        write_buf = data + 1;
        write_len = (int) length - 1;
    }

    FILE_Seek_real((int) file_handle, 0, 2);
    int wrote = FILE_WriteAll_real((int) file_handle, write_buf, write_len);

    if (!freed_original) CS_knlFree_real(handle);
    if (lzma_buf) MEM_Free_real(lzma_buf);

    l_debug("Corrida 47: RES_MoveToFilePack(\"%s\") recovered a valid pointer (%p) the real .so's own "
            "sign check would have discarded -> wrote=%d", name, (void *)(uintptr_t) handle, wrote);
    return wrote != 0;
}

// --- Corrida 48: which GAMELOADER_ProcessInstall record actually fails? ----
//
// Corrida 47's sign-bug fix did NOT clear the boot stall: log_2026-08-07_
// 01-42-41.txt still lands on INITSTATE_nState==3 on the very FIRST
// LoadFrameCnt tick of nState==1 -- i.e. GAMELOADER_ProcessInstall()
// (out_ghidra.c:60241-60305) fails on its first call. That function's
// install loop (over every INSTALLBASE_nRecordCount record, calling either
// GAMELOADER_Install() for a plain file or FILEPACK_Install() for a packed
// one, both of which funnel through RES_MoveToFileEx() ->
// RES_MoveToFilePack()) runs entirely synchronously inside that ONE call --
// the outer MAINPROC-level nState/LoadFrameCnt log can't say which record
// failed or through which path. It also showed GAMELOADER_pText as an empty
// string even though the failure path unconditionally CS_knlSprintk()s an
// error message into it (out_ghidra.c:60276-60283) -- a second symptom of
// the same failure (its format string, looked up via MEMORYTEXT_GetText(),
// was itself empty), not a separate bug. All four functions here are
// exported (confirmed via `nm -D`); hook them transparently -- calls
// through unchanged -- to log every record's name/index and real return
// value, plus GAMELOADER_pText's actual content right when
// GAMELOADER_ProcessInstall returns, so the next log capture shows exactly
// which record/file/pack fails and how.
static so_hook h_GAMELOADER_Install, h_FILEPACK_Install, h_RES_MoveToFileEx,
    h_GAMELOADER_ProcessInstall;
static uint32_t *g_ptr_GAMELOADER_pText = NULL;
static int g_install_log_count = 0;
#define INSTALL_LOG_MAX 200

static uint32_t GAMELOADER_Install_hook(uint32_t name_ptr, uint32_t compressed) {
    uint32_t result = SO_CONTINUE(uint32_t, h_GAMELOADER_Install, name_ptr, compressed);
    if (g_install_log_count < INSTALL_LOG_MAX) {
        l_debug("Corrida 48: GAMELOADER_Install(name=\"%s\", compressed=%u) -> %u",
                name_ptr ? (const char *)(uintptr_t) name_ptr : "(null)", compressed, result);
        g_install_log_count++;
    }
    return result;
}

static uint32_t FILEPACK_Install_hook(int index) {
    uint32_t result = SO_CONTINUE(uint32_t, h_FILEPACK_Install, index);
    if (g_install_log_count < INSTALL_LOG_MAX) {
        l_debug("Corrida 48: FILEPACK_Install(index=%d) -> %u", index, result);
        g_install_log_count++;
    }
    return result;
}

static uint32_t RES_MoveToFileEx_hook(uint32_t name_ptr, uint32_t compressed, uint32_t param_3,
                                       uint32_t param_4) {
    uint32_t result = SO_CONTINUE(uint32_t, h_RES_MoveToFileEx, name_ptr, compressed, param_3, param_4);
    if (g_install_log_count < INSTALL_LOG_MAX) {
        l_debug("Corrida 48: RES_MoveToFileEx(name=\"%s\", compressed=%u, %u, %u) -> %u",
                name_ptr ? (const char *)(uintptr_t) name_ptr : "(null)", compressed, param_3, param_4,
                result);
        g_install_log_count++;
    }
    return result;
}

static uint32_t GAMELOADER_ProcessInstall_hook(void) {
    uint32_t result = SO_CONTINUE(uint32_t, h_GAMELOADER_ProcessInstall);
    uint32_t ptext = g_ptr_GAMELOADER_pText ? *g_ptr_GAMELOADER_pText : 0;
    l_debug("Corrida 48: GAMELOADER_ProcessInstall() -> %u, GAMELOADER_pText=0x%x \"%s\"",
            result, ptext, ptext ? (const char *)(uintptr_t) ptext : "");
    return result;
}

// --- Corrida 49: EXCELDATA_LoadFullResource has the same sign-bit bug as
// Corridas 42/47 -- and it's the one loading INSTALLBASE/SYMBOLBASE/
// MEMORYTEXT off a packaged (non-loose-file) resource -----------------------
//
// Corrida 42 already flagged EXCELDATA_LoadFullResource (out_ghidra.c:
// 58498-58537) as an unconfirmed sibling risk. Reading it now: its OWN
// check on RES_LoadToHeap's return is `if (iVar1 < 0) { ...fail... }`
// (:58510) -- logically the exact same test as the already-fixed call
// sites' `if (-1 < iVar1) { ...success... }`, just written negated. A real
// heap pointer from CS_knlCalloc always has its top bit set on this port's
// memory layout (confirmed in Corridas 42/47), always negative as a signed
// int, so `iVar1 < 0` is true for EVERY successful load too -- this
// function fails unconditionally whenever RES_LoadToHeap actually succeeds.
// EXCELDATA_LoadGameData()/EXCELDATA_LoadMemoryText()/
// EXCELDATA_LoadResourceDataTable() (:58614-58559) all call straight into
// it whenever FILE_IsExist() says the resource ISN'T a loose file on disk --
// exactly the packaged-resource path CLAUDE.md already flags as this port's
// likely asset layout, and exactly the path EXCELDATA_Create() (run at boot,
// INITSTATE_LoadFrameCnt==5, out_ghidra.c:111833) uses to populate
// INSTALLBASE/SYMBOLBASE/MEMORYTEXT before GAMELOADER_ProcessInstall() ever
// runs. This lines up with BOTH observed symptoms in log_2026-08-07_
// 01-42-41.txt: GAMELOADER_ProcessInstall() failing on record 0 (whichever
// table silently loaded empty/stale instead of erroring loudly), and
// GAMELOADER_pText coming back empty (MEMORYTEXT_GetText()'s format-string
// table failing the same way). Fix before waiting on another hardware round
// trip, same technique as Corridas 42/47: reimplement faithfully
// (out_ghidra.c:58498-58537) distinguishing RES_LoadToHeap's three explicit
// error codes from "any other value is a real pointer" instead of trusting
// the sign bit. Calls straight through to the real RES_LoadToHeap/
// LZMA_Decode/MEM_Malloc/CS_knlFree (all exported, all left completely
// unmodified, all already resolved via Corrida 42/47's OFF_* offsets) --
// only the success/failure test changes. Exported (confirmed via `nm -D`),
// so this replaces the whole function directly like Corridas 42/47 do,
// rather than chaining through SO_CONTINUE.
static uint32_t EXCELDATA_LoadFullResource_hook(const char *name, void **out_ptr) {
    fn_res_load_to_heap_t RES_LoadToHeap_real = (fn_res_load_to_heap_t)(so_mod.text_base + OFF_RES_LoadToHeap);
    fn_lzma_decode_t LZMA_Decode_real = (fn_lzma_decode_t)(so_mod.text_base + OFF_LZMA_Decode);
    fn_mem_malloc_t MEM_Malloc_real = (fn_mem_malloc_t)(so_mod.text_base + OFF_MEM_Malloc);
    fn_cs_knlfree_t CS_knlFree_real = (fn_cs_knlfree_t)(so_mod.text_base + OFF_CS_knlFree);

    unsigned long length = 0;
    int handle = RES_LoadToHeap_real(name, &length);
    if (handle == -1 || handle == -2 || handle == -3) {
        l_debug("Corrida 49: EXCELDATA_LoadFullResource(\"%s\"): RES_LoadToHeap failed with real "
                "error code %d", name, handle);
        return 0xffffffff;
    }

    unsigned char *data = (unsigned char *) (uintptr_t) handle; // CS_GETDPTR is identity
    void *out_buf;
    uint32_t out_len;

    if (data[0] == 1) {
        int ok = LZMA_Decode_real(data + 1, (int) length - 1, &out_buf, (int *) &out_len);
        if (!ok) {
            CS_knlFree_real(handle);
            l_debug("Corrida 49: EXCELDATA_LoadFullResource(\"%s\"): LZMA_Decode failed", name);
            return 0xffffffff;
        }
    } else {
        out_len = (uint32_t) length - 1;
        out_buf = MEM_Malloc_real(out_len);
        if (!out_buf) {
            CS_knlFree_real(handle);
            return 0xffffffff;
        }
        memcpy(out_buf, data + 1, out_len);
    }

    CS_knlFree_real(handle);
    *out_ptr = out_buf;
    l_debug("Corrida 49: EXCELDATA_LoadFullResource(\"%s\") recovered a valid pointer (%p) the real "
            ".so's own sign check would have discarded -> length=%u", name,
            (void *)(uintptr_t) handle, out_len);
    return out_len;
}

// --- Corrida 54: __udivsi3 is the .so's own PRIVATE, statically-linked
// division primitive -- not an import Corridas 51-53 could ever reach ------
//
// Corrida 53's raise() wrapper finally caught the recurring SIGFPE crash
// (same crash every run since Corrida 50's Hub fix let the boot get this
// far) and logged its caller: 0x9811e114. Resolving that against the REAL
// .so at its actual fixed load address (source/utils/init.c's
// `LOAD_ADDRESS 0x98000000`, confirmed via `nm -D`/`objdump -d`, NOT the
// crash-dump tool's own auto-detected guess -- that guess has been wrong
// or misleading in every dump analyzed so far in this port, see the
// TILESYSTEM_Create/PLAYER_ProcessHate false leads earlier in this file's
// history) lands exactly on `__udivsi3+0xa4` (offset 0x11ce64, right after
// `bl __div0` at 0x11ce60) -- `__udivsi3` (aliased as `__aeabi_uidiv`, and
// also the routine `__aeabi_uidivmod` itself calls internally for the
// actual division) is a COMPLETE, self-contained division routine that
// this .so's own build statically linked in from libgcc, not a dynamic
// import -- so wrapping the .so's *imported* __aeabi_uidiv/__aeabi_uidivmod
// (Corridas 51/52) could never have caught this: the .so never calls out to
// the environment for unsigned division, it has its own private copy.
// __udivsi3 IS exported in .dynsym though (confirmed via `nm -D`) despite
// being for-internal-use-only, so it can be hooked directly like any other
// .so function this file already hooks -- this will finally show the real
// game function that divided by zero, not just the runtime helper it fell
// through on the way to raise().
static so_hook h_udivsi3;

static uint32_t udivsi3_hook(uint32_t numerator, uint32_t denominator) {
    if (denominator == 0) {
        l_fatal("Corrida 54: __udivsi3(%u, 0) -- divide by zero, .so caller return address=%p",
                numerator, __builtin_return_address(0));
    }
    return SO_CONTINUE(uint32_t, h_udivsi3, numerator, denominator);
}

// --- Corrida 55: found and fixed -- SGL_Util::GetGLSize() overflows to 0 --
//
// Corrida 54's hook caught the real crash: `Corrida 54: __udivsi3(1508900864,
// 0)`, called from .so return address 0x9811e114. Resolving 0x9811e114
// against the .so's real fixed load address (0x98000000, same as Corrida 54's
// comment) lands on `_ZN11SGL_Texture6CreateEjjjjhPv+0xd7` (confirmed via
// `objdump -d` on the real .so, not the decompile alone) -- specifically the
// FIRST of two fixed-point ratio divisions in SGL_Texture::Create
// (out_ghidra.c:161354-161395): `__aeabi_uidiv(param_3 << 0x10,
// SGL_Util::GetGLSize(param_3))`, called from SGL_Font_Android::
// RenderToTextureInRect (out_ghidra.c:161897, itself confirmed sitting on
// the same crash's stack) while rendering the Hub screen's Title/Message
// text (HUB_InitString, fixed for its own NULL-strcpy crash in Corrida 50).
// SGL_Util::GetGLSize(uint) (out_ghidra.c:160656, real code at 0xf8e50) is
// meant to round its input UP to the next power of two for GL texture
// sizing -- but its doubling loop (`uVar1 = uVar1 * 2` each iteration until
// `uVar1 >= param_1`) has no overflow guard: for any input with bit 31 set
// (a plainly-invalid "width", but the loop doesn't validate that), the
// required next power of two is 2^32, which wraps a 32-bit register to
// exactly 0 -- precisely the crash logged (1508900864 = 0x59F00000 is
// `param_3 << 16`, i.e. param_3's low 16 bits are 0x59F0 with garbage above
// bit 15, wide enough to trigger the wraparound). `__utilGetGLSize`
// (0xf1510) is a byte-for-byte identical implementation, same risk, same
// fix, no confirmed crash yet -- fixed alongside per Corrida 47's
// established precedent (same pattern, same real function shape, fix both
// once one is confirmed rather than re-deriving this later). Both symbols
// are exported (confirmed via `nm -D`); replace them outright (like
// Corridas 42/47/49) with a version that clamps at the largest
// representable power of two instead of wrapping -- identical output for
// every legitimate input, and turns the "garbage width" case into a
// large-but-valid texture size instead of a divide-by-zero. (The garbage
// width itself -- why RenderToTextureInRect ends up asking for one -- is a
// separate, cosmetic-at-worst question for a follow-up Corrida once the
// game no longer crashes outright.)
static uint32_t gl_size_fixed(uint32_t input) {
    if (input == 0 || input == 1) return 1;
    uint32_t result = 1;
    while (result < input) {
        if (result >= 0x80000000u) return 0x80000000u; // next step would overflow to 0 -- clamp instead
        result <<= 1;
    }
    return result;
}

static uint32_t sgl_util_getglsize_hook(uint32_t input) {
    return gl_size_fixed(input);
}

static uint32_t util_getglsize_hook(uint32_t input) {
    return gl_size_fixed(input);
}

// --- Corrida 56: picking a save slot/character crashes in MEM_ReadUint16 --
// with a NULL pointer -- MAP_LoadAllToHeap has the same RES_LoadToHeap
// sign-bit bug as Corridas 42/47/49, and MAP_LoadFromHeap/MAP_DestroyFromHeap
// carry their own copies of the same idiom on the resulting handle ----------
//
// This port's newest crash dump (Data Abort, PC at MEM_ReadUint16's very
// first instruction `ldrb r2,[r0]`, R0==0) has LR sitting inside
// MEM_ReadUint16_S right after its `bl MEM_ReadUint16`, and the crash's own
// R5 register holds 0x9815de2c -- resolving that against the .so's real
// fixed load address (0x98000000, same correction as Corrida 54/55; the
// crash-dump tool's own auto-detected base is unreliable) lands EXACTLY on
// the global MAP_nHeapID (real addr 0x15de2c, confirmed via `nm -D`). The
// crash's own stack additionally contains STATEMAINMENU_EnterSelectClass
// and MAP_Load/MAP_LoadFromHeap as live return addresses -- confirming the
// reported repro (picking a character / starting a slot) goes through
// MAP_Load -> MAP_LoadFromHeap -> MEM_ReadUint16_S -> MEM_ReadUint16.
//
// MAP_LoadFromHeap (out_ghidra.c:68515) reads: `if ((-1 < MAP_nHeapID) ||
// (MAP_nHeapID = MAP_LoadAllToHeap(), -1 < MAP_nHeapID)) { local_1c[0] =
// CS_GETDPTR(MAP_nHeapID); MEM_ReadUint16_S(local_1c); ... }`. A raw hex
// dump of the .so's real .data section confirms MAP_nHeapID's initializer
// really is -1, so the very first call correctly falls through to
// MAP_LoadAllToHeap() -- that function (out_ghidra.c:68483) is where the bug
// actually lives: `iVar1 = RES_LoadToHeap("map.dat",&local_c); iVar2 = 0; if
// (-1 < iVar1) { ...real success path, returns the LZMA-decoded heap
// handle... }` -- the EXACT same `-1 < iVar1`-on-RES_LoadToHeap idiom
// already fixed three times over (Corridas 42/47/49): a genuine heap handle
// on this port's memory layout always has its top bit set, always negative
// as a signed int, so `-1 < iVar1` is false even on success, and the
// function falls through to its pre-set `iVar2 = 0` default -- silently
// discarding a valid "map.dat" handle and returning 0 instead of a real
// handle or a real error code. That 0 becomes the new MAP_nHeapID, and
// MAP_LoadFromHeap's OWN `-1 < MAP_nHeapID` check reads 0 (0 > -1) as a
// VALID handle, proceeds to `CS_GETDPTR(0)` == NULL, and hands that NULL
// straight into MEM_ReadUint16_S/MEM_ReadUint16 -- the crash.
//
// Fixing MAP_LoadAllToHeap alone is not enough: once it correctly returns a
// real (sign-bit-set, so still "negative") success handle, MAP_LoadFromHeap's
// OWN `-1 < MAP_nHeapID` check would then reject that GENUINE success the
// same way (a real handle is more negative than -1), permanently breaking
// map loading (no crash, but nothing ever loads). MAP_DestroyFromHeap
// (out_ghidra.c:68386) has the identical `-1 < MAP_nHeapID` idiom guarding
// its cleanup and would likewise never free/reset a real handle -- flagged
// and fixed alongside per Corrida 47's established precedent (same pattern,
// same real global, fix all confirmed call sites in one pass instead of
// waiting for a separate hardware round trip per site).
//
// Fix: reimplement all three functions faithfully (out_ghidra.c:68386-68540)
// but replace every `-1 < X` heap-handle-validity test with `X != -1` --
// -1 is this port's own confirmed "not loaded" sentinel (MAP_DestroyFromHeap
// itself writes exactly that value), so this is the correct validity check
// regardless of which way a real handle's sign bit falls. Calls straight
// through to the real RES_LoadToHeap/LZMA_DecodeToHeap/MEM_ReadUint16_S/
// MEM_ReadUint24/MEM_Malloc/CS_knlFree (all exported, all left completely
// unmodified) -- only the success/failure tests change. All three symbols
// are exported (confirmed via `nm -D`); replaced outright like
// Corridas 42/47/49, rather than chained through SO_CONTINUE.
#define OFF_LZMA_DecodeToHeap 0x0006eb91
#define OFF_MEM_ReadUint16_S  0x0006f21d
#define OFF_MEM_ReadUint24    0x0006f231

typedef int (*fn_lzma_decode_to_heap_t)(void *, int, int *, int *);
typedef int (*fn_mem_read_uint16_s_t)(void *);
typedef int (*fn_mem_read_uint24_t)(void *);

static int *g_ptr_MAP_nHeapID = NULL;

static int MAP_LoadAllToHeap_hook(void) {
    fn_res_load_to_heap_t RES_LoadToHeap_real = (fn_res_load_to_heap_t)(so_mod.text_base + OFF_RES_LoadToHeap);
    fn_lzma_decode_to_heap_t LZMA_DecodeToHeap_real =
        (fn_lzma_decode_to_heap_t)(so_mod.text_base + OFF_LZMA_DecodeToHeap);
    fn_cs_knlfree_t CS_knlFree_real = (fn_cs_knlfree_t)(so_mod.text_base + OFF_CS_knlFree);

    unsigned long length = 0;
    int handle = RES_LoadToHeap_real("map.dat", &length);
    if (handle == -1 || handle == -2 || handle == -3) {
        l_debug("Corrida 56: MAP_LoadAllToHeap: RES_LoadToHeap(\"map.dat\") failed with real error "
                "code %d", handle);
        return -1;
    }

    unsigned char *data = (unsigned char *) (uintptr_t) handle; // CS_GETDPTR is identity
    if (data[8] == 1) {
        int decoded_handle = -1, decoded_len = 0;
        int ok = LZMA_DecodeToHeap_real(data + 9, (int) (length - 9), &decoded_handle, &decoded_len);
        CS_knlFree_real(handle);
        if (ok) {
            l_debug("Corrida 56: MAP_LoadAllToHeap(\"map.dat\") recovered a valid decoded handle (%d) "
                    "the real .so's own sign check would have discarded", decoded_handle);
            return decoded_handle;
        }
        l_debug("Corrida 56: MAP_LoadAllToHeap: LZMA_DecodeToHeap(\"map.dat\") failed");
        return -1;
    }

    CS_knlFree_real(handle);
    l_debug("Corrida 56: MAP_LoadAllToHeap: \"map.dat\" not marked compressed -- unexpected format");
    return -1;
}

static uint32_t MAP_LoadFromHeap_hook(int map_id, void **out_ptr) {
    fn_mem_read_uint16_s_t MEM_ReadUint16_S_real =
        (fn_mem_read_uint16_s_t)(so_mod.text_base + OFF_MEM_ReadUint16_S);
    fn_mem_read_uint24_t MEM_ReadUint24_real = (fn_mem_read_uint24_t)(so_mod.text_base + OFF_MEM_ReadUint24);
    fn_mem_malloc_t MEM_Malloc_real = (fn_mem_malloc_t)(so_mod.text_base + OFF_MEM_Malloc);

    if (*g_ptr_MAP_nHeapID == -1) {
        *g_ptr_MAP_nHeapID = MAP_LoadAllToHeap_hook();
    }
    if (*g_ptr_MAP_nHeapID == -1) {
        return 0;
    }

    void *cursor = (void *) (uintptr_t) *g_ptr_MAP_nHeapID; // CS_GETDPTR is identity
    int count = MEM_ReadUint16_S_real(&cursor); // advances `cursor` by 2, returns the uint16 read
    if (map_id >= count) {
        return 0;
    }

    int off_a = MEM_ReadUint24_real((void *) ((uintptr_t) cursor + (unsigned) map_id * 3));
    int off_b = MEM_ReadUint24_real((void *) ((uintptr_t) cursor + (unsigned) (map_id + 1) * 3));
    void *record_ptr = (void *) ((uintptr_t) cursor + off_a + (count + 1) * 3);
    void *dest = MEM_Malloc_real((unsigned long) (off_b - off_a));
    if (!dest) {
        return 0;
    }
    memcpy(dest, record_ptr, (size_t) (off_b - off_a));
    *out_ptr = dest;
    return 1;
}

static void MAP_DestroyFromHeap_hook(void) {
    fn_cs_knlfree_t CS_knlFree_real = (fn_cs_knlfree_t)(so_mod.text_base + OFF_CS_knlFree);
    if (*g_ptr_MAP_nHeapID != -1) {
        CS_knlFree_real(*g_ptr_MAP_nHeapID);
        *g_ptr_MAP_nHeapID = -1;
    }
}

// --- Corrida 57: Data abort in UI_DrawNumber entering combat after picking a
// character -- UTIL_LoadSpriteListFromResource has the same RES_LoadToHeap
// sign-bit bug as Corridas 42/47/49/56, this time leaving IMGFONT_pSprite
// permanently NULL --------------------------------------------------------
//
// This port's newest crash dump (Data Abort) has PC/LR both landing inside
// UI_DrawNumber (real .so offsets 0xb2012/0xb200a against the confirmed
// fixed load address 0x98000000 -- the crash-dump tool's own auto-detected
// base, as in every previous Corrida, resolves this to the wrong function).
// Disassembly at the fault (`5a73  ldrh r3, [r6, r1]`, out_ghidra.c:92129's
// `param_5[(extraout_r1 & 0xff) * 0xc]`) reads a digit-width table through
// `r1` -- and the dump's own R1/R8 = 0x000008d0. That is not a wild pointer:
// it is EXACTLY the constant `IMGFONT_GetCombatNumberSR()` returns
// (out_ghidra.c:96225-96229, `return IMGFONT_pSprite + 0x8d0;`) when the
// global `IMGFONT_pSprite` is 0. The call chain confirms it end to end --
// GAMESTATE_Draw -> GAMESTATE_DrawPlay -> EFFECTSYSTEM_DrawDamage ->
// EFFECTDAMAGE_Draw (out_ghidra.c:51062-51102, cVar1 in {3,4} selects the SR
// combat-number variant) -> UI_DrawNumber(..., IMGFONT_GetCombatNumberSR())
// -- i.e. the very first damage-number popup drawn once real gameplay
// starts, matching the reported repro exactly (character select -> start
// game -> crash).
//
// `IMGFONT_pSprite` is only ever written by IMGFONT_Create()
// (out_ghidra.c:96435-96445), called unconditionally from
// GAMELOADER_ProcessLoad() at INITSTATE_LoadFrameCnt==3 (:60009-60011) with
// its bool return value silently discarded -- so a failure here produces no
// visible error, no crash, nothing until the first combat-number draw much
// later. IMGFONT_Create calls UTIL_LoadSpriteListFromResource("i_imgfont.dat",
// &IMGFONT_pSprite, 0, 0x72) (out_ghidra.c:104072-104088):
//   iVar1 = RES_LoadToHeap(param_1, 0);
//   uVar2 = 0;
//   if (-1 < iVar1) { uVar2 = SNASYS_LoadSpriteListFromMemory(CS_GETDPTR(iVar1), param_2, param_3, param_4); CS_knlFree(iVar1); }
//   return uVar2;
// -- the exact same `-1 < iVar1`-on-RES_LoadToHeap idiom already fixed four
// times over: a genuine heap handle from CS_knlCalloc's calloc() always has
// its top bit set on this port's memory layout, always negative as a signed
// int, so the check is false even on success, `SNASYS_LoadSpriteListFromMemory`
// is never called, `IMGFONT_pSprite` (passed by reference) is never written,
// and it stays at its static-initializer value of 0 forever.
//
// Fix: reimplement UTIL_LoadSpriteListFromResource faithfully
// (out_ghidra.c:104072-104088), distinguishing RES_LoadToHeap's three
// explicit error codes (-1/-2/-3) from "any other value = a real pointer",
// same as Corridas 42/47/49/56. Calls straight through to the real
// RES_LoadToHeap/SNASYS_LoadSpriteListFromMemory/CS_knlFree (all exported,
// all left completely unmodified) -- only the success/failure test changes.
// UTIL_LoadSpriteListFromResource is exported (confirmed via `nm -D`) with
// exactly one call site in the whole decompile (IMGFONT_Create), so this
// can't affect any other code path.
//
// (First attempt at this fix regressed the boot sequence -- it now crashed
// during loading, before ever reaching Title/Menu, with an Undefined
// Instruction exception 4 bytes into SNASYS_LoadSpriteListFromMemory. Every
// other OFF_* constant in this file is deliberately ODD -- the system `nm -D`/
// `objdump -T` used to look these up reports the even, Thumb-bit-stripped
// address, and calling through it directly makes the CPU decode genuine
// Thumb-2 instructions as ARM, which reliably hits an illegal encoding
// almost immediately. This offset was copy-pasted from a plain `nm -D`
// lookup without the `+1` every other OFF_* here already carries -- fixed.)
#define OFF_SNASYS_LoadSpriteListFromMemory 0x000b0c11

typedef uint32_t (*fn_snasys_load_sprite_list_from_memory_t)(void *, void *, uint32_t, uint32_t);

static int UTIL_LoadSpriteListFromResource_hook(const char *name, void *out_ptr,
                                                 uint32_t param_3, uint32_t param_4) {
    fn_res_load_to_heap_t RES_LoadToHeap_real = (fn_res_load_to_heap_t)(so_mod.text_base + OFF_RES_LoadToHeap);
    fn_cs_knlfree_t CS_knlFree_real = (fn_cs_knlfree_t)(so_mod.text_base + OFF_CS_knlFree);
    fn_snasys_load_sprite_list_from_memory_t SNASYS_LoadSpriteListFromMemory_real =
        (fn_snasys_load_sprite_list_from_memory_t)(so_mod.text_base + OFF_SNASYS_LoadSpriteListFromMemory);

    int handle = RES_LoadToHeap_real(name, NULL);
    if (handle == -1 || handle == -2 || handle == -3) {
        l_debug("Corrida 57: UTIL_LoadSpriteListFromResource: RES_LoadToHeap(\"%s\") failed with real "
                "error code %d", name, handle);
        return 0;
    }

    void *data = (void *) (uintptr_t) handle; // CS_GETDPTR is identity
    uint32_t result = SNASYS_LoadSpriteListFromMemory_real(data, out_ptr, param_3, param_4);
    CS_knlFree_real(handle);
    l_debug("Corrida 57: UTIL_LoadSpriteListFromResource(\"%s\") recovered a valid pointer (%p) the real "
            ".so's own sign check would have discarded -> %u", name, data, result);
    return (int) result;
}

// --- Corrida 58: does GRP_PutAsciiChar/GRP_DrawPixel (the CSFB software text
// renderer) actually run, and if it does, is it writing into the SAME buffer
// SGL_FrameBufferToScreen uploads to the screen? ----------------------------
//
// The user reports ALL dynamically-drawn text is invisible (menus, HP, item
// names, dialogues) while sprite-baked "image text" renders fine. Every one
// of those goes through GRP_PutAsciiChar/GRP_PutHanChar (out_ghidra.c:20971/
// 21003) -> GRP_DrawPixel (out_ghidra.c:20948), a per-pixel software
// rasterizer that writes directly into whatever
// CS_GRP_GET_FRAME_BUFFER_POINTER(GRP_MainFrameBuffer) resolves to -- a
// completely different code path from the SPR_Draw/GL sprite rendering
// already confirmed working. Corrida 30 established that this "CSFB"
// abstraction's own flush function (CS_grpFlushLcd) has zero call sites
// anywhere in the .so, and that what actually presents frames is
// GRP_Flush -> ... -> SGL_FrameBufferToScreen (glTexSubImage2D from
// g_pSGLFrameBuffer/g_pSGLFrameBufferImage, out_ghidra.c:107251) -- but it
// was never confirmed whether CSFB's software buffer and SGL's uploaded
// buffer are the SAME memory (text should already show if so) or two
// unrelated allocations (in which case GRP_PutAsciiChar's output is written
// somewhere that's simply never displayed).
//
// Rather than guess further across two only-partially-decompiled graphics
// abstractions, this hooks GRP_PutAsciiChar (confirms text drawing is even
// reached, with which glyph/position) and GRP_DrawPixel (resolves and logs
// the real destination pointer via the SAME CS_GRP_GET_FRAME_BUFFER_POINTER
// call the .so itself uses, plus the active clip rect/color) for the first
// few calls, alongside g_pSGLFrameBuffer/g_pSGLFrameBufferImage's raw values
// at that same moment -- directly answering whether they're the same buffer.
// All symbols are exported (confirmed via `nm -D`); transparent hooks, call
// through unchanged.
typedef void *(*fn_get_frame_buffer_ptr_t)(int);

static so_hook h_GRP_PutAsciiChar, h_GRP_DrawPixel;
static int *g_ptr_GRP_MainFrameBuffer = NULL;
static int *g_ptr_GRP_nColor = NULL;
static int *g_ptr_GRP_nClipBX = NULL, *g_ptr_GRP_nClipBY = NULL,
        *g_ptr_GRP_nClipEX = NULL, *g_ptr_GRP_nClipEY = NULL;
static void **g_ptr_g_pSGLFrameBuffer = NULL, **g_ptr_g_pSGLFrameBufferImage = NULL;
static int g_text_log_count = 0;
#define TEXT_LOG_MAX 20

static int g_ascii_log_count = 0;

static void GRP_PutAsciiChar_hook(int x, int y, int ch) {
    if (g_ascii_log_count < TEXT_LOG_MAX) {
        l_debug("Corrida 58: GRP_PutAsciiChar(x=%d, y=%d, ch=%d '%c')", x, y, ch,
                (ch >= 0x20 && ch < 0x7f) ? ch : '?');
        g_ascii_log_count++;
    }
    SO_CONTINUE_VOID(h_GRP_PutAsciiChar, x, y, ch);
}

static void GRP_DrawPixel_hook(int x, int y) {
    if (g_text_log_count < TEXT_LOG_MAX) {
        fn_get_frame_buffer_ptr_t CS_GRP_GET_FRAME_BUFFER_POINTER_real =
            (fn_get_frame_buffer_ptr_t) so_symbol(&so_mod, "CS_GRP_GET_FRAME_BUFFER_POINTER");
        void *fb_ptr = (CS_GRP_GET_FRAME_BUFFER_POINTER_real && g_ptr_GRP_MainFrameBuffer)
            ? CS_GRP_GET_FRAME_BUFFER_POINTER_real(*g_ptr_GRP_MainFrameBuffer) : NULL;
        l_debug("Corrida 58: GRP_DrawPixel(x=%d, y=%d) CSFB_fb=%p color=%08x "
                "clip=[%d,%d,%d,%d] sgl_fb=%p sgl_fb_image=%p", x, y, fb_ptr,
                g_ptr_GRP_nColor ? *g_ptr_GRP_nColor : -1,
                g_ptr_GRP_nClipBX ? *g_ptr_GRP_nClipBX : -1,
                g_ptr_GRP_nClipBY ? *g_ptr_GRP_nClipBY : -1,
                g_ptr_GRP_nClipEX ? *g_ptr_GRP_nClipEX : -1,
                g_ptr_GRP_nClipEY ? *g_ptr_GRP_nClipEY : -1,
                g_ptr_g_pSGLFrameBuffer ? *g_ptr_g_pSGLFrameBuffer : NULL,
                g_ptr_g_pSGLFrameBufferImage ? *g_ptr_g_pSGLFrameBufferImage : NULL);
        g_text_log_count++;
    }
    SO_CONTINUE_VOID(h_GRP_DrawPixel, x, y);
}

void so_patch(void) {
    hook_addr(so_mod.text_base + OFF_excuteDirectly, (uintptr_t)&excuteDirectly_bypass);
    hook_addr(so_mod.text_base + OFF_jIsCracked, (uintptr_t)&jIsCracked_bypass);
    hook_addr(so_mod.text_base + OFF_png_create_struct_2, (uintptr_t)&png_create_struct_2_hook);
    hook_addr(so_mod.text_base + OFF_png_error, (uintptr_t)&png_error_hook);
    hook_addr(so_mod.text_base + OFF_File_GetFullPathName, (uintptr_t)&File_GetFullPathName_hook);

    g_ptr_CS_grpFlushLcd = (void (**)(void)) so_symbol(&so_mod, "__CS_grpFlushLcd");
    uintptr_t addr_CS_grpFlushLcd = so_symbol(&so_mod, "CS_grpFlushLcd");
    if (addr_CS_grpFlushLcd) {
        hook_addr(addr_CS_grpFlushLcd, (uintptr_t)&CS_grpFlushLcd_hook);
    } else {
        l_warn("CS_grpFlushLcd not found in .dynsym -- Corrida 28 hook skipped");
    }

    uintptr_t addr_GRP_Flush = so_symbol(&so_mod, "GRP_Flush");
    uintptr_t addr_GRPX_GetFlag = so_symbol(&so_mod, "GRPX_GetFlag");
    uintptr_t addr_GRPX_FlushAll = so_symbol(&so_mod, "GRPX_FlushAll");
    uintptr_t addr_GRPX_Flush = so_symbol(&so_mod, "GRPX_Flush");
    uintptr_t addr_SGL_grpFlushLcd = so_symbol(&so_mod, "SGL_grpFlushLcd");
    uintptr_t addr_SGL_FrameBufferToScreen = so_symbol(&so_mod, "SGL_FrameBufferToScreen");

    if (addr_GRP_Flush && addr_GRPX_GetFlag && addr_GRPX_FlushAll && addr_GRPX_Flush &&
        addr_SGL_grpFlushLcd && addr_SGL_FrameBufferToScreen) {
        h_GRP_Flush = hook_addr(addr_GRP_Flush, (uintptr_t)&GRP_Flush_hook);
        h_GRPX_GetFlag = hook_addr(addr_GRPX_GetFlag, (uintptr_t)&GRPX_GetFlag_hook);
        h_GRPX_FlushAll = hook_addr(addr_GRPX_FlushAll, (uintptr_t)&GRPX_FlushAll_hook);
        h_GRPX_Flush = hook_addr(addr_GRPX_Flush, (uintptr_t)&GRPX_Flush_hook);
        h_SGL_grpFlushLcd = hook_addr(addr_SGL_grpFlushLcd, (uintptr_t)&SGL_grpFlushLcd_hook);
        h_SGL_FrameBufferToScreen = hook_addr(addr_SGL_FrameBufferToScreen, (uintptr_t)&SGL_FrameBufferToScreen_hook);
    } else {
        l_warn("Corrida 30: one or more GRP/GRPX/SGL flush symbols not found in .dynsym -- hooks skipped");
    }

    uintptr_t addr_MainProcess = so_symbol(&so_mod, "MainProcess");
    g_ptr_gAppSleepInfo = (uint8_t *) so_symbol(&so_mod, "gAppSleepInfo");
    if (addr_MainProcess) {
        h_MainProcess = hook_addr(addr_MainProcess, (uintptr_t)&MainProcess_hook);
    } else {
        l_warn("Corrida 32: MainProcess not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_CS_UserAgreeSetCallback = so_symbol(&so_mod, "CS_UserAgreeSetCallback");
    if (addr_CS_UserAgreeSetCallback) {
        h_CS_UserAgreeSetCallback = hook_addr(addr_CS_UserAgreeSetCallback,
                                               (uintptr_t)&CS_UserAgreeSetCallback_hook);
    } else {
        l_warn("Corrida 33: CS_UserAgreeSetCallback not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_ResourceManager_Add = so_symbol(&so_mod, "_ZN15ResourceManager3AddEPKcPm");
    if (addr_ResourceManager_Add) {
        h_ResourceManager_Add = hook_addr(addr_ResourceManager_Add, (uintptr_t)&ResourceManager_Add_hook);
    } else {
        l_warn("Corrida 35: ResourceManager::Add(char const*, ulong*) not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_MMappedFile_Open = so_symbol(&so_mod, "_ZN11MMappedFile4OpenEPKcmmii");
    if (addr_MMappedFile_Open) {
        h_MMappedFile_Open = hook_addr(addr_MMappedFile_Open, (uintptr_t)&MMappedFile_Open_hook);
    } else {
        l_warn("Corrida 35: MMappedFile::Open not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_CS_knlGetResource = so_symbol(&so_mod, "CS_knlGetResource");
    if (addr_CS_knlGetResource) {
        h_CS_knlGetResource = hook_addr(addr_CS_knlGetResource, (uintptr_t)&CS_knlGetResource_hook);
    } else {
        l_warn("Corrida 36: CS_knlGetResource not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_FONT_Load = so_symbol(&so_mod, "FONT_Load");
    if (addr_FONT_Load) {
        h_FONT_Load = hook_addr(addr_FONT_Load, (uintptr_t)&FONT_Load_hook);
    } else {
        l_warn("Corrida 41: FONT_Load not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_RES_LoadToPool = so_symbol(&so_mod, "RES_LoadToPool");
    if (addr_RES_LoadToPool) {
        hook_addr(addr_RES_LoadToPool, (uintptr_t)&RES_LoadToPool_hook);
    } else {
        l_warn("Corrida 42: RES_LoadToPool not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_RES_MoveToFilePack = so_symbol(&so_mod, "RES_MoveToFilePack");
    if (addr_RES_MoveToFilePack) {
        hook_addr(addr_RES_MoveToFilePack, (uintptr_t)&RES_MoveToFilePack_hook);
    } else {
        l_warn("Corrida 47: RES_MoveToFilePack not found in .dynsym -- hook skipped");
    }

    g_ptr_GAMELOADER_pText = (uint32_t *) so_symbol(&so_mod, "GAMELOADER_pText");

    uintptr_t addr_GAMELOADER_Install = so_symbol(&so_mod, "GAMELOADER_Install");
    if (addr_GAMELOADER_Install) {
        h_GAMELOADER_Install = hook_addr(addr_GAMELOADER_Install, (uintptr_t)&GAMELOADER_Install_hook);
    } else {
        l_warn("Corrida 48: GAMELOADER_Install not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_FILEPACK_Install = so_symbol(&so_mod, "FILEPACK_Install");
    if (addr_FILEPACK_Install) {
        h_FILEPACK_Install = hook_addr(addr_FILEPACK_Install, (uintptr_t)&FILEPACK_Install_hook);
    } else {
        l_warn("Corrida 48: FILEPACK_Install not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_RES_MoveToFileEx = so_symbol(&so_mod, "RES_MoveToFileEx");
    if (addr_RES_MoveToFileEx) {
        h_RES_MoveToFileEx = hook_addr(addr_RES_MoveToFileEx, (uintptr_t)&RES_MoveToFileEx_hook);
    } else {
        l_warn("Corrida 48: RES_MoveToFileEx not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_GAMELOADER_ProcessInstall = so_symbol(&so_mod, "GAMELOADER_ProcessInstall");
    if (addr_GAMELOADER_ProcessInstall) {
        h_GAMELOADER_ProcessInstall = hook_addr(addr_GAMELOADER_ProcessInstall,
                                                 (uintptr_t)&GAMELOADER_ProcessInstall_hook);
    } else {
        l_warn("Corrida 48: GAMELOADER_ProcessInstall not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_EXCELDATA_LoadFullResource = so_symbol(&so_mod, "EXCELDATA_LoadFullResource");
    if (addr_EXCELDATA_LoadFullResource) {
        hook_addr(addr_EXCELDATA_LoadFullResource, (uintptr_t)&EXCELDATA_LoadFullResource_hook);
    } else {
        l_warn("Corrida 49: EXCELDATA_LoadFullResource not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_udivsi3 = so_symbol(&so_mod, "__udivsi3");
    if (addr_udivsi3) {
        h_udivsi3 = hook_addr(addr_udivsi3, (uintptr_t)&udivsi3_hook);
    } else {
        l_warn("Corrida 54: __udivsi3 not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_sgl_util_getglsize = so_symbol(&so_mod, "_ZN8SGL_Util9GetGLSizeEj");
    if (addr_sgl_util_getglsize) {
        hook_addr(addr_sgl_util_getglsize, (uintptr_t)&sgl_util_getglsize_hook);
    } else {
        l_warn("Corrida 55: SGL_Util::GetGLSize not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_util_getglsize = so_symbol(&so_mod, "__utilGetGLSize");
    if (addr_util_getglsize) {
        hook_addr(addr_util_getglsize, (uintptr_t)&util_getglsize_hook);
    } else {
        l_warn("Corrida 55: __utilGetGLSize not found in .dynsym -- hook skipped");
    }

    g_ptr_MAP_nHeapID = (int *) so_symbol(&so_mod, "MAP_nHeapID");

    uintptr_t addr_MAP_LoadAllToHeap = so_symbol(&so_mod, "MAP_LoadAllToHeap");
    if (addr_MAP_LoadAllToHeap && g_ptr_MAP_nHeapID) {
        hook_addr(addr_MAP_LoadAllToHeap, (uintptr_t)&MAP_LoadAllToHeap_hook);
    } else {
        l_warn("Corrida 56: MAP_LoadAllToHeap or MAP_nHeapID not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_MAP_LoadFromHeap = so_symbol(&so_mod, "MAP_LoadFromHeap");
    if (addr_MAP_LoadFromHeap && g_ptr_MAP_nHeapID) {
        hook_addr(addr_MAP_LoadFromHeap, (uintptr_t)&MAP_LoadFromHeap_hook);
    } else {
        l_warn("Corrida 56: MAP_LoadFromHeap or MAP_nHeapID not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_MAP_DestroyFromHeap = so_symbol(&so_mod, "MAP_DestroyFromHeap");
    if (addr_MAP_DestroyFromHeap && g_ptr_MAP_nHeapID) {
        hook_addr(addr_MAP_DestroyFromHeap, (uintptr_t)&MAP_DestroyFromHeap_hook);
    } else {
        l_warn("Corrida 56: MAP_DestroyFromHeap or MAP_nHeapID not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_UTIL_LoadSpriteListFromResource = so_symbol(&so_mod, "UTIL_LoadSpriteListFromResource");
    if (addr_UTIL_LoadSpriteListFromResource) {
        hook_addr(addr_UTIL_LoadSpriteListFromResource, (uintptr_t)&UTIL_LoadSpriteListFromResource_hook);
    } else {
        l_warn("Corrida 57: UTIL_LoadSpriteListFromResource not found in .dynsym -- hook skipped");
    }

    g_ptr_GRP_MainFrameBuffer = (int *) so_symbol(&so_mod, "GRP_MainFrameBuffer");
    g_ptr_GRP_nColor = (int *) so_symbol(&so_mod, "GRP_nColor");
    g_ptr_GRP_nClipBX = (int *) so_symbol(&so_mod, "GRP_nClipBX");
    g_ptr_GRP_nClipBY = (int *) so_symbol(&so_mod, "GRP_nClipBY");
    g_ptr_GRP_nClipEX = (int *) so_symbol(&so_mod, "GRP_nClipEX");
    g_ptr_GRP_nClipEY = (int *) so_symbol(&so_mod, "GRP_nClipEY");
    g_ptr_g_pSGLFrameBuffer = (void **) so_symbol(&so_mod, "g_pSGLFrameBuffer");
    g_ptr_g_pSGLFrameBufferImage = (void **) so_symbol(&so_mod, "g_pSGLFrameBufferImage");

    uintptr_t addr_GRP_PutAsciiChar = so_symbol(&so_mod, "GRP_PutAsciiChar");
    if (addr_GRP_PutAsciiChar) {
        h_GRP_PutAsciiChar = hook_addr(addr_GRP_PutAsciiChar, (uintptr_t)&GRP_PutAsciiChar_hook);
    } else {
        l_warn("Corrida 58: GRP_PutAsciiChar not found in .dynsym -- hook skipped");
    }

    uintptr_t addr_GRP_DrawPixel = so_symbol(&so_mod, "GRP_DrawPixel");
    if (addr_GRP_DrawPixel) {
        h_GRP_DrawPixel = hook_addr(addr_GRP_DrawPixel, (uintptr_t)&GRP_DrawPixel_hook);
    } else {
        l_warn("Corrida 58: GRP_DrawPixel not found in .dynsym -- hook skipped");
    }

    // The Com2uS library in this game relies on multiple static JavaVM pointers.
    // They are left uninitialized because JNI_OnLoad only initializes one copy
    // (DAT_0017de70, out_ghidra.c:144175 -- `DAT_0017de70 = param_1;` inside
    // JNI_OnLoad itself, so that one's already correct and must NOT be
    // touched here). Every other copy is meant to be populated lazily, on
    // first use, by each subsystem's own init function calling GetJavaVM
    // through the JNIEnv vtable (`(**(code**)(*env + 0x36c))(env, &DAT_xxx)`,
    // confirmed via decompile grep for all 7 copies: 0x16dda4/0x16de24/
    // 0x16de40/0x16de50/0x16de58/0x16de64/0x16de70) -- but this port never
    // drives those subsystems (Hub/ActiveUser/ads/etc.) through whatever
    // real init call would normally trigger that lazy population, so they
    // stay NULL forever. Corrida 38: confirmed by disassembly (not
    // hypothesis) that this is exactly what crashes CS_ADSetGameResolution
    // (`ldr r3, [r0, #0]` with r0 == NULL, R0 in the dump was literally
    // 0x00000000) -- startAd() (the ads/Mopub/Tapjoy bring-up path, already
    // flagged in CLAUDE.md as a no-op-until-proven-necessary subsystem) still
    // runs and reads its own cached copy (0x16de40) before anything ever
    // wrote to it. We must manually initialize every copy except 0x16de70 to
    // &jvm, otherwise any CS_*/ad-related function reading its own copy
    // crashes the same way the instant it's reached.
    static const uintptr_t g_com2us_vm_offsets[] = {
        0x16dda4, 0x16de24, 0x16de40, 0x16de50, 0x16de58, 0x16de64,
    };
    for (size_t i = 0; i < sizeof(g_com2us_vm_offsets) / sizeof(g_com2us_vm_offsets[0]); i++) {
        void **slot = (void **)(so_mod.text_base + g_com2us_vm_offsets[i]);
        *slot = &jvm;
    }
}
