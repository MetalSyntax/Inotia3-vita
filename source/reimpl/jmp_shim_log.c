/*
 * @file  jmp_shim_log.c
 * @brief Diagnostic counters for jmp_shim.S's setjmp_soloader/longjmp_soloader
 *        -- see PORTING_PLAN.md Fase 9 (Corrida 5): the stack-protector check
 *        inside libpng's png_create_read_struct_2 kept failing even after
 *        jmp_shim.S was verified correct, so we need concrete data on
 *        whether/how many times setjmp/longjmp run before the crash.
 *
 * Corrida 7/8: per-call file logging (source/utils/logger.c) turned out to
 * be unreliable across a hard crash on this device/card -- neither
 * sceIoSyncByFd() nor open+write+close per line got the earlier lines to
 * survive, only the final l_fatal() from __stack_chk_fail_soloader() ever
 * makes it into the log, every single time. So instead of separate log
 * lines, these are exposed as plain global counters that
 * __stack_chk_fail_soloader() (source/reimpl/sys.c) reads and prints
 * directly in that SAME reliably-delivered message -- no extra I/O, no
 * extra window for data loss.
 */

#include "utils/logger.h"

#include <stdint.h>

int g_setjmp_call_count = 0;
int g_longjmp_call_count = 0;
void *g_last_setjmp_buf = 0;
void *g_last_longjmp_buf = 0;
int g_last_longjmp_val = 0;

void jmp_shim_log_setjmp(void *buf) {
    g_setjmp_call_count++;
    g_last_setjmp_buf = buf;
}

void jmp_shim_log_longjmp(void *buf, int val) {
    g_longjmp_call_count++;
    g_last_longjmp_buf = buf;
    g_last_longjmp_val = val;
}
