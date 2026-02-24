#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define AUDIO_SERVICE_MAX_FILES      24
#define AUDIO_SERVICE_MAX_NAME_LEN   48

esp_err_t audio_service_init(void);

bool audio_service_is_recording(void);
bool audio_service_is_playing(void);

esp_err_t audio_service_start_recording(char *out_path, size_t out_path_size);
esp_err_t audio_service_stop_recording(void);
esp_err_t audio_service_play_file(const char *path);
esp_err_t audio_service_set_playback_volume_percent(int percent);
int audio_service_get_playback_volume_percent(void);

esp_err_t audio_service_list_files(char names[][AUDIO_SERVICE_MAX_NAME_LEN],
                                   size_t max_files,
                                   size_t *out_count);
