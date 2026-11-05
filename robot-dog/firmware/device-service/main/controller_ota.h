#pragma once
#include <stdbool.h>
#include "cJSON.h"
#include "esp_http_server.h"
bool controller_ota_busy(void);
cJSON *controller_ota_state(void);
esp_err_t controller_ota_upload(httpd_req_t *req,const char *boot);
