/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include "settings.h"

#define CONFIG_FILE_PATH DATA_PATH"config.txt"

int  setting_sampleSetting;
bool setting_sampleSetting2;
unsigned int setting_button_remap[SETTING_BUTTON_REMAP_COUNT];
int setting_fps;

void settings_reset() {
    setting_sampleSetting  = 1;
    setting_sampleSetting2 = true;
    setting_fps = 15;
    for (int i = 0; i < SETTING_BUTTON_REMAP_COUNT; i++) setting_button_remap[i] = 0;
}

void settings_load() {
    settings_reset();

    char buffer[30];
    int value;

    FILE *config = fopen(CONFIG_FILE_PATH, "r");

    if (config) {
        while (EOF != fscanf(config, "%[^ ] %d\n", buffer, &value)) {
            if 		(strcmp("setting_sampleSetting", buffer) == 0) 	setting_sampleSetting  = (int)value;
            else if (strcmp("setting_sampleSetting2", buffer) == 0) setting_sampleSetting2 = (bool)value;
            else if (strcmp("setting_fps", buffer) == 0) setting_fps = value;
            else {
                for (int i = 0; i < SETTING_BUTTON_REMAP_COUNT; i++) {
                    char key[30];
                    snprintf(key, sizeof(key), "button_remap_%d", i);
                    if (strcmp(key, buffer) == 0) {
                        setting_button_remap[i] = (unsigned int) value;
                        break;
                    }
                }
            }
        }
        fclose(config);
    }
    if (setting_fps < 15 || setting_fps > 30) setting_fps = 15; // config.txt is user-editable
}

void settings_save() {
    FILE *config = fopen(CONFIG_FILE_PATH, "w+");

    if (config) {
        fprintf(config, "%s %d\n", "setting_sampleSetting", (int)(setting_sampleSetting));
        fprintf(config, "%s %d\n", "setting_sampleSetting2", (int)(setting_sampleSetting2));
        fprintf(config, "%s %d\n", "setting_fps", setting_fps);
        for (int i = 0; i < SETTING_BUTTON_REMAP_COUNT; i++) {
            fprintf(config, "button_remap_%d %u\n", i, setting_button_remap[i]);
        }
        fclose(config);
    }
}
