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

// Corrida 18 (PORTING_PLAN.md): png_create_read_struct_2's own stack canary
// (verified via disassembly to live at a fixed sp+132 in ITS frame, see
// so_frame_sp[33] in __stack_chk_fail_impl below) was proven correct right
// after the FIRST setjmp() call (checkpoint via png_create_struct_2's hook in
// patch.c) but corrupted by the time the function's epilogue re-checks it --
// narrowing the corruption window to somewhere between that point and the
// canary check, which includes this function's OWN second setjmp() call.
// Since setjmp/longjmp are already fully our own code (jmp_shim.S), no
// invasive hook on the .so is needed to checkpoint here too: caller_sp is the
// stack frame of WHATEVER function called setjmp/longjmp (not necessarily
// png_create_read_struct_2 -- libpng/other subsystems call setjmp elsewhere
// too), so sp+132 is only meaningful when caller_sp matches the known frame
// address from a "Stack collapsed" crash; harmless to log unconditionally
// otherwise.
void jmp_shim_log_setjmp(void *buf, void *caller_sp) {
    g_setjmp_call_count++;
    g_last_setjmp_buf = buf;
    uint32_t canary_peek = ((uint32_t *)caller_sp)[33];
    l_info("setjmp_soloader checkpoint: call#=%d buf=%p caller_sp=%p canary_at_sp132=%08x",
           g_setjmp_call_count, buf, caller_sp, canary_peek);
}

void jmp_shim_log_longjmp(void *buf, int val, void *caller_sp) {
    g_longjmp_call_count++;
    g_last_longjmp_buf = buf;
    g_last_longjmp_val = val;
    uint32_t canary_peek = ((uint32_t *)caller_sp)[33];
    l_info("longjmp_soloader checkpoint: call#=%d buf=%p val=%d caller_sp=%p canary_at_sp132=%08x",
           g_longjmp_call_count, buf, val, caller_sp, canary_peek);
}
