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

// Corrida 60: implementing real text rendering (Corrida 59) multiplied this
// port's JNI log volume several times over (every on-screen string now walks
// GetByteArrayElements/GetArrayLength/ReleaseByteArrayElements plus the
// existing GetStringWidthHeightInRect calls, every single frame, for every
// label on screen) -- and every LT_DEBUG/LT_INFO line below used to pay for
// its own sceIoOpen+sceIoWrite+sceIoClose, a full filesystem round trip PER
// LINE (10000+ lines in a normal play session, confirmed against log_005.txt).
// That per-line open/close was deliberate (so-crash-triage, 2026-07-21): a
// single long-lived fd written via sceIoWrite lost data on a real hard crash,
// even with an explicit sceIoSyncByFd() after every write -- only closing the
// fd was a reliable flush boundary on this device/storage combo. Batching
// keeps that same guarantee for anything crash-relevant while cutting the
// I/O cost for the bulk, high-frequency traffic that doesn't need it:
// LT_DEBUG/LT_INFO lines accumulate in this small in-memory ring instead of
// hitting the filesystem individually, flushed (one single open/write/close
// for the whole batch) whenever it's about to overflow -- roughly an
// order-of-magnitude fewer filesystem round trips for the exact same log
// content. LT_WARN/LT_ERROR/LT_FATAL (rare, and the whole reason the log
// exists during a crash-triage session) flush the ring FIRST (so the debug
// context leading up to them isn't silently dropped) and then still write
// immediately with the original open+write+close, unchanged -- the crash-time
// guarantee this project has relied on all session is not weakened. Sized
// small on purpose (not "as big as possible") to bound how many lines a
// genuine hard crash (which bypasses this logger entirely, caught by the
// system exception handler instead) could still lose unflushed -- at most
// the handful of lines that fit in one ring, not an arbitrary batch.
#define LOG_RING_CAPACITY 4096
static char log_ring[LOG_RING_CAPACITY];
static size_t log_ring_len = 0;

static void log_flush_ring(void) {
    if (log_ring_len == 0) return;
    SceUID fd = sceIoOpen(log_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, log_ring, log_ring_len);
        sceIoClose(fd);
    }
    log_ring_len = 0;
}

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

        // so-crash-triage (2026-08-07): this console's RTC resets to the
        // same fixed value on every boot (no battery-backed clock), so a
        // timestamp-based filename collided across sessions -- a fresh run
        // silently appended onto a previous, already-analyzed log instead of
        // starting a new file, making it look like no new log was ever
        // captured. An incremental counter has no such dependency: scan for
        // the first log_NNN.txt that doesn't exist yet so every run gets its
        // own file regardless of the clock. Once all 999 slots are taken,
        // wrap back to 001 and truncate it (oldest log gets overwritten)
        // rather than silently stop logging.
        int slot = 1;
        for (; slot <= 999; slot++) {
            sceClibSnprintf(log_path, sizeof(log_path), "ux0:data/inotia3/logs/log_%03d.txt", slot);
            SceUID probe = sceIoOpen(log_path, SCE_O_RDONLY, 0);
            if (probe < 0) {
                break; // doesn't exist yet -- this is our slot
            }
            sceIoClose(probe);
        }
        if (slot > 999) {
            slot = 1;
            sceClibSnprintf(log_path, sizeof(log_path), "ux0:data/inotia3/logs/log_%03d.txt", slot);
            SceUID trunc = sceIoOpen(log_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
            if (trunc >= 0) {
                sceIoClose(trunc);
            }
        }
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

    size_t line_len = strlen(buffer_b);
    if (t == LT_WARN || t == LT_ERROR || t == LT_FATAL) {
        // Flush whatever debug/info context is already buffered first, so
        // the lines leading up to this warning/error/fatal aren't dropped --
        // then write THIS line the original, immediately-durable way.
        log_flush_ring();
        SceUID fd = sceIoOpen(log_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, buffer_b, line_len);
            sceIoClose(fd);
        }
    } else if (line_len >= LOG_RING_CAPACITY) {
        // Longer than the whole ring -- flush what's pending and write this
        // one line directly rather than trying to fit it in pieces.
        log_flush_ring();
        SceUID fd = sceIoOpen(log_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, buffer_b, line_len);
            sceIoClose(fd);
        }
    } else {
        if (log_ring_len + line_len > LOG_RING_CAPACITY) {
            log_flush_ring();
        }
        memcpy(log_ring + log_ring_len, buffer_b, line_len);
        log_ring_len += line_len;
    }

    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
    }
}
