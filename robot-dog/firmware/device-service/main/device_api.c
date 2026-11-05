#include "device_api.h"
#include "motion_bridge.h"
#include "board_controls.h"
#include "rtc_service.h"
#include "board_profile.h"
#include "api_token.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "../../shared/action_catalog.h"
#include "audio_service.h"
#include "countdown_service.h"
#include "riddle_service.h"
#include "cloud_dialog.h"
#include "voice_intent.h"
#include "user_settings.h"
#include "interaction.h"
#include "user_profile.h"
#include "companion_memory.h"
#include "display_service.h"
#include "camera_service.h"
#include "ota_service.h"
#include "controller_ota.h"
#include "ble_service.h"
#include "wifi_control.h"

static char boot_id[17];
const char *device_api_boot_id(void){return boot_id;}
extern const unsigned char control_html_start[] asm("_binary_control_html_start");
extern const unsigned char control_html_end[] asm("_binary_control_html_end");
static httpd_handle_t server;
static const struct { const char *name; bool supported; } modules[] = {
    {"motion",true},{"telemetry",true},{"audio",true},{"dialog",true},
    {"config",true},{"diagnostics_export",false},{"ota",true},{"ble",true},{"discovery",false}
};
static const char *status_name(int status)
{
    switch(status){case 200:return "200 OK";case 202:return "202 Accepted";
    case 400:return "400 Bad Request";case 401:return "401 Unauthorized";
    case 404:return "404 Not Found";case 409:return "409 Conflict";
    case 413:return "413 Payload Too Large";case 422:return "422 Unprocessable Entity";
    case 501:return "501 Not Implemented";default:return "503 Service Unavailable";}
}
static esp_err_t json_reply(httpd_req_t *req,cJSON *j,int status)
{
    if(!j)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"allocation_failed");
    cJSON_AddStringToObject(j,"boot_id",boot_id);
    char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);
    if(!text)return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"allocation_failed");
    httpd_resp_set_type(req,"application/json");httpd_resp_set_status(req,status_name(status));
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    esp_err_t rc=httpd_resp_send(req,text,HTTPD_RESP_USE_STRLEN);cJSON_free(text);return rc;
}
static esp_err_t error_reply(httpd_req_t *req,int code,const char *message)
{
    cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"error",message);return json_reply(req,j,code);
}
static bool authenticated(httpd_req_t *req)
{
    char actual[128];const char *expected="Bearer " DOG_API_TOKEN;
    size_t len=httpd_req_get_hdr_value_len(req,"Authorization");
    if(len!=strlen(expected)||len>=sizeof(actual))return false;
    if(httpd_req_get_hdr_value_str(req,"Authorization",actual,sizeof(actual))!=ESP_OK)return false;
    unsigned difference=0;for(size_t i=0;i<len;++i)difference|=(unsigned char)actual[i]^(unsigned char)expected[i];
    return difference==0;
}
static bool valid_id(const char *id)
{
    if(!id||!id[0]||strlen(id)>48)return false;
    for(const unsigned char *p=(const unsigned char*)id;*p;++p)
        if(!isalnum(*p)&&*p!='-'&&*p!='_'&&*p!='.')return false;
    return true;
}
static bool fields(cJSON *object,const char *const *allowed,unsigned count)
{
    if(!cJSON_IsObject(object))return false;
    for(cJSON *item=object->child;item;item=item->next){
        bool found=false;
        for(unsigned i=0;i<count;++i)if(item->string&&!strcmp(item->string,allowed[i]))found=true;
        if(!found)return false;
        /* Reject duplicate JSON properties rather than silently selecting one. */
        for(cJSON *next=item->next;next;next=next->next)
            if(next->string&&!strcmp(item->string,next->string))return false;
    }
    return true;
}
static bool integer(cJSON *item,int low,int high,int fallback,int *result)
{
    if(!item){*result=fallback;return true;}
    if(!cJSON_IsNumber(item)||item->valuedouble<low||item->valuedouble>high)return false;
    int n=(int)item->valuedouble;if(item->valuedouble!=n)return false;*result=n;return true;
}
static cJSON *read_body(httpd_req_t *req)
{
    char body[513];int received=0;int64_t deadline=esp_timer_get_time()+2000000;
    while(received<req->content_len){
        if(esp_timer_get_time()>deadline)return NULL;
        int n=httpd_req_recv(req,body+received,req->content_len-received);
        if(n<=0)return NULL;
        received+=n;
    }
    body[received]=0;
    if(strstr(body,"\\u0000"))return NULL;
    const char *end=NULL;
    return cJSON_ParseWithLengthOpts(body,received+1,&end,true);
}
static esp_err_t actions(httpd_req_t *req)
{
    if(interaction_mode()==PAVLOV_REST)return error_reply(req,409,"rest_mode_motion_blocked");
    if(req->content_len<=0||req->content_len>512)return error_reply(req,413,"body_size_limit_512");
    cJSON *body=read_body(req);
    static const char *const top[]={"request_id","action","args","ttl_ms","boot_id"};
    if(!body||!fields(body,top,5)){cJSON_Delete(body);return error_reply(req,400,"invalid_json_fields");}
    cJSON *id=cJSON_GetObjectItemCaseSensitive(body,"request_id");
    cJSON *action=cJSON_GetObjectItemCaseSensitive(body,"action");
    cJSON *args=cJSON_GetObjectItemCaseSensitive(body,"args");
    cJSON *boot=cJSON_GetObjectItemCaseSensitive(body,"boot_id");
    if(!cJSON_IsString(id)||!valid_id(id->valuestring)||!cJSON_IsString(action)){
        cJSON_Delete(body);return error_reply(req,400,"request_id_and_action_required");
    }
    if(boot&&(!cJSON_IsString(boot)||strcmp(boot->valuestring,boot_id))){
        cJSON_Delete(body);return error_reply(req,409,"device_boot_changed");
    }
    int ttl,argument,channel=0,speed=2,strength=100;const char *profile="legacy";
    if(!integer(cJSON_GetObjectItemCaseSensitive(body,"ttl_ms"),1,5000,2000,&ttl)){
        cJSON_Delete(body);return error_reply(req,422,"invalid_ttl_ms");
    }
    bool valid=false;
    if(!strcmp(action->valuestring,"motion.walk")){
        static const char *const allowed[]={"cycles","profile"};
        cJSON *selected=args?cJSON_GetObjectItemCaseSensitive(args,"profile"):NULL;
        if(selected&&cJSON_IsString(selected))profile=selected->valuestring;
        valid=(!args||fields(args,allowed,2))&&integer(args?cJSON_GetObjectItemCaseSensitive(args,"cycles"):NULL,1,4,1,&argument)&&
            (!selected||cJSON_IsString(selected))&&(!strcmp(profile,"legacy")||!strcmp(profile,"vendor_raw")||!strcmp(profile,"vendor_mapped"));
    }else if(!strcmp(action->valuestring,"motion.tail")){
        static const char *const allowed[]={"delta_us"};
        valid=(!args||fields(args,allowed,1))&&integer(args?cJSON_GetObjectItemCaseSensitive(args,"delta_us"):NULL,-50,50,30,&argument)&&argument!=0;
    }else if(!strcmp(action->valuestring,"diagnostics.leg")){
        static const char *const allowed[]={"channel","delta_us"};
        valid=args&&fields(args,allowed,2)&&
            integer(cJSON_GetObjectItemCaseSensitive(args,"channel"),1,4,0,&channel)&&channel!=0&&
            integer(cJSON_GetObjectItemCaseSensitive(args,"delta_us"),-250,250,0,&argument)&&argument!=0;
    }else if(!strcmp(action->valuestring,"motion.home")){
        valid=!args||(cJSON_IsObject(args)&&args->child==NULL);argument=0;
    }else if(!strncmp(action->valuestring,"motion.",7)&&dog_action_by_name(action->valuestring+7)){
        static const char *const allowed[]={"repeats","speed","strength"};
        cJSON *selected=args?cJSON_GetObjectItemCaseSensitive(args,"speed"):NULL;
        bool speed_valid=!selected;
        if(cJSON_IsString(selected)){
            if(!strcmp(selected->valuestring,"slow")){speed=1;speed_valid=true;}
            else if(!strcmp(selected->valuestring,"normal")){speed=2;speed_valid=true;}
            else if(!strcmp(selected->valuestring,"fast")){speed=3;speed_valid=true;}
        }
        valid=(!args||fields(args,allowed,3))&&speed_valid&&
            integer(args?cJSON_GetObjectItemCaseSensitive(args,"repeats"):NULL,1,3,1,&argument)&&
            integer(args?cJSON_GetObjectItemCaseSensitive(args,"strength"):NULL,25,DOG_ACTION_MAX_STRENGTH,DOG_ACTION_DEFAULT_STRENGTH,&strength);
    }else{
        cJSON_Delete(body);return error_reply(req,501,"action_not_supported");
    }
    if(!valid){cJSON_Delete(body);return error_reply(req,422,"invalid_action_arguments");}
    int status;
    cJSON *reply=motion_bridge_submit(id->valuestring,action->valuestring,argument,channel,profile,speed,strength,ttl,&status);
    cJSON_Delete(body);return json_reply(req,reply,status);
}
static cJSON *capabilities(void)
{
    cJSON *j=cJSON_CreateObject(),*m=cJSON_AddObjectToObject(j,"modules");
    for(unsigned i=0;i<sizeof(modules)/sizeof(modules[0]);++i)cJSON_AddBoolToObject(m,modules[i].name,modules[i].supported);
    cJSON *motion=cJSON_AddObjectToObject(j,"motion");
    cJSON *walk=cJSON_AddObjectToObject(motion,"motion.walk");
    cJSON_AddNumberToObject(walk,"min_cycles",1);cJSON_AddNumberToObject(walk,"max_cycles",4);
    cJSON_AddNumberToObject(walk,"cycle_ms",2000);cJSON_AddNumberToObject(walk,"max_duration_ms",8000);
    cJSON_AddStringToObject(walk,"default_profile","legacy");
    cJSON *profiles=cJSON_AddObjectToObject(walk,"profiles");
    const char *profile_names[]={"legacy","vendor_raw","vendor_mapped"};
    for(unsigned i=0;i<3;++i){
        cJSON *p=cJSON_AddObjectToObject(profiles,profile_names[i]);
        cJSON_AddNumberToObject(p,"cycle_ms",i?750:2000);
        cJSON_AddNumberToObject(p,"return_ms",i?150:0);
        cJSON_AddNumberToObject(p,"max_duration_ms",i?3150:8000);
        cJSON_AddNumberToObject(p,"leg_amplitude_us",i?300:DOG_WALK_AMPLITUDE_US);
        cJSON_AddNumberToObject(p,"tail_amplitude_us",i?300:0);
        cJSON_AddStringToObject(p,"channel_mapping",i==1?"factory_raw_channels":i==2?"observed_positions_raw_signs_hypothesis":"observed_foot_directions");
        cJSON_AddBoolToObject(p,"physical_walking_verified",false);
    }
    cJSON *tail=cJSON_AddObjectToObject(motion,"motion.tail");
    cJSON_AddNumberToObject(tail,"min_delta_us",-50);cJSON_AddNumberToObject(tail,"max_delta_us",50);
    cJSON_AddBoolToObject(motion,"motion.home",true);
    for(unsigned i=0;i<DOG_ACTION_COUNT;++i){
        const dog_action *a=&dog_actions[i];char name[32];snprintf(name,sizeof(name),"motion.%s",a->name);
        cJSON *entry=cJSON_AddObjectToObject(motion,name);
        cJSON_AddNumberToObject(entry,"min_repeats",1);cJSON_AddNumberToObject(entry,"max_repeats",3);
        cJSON_AddNumberToObject(entry,"min_strength",25);cJSON_AddNumberToObject(entry,"max_strength",DOG_ACTION_MAX_STRENGTH);
        cJSON_AddNumberToObject(entry,"default_strength",DOG_ACTION_DEFAULT_STRENGTH);
        cJSON_AddStringToObject(entry,"strength_basis","percent_of_action_frame_offsets_with_controller_pwm_range_check");
        cJSON_AddStringToObject(entry,"speeds","slow,normal,fast");
        cJSON_AddNumberToObject(entry,"default_duration_ms",dog_action_duration(a,1,2));
        cJSON_AddNumberToObject(entry,"max_duration_ms",dog_action_duration(a,3,1));
        cJSON_AddBoolToObject(entry,"auto_return_to_baseline",true);
        cJSON_AddBoolToObject(entry,"physical_motion_verified",false);
        cJSON_AddStringToObject(entry,"channel_mapping","factory_raw_channels");
    }
    cJSON *diagnostic=cJSON_AddObjectToObject(j,"diagnostics.leg");
    cJSON_AddNumberToObject(diagnostic,"min_channel",1);cJSON_AddNumberToObject(diagnostic,"max_channel",4);
    cJSON_AddNumberToObject(diagnostic,"min_delta_us",-250);cJSON_AddNumberToObject(diagnostic,"max_delta_us",250);
    cJSON_AddNumberToObject(diagnostic,"max_duration_ms",3500);
    cJSON_AddStringToObject(diagnostic,"channel_labels","1=front_left,2=front_right,3=rear_right,4=rear_left");
    cJSON_AddStringToObject(diagnostic,"mapping_evidence","user_observed_first_leg_and_clockwise_order_2026-10-27");
    cJSON_AddStringToObject(diagnostic,"positive_delta_foot_direction","toward_head_user_observed_all_four");
    cJSON_AddBoolToObject(diagnostic,"auto_return_to_baseline",true);
    cJSON_AddStringToObject(motion,"stop_behavior","hold_current_pwm");
    cJSON_AddStringToObject(motion,"completion_evidence","pwm_sequence");
    cJSON_AddBoolToObject(motion,"physical_position_verified",false);
    cJSON_AddNumberToObject(j,"dedup_history_records",16);
    cJSON *audio=cJSON_AddObjectToObject(j,"audio");
    cJSON_AddNumberToObject(audio,"sample_rate",16000);cJSON_AddNumberToObject(audio,"channels",1);
    cJSON_AddNumberToObject(audio,"bits_per_sample",16);cJSON_AddNumberToObject(audio,"max_pcm_bytes",96000);
    cJSON_AddNumberToObject(audio,"max_record_ms",3000);cJSON_AddBoolToObject(audio,"hardware_audio_verified",false);
    cJSON_AddBoolToObject(audio,"streaming",false);cJSON_AddBoolToObject(audio,"cloud_dialog",true);
    cJSON_AddItemToObject(j,"dialog",cloud_dialog_state());
    cJSON_AddItemToObject(j,"timer",countdown_service_json());
    cJSON_AddStringToObject(j,"dedup_scope","current_esp32_boot_last_16_requests");
    return j;
}
static esp_err_t api(httpd_req_t *req)
{
    if(!authenticated(req)){
        httpd_resp_set_hdr(req,"WWW-Authenticate","Bearer");return error_reply(req,401,"authentication_required");
    }
    if(strchr(req->uri,'?'))return error_reply(req,400,"query_not_supported");
    if(req->method==HTTP_GET&&!strcmp(req->uri,"/api/v1/controller/ota/state"))return json_reply(req,controller_ota_state(),200);
    if(req->method==HTTP_POST&&!strcmp(req->uri,"/api/v1/controller/ota/upload"))return controller_ota_upload(req,boot_id);
    if(req->method==HTTP_GET&&!strcmp(req->uri,"/api/v1/ble/state"))return json_reply(req,ble_service_state(),200);
    if(req->method==HTTP_GET&&!strcmp(req->uri,"/api/v1/board"))return json_reply(req,board_controls_state(),200);
    if(req->method==HTTP_GET&&!strcmp(req->uri,"/api/v1/ota/state"))return json_reply(req,ota_service_state(),200);
    if(req->method==HTTP_POST&&!strcmp(req->uri,"/api/v1/ota/upload"))return ota_service_upload(req,boot_id);
    if(req->method==HTTP_POST&&ota_service_busy()&&strcmp(req->uri,"/api/v1/motion/stop"))return error_reply(req,409,"ota_in_progress");
    if(!strcmp(req->uri,"/api/v1/rtc")){
        if(req->method==HTTP_GET)return json_reply(req,motion_bridge_rtc_state(),200);
        if(req->method!=HTTP_POST)return error_reply(req,405,"method_not_allowed");
        if(req->content_len<2||req->content_len>96)return error_reply(req,422,"invalid_rtc_command");
        cJSON *body=read_body(req);const char *const keys[]={"command","utc"};cJSON *cmd=cJSON_GetObjectItemCaseSensitive(body,"command"),*utc=cJSON_GetObjectItemCaseSensitive(body,"utc");
        if(!fields(body,keys,2)||!cJSON_IsString(cmd)||(strcmp(cmd->valuestring,"sync")&&strcmp(cmd->valuestring,"remind"))||!cJSON_IsString(utc)){cJSON_Delete(body);return error_reply(req,422,"invalid_rtc_command");}
        if(interaction_mode()!=PAVLOV_REST||!cloud_dialog_can_update()||audio_service_busy()){cJSON_Delete(body);return error_reply(req,409,"rtc_requires_rest");}
        int status;cJSON *j=!strcmp(cmd->valuestring,"sync")?motion_bridge_rtc_sync(utc->valuestring,&status):rtc_service_reminder(utc->valuestring,&status);cJSON_Delete(body);return json_reply(req,j,status);
    }
    if(req->method==HTTP_POST&&!strcmp(req->uri,"/api/v1/board")){
        if(req->content_len<2||req->content_len>96)return error_reply(req,422,"invalid_board_command");
        cJSON *body=read_body(req);cJSON *cmd=cJSON_GetObjectItemCaseSensitive(body,"command");
        const char *name=cJSON_IsString(cmd)?cmd->valuestring:"";int value=0;
        if(!strcmp(name,"inspect_eeprom")){
            const char *const inspect_keys[]={"command"};
            if(!fields(body,inspect_keys,1)){cJSON_Delete(body);return error_reply(req,422,"invalid_board_command");}
            if(interaction_mode()!=PAVLOV_REST||!cloud_dialog_can_update()||audio_service_busy()){cJSON_Delete(body);return error_reply(req,409,"eeprom_requires_rest");}
            int status;cJSON *j=motion_bridge_eeprom_inspect(&status);cJSON_Delete(body);return json_reply(req,j,status);
        }
        if(!strcmp(name,"probe_storage")){
            const char *const storage_keys[]={"command"};
            if(!fields(body,storage_keys,1)){cJSON_Delete(body);return error_reply(req,422,"invalid_board_command");}
            if(interaction_mode()!=PAVLOV_REST||!cloud_dialog_can_update()||audio_service_busy()){cJSON_Delete(body);return error_reply(req,409,"board_aux_requires_rest");}
            int status;cJSON *j=board_controls_probe_storage(&status);cJSON_Delete(body);return json_reply(req,j,status);
        }
        const char *const keys[]={"command","value"};bool probe=!strcmp(name,"probe");
        bool valid=fields(body,keys,probe?1:2)&&(probe||((!strcmp(name,"beep")||!strcmp(name,"led"))&&integer(cJSON_GetObjectItemCaseSensitive(body,"value"),0,!strcmp(name,"beep")?300:2,-1,&value)));
        if(!valid){cJSON_Delete(body);return error_reply(req,422,"invalid_board_command");}
        if(interaction_mode()!=PAVLOV_REST||!cloud_dialog_can_update()||audio_service_busy()){cJSON_Delete(body);return error_reply(req,409,"board_aux_requires_rest");}
        int status;cJSON *j=motion_bridge_aux_command(name,(unsigned)value,&status);cJSON_Delete(body);return json_reply(req,j,status);
    }
    if(!strcmp(req->uri,"/api/v1/compass")){
        cJSON *body=NULL;const char *command=NULL;
        if(req->method==HTTP_POST){
            if(req->content_len<2||req->content_len>64)return error_reply(req,422,"invalid_compass_command");
            body=read_body(req);const char *const allowed[]={"command"};cJSON *cmd=cJSON_GetObjectItemCaseSensitive(body,"command");
            if(!fields(body,allowed,1)||!cJSON_IsString(cmd)){cJSON_Delete(body);return error_reply(req,422,"invalid_compass_command");}command=cmd->valuestring;
            if(strcmp(command,"cancel")&&strcmp(command,"clear")&&(interaction_mode()!=PAVLOV_REST||!cloud_dialog_can_update())){cJSON_Delete(body);return error_reply(req,409,"compass_requires_rest");}
        }
        int status;cJSON *j=motion_bridge_compass(command,&status);cJSON_Delete(body);return json_reply(req,j,status);
    }
    if(!strcmp(req->uri,"/api/v1/power/limit")){
        cJSON *body=NULL;
        if(req->method==HTTP_POST){
            if(req->content_len<2||req->content_len>192)return error_reply(req,422,"invalid_power_limit_settings");
            body=read_body(req);if(!cJSON_IsObject(body)){cJSON_Delete(body);return error_reply(req,422,"invalid_power_limit_settings");}
            if(interaction_mode()!=PAVLOV_REST||!cloud_dialog_can_update()||audio_service_busy()){cJSON_Delete(body);return error_reply(req,409,"power_limit_requires_rest");}
        }
        int status;cJSON *j=motion_bridge_power_limit(body,&status);cJSON_Delete(body);return json_reply(req,j,status);
    }
    if(!strcmp(req->uri,"/api/v1/pose/protection")){
        cJSON *body=NULL;
        if(req->method==HTTP_POST){
            if(req->content_len<2||req->content_len>192)return error_reply(req,422,"invalid_pose_protection_settings");
            body=read_body(req);if(!cJSON_IsObject(body)){cJSON_Delete(body);return error_reply(req,422,"invalid_pose_protection_settings");}
            if(interaction_mode()!=PAVLOV_REST||!cloud_dialog_can_update()||audio_service_busy()){cJSON_Delete(body);return error_reply(req,409,"pose_protection_requires_rest");}
        }
        int status;cJSON *j=motion_bridge_pose_protection(body,&status);cJSON_Delete(body);return json_reply(req,j,status);
    }
    if(!strcmp(req->uri,"/api/v1/power/history")){
        if(req->method!=HTTP_GET)return error_reply(req,405,"read_only");
        return json_reply(req,motion_bridge_power_history(),200);
    }
    if(!strcmp(req->uri,"/api/v1/power")){
        cJSON *body=NULL;
        if(req->method==HTTP_POST){
            if(req->content_len<2||req->content_len>256)return error_reply(req,422,"invalid_power_settings");
            body=read_body(req);if(!cJSON_IsObject(body)){cJSON_Delete(body);return error_reply(req,422,"invalid_power_settings");}
        }
        int status;cJSON *j=motion_bridge_power(body,&status);cJSON_Delete(body);return json_reply(req,j,status);
    }
    if(!strcmp(req->uri,"/api/v1/pose/calibration")){
        const char *command=NULL;cJSON *body=NULL;
        if(req->method==HTTP_POST){
            if(req->content_len<2||req->content_len>64)return error_reply(req,422,"invalid_calibration_command");
            body=read_body(req);const char *const allowed[]={"command"};cJSON *cmd=cJSON_GetObjectItemCaseSensitive(body,"command");
            if(!fields(body,allowed,1)||!cJSON_IsString(cmd)){cJSON_Delete(body);return error_reply(req,422,"invalid_calibration_command");}
            command=cmd->valuestring;
            if(strcmp(command,"cancel")&&(interaction_mode()!=PAVLOV_REST||!cloud_dialog_can_update())){cJSON_Delete(body);return error_reply(req,409,"pose_calibration_requires_rest");}
        }
        int status;cJSON *j=motion_bridge_pose_calibration(command,&status);cJSON_Delete(body);return json_reply(req,j,status);
    }
    if(req->method==HTTP_POST&&!strcmp(req->uri,"/api/v1/diagnostics/wifi")){
        if(req->content_len<2||req->content_len>64)return error_reply(req,422,"invalid_wifi_diagnostic");
        cJSON *body=read_body(req);const char *const allowed[]={"pause_seconds"};int seconds=0;
        bool valid=fields(body,allowed,1)&&cJSON_HasObjectItem(body,"pause_seconds")&&integer(cJSON_GetObjectItemCaseSensitive(body,"pause_seconds"),3,45,0,&seconds);cJSON_Delete(body);
        if(!valid)return error_reply(req,422,"invalid_wifi_diagnostic");
        dog_ble_snapshot b;ble_service_snapshot(&b);
        if(!b.authenticated||b.pending||b.busy||!cloud_dialog_can_update())return error_reply(req,409,"wifi_diagnostic_requires_idle_ble");
        if(wifi_control_pause(seconds)!=ESP_OK)return error_reply(req,409,"wifi_diagnostic_busy");
        cJSON *j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"accepted",true);cJSON_AddNumberToObject(j,"automatic_resume_seconds",seconds);return json_reply(req,j,202);
    }
    if(req->method==HTTP_POST&&!strcmp(req->uri,"/api/v1/ble/control")){
        if(req->content_len<2||req->content_len>128)return error_reply(req,422,"invalid_ble_command");
        cJSON *body=read_body(req),*cmd=cJSON_GetObjectItemCaseSensitive(body,"command");const char *const allowed[]={"command"};
        bool accepted=fields(body,allowed,1)&&cJSON_IsString(cmd)&&!strcmp(cmd->valuestring,"pair")&&ble_service_pair_window();cJSON_Delete(body);
        return accepted?json_reply(req,ble_service_state(),202):error_reply(req,409,"ble_pairing_unavailable");
    }
    if(req->method==HTTP_GET&&!strcmp(req->uri,"/api/v1/display/state"))return json_reply(req,display_service_state(),200);
    if(req->method==HTTP_GET&&!strcmp(req->uri,"/api/v1/camera/state"))return json_reply(req,camera_service_state(),200);
    if(req->method==HTTP_POST&&!strcmp(req->uri,"/api/v1/camera/control")){
        if(req->content_len<2||req->content_len>128)return error_reply(req,422,"invalid_camera_command");
        cJSON *body=read_body(req);const char *const allowed[]={"command"};
        cJSON *command=cJSON_GetObjectItemCaseSensitive(body,"command");
        bool accepted=fields(body,allowed,1)&&cJSON_IsString(command)&&camera_service_command(command->valuestring);
        cJSON_Delete(body);if(!accepted)return error_reply(req,409,"camera_command_rejected");
        return json_reply(req,camera_service_state(),202);
    }
    if(req->method==HTTP_GET&&!strcmp(req->uri,"/api/v1/camera/photo")){
        uint8_t *data;size_t length;esp_err_t err=camera_service_photo(&data,&length);
        if(err!=ESP_OK)return error_reply(req,err==ESP_ERR_NOT_FOUND?404:503,"photo_unavailable");
        httpd_resp_set_type(req,"image/jpeg");httpd_resp_set_hdr(req,"Cache-Control","no-store");
        esp_err_t sent=httpd_resp_send(req,(const char*)data,length);free(data);return sent;
    }
    if(!strcmp(req->uri,"/api/v1/game")){
        if(req->method==HTTP_GET)return json_reply(req,riddle_service_json(),200);
        if(req->method==HTTP_POST){
            if(req->content_len<2||req->content_len>512)return error_reply(req,422,"invalid_game_command");
            cJSON *body=read_body(req);const char *const allowed[]={"command","text"};
            cJSON *command=cJSON_GetObjectItemCaseSensitive(body,"command"),*text=cJSON_GetObjectItemCaseSensitive(body,"text");
            bool valid=fields(body,allowed,2)&&cJSON_IsString(command);
            if(valid&&!strcmp(command->valuestring,"guess"))valid=cJSON_IsString(text)&&text->valuestring[0]&&strlen(text->valuestring)<=240;
            else if(text)valid=false;
            bool result=valid&&riddle_service_command(command->valuestring,cJSON_IsString(text)?text->valuestring:NULL);
            cJSON_Delete(body);if(!result)return error_reply(req,422,"invalid_game_command");
            return json_reply(req,riddle_service_json(),200);
        }
    }
    if(!strcmp(req->uri,"/api/v1/timer")){
        if(req->method==HTTP_GET)return json_reply(req,countdown_service_json(),200);
        if(req->method==HTTP_POST){
            if(req->content_len<2||req->content_len>256)return error_reply(req,422,"invalid_timer");
            cJSON *body=read_body(req);const char *const allowed[]={"seconds","command"};
            cJSON *seconds=cJSON_GetObjectItemCaseSensitive(body,"seconds"),*command=cJSON_GetObjectItemCaseSensitive(body,"command");
            int duration=0;bool cancel=cJSON_IsString(command)&&!strcmp(command->valuestring,"cancel");
            bool valid=fields(body,allowed,2)&&((cancel&&!seconds)||(!command&&seconds&&integer(seconds,1,86400,0,&duration)));
            cJSON_Delete(body);if(!valid)return error_reply(req,422,"invalid_timer");
            if(cancel)countdown_service_cancel();else countdown_service_set((unsigned)duration);
            return json_reply(req,countdown_service_json(),200);
        }
    }
    if(!strcmp(req->uri,"/api/v1/memory")){
        if(req->method==HTTP_GET)return json_reply(req,companion_memory_json(),200);
        if(req->method==HTTP_POST){
            if(req->content_len<2||req->content_len>768)return error_reply(req,422,"invalid_memory");
            cJSON *body=read_body(req);int err=cloud_dialog_memory_update(body);cJSON_Delete(body);
            if(err)return error_reply(req,err==ESP_ERR_INVALID_ARG?422:err==ESP_ERR_NOT_FOUND?404:err==ESP_ERR_NO_MEM?409:503,"memory_not_saved");
            return json_reply(req,companion_memory_json(),200);
        }
    }
    if(!strcmp(req->uri,"/api/v1/profile")){
        if(req->method==HTTP_GET)return json_reply(req,user_profile_json(),200);
        if(req->method==HTTP_POST){
            if(req->content_len<2||req->content_len>512)return error_reply(req,422,"invalid_profile");
            cJSON *body=read_body(req);int err=cloud_dialog_profile_update(body);cJSON_Delete(body);
            if(err)return error_reply(req,err==ESP_ERR_INVALID_ARG?422:503,"profile_not_saved");
            return json_reply(req,user_profile_json(),200);
        }
    }
    if(!strcmp(req->uri,"/api/v1/mode")){
        if(req->method==HTTP_GET)return json_reply(req,interaction_state(),200);
        if(req->method==HTTP_POST){
            if(req->content_len<2||req->content_len>512)return error_reply(req,422,"invalid_mode");
            cJSON *body=read_body(req);const char *const allowed[]={"mode"};
            cJSON *mode=cJSON_GetObjectItemCaseSensitive(body,"mode");
            if(!fields(body,allowed,1)||!cJSON_IsString(mode)){cJSON_Delete(body);return error_reply(req,422,"invalid_mode");}
            int err=cloud_dialog_change_mode(mode->valuestring);cJSON_Delete(body);
            if(err)return error_reply(req,err==ESP_ERR_INVALID_ARG?422:503,"mode_not_saved");
            return json_reply(req,interaction_state(),200);
        }
    }
    if(!strcmp(req->uri,"/api/v1/settings")){
        if(req->method==HTTP_GET)return json_reply(req,user_settings_json(),200);
        if(req->method==HTTP_POST){
            if(req->content_len<=0||req->content_len>512)return error_reply(req,422,"invalid_settings");
            cJSON *body=read_body(req);esp_err_t err=user_settings_update(body);cJSON_Delete(body);
            if(err)return error_reply(req,err==ESP_ERR_INVALID_ARG?422:503,"settings_not_saved");
            return json_reply(req,user_settings_json(),200);
        }
    }
    if(!strncmp(req->uri,"/api/v1/dialog/",15)){
        if(req->method==HTTP_POST&&!strcmp(req->uri,"/api/v1/dialog/transport")){
            if(req->content_len<2||req->content_len>512)return error_reply(req,422,"invalid_transport_config");
            cJSON *body=read_body(req);const char *const allowed[]={"tcp_nodelay","frame_interval_ms"};cJSON *value=cJSON_GetObjectItemCaseSensitive(body,"tcp_nodelay");
            int interval=0;cJSON *frame=cJSON_GetObjectItemCaseSensitive(body,"frame_interval_ms");
            if(!fields(body,allowed,2)||!cJSON_IsBool(value)||!integer(frame,20,100,0,&interval)||
               (frame&&interval!=20&&interval!=50&&interval!=100)){cJSON_Delete(body);return error_reply(req,422,"invalid_transport_config");}
            int err=cloud_dialog_set_transport(cJSON_IsTrue(value),interval);cJSON_Delete(body);
            if(err)return error_reply(req,409,"pause_dialogue_before_transport_change");
            return json_reply(req,cloud_dialog_state(),200);
        }
        if(req->method==HTTP_GET&&!strcmp(req->uri,"/api/v1/dialog/state"))return json_reply(req,cloud_dialog_state(),200);
        if(req->method==HTTP_POST&&!strcmp(req->uri,"/api/v1/dialog/validate")){
            if(req->content_len<2||req->content_len>512)return error_reply(req,413,"body_size_limit_512");
            cJSON *body=read_body(req);voice_intent intent;bool valid=voice_intent_parse(body,&intent);cJSON_Delete(body);
            cJSON *result=cJSON_CreateObject();cJSON_AddBoolToObject(result,"valid",valid);
            if(valid)cJSON_AddItemToObject(result,"intent",voice_intent_json(&intent));
            cJSON_AddBoolToObject(result,"motion_executed",false);return json_reply(req,result,200);
        }
        if(req->method==HTTP_POST&&!strcmp(req->uri,"/api/v1/dialog/control")){
            if(req->content_len<2||req->content_len>512)return error_reply(req,413,"body_size_limit_512");
            cJSON *body=read_body(req);static const char *const allowed[]={"command","text","execute_motion"};
            cJSON *command=cJSON_GetObjectItemCaseSensitive(body,"command"),*text=cJSON_GetObjectItemCaseSensitive(body,"text");
            cJSON *execute=cJSON_GetObjectItemCaseSensitive(body,"execute_motion");
            if(!fields(body,allowed,3)||!cJSON_IsString(command)||(text&&!cJSON_IsString(text))||
               (execute&&(!cJSON_IsBool(execute)||!cJSON_IsString(command)||strcmp(command->valuestring,"selftest")))){cJSON_Delete(body);return error_reply(req,422,"invalid_dialog_command");}
            bool accepted=cloud_dialog_command(command->valuestring,cJSON_IsString(text)?text->valuestring:NULL,cJSON_IsTrue(execute));
            cJSON_Delete(body);if(!accepted)return error_reply(req,409,"dialog_command_rejected");
            return json_reply(req,cloud_dialog_state(),202);
        }
        return error_reply(req,501,"dialog_endpoint_not_implemented");
    }
    if(!strncmp(req->uri,"/api/v1/audio/",14))return audio_service_http(req,boot_id);
    if(req->method==HTTP_GET){
        if(!strcmp(req->uri,"/api/v1/device")){
            cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"device_id",DOG_DEVICE_ID);
            cJSON_AddStringToObject(j,"api_version","1");
            cJSON_AddStringToObject(j,"esp32_firmware",esp_app_get_description()->version);
            cJSON_AddStringToObject(j,"project",esp_app_get_description()->project_name);
            cJSON_AddStringToObject(j,"hostname","pavlov.local");
            cJSON_AddStringToObject(j,"control_url","http://pavlov.local/");
            cJSON *state=motion_bridge_state();
            cJSON *version=cJSON_GetObjectItemCaseSensitive(state,"controller_version");
            cJSON_AddStringToObject(j,"controller_version",cJSON_IsString(version)?version->valuestring:"not_reported");
            cJSON_Delete(state);
            cJSON_AddNumberToObject(j,"uptime_ms",esp_timer_get_time()/1000);
            return json_reply(req,j,200);
        }
        if(!strcmp(req->uri,"/api/v1/capabilities"))return json_reply(req,capabilities(),200);
        if(!strcmp(req->uri,"/api/v1/state")){
            cJSON *j=motion_bridge_state();wifi_ap_record_t ap;
            if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK){cJSON_AddNumberToObject(j,"wifi_rssi_dbm",ap.rssi);cJSON_AddNumberToObject(j,"wifi_channel",ap.primary);}
            esp_netif_ip_info_t ip;esp_netif_t *iface=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if(iface&&esp_netif_get_ip_info(iface,&ip)==ESP_OK){char text[20];snprintf(text,sizeof(text),IPSTR,IP2STR(&ip.ip));cJSON_AddStringToObject(j,"ip",text);}
            return json_reply(req,j,200);
        }
        const char *prefix="/api/v1/requests/";
        if(!strncmp(req->uri,prefix,strlen(prefix))){
            const char *id=req->uri+strlen(prefix);if(!valid_id(id))return error_reply(req,400,"invalid_request_id");
            cJSON *j=motion_bridge_request(id);if(!j)return error_reply(req,404,"request_not_found");
            return json_reply(req,j,200);
        }
    }else if(req->method==HTTP_POST){
        if(!strcmp(req->uri,"/api/v1/actions"))return actions(req);
        if(!strcmp(req->uri,"/api/v1/motion/stop")){
            if(req->content_len!=0)return error_reply(req,400,"stop_requires_empty_body");
            motion_bridge_stop();cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"state","stop_requested");
            cJSON_AddBoolToObject(j,"controller_acknowledged",false);return json_reply(req,j,202);
        }
    }
    return error_reply(req,501,"endpoint_not_implemented");
}
static esp_err_t control_page(httpd_req_t *req)
{
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"Referrer-Policy","no-referrer");
    httpd_resp_set_hdr(req,"X-Frame-Options","DENY");
    httpd_resp_set_hdr(req,"Content-Security-Policy","default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src blob:; base-uri 'none'; frame-ancestors 'none'; form-action 'self'");
    return httpd_resp_send(req,(const char*)control_html_start,(control_html_end-control_html_start)-1);
}
void device_api_start(void)
{
    if(server)return;
    snprintf(boot_id,sizeof(boot_id),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
    httpd_config_t config=HTTPD_DEFAULT_CONFIG();config.stack_size=8192;
    config.uri_match_fn=httpd_uri_match_wildcard;config.lru_purge_enable=true;
    config.recv_wait_timeout=1;config.send_wait_timeout=2;
    ESP_ERROR_CHECK(httpd_start(&server,&config));
    httpd_uri_t get={.uri="/api/v1/*",.method=HTTP_GET,.handler=api};
    httpd_uri_t post={.uri="/api/v1/*",.method=HTTP_POST,.handler=api};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&get));ESP_ERROR_CHECK(httpd_register_uri_handler(server,&post));
    httpd_uri_t page={.uri="/",.method=HTTP_GET,.handler=control_page};
    httpd_uri_t panel={.uri="/control",.method=HTTP_GET,.handler=control_page};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&page));ESP_ERROR_CHECK(httpd_register_uri_handler(server,&panel));
    ESP_LOGI("DOG_API","API v1 ready on port 80; authentication required");
}
