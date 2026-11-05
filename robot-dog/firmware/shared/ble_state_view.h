#pragma once
#include <stdbool.h>
#include "cJSON.h"
static inline void dog_ble_copy_fields(cJSON *to,const cJSON *from,const char *const *keys,unsigned count)
{
    for(unsigned i=0;i<count;i++){
        const cJSON *v=from?cJSON_GetObjectItemCaseSensitive(from,keys[i]):NULL;
        if(v)cJSON_AddItemToObject(to,keys[i],cJSON_Duplicate(v,true));
    }
}
/* Deliberate projection: HTTP may grow without exhausting a BLE receipt.
 * Raw samples, calibration matrices and verbose reference metadata stay HTTP-only. */
static inline cJSON *dog_ble_motion_view(const cJSON *full)
{
    cJSON *out=cJSON_CreateObject();
    const char *const root[]={"network_online","calibration_active","controller_version","controller_online","controller_status_age_ms","baseline_set","armed","controller_moving","stop_pending","tail_period_us","pulse_us","active_request","last_error","physical_position_verified"};
    dog_ble_copy_fields(out,full,root,sizeof(root)/sizeof(root[0]));
    const cJSON *supply=full?cJSON_GetObjectItemCaseSensitive(full,"supply"):NULL;
    cJSON *s=cJSON_AddObjectToObject(out,"supply");
    const char *const sk[]={"supported","available","calibrated","millivolts","raw","age_ms","battery_percent","classification","battery_terminal_measurement"};
    dog_ble_copy_fields(s,supply,sk,sizeof(sk)/sizeof(sk[0]));
    const cJSON *monitor=supply?cJSON_GetObjectItemCaseSensitive(supply,"monitor"):NULL;
    cJSON *m=cJSON_AddObjectToObject(s,"monitor");const char *const mk[]={"source","status","alerts_enabled","low"};
    dog_ble_copy_fields(m,monitor,mk,sizeof(mk)/sizeof(mk[0]));
    const cJSON *sensors=full?cJSON_GetObjectItemCaseSensitive(full,"sensors"):NULL,*pose=sensors?cJSON_GetObjectItemCaseSensitive(sensors,"pose"):NULL;
    cJSON *ps=cJSON_AddObjectToObject(cJSON_AddObjectToObject(out,"sensors"),"pose");
    const char *const pk[]={"available","mount_calibrated","roll_deg","pitch_deg","posture","motion_protection_enabled"};
    dog_ble_copy_fields(ps,pose,pk,sizeof(pk)/sizeof(pk[0]));
    const cJSON *compass=sensors?cJSON_GetObjectItemCaseSensitive(sensors,"compass"):NULL;
    cJSON *cs=cJSON_AddObjectToObject(cJSON_GetObjectItemCaseSensitive(out,"sensors"),"compass");
    const char *const ck[]={"available","calibrated","heading_deg","status"};dog_ble_copy_fields(cs,compass,ck,sizeof(ck)/sizeof(ck[0]));return out;
}
