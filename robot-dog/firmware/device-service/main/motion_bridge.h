#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "../../shared/posture_policy.h"
#include "../../shared/rtc_calendar.h"
typedef struct {bool available,valid,pending;dog_rtc_date utc;uint32_t age_ms;} dog_rtc_snapshot;
void motion_bridge_rtc_snapshot(dog_rtc_snapshot *out);
void motion_bridge_init(void);
void motion_bridge_network(bool online);
/* Returned JSON is owned by the caller. HTTP status describes gateway receipt. */
cJSON *motion_bridge_submit(const char *id, const char *action, int argument, int channel,
                            const char *profile, int speed, int strength, uint32_t ttl_ms, int *http_status);
cJSON *motion_bridge_request(const char *id);
cJSON *motion_bridge_state(void);
void motion_bridge_stop(void);
/* Atomic with action submission under the bridge mutex. NULL reserves until
 * release on upload failure; successful OTA keeps the reservation until reboot. */
const char *motion_bridge_ota_reserve(void);
void motion_bridge_ota_release(void);
uint32_t motion_bridge_user_button_events(void);
cJSON *motion_bridge_aux_state(void);
cJSON *motion_bridge_aux_command(const char *command,unsigned value,int *status);
cJSON *motion_bridge_rtc_state(void);
cJSON *motion_bridge_rtc_sync(const char *utc,int *status);
cJSON *motion_bridge_eeprom_state(void);
cJSON *motion_bridge_eeprom_inspect(int *status);
cJSON *motion_bridge_pose_calibration(const char *command,int *http_status);
cJSON *motion_bridge_pose_calibration_ble(uint32_t session,const char *command,int *http_status);
dog_posture motion_bridge_posture(void);
bool motion_bridge_handling_hint(void);
cJSON *motion_bridge_power(cJSON *request,int *http_status);
cJSON *motion_bridge_power_history(void);
cJSON *motion_bridge_power_limit(cJSON *request,int *http_status);
bool motion_bridge_power_limit_warning(void);
cJSON *motion_bridge_pose_protection(cJSON *request,int *http_status);
bool motion_bridge_pose_protection_warning(void);
cJSON *motion_bridge_compass(const char *command,int *http_status);
cJSON *motion_bridge_compass_ble(uint32_t session,const char *command,int *http_status);
bool motion_bridge_calibration_active(void);
void motion_bridge_power_warning(bool *low,bool *show_message);
void motion_bridge_ble_session(uint32_t session);
cJSON *motion_bridge_submit_ble(uint32_t session,const char *id,const char *action,int repeats,int speed,int strength,int *http_status);

bool motion_bridge_controller_uart_acquire(void);
void motion_bridge_controller_uart_release(void);
