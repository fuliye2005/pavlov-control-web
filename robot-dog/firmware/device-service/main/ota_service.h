#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "esp_http_server.h"
void ota_service_init(void);
void ota_service_services_ready(void);
bool ota_service_busy(void);
cJSON *ota_service_state(void);
/* Caller must authenticate before dispatching the binary body. */
esp_err_t ota_service_upload(httpd_req_t *req,const char *boot_id);
