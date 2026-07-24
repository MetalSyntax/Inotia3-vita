/*
 * @file  apk_zip.c
 * @brief See apk_zip.h.
 *
 * Fase 6/7 follow-up (2026-07-21): a real device log showed
 * WrapperJinterface.GetFileDescriptor("MainMenu.png", ...) always failing
 * (our old stub returned NULL unconditionally) right before the .so gave up
 * loading the main menu background -- plausible root cause of the reported
 * black screen. Cross-referencing decompiled_apk's WrapperData.java
 * (GAME_ASSETS_PATH="common/game_res", RES_NAME_APPEND=".jpg") and
 * WrapperJinterface.java's real GetFileDescriptor() confirms the actual
 * asset name inside the APK zip is "assets/common/game_res/<filename>.jpg"
 * -- exactly matching the NAME.ogg.jpg / NAME.png.jpg files already found in
 * decompiled_apk/resources/assets/common/game_res/ (PORTING_PLAN.md Fase 7).
 * That ".jpg" suffix is NOT a red herring to strip when staging assets --
 * it's the literal, load-bearing zip entry name the game already expects.
 *
 * decompiled_so/libgame_armeabi/out_ghidra.c's jGetFileDescriptorEx()
 * confirms the call site that matters here (CS_fsGetFileDescriptor, called
 * with closeFDdirectly=1) only reads the returned offset/length pair -- it
 * never dereferences the returned Java object -- then separately opens the
 * real APK (GetApkFilePath()) and reads the asset directly at that offset.
 * So all this needs to provide is *offset + length* within the real
 * ux0:/data/inotia3/inotia3.apk file already confirmed openable in the
 * device log; no fake FileDescriptor jobject is required.
 */

#include "apk_zip.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define EOCD_SIG 0x06054b50u
#define CDFH_SIG 0x02014b50u
#define LFH_SIG  0x04034b50u

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

int apk_zip_find_stored_entry(const char *apk_path, const char *entry_name,
                               uint32_t *out_offset, uint32_t *out_length) {
    FILE *f = fopen(apk_path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long file_size = ftell(f);
    if (file_size < 22) { fclose(f); return -1; }

    // Scan backward for the End Of Central Directory record. No Zip64
    // support needed (an APK this old/small never needs it); the trailing
    // comment field the EOCD signature can hide behind is capped at 64KB by
    // the ZIP format itself.
    long scan_len = file_size < (64 * 1024 + 22) ? file_size : (64 * 1024 + 22);
    uint8_t *tail = (uint8_t *) malloc(scan_len);
    if (!tail) { fclose(f); return -1; }
    fseek(f, file_size - scan_len, SEEK_SET);
    if (fread(tail, 1, scan_len, f) != (size_t) scan_len) { free(tail); fclose(f); return -1; }

    long eocd_pos = -1;
    for (long i = scan_len - 22; i >= 0; i--) {
        if (rd_u32(tail + i) == EOCD_SIG) { eocd_pos = i; break; }
    }
    if (eocd_pos < 0) { free(tail); fclose(f); return -1; }

    uint32_t cd_size = rd_u32(tail + eocd_pos + 12);
    uint32_t cd_offset = rd_u32(tail + eocd_pos + 16);
    free(tail);

    uint8_t *cd = (uint8_t *) malloc(cd_size);
    if (!cd) { fclose(f); return -1; }
    fseek(f, cd_offset, SEEK_SET);
    if (fread(cd, 1, cd_size, f) != cd_size) { free(cd); fclose(f); return -1; }

    size_t want_len = strlen(entry_name);
    uint32_t pos = 0;
    int found = 0;
    uint16_t method = 0;
    uint32_t comp_size = 0;
    uint32_t local_header_offset = 0;

    while (pos + 46 <= cd_size) {
        if (rd_u32(cd + pos) != CDFH_SIG) break;

        uint16_t this_method = rd_u16(cd + pos + 10);
        uint32_t this_comp_size = rd_u32(cd + pos + 20);
        uint16_t name_len = rd_u16(cd + pos + 28);
        uint16_t extra_len = rd_u16(cd + pos + 30);
        uint16_t comment_len = rd_u16(cd + pos + 32);
        uint32_t this_local_offset = rd_u32(cd + pos + 42);

        if (pos + 46 + name_len > cd_size) break; // malformed/truncated -- bail out safely

        if (name_len == want_len && memcmp(cd + pos + 46, entry_name, name_len) == 0) {
            found = 1;
            method = this_method;
            comp_size = this_comp_size;
            local_header_offset = this_local_offset;
            break;
        }

        pos += 46 + name_len + extra_len + comment_len;
    }
    free(cd);

    if (!found) { fclose(f); return -1; }
    if (method != 0) { fclose(f); return -1; } // only STORED entries have a usable raw offset

    // The actual file data starts after the LOCAL file header, whose
    // name/extra field lengths aren't guaranteed to match the central
    // directory's (rare in practice, but the format allows it).
    uint8_t lfh[30];
    fseek(f, local_header_offset, SEEK_SET);
    if (fread(lfh, 1, 30, f) != 30 || rd_u32(lfh) != LFH_SIG) { fclose(f); return -1; }
    uint16_t lfh_name_len = rd_u16(lfh + 26);
    uint16_t lfh_extra_len = rd_u16(lfh + 28);

    *out_offset = local_header_offset + 30 + lfh_name_len + lfh_extra_len;
    *out_length = comp_size;

    fclose(f);
    return 0;
}
