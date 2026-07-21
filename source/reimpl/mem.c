/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/mem.h"
#include "utils/logger.h"

#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <psp2/kernel/clib.h>

void *sceClibMemclr(void *dst, size_t len) {
    return sceClibMemset(dst, 0, len);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offs) {
    l_warn("mmap(%p, %i, %i, %i, %i, %li)", addr, length, prot, flags, fd, offs);

    if (length <= 0) {
        return MAP_FAILED;
    }
    
    void* ret = malloc(length);
    if (!ret) return MAP_FAILED;
    
    if (fd != -1) {
        off_t old_pos = lseek(fd, 0, SEEK_CUR);
        lseek(fd, offs, SEEK_SET);
        read(fd, ret, length);
        lseek(fd, old_pos, SEEK_SET);
    } else {
        memset(ret, 0, length);
    }
    
    return ret;
}

int munmap(void *addr, size_t length) {
    if (addr) free(addr);
    return 0;
}
