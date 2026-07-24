/*
 * @file  jmp_shim.h
 * @brief See jmp_shim.S for why this exists (Bionic vs. newlib jmp_buf
 *        size mismatch -- PORTING_PLAN.md Fase 9).
 */

#ifndef INOTIA3_JMP_SHIM_H
#define INOTIA3_JMP_SHIM_H

// buf must point to at least 10 words (40 bytes) of storage -- the .so's own
// jmp_buf allocation (sized for Android Bionic) is guaranteed to be at least
// that large, since that's the minimum any AAPCS-compliant setjmp/longjmp
// needs regardless of implementation.
int setjmp_soloader(void *buf);
void longjmp_soloader(void *buf, int val);

#endif // INOTIA3_JMP_SHIM_H
