#include "ble_service.h"
#include "ble_frames.h"
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "device_api.h"
#include "motion_bridge.h"
#include "../../shared/ble_state_view.h"
#include "cloud_dialog.h"
#include "interaction.h"
#include "user_settings.h"
#include "display_service.h"
#include "camera_service.h"
#include "ota_service.h"
#include "wifi_provision.h"
#include "wifi_control.h"
#include "audio_service.h"
#include "mbedtls/sha256.h"
#include "../../shared/action_catalog.h"
#include "rtc_service.h"
#include "countdown_service.h"

#define BLE_SERVICE_UUID "a7c00001-4c8b-4f4a-9d55-21db2ec0a001"
#define UUID(n) BLE_UUID128_INIT(0x01,0xa0,0xc0,0x2e,0xdb,0x21,0x55,0x9d,0x4a,0x4f,0x8b,0x4c,n,0x00,0xc0,0xa7)
static const ble_uuid128_t service_uuid=UUID(1),info_uuid=UUID(2),rx_uuid=UUID(3),tx_uuid=UUID(4);
typedef struct {uint32_t epoch;uint16_t transfer;char data[DOG_BLE_REQUEST_MAX+1];} ble_job;
typedef struct {char id[33];uint8_t request_hash[32];char response[DOG_BLE_RESPONSE_MAX];} receipt;
static SemaphoreHandle_t guard;
static QueueHandle_t jobs;
static receipt *receipts;
static unsigned receipt_cursor,commands,duplicates,connections,notify_errors;
static uint16_t connection=BLE_HS_CONN_HANDLE_NONE,tx_handle;
static uint32_t epoch,passkey,window_until,pair_until,last_sequence;
static char session[17];
static bool synced,authenticated,pair_pending,processing,transmitting,subscribed;
static int last_error;
static uint8_t address_type;
static dog_ble_receiver receiver;
static uint16_t last_transfer,last_response_bytes;
static uint32_t now_ms(void){return esp_timer_get_time()/1000;}
static bool before(uint32_t until){return until&&(int32_t)(until-now_ms())>0;}
static bool alive(uint32_t expected)
{xSemaphoreTake(guard,portMAX_DELAY);bool yes=connection!=BLE_HS_CONN_HANDLE_NONE&&authenticated&&epoch==expected;xSemaphoreGive(guard);return yes;}
void ble_service_snapshot(dog_ble_snapshot *s)
{
    memset(s,0,sizeof(*s));if(!guard)return;
    xSemaphoreTake(guard,portMAX_DELAY);s->initialized=synced;s->connected=connection!=BLE_HS_CONN_HANDLE_NONE;s->authenticated=authenticated;
    s->pairing_window=before(window_until);s->pending=pair_pending;s->busy=processing||transmitting;s->passkey=passkey;s->error=last_error;xSemaphoreGive(guard);
}
bool ble_service_ready(void){dog_ble_snapshot s;ble_service_snapshot(&s);return s.initialized&&!s.error;}
cJSON *ble_service_state(void)
{
    dog_ble_snapshot s;ble_service_snapshot(&s);cJSON *j=cJSON_CreateObject();
    cJSON_AddBoolToObject(j,"supported",true);cJSON_AddStringToObject(j,"name","Pavlov-DOG001");cJSON_AddStringToObject(j,"service_uuid",BLE_SERVICE_UUID);
    cJSON_AddBoolToObject(j,"initialized",s.initialized);cJSON_AddBoolToObject(j,"connected",s.connected);cJSON_AddBoolToObject(j,"authenticated",s.authenticated);
    cJSON_AddBoolToObject(j,"pairing_window",s.pairing_window);cJSON_AddBoolToObject(j,"confirmation_pending",s.pending);if(s.pending)cJSON_AddNumberToObject(j,"passkey",s.passkey);
    cJSON_AddNumberToObject(j,"last_error",s.error);cJSON_AddNumberToObject(j,"internal_heap_free",heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(j,"internal_heap_min",heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(j,"internal_largest_block",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if(guard){xSemaphoreTake(guard,portMAX_DELAY);cJSON_AddNumberToObject(j,"commands",commands);cJSON_AddNumberToObject(j,"duplicates",duplicates);cJSON_AddNumberToObject(j,"connections",connections);cJSON_AddNumberToObject(j,"notify_errors",notify_errors);cJSON_AddBoolToObject(j,"busy",processing||transmitting);xSemaphoreGive(guard);}
    return j;
}
bool ble_service_pair_window(void)
{
    if(!guard||ota_service_busy())return false;
    xSemaphoreTake(guard,portMAX_DELAY);bool allowed=synced&&connection==BLE_HS_CONN_HANDLE_NONE;
    if(allowed)window_until=now_ms()+60000;
    xSemaphoreGive(guard);return allowed;
}
bool ble_service_confirm(bool accept)
{
    if(!guard)return false;
    xSemaphoreTake(guard,portMAX_DELAY);bool pending=pair_pending;uint16_t conn=connection;
    if(pending){pair_pending=false;pair_until=0;if(accept)window_until=0;}
    xSemaphoreGive(guard);if(!pending||conn==BLE_HS_CONN_HANDLE_NONE)return false;
    struct ble_sm_io io={.action=BLE_SM_IOACT_NUMCMP,.numcmp_accept=accept};return ble_sm_inject_io(conn,&io)==0;
}
static bool bonded(const ble_addr_t *address)
{
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];int count=0;
    if(ble_store_util_bonded_peers(peers,&count,CONFIG_BT_NIMBLE_MAX_BONDS))return false;
    for(int i=0;i<count;i++)if(!ble_addr_cmp(address,&peers[i]))return true;
    return false;
}
static int gap_event(struct ble_gap_event *event,void *arg);
static void advertise(void)
{
    struct ble_hs_adv_fields fields={0};fields.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128=(ble_uuid128_t *)&service_uuid;fields.num_uuids128=1;fields.uuids128_is_complete=1;
    int rc=ble_gap_adv_set_fields(&fields);
    if(!rc){memset(&fields,0,sizeof(fields));fields.name=(uint8_t *)"Pavlov-DOG001";fields.name_len=strlen((char *)fields.name);fields.name_is_complete=1;rc=ble_gap_adv_rsp_set_fields(&fields);}
    struct ble_gap_adv_params params={.conn_mode=BLE_GAP_CONN_MODE_UND,.disc_mode=BLE_GAP_DISC_MODE_GEN,.itvl_min=320,.itvl_max=480};
    if(!rc)rc=ble_gap_adv_start(address_type,NULL,BLE_HS_FOREVER,&params,gap_event,NULL);
    if(rc){xSemaphoreTake(guard,portMAX_DELAY);last_error=rc;xSemaphoreGive(guard);ESP_LOGE("DOG_BLE","Advertising failed: %d",rc);}
}
static int gap_event(struct ble_gap_event *e,void *arg)
{
    (void)arg;struct ble_gap_conn_desc desc;
    switch(e->type){
    case BLE_GAP_EVENT_CONNECT:
        if(e->connect.status){advertise();return 0;}
        if(ble_gap_conn_find(e->connect.conn_handle,&desc))return 0;
        bool known=bonded(&desc.peer_id_addr);xSemaphoreTake(guard,portMAX_DELAY);
        if(!before(window_until)&&!known){xSemaphoreGive(guard);ble_gap_terminate(e->connect.conn_handle,BLE_ERR_REM_USER_CONN_TERM);return 0;}
        connection=e->connect.conn_handle;authenticated=false;subscribed=false;pair_pending=false;processing=false;transmitting=false;last_sequence=0;++epoch;++connections;
        snprintf(session,sizeof(session),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
        memset(&receiver,0,sizeof(receiver));memset(receipts,0,4*sizeof(*receipts));receipt_cursor=0;xQueueReset(jobs);
        xSemaphoreGive(guard);if(known)ble_gap_security_initiate(e->connect.conn_handle);return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        xSemaphoreTake(guard,portMAX_DELAY);connection=BLE_HS_CONN_HANDLE_NONE;authenticated=false;pair_pending=false;processing=false;transmitting=false;subscribed=false;++epoch;session[0]=0;xQueueReset(jobs);memset(&receiver,0,sizeof(receiver));motion_bridge_ble_session(0);wifi_provision_session_closed();xSemaphoreGive(guard);advertise();return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if(ble_gap_conn_find(e->enc_change.conn_handle,&desc))return 0;
        xSemaphoreTake(guard,portMAX_DELAY);authenticated=!e->enc_change.status&&desc.sec_state.encrypted&&desc.sec_state.authenticated;
        motion_bridge_ble_session(authenticated?epoch:0);xSemaphoreGive(guard);return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if(e->subscribe.attr_handle==tx_handle){xSemaphoreTake(guard,portMAX_DELAY);subscribed=e->subscribe.cur_notify;xSemaphoreGive(guard);}return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if(e->passkey.params.action!=BLE_SM_IOACT_NUMCMP){ble_gap_terminate(e->passkey.conn_handle,BLE_ERR_AUTH_FAIL);return 0;}
        xSemaphoreTake(guard,portMAX_DELAY);
        if(!before(window_until)){xSemaphoreGive(guard);struct ble_sm_io io={.action=BLE_SM_IOACT_NUMCMP,.numcmp_accept=0};ble_sm_inject_io(e->passkey.conn_handle,&io);return 0;}
        passkey=e->passkey.params.numcmp;pair_pending=true;pair_until=now_ms()+30000;xSemaphoreGive(guard);return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        xSemaphoreTake(guard,portMAX_DELAY);bool permit=before(window_until);xSemaphoreGive(guard);
        if(!permit)return BLE_GAP_REPEAT_PAIRING_IGNORE;
        if(!ble_gap_conn_find(e->repeat_pairing.conn_handle,&desc))ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    default:return 0;
    }
}
static int access_value(uint16_t conn,uint16_t attr,struct ble_gatt_access_ctxt *ctxt,void *arg)
{
    (void)conn;(void)attr;int kind=(int)(intptr_t)arg;
    if(ctxt->op==BLE_GATT_ACCESS_OP_READ_CHR){
        char text[340];xSemaphoreTake(guard,portMAX_DELAY);
        if(kind==0)snprintf(text,sizeof(text),"{\"protocol\":1,\"device_id\":\"DOG-001\",\"version\":\"%s\",\"boot_id\":\"%s\",\"session\":\"%s\",\"max_request\":%d}",esp_app_get_description()->version,device_api_boot_id(),session,DOG_BLE_REQUEST_MAX);
        else snprintf(text,sizeof(text),"{\"busy\":%s,\"last_transfer\":%u,\"response_bytes\":%u}",processing?"true":"false",last_transfer,last_response_bytes);
        xSemaphoreGive(guard);return os_mbuf_append(ctxt->om,text,strlen(text))?BLE_ATT_ERR_INSUFFICIENT_RES:0;
    }
    if(kind!=1||ctxt->op!=BLE_GATT_ACCESS_OP_WRITE_CHR)return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    uint8_t packet[DOG_BLE_REQUEST_MAX+6];uint16_t length=0;
    if(OS_MBUF_PKTLEN(ctxt->om)>sizeof(packet)||ble_hs_mbuf_to_flat(ctxt->om,packet,sizeof(packet),&length))return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    xSemaphoreTake(guard,portMAX_DELAY);
    if(!authenticated||!subscribed||processing){xSemaphoreGive(guard);return BLE_ATT_ERR_INSUFFICIENT_RES;}
    int result=dog_ble_receive(&receiver,packet,length,now_ms());
    if(result==1){ble_job job={.epoch=epoch,.transfer=receiver.id};memcpy(job.data,receiver.data,receiver.total+1);
        if(xQueueSend(jobs,&job,0)!=pdTRUE)result=-1;else processing=true;}
    xSemaphoreGive(guard);return result<0?BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN:0;
}
static const struct ble_gatt_svc_def services[]={
    {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&service_uuid.u,.characteristics=(struct ble_gatt_chr_def[]){
        {.uuid=&info_uuid.u,.access_cb=access_value,.arg=(void *)0,.flags=BLE_GATT_CHR_F_READ},
        {.uuid=&rx_uuid.u,.access_cb=access_value,.arg=(void *)1,.flags=BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_ENC|BLE_GATT_CHR_F_WRITE_AUTHEN},
        {.uuid=&tx_uuid.u,.access_cb=access_value,.arg=(void *)2,.val_handle=&tx_handle,.flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_READ_ENC|BLE_GATT_CHR_F_READ_AUTHEN|BLE_GATT_CHR_F_NOTIFY},
        {0}}},{0}};
static const char *string(cJSON *j,const char *key){cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);return cJSON_IsString(v)?v->valuestring:NULL;}
static bool only(cJSON *j,const char *const *names,unsigned count)
{
    if(!cJSON_IsObject(j))return false;
    for(cJSON *v=j->child;v;v=v->next){bool found=false;for(unsigned n=0;n<count;n++)if(v->string&&!strcmp(v->string,names[n]))found=true;
        if(!found)return false;
        for(cJSON *other=v->next;other;other=other->next)if(!strcmp(v->string,other->string))return false;}
    return true;
}
static bool identifier(const char *id)
{if(!id||!id[0]||strlen(id)>32)return false;for(;*id;id++)if(!isalnum((unsigned char)*id)&&*id!='-'&&*id!='_'&&*id!='.')return false;return true;}
static cJSON *error(int *status,int code,const char *name)
{*status=code;cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"error",name);return j;}
static cJSON *compact_state(void)
{
    cJSON *j=cJSON_CreateObject();cloud_display_snapshot d;cloud_dialog_display_snapshot(&d);user_preferences p=user_settings_get();
    cJSON_AddStringToObject(j,"version",esp_app_get_description()->version);cJSON_AddStringToObject(j,"mode",interaction_mode()==PAVLOV_REST?"rest":interaction_mode()==PAVLOV_ACTION?"action":"companion");
    cJSON_AddBoolToObject(j,"wifi",d.wifi);cJSON_AddBoolToObject(j,"listening",d.listening);cJSON_AddNumberToObject(j,"volume",p.volume);cJSON_AddNumberToObject(j,"strength_cap",DOG_ACTION_MAX_STRENGTH);
    cJSON_AddBoolToObject(j,"wifi_configuration_supported",true);
    cJSON_AddBoolToObject(j,"sensor_calibration_supported",true);
    cJSON_AddBoolToObject(j,"power_configuration_supported",true);
    cJSON_AddBoolToObject(j,"power_history_supported",true);
    cJSON_AddBoolToObject(j,"pose_protection_supported",true);
    cJSON_AddBoolToObject(j,"power_limit_supported",true);cJSON_AddBoolToObject(j,"rtc_supported",true);cJSON_AddBoolToObject(j,"eeprom_readonly_supported",true);
    cJSON *motion=motion_bridge_state();cJSON_AddItemToObject(j,"motion",dog_ble_motion_view(motion));cJSON_Delete(motion);
    cJSON *s=display_service_state();cJSON_AddBoolToObject(j,"screen",cJSON_IsTrue(cJSON_GetObjectItem(s,"initialized")));cJSON_AddBoolToObject(j,"touch",cJSON_IsTrue(cJSON_GetObjectItem(s,"touch_initialized")));cJSON_Delete(s);
    s=camera_service_state();cJSON_AddBoolToObject(j,"camera_connected",cJSON_IsTrue(cJSON_GetObjectItem(s,"connected")));cJSON_AddBoolToObject(j,"camera_busy",cJSON_IsTrue(cJSON_GetObjectItem(s,"busy")));cJSON_Delete(s);return j;
}
static cJSON *dispatch(cJSON *request,uint32_t current,int *status)
{
    const char *op=string(request,"op"),*boot=string(request,"boot_id");cJSON *value=cJSON_GetObjectItemCaseSensitive(request,"value");*status=200;
    if(!op)return error(status,400,"op_required");
    if(!strcmp(op,"state"))return value?error(status,422,"value_not_allowed"):compact_state();
    if(!strcmp(op,"wifi_state"))return value?error(status,422,"value_not_allowed"):wifi_provision_state();
    if(!strcmp(op,"pose_state"))return value?error(status,422,"value_not_allowed"):motion_bridge_pose_calibration(NULL,status);
    if(!strcmp(op,"compass_state"))return value?error(status,422,"value_not_allowed"):motion_bridge_compass(NULL,status);
    if(!strcmp(op,"power_state"))return value?error(status,422,"value_not_allowed"):motion_bridge_power(NULL,status);
    if(!strcmp(op,"power_history"))return value?error(status,422,"value_not_allowed"):motion_bridge_power_history();
    if(!strcmp(op,"pose_protection_state"))return value?error(status,422,"value_not_allowed"):motion_bridge_pose_protection(NULL,status);
    if(!strcmp(op,"power_limit_state"))return value?error(status,422,"value_not_allowed"):motion_bridge_power_limit(NULL,status);
    if(!strcmp(op,"rtc_state"))return value?error(status,422,"value_not_allowed"):motion_bridge_rtc_state();
    if(!strcmp(op,"timer_state"))return value?error(status,422,"value_not_allowed"):countdown_service_json();
    if(!strcmp(op,"eeprom_state"))return value?error(status,422,"value_not_allowed"):motion_bridge_eeprom_state();
    if(!strcmp(op,"stop")){
        if(value)return error(status,422,"value_not_allowed");
        motion_bridge_stop();if(!ota_service_busy())cloud_dialog_command("stop",NULL,false);return compact_state();
    }
    if(!boot||strcmp(boot,device_api_boot_id()))return error(status,409,"device_boot_changed");
    if(ota_service_busy())return error(status,409,"ota_in_progress");
    if(!strcmp(op,"timer_cancel")){if(value)return error(status,422,"value_not_allowed");countdown_service_cancel();return countdown_service_json();}
    if(!strcmp(op,"wifi_cancel")){if(value)return error(status,422,"value_not_allowed");wifi_provision_cancel();return wifi_provision_state();}
    if(!strcmp(op,"wifi_config")||!strcmp(op,"wifi_reset")){
        if(!cloud_dialog_can_update()||audio_service_busy()||motion_bridge_calibration_active()||wifi_control_transition_active())return error(status,409,"wifi_configuration_requires_idle");
        cJSON *motion=motion_bridge_state();bool moving=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(motion,"controller_moving"))||cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(motion,"stop_pending"));cJSON_Delete(motion);
        if(moving)return error(status,409,"wifi_configuration_requires_idle");
        bool reset=!strcmp(op,"wifi_reset");const char *ssid=NULL,*password=NULL;
        if(reset){if(value)return error(status,422,"value_not_allowed");}
        else{const char *const fields[]={"ssid","password"};ssid=string(value,"ssid");password=string(value,"password");if(!only(value,fields,2)||!ssid||!password)return error(status,422,"invalid_wifi_credentials");}
        esp_err_t err=wifi_provision_apply(ssid,password,reset);
        return err?error(status,err==ESP_ERR_INVALID_ARG?422:409,err==ESP_ERR_INVALID_ARG?"invalid_wifi_credentials":"wifi_configuration_busy"):wifi_provision_state();
    }
    if(wifi_provision_busy())return error(status,409,"wifi_configuration_busy");
    if(!strcmp(op,"eeprom_inspect")){
        if(value)return error(status,422,"value_not_allowed");
        if(interaction_mode()!=PAVLOV_REST||!cloud_dialog_can_update()||audio_service_busy())return error(status,409,"eeprom_requires_rest");
        return motion_bridge_eeprom_inspect(status);
    }
    if(!strcmp(op,"rtc_sync")||!strcmp(op,"rtc_reminder")){
        if(interaction_mode()!=PAVLOV_REST||!cloud_dialog_can_update()||audio_service_busy())return error(status,409,"rtc_requires_rest");
        const char *const fields[]={"utc"};const char *utc=string(value,"utc");
        if(!only(value,fields,1)||!utc)return error(status,422,"invalid_rtc_utc");
        return !strcmp(op,"rtc_sync")?motion_bridge_rtc_sync(utc,status):rtc_service_reminder(utc,status);
    }
    if(!strcmp(op,"power_limit_config")){
        if(!cJSON_IsObject(value))return error(status,422,"invalid_power_limit_settings");
        if(interaction_mode()!=PAVLOV_REST||!cloud_dialog_can_update()||audio_service_busy())return error(status,409,"power_limit_requires_rest");
        return motion_bridge_power_limit(value,status);
    }
    if(!strcmp(op,"pose_protection_config")){
        if(!cJSON_IsObject(value))return error(status,422,"invalid_pose_protection_settings");
        if(interaction_mode()!=PAVLOV_REST||!cloud_dialog_can_update()||audio_service_busy())return error(status,409,"pose_protection_requires_rest");
        return motion_bridge_pose_protection(value,status);
    }
    if(!strcmp(op,"power_config")){
        if(!cJSON_IsObject(value))return error(status,422,"invalid_power_settings");
        if(interaction_mode()!=PAVLOV_REST||!cloud_dialog_can_update()||audio_service_busy())return error(status,409,"power_settings_require_rest");
        return motion_bridge_power(value,status);
    }
    if(!strcmp(op,"pose_calibration")||!strcmp(op,"compass_calibration")){
        if(!cJSON_IsString(value))return error(status,422,"invalid_calibration_command");
        const char *cmd=value->valuestring;bool pose=!strcmp(op,"pose_calibration");
        bool valid=!strcmp(cmd,"cancel")||(pose?(!strcmp(cmd,"level")||!strcmp(cmd,"forward")||!strcmp(cmd,"left")):(!strcmp(cmd,"start")||!strcmp(cmd,"finish")||!strcmp(cmd,"clear")));
        if(!valid)return error(status,422,"invalid_calibration_command");
        if(strcmp(cmd,"cancel")&&(interaction_mode()!=PAVLOV_REST||!cloud_dialog_can_update()||audio_service_busy()))return error(status,409,"calibration_requires_rest");
        return pose?motion_bridge_pose_calibration_ble(current,cmd,status):motion_bridge_compass_ble(current,cmd,status);
    }
    if(!strcmp(op,"volume")){
        if(!cJSON_IsNumber(value)||value->valuedouble<0||value->valuedouble>100||value->valuedouble!=value->valueint)return error(status,422,"invalid_volume");
        cJSON *patch=cJSON_CreateObject();cJSON_AddNumberToObject(patch,"volume",value->valueint);int err=user_settings_update(patch);cJSON_Delete(patch);
        return err?error(status,503,"settings_not_saved"):compact_state();
    }
    if(!strcmp(op,"mode"))return !cJSON_IsString(value)||cloud_dialog_change_mode(value->valuestring)?error(status,422,"invalid_mode"):compact_state();
    if(!strcmp(op,"listen")){
        if(!cJSON_IsBool(value))return error(status,422,"invalid_listening_state");
        cloud_display_snapshot d;cloud_dialog_display_snapshot(&d);if(cJSON_IsTrue(value)&&!d.wifi)return error(status,409,"cloud_requires_wifi");
        return cloud_dialog_command(cJSON_IsTrue(value)?"start":"stop",NULL,false)?compact_state():error(status,409,"dialog_command_rejected");
    }
    if(!strcmp(op,"receipt")){
        if(!cJSON_IsString(value)||strlen(value->valuestring)>48)return error(status,422,"invalid_request_id");
        cJSON *j=motion_bridge_request(value->valuestring);return j?j:error(status,404,"request_not_found");
    }
    if(!strcmp(op,"action")){
        if(interaction_mode()==PAVLOV_REST)return error(status,409,"rest_mode_motion_blocked");
        if(!cloud_dialog_can_update())return error(status,409,"pause_listening_before_motion");
        const char *const allowed[]={"name","repeats","speed","strength"};const char *name=string(value,"name"),*speed=string(value,"speed");
        cJSON *repeats=cJSON_GetObjectItemCaseSensitive(value,"repeats"),*strength=cJSON_GetObjectItemCaseSensitive(value,"strength");
        if(!only(value,allowed,4)||!name||!dog_action_by_name(name)||
            (repeats&&(!cJSON_IsNumber(repeats)||repeats->valueint<1||repeats->valueint>3||repeats->valuedouble!=repeats->valueint))||
            (strength&&(!cJSON_IsNumber(strength)||strength->valuedouble<25||strength->valuedouble>100||strength->valuedouble!=strength->valueint))||
            (cJSON_HasObjectItem(value,"speed")&&(!speed||(strcmp(speed,"slow")&&strcmp(speed,"normal")&&strcmp(speed,"fast")))))return error(status,422,"invalid_action_arguments");
        char id[49],action[24];snprintf(id,sizeof(id),"bl.%08lx.%s",(unsigned long)current,string(request,"id"));snprintf(action,sizeof(action),"motion.%s",name);
        return motion_bridge_submit_ble(current,id,action,repeats?repeats->valueint:1,speed&&!strcmp(speed,"slow")?1:speed&&!strcmp(speed,"fast")?3:2,strength?strength->valueint:DOG_ACTION_DEFAULT_STRENGTH,status);
    }
    return error(status,422,"unsupported_operation");
}
static void send_response(uint32_t expected,uint16_t transfer,const char *text)
{
    size_t length=strlen(text),offset=0;
    while(offset<length&&alive(expected)){
        xSemaphoreTake(guard,portMAX_DELAY);uint16_t conn=connection;xSemaphoreGive(guard);
        int capacity=ble_att_mtu(conn)-3-6;if(capacity<1)break;if(capacity>238)capacity=238;
        size_t count=length-offset;if(count>(size_t)capacity)count=capacity;
        uint8_t packet[244]={transfer&255,transfer>>8,length&255,length>>8,offset&255,offset>>8};memcpy(packet+6,text+offset,count);
        int rc=BLE_HS_ENOMEM;
        for(unsigned retry=0;retry<25&&alive(expected);retry++){
            struct os_mbuf *om=ble_hs_mbuf_from_flat(packet,count+6);if(om)rc=ble_gatts_notify_custom(conn,tx_handle,om);
            if(!rc)break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if(rc){xSemaphoreTake(guard,portMAX_DELAY);++notify_errors;xSemaphoreGive(guard);break;}
        offset+=count;vTaskDelay(pdMS_TO_TICKS(10));
    }
}
static void command_task(void *arg)
{
    (void)arg;ble_job job;
    for(;;){
        xSemaphoreTake(guard,portMAX_DELAY);bool expired=pair_pending&&!before(pair_until);xSemaphoreGive(guard);if(expired)ble_service_confirm(false);
        if(xQueueReceive(jobs,&job,pdMS_TO_TICKS(200))!=pdTRUE)continue;
        char output[DOG_BLE_RESPONSE_MAX];output[0]=0;uint8_t request_hash[32];
        if(mbedtls_sha256((const unsigned char*)job.data,strlen(job.data),request_hash,0)!=0){xSemaphoreTake(guard,portMAX_DELAY);if(job.epoch==epoch)processing=false;xSemaphoreGive(guard);memset(&job,0,sizeof(job));continue;}
        xSemaphoreTake(guard,portMAX_DELAY);
        if(connection==BLE_HS_CONN_HANDLE_NONE||!authenticated||job.epoch!=epoch){xSemaphoreGive(guard);memset(&job,0,sizeof(job));continue;}
        const char *end=NULL;cJSON *request=strstr(job.data,"\\u0000")?NULL:cJSON_ParseWithLengthOpts(job.data,strlen(job.data)+1,&end,true);
        const char *id=string(request,"id"),*nonce=string(request,"session");const char *const allowed[]={"id","session","boot_id","op","value","seq"};
        cJSON *sequence=cJSON_GetObjectItemCaseSensitive(request,"seq");
        cJSON *result=NULL;int status=200;receipt *cached=NULL;
        if(!only(request,allowed,6)||!identifier(id)||!cJSON_IsNumber(sequence)||sequence->valuedouble<1||sequence->valuedouble>UINT32_MAX||sequence->valuedouble!=(uint32_t)sequence->valuedouble)result=error(&status,400,"invalid_request");
        else if(!nonce||strcmp(nonce,session))result=error(&status,409,"ble_session_changed");
        else{
            for(unsigned i=0;i<4;i++)if(!strcmp(receipts[i].id,id)){cached=&receipts[i];break;}
            if(cached){if(memcmp(cached->request_hash,request_hash,sizeof(request_hash)))result=error(&status,409,"request_id_conflict");else{snprintf(output,sizeof(output),"%s",cached->response);++duplicates;}}
            else if((uint32_t)sequence->valuedouble<=last_sequence)result=error(&status,409,"stale_sequence");
            else{last_sequence=(uint32_t)sequence->valuedouble;result=dispatch(request,job.epoch,&status);++commands;}
        }
        if(result){
            cJSON *envelope=cJSON_CreateObject();cJSON_AddStringToObject(envelope,"id",identifier(id)?id:"");cJSON_AddStringToObject(envelope,"boot_id",device_api_boot_id());cJSON_AddNumberToObject(envelope,"status",status);cJSON_AddItemToObject(envelope,"result",result);
            char *serialized=cJSON_PrintUnformatted(envelope);cJSON_Delete(envelope);
            if(serialized&&strlen(serialized)<sizeof(output))snprintf(output,sizeof(output),"%s",serialized);
            else snprintf(output,sizeof(output),"{\"id\":\"%s\",\"status\":503,\"result\":{\"error\":\"response_unavailable\"}}",identifier(id)?id:"");
            cJSON_free(serialized);
            if(identifier(id)&&nonce&&!strcmp(nonce,session)&&!cached){receipt *slot=&receipts[receipt_cursor++%4];snprintf(slot->id,sizeof(slot->id),"%s",id);memcpy(slot->request_hash,request_hash,sizeof(request_hash));snprintf(slot->response,sizeof(slot->response),"%s",output);}
        }
        cJSON *credentials=cJSON_GetObjectItemCaseSensitive(request,"value"),*password=cJSON_GetObjectItemCaseSensitive(credentials,"password");if(cJSON_IsString(password))memset(password->valuestring,0,strlen(password->valuestring));
        cJSON_Delete(request);memset(job.data,0,sizeof(job.data));last_transfer=job.transfer;last_response_bytes=strlen(output);processing=false;transmitting=true;xSemaphoreGive(guard);
        send_response(job.epoch,job.transfer,output);
        xSemaphoreTake(guard,portMAX_DELAY);if(job.epoch==epoch)transmitting=false;xSemaphoreGive(guard);
    }
}
static void on_reset(int reason)
{xSemaphoreTake(guard,portMAX_DELAY);last_error=reason;synced=false;connection=BLE_HS_CONN_HANDLE_NONE;authenticated=false;pair_pending=false;processing=false;transmitting=false;++epoch;xQueueReset(jobs);motion_bridge_ble_session(0);xSemaphoreGive(guard);}
static void on_sync(void)
{
    int rc=ble_hs_util_ensure_addr(0);if(!rc)rc=ble_hs_id_infer_auto(0,&address_type);
    xSemaphoreTake(guard,portMAX_DELAY);last_error=rc;synced=!rc;xSemaphoreGive(guard);if(!rc)advertise();
}
static void host_task(void *arg){(void)arg;nimble_port_run();nimble_port_freertos_deinit();}
void ble_store_config_init(void);
void ble_service_init(void)
{
    guard=xSemaphoreCreateMutex();jobs=xQueueCreate(1,sizeof(ble_job));receipts=heap_caps_calloc(4,sizeof(*receipts),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);assert(guard&&jobs&&receipts);
    ESP_ERROR_CHECK(nimble_port_init());ble_hs_cfg.reset_cb=on_reset;ble_hs_cfg.sync_cb=on_sync;ble_hs_cfg.store_status_cb=ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap=BLE_HS_IO_DISPLAY_YESNO;ble_hs_cfg.sm_bonding=1;ble_hs_cfg.sm_mitm=1;ble_hs_cfg.sm_sc=1;
    ble_hs_cfg.sm_our_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;ble_hs_cfg.sm_their_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();ble_svc_gatt_init();assert(!ble_gatts_count_cfg(services));assert(!ble_gatts_add_svcs(services));assert(!ble_svc_gap_device_name_set("Pavlov-DOG001"));
    ble_att_set_preferred_mtu(247);ble_store_config_init();assert(xTaskCreate(command_task,"dog_ble_control",8192,NULL,4,NULL)==pdPASS);nimble_port_freertos_init(host_task);
}
