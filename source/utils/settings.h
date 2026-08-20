/*
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  settings.h
 * @brief Loader settings that can be set via a configurator app.
 */

#ifndef SOLOADER_SETTINGS_H
#define SOLOADER_SETTINGS_H

#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int  setting_sampleSetting;
extern bool setting_sampleSetting2;

// Physical-button remap table: setting_button_remap[i] holds the SCE_CTRL_*
// bitmask of whichever physical button should now trigger wr_touch.c's
// BUTTON_MAP[i] action (0 == "use BUTTON_MAP[i]'s own compiled-in default").
// Kept as a plain uint array (not an SCE_CTRL_* typed one) so this generic
// settings module doesn't need to depend on psp2/ctrl.h. The size is fixed
// at Inotia3's current 12 BUTTON_MAP entries (source/wr_touch.c) -- if that
// table ever gains/loses an entry, update SETTING_BUTTON_REMAP_COUNT to match.
#define SETTING_BUTTON_REMAP_COUNT 12
extern unsigned int setting_button_remap[SETTING_BUTTON_REMAP_COUNT];

void settings_load();
void settings_save();
void settings_reset();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_SETTINGS_H
