#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"
void audio_service_init(void);
void audio_service_stop(void);
bool audio_service_busy(void);
esp_err_t audio_service_http(httpd_req_t *req,const char *boot_id);
/* Exclusive synchronous audio operations for the on-device cloud task. */
esp_err_t audio_prepare_voice(volatile bool *cancel);
esp_err_t audio_capture_voice(uint8_t *pcm,size_t capacity,size_t *bytes,volatile bool *cancel);
esp_err_t audio_play_pcm(const uint8_t *pcm,size_t bytes,volatile bool *cancel);
