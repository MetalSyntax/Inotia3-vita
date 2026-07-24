/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>

#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

static SceKernelLwMutexWork _log_mutex;
// ATOMIC_VAR_INIT was deprecated in C17 and removed in C23; plain
// initialisation is the documented replacement and builds on every toolchain.
static atomic_bool _log_mutex_ready = false;
static atomic_bool _log_path_ready = false;
static char log_path[128];

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];

void _log_print(int t, const char* fmt, ...) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    if (!atomic_load_explicit(&_log_path_ready, memory_order_relaxed)) {
        sceIoMkdir("ux0:data/inotia3", 0777);
        sceIoMkdir("ux0:data/inotia3/logs", 0777);

        SceDateTime time;
        sceRtcGetCurrentClockLocalTime(&time);
        sceClibSnprintf(log_path, sizeof(log_path), "ux0:data/inotia3/logs/log_%04d-%02d-%02d_%02d-%02d-%02d.txt",
                        time.year, time.month, time.day, time.hour, time.minute, time.second);
        atomic_store_explicit(&_log_path_ready, true, memory_order_relaxed);
    }

    switch (t) {
        case LT_DEBUG:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s• debug%s    %s\n",
                            COLOR_PINK, COLOR_END, fmt); break;
        case LT_INFO:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %sℹ info%s     %s\n",
                            COLOR_BLUE, COLOR_END, fmt); break;
        case LT_WARN:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⚠ warning%s  %s\n",
                            COLOR_ORANGE, COLOR_END, fmt); break;
        case LT_ERROR:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⨯ error%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_FATAL:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! fatal%s    %s\n",
                            COLOR_RED, COLOR_END, fmt); break;
        case LT_SUCCESS:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! success%s  %s\n",
                            COLOR_GREEN, COLOR_END, fmt); break;
        case LT_WAIT:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s… waiting%s  %s\n",
                            COLOR_CYAN, COLOR_END, fmt); break;
        default:
            if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
                sceKernelUnlockLwMutex(&_log_mutex, 1);
            }
            return;
    }

    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);
    
    sceClibPrintf(buffer_b);
    // so-crash-triage (2026-07-21): keeping a single log_fd open for the
    // whole process and writing to it via sceIoWrite() lost data on a real
    // hard crash -- confirmed empirically (one run captured all 475 lines,
    // others came back truncated to just the final fatal line, same code
    // path every time) and NOT fixed by adding sceIoSyncByFd() after each
    // write either. Opening, writing, and closing the file fresh on every
    // single line is slower but makes each line's durability independent of
    // whatever OS-level write-back caching this device/storage combo does --
    // a close() is a far more universally-honored flush boundary than an
    // explicit sync call.
    SceUID fd = sceIoOpen(log_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buffer_b, strlen(buffer_b));
        sceIoClose(fd);
    }

    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
    }
}
