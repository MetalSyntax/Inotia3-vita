/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/sys.h"

#include <sys/errno.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <string.h>
#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>
#include <stdlib.h>

#include "utils/utils.h"
#include "utils/logger.h"

#define BIONIC_CLOCK_REALTIME           0
#define BIONIC_CLOCK_MONOTONIC          1
#define BIONIC_CLOCK_PROCESS_CPUTIME_ID 2
#define BIONIC_CLOCK_THREAD_CPUTIME_ID  3
#define BIONIC_CLOCK_MONOTONIC_RAW      4
#define BIONIC_CLOCK_REALTIME_COARSE    5
#define BIONIC_CLOCK_MONOTONIC_COARSE   6
#define BIONIC_CLOCK_BOOTTIME           7
#define BIONIC_CLOCK_REALTIME_ALARM     8
#define BIONIC_CLOCK_BOOTTIME_ALARM     9
#define BIONIC_CLOCK_SGI_CYCLE         10
#define BIONIC_CLOCK_TAI               11

// 1969 years in microseconds, used to adjust SCE tick to UNIX timestamp
#define __epoch 62135587294000000

int clock_gettime_soloader(clockid_t clock_id, struct timespec * tp) {
    switch (clock_id) {
        case BIONIC_CLOCK_MONOTONIC:
        case BIONIC_CLOCK_MONOTONIC_RAW:
        case BIONIC_CLOCK_MONOTONIC_COARSE:
        case BIONIC_CLOCK_BOOTTIME:
        case BIONIC_CLOCK_BOOTTIME_ALARM:
        case BIONIC_CLOCK_SGI_CYCLE:
        case BIONIC_CLOCK_PROCESS_CPUTIME_ID:
        case BIONIC_CLOCK_THREAD_CPUTIME_ID: {
            uint64_t proctime = sceKernelGetProcessTimeWide();

            tp->tv_sec = (proctime / 1000000);
            tp->tv_nsec = ((proctime - (tp->tv_sec * 1000000)) * 1000);
            break;
        }
        case BIONIC_CLOCK_REALTIME:
        case BIONIC_CLOCK_REALTIME_COARSE:
        case BIONIC_CLOCK_REALTIME_ALARM:
        case BIONIC_CLOCK_TAI: {
            SceRtcTick tick;
            sceRtcGetCurrentTick(&tick);
            tick.tick -= __epoch;

            tp->tv_sec = (tick.tick / 1000000);
            tp->tv_nsec = ((tick.tick - (tp->tv_sec * 1000000)) * 1000);
            break;
        }
        default:
            l_error("clock_gettime / unexpected clock id %i", clock_id);
    }

    return 0;
}

int clock_getres_soloader(clockid_t clock_id, struct timespec * res) {
    res->tv_sec = 0;
    res->tv_nsec = 1000;
    return 0;
}

clock_t clock_soloader(void) {
    return sceKernelGetProcessTimeLow();
}

int sigaction(int signum, const struct sigaction * act, struct sigaction * oldact) {
    l_warn("sigaction(%i, ...): not implemented", signum);
    return 0;
}

int __system_property_get_soloader(const char *name, char *value) {
    l_warn("__system_property_get(%s, %p): not implemented", name, value);
    strncpy(value, "psvita", 7);
    return 7;
}

void assert2(const char* f, int l, const char* func, const char* msg) {
    l_fatal("[%s:%i][%s] Assertion failed: %s", f, l, func, msg);
}

void syscall(int c) {
    l_warn("syscall(%i): not implemented", c);
}

// Fase 9 (2026-07-21): __stack_chk_fail is documented `noreturn` on every
// real libc (Bionic included) -- the compiler emits NO valid code after the
// call site in any stack-protected function, only its literal constant
// pool. Our old implementation just logged and fell off the end, so once
// the .so's own code (compiled expecting __stack_chk_fail to never return)
// resumed after the call, it executed that constant pool as instructions --
// confirmed via a real crash dump (PC landed a few bytes past the call,
// squarely inside the same function's literal pool, a Data abort). abort()
// (matching abort_soloader() below) actually halts instead of returning.
// Fase 9 (Corrida 7/8): per-line file logging proved unreliable across a
// hard crash on this device -- only THIS message ever survives to the log,
// every single time, no matter what we tried on the logger's I/O side. So
// jmp_shim.S's setjmp/longjmp call counters (source/reimpl/jmp_shim_log.c)
// get folded directly into this one reliably-delivered message instead of
// depending on their own separate log lines.
extern int g_setjmp_call_count;
extern int g_longjmp_call_count;
extern void *g_last_longjmp_buf;
extern int g_last_longjmp_val;
extern void *__stack_chk_guard;

// Corrida 16 (PORTING_PLAN.md): the stack-size bump (Corrida 15) didn't stop
// __stack_chk_fail from firing at the exact same spot inside libpng's
// png_create_read_struct_2 -- next step is reading the .so's OWN 136-byte
// stack frame at the moment of the mismatch instead of guessing from static
// disassembly. `blx __stack_chk_fail@plt` doesn't touch SP, so SP at the
// crash IS still exactly that function's frame (the canary-fail path never
// runs `add sp, #136`) -- a naked trampoline captures it before our own
// prologue could adjust it, then hands it to the real (non-naked) C function
// as an argument. Everything is folded into ONE l_fatal() call, matching the
// jmp_shim counters above -- per Fase 9 (Corrida 7/8), only the first fatal
// line is guaranteed to survive a hard crash on this device.
__attribute__((noreturn, noinline, used))
void __stack_chk_fail_impl(uint32_t *so_frame_sp) {
    l_fatal("Stack collapsed at %p (setjmp=%d longjmp=%d last_longjmp_buf=%p last_longjmp_val=%d) "
            "guard_now=%08x saved=%08x sp=%p "
            "frame=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,"
            "%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
            __builtin_return_address(0), g_setjmp_call_count, g_longjmp_call_count,
            g_last_longjmp_buf, g_last_longjmp_val,
            (unsigned int)(uintptr_t)__stack_chk_guard, so_frame_sp[33], so_frame_sp,
            so_frame_sp[0], so_frame_sp[1], so_frame_sp[2], so_frame_sp[3],
            so_frame_sp[4], so_frame_sp[5], so_frame_sp[6], so_frame_sp[7],
            so_frame_sp[8], so_frame_sp[9], so_frame_sp[10], so_frame_sp[11],
            so_frame_sp[12], so_frame_sp[13], so_frame_sp[14], so_frame_sp[15],
            so_frame_sp[16], so_frame_sp[17], so_frame_sp[18], so_frame_sp[19],
            so_frame_sp[20], so_frame_sp[21], so_frame_sp[22], so_frame_sp[23],
            so_frame_sp[24], so_frame_sp[25], so_frame_sp[26], so_frame_sp[27],
            so_frame_sp[28], so_frame_sp[29], so_frame_sp[30], so_frame_sp[31],
            so_frame_sp[32], so_frame_sp[33]);
    abort();
}

__attribute__((naked, noreturn))
void __stack_chk_fail_soloader(void) {
    asm volatile(
        "mov r0, sp\n"
        "b __stack_chk_fail_impl\n"
    );
}

void abort_soloader() {
    l_fatal("Abort called from address %p", __builtin_return_address(0));
    abort();
}

void exit_soloader(int status) {
    l_fatal("Exit(%i) called from %p", status, __builtin_return_address(0));
    exit(status);
}

int __atomic_dec(volatile int *ptr) {
    return __sync_fetch_and_sub(ptr, 1);
}

int __atomic_inc(volatile int *ptr) {
    return __sync_fetch_and_add(ptr, 1);
}

int __atomic_swap(int new_value, volatile int *ptr) {
    int old_value;
    do {
        old_value = *ptr;
    } while (__sync_val_compare_and_swap(ptr, old_value, new_value) != old_value);
    return old_value;
}

int __atomic_cmpxchg(int old_value, int new_value, volatile int* ptr) {
    /* We must return 0 on success */
    return __sync_val_compare_and_swap(ptr, old_value, new_value) != old_value;
}

char * getenv_soloader(const char * var) {
    l_warn("getenv(\"%s\"): not implemented.", var);
    return NULL;
}

int setenv_soloader(const char * name, const char * value, int overwrite) {
    l_warn("setenv(\"%s\", \"%s\"): not implemented.", name, value);
    return 0;
}

int getpagesize(void) {
    return PAGE_SIZE;
}
