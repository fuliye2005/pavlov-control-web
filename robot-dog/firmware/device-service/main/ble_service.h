#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
typedef struct {bool initialized,connected,authenticated,pairing_window,pending,busy;uint32_t passkey;int error;} dog_ble_snapshot;
void ble_service_init(void);
bool ble_service_ready(void);
void ble_service_snapshot(dog_ble_snapshot *out);
cJSON *ble_service_state(void);
bool ble_service_pair_window(void);
bool ble_service_confirm(bool accept);
