/*
 * @file  apk_zip.h
 * @brief Minimal ZIP central-directory reader for pulling STORED (never
 *        deflated) entries directly out of the real inotia3.apk on disk --
 *        see apk_zip.c for why this exists (WrapperJinterface.GetFileDescriptor).
 */

#ifndef INOTIA3_APK_ZIP_H
#define INOTIA3_APK_ZIP_H

#include <stdint.h>

// Looks up `entry_name` (the full path within the zip, e.g.
// "assets/common/game_res/MainMenu.png.jpg") inside the zip file at
// `apk_path`. Only STORED (uncompressed, method 0) entries are supported --
// that's the only kind Android's AssetManager.openFd() can ever return a
// raw offset+length for in the first place, which is exactly why the
// original APK renames these assets to already-compressed extensions
// (forces aapt to store them without deflating).
//
// Returns 0 on success with *out_offset/*out_length filled (raw byte offset
// and stored size within apk_path, ready for a plain fseek+fread), -1 if the
// file can't be opened, the entry isn't found, or it isn't STORED.
int apk_zip_find_stored_entry(const char *apk_path, const char *entry_name,
                               uint32_t *out_offset, uint32_t *out_length);

#endif // INOTIA3_APK_ZIP_H
