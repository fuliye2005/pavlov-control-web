#include "ota_service.h"
#include "controller_ota.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "cloud_dialog.h"
#include "audio_service.h"
#include "camera_service.h"
#include "motion_bridge.h"
#include "wifi_provision.h"
#include "countdown_service.h"
#include "display_service.h"
#include "ble_service.h"

static SemaphoreHandle_t guard;
static esp_timer_handle_t restart_timer;
static bool updating,services_ready;
static const char *phase="idle",*failure="",*boot_check="starting";
static size_t received,total;
static char target_version[32];
static const char *image_state(esp_ota_img_states_t s)
{
    switch(s){case ESP_OTA_IMG_NEW:return "new";case ESP_OTA_IMG_PENDING_VERIFY:return "pending_verify";
    case ESP_OTA_IMG_VALID:return "valid";case ESP_OTA_IMG_INVALID:return "invalid";
    case ESP_OTA_IMG_ABORTED:return "aborted";default:return "undefined";}
}
static bool flag(cJSON *j,const char *name){return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,name));}
static const char *str(cJSON *j,const char *name){const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,name);return cJSON_IsString(v)?v->valuestring:"";}
bool ota_service_busy(void)
{
    if(controller_ota_busy())return true;
    if(!guard)return false;
    xSemaphoreTake(guard,portMAX_DELAY);bool value=updating||!strcmp(boot_check,"pending");xSemaphoreGive(guard);return value;
}
/* No automatic changes to persisted listening, mode, volume or strength. */
static const char *blocked_reason(void)
{
    if(wifi_provision_busy())return "ota_wifi_configuration_active";
    dog_ble_snapshot ble;ble_service_snapshot(&ble);
    if(ble.pending||ble.pairing_window)return "ota_ble_pairing_pending";
    if(ble.busy)return "ota_ble_busy";
    if(!cloud_dialog_can_update())return "ota_pause_listening";
    if(audio_service_busy())return "ota_audio_busy";
    cJSON *j=camera_service_state();bool camera_busy=flag(j,"busy")||flag(j,"streaming");cJSON_Delete(j);
    if(camera_busy)return "ota_camera_busy";
    j=motion_bridge_state();bool moving=flag(j,"controller_moving")||flag(j,"stop_pending")||cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(j,"active_request")),calibrating=flag(j,"calibration_active");cJSON_Delete(j);
    if(calibrating)return "ota_calibration_active";
    if(moving)return "ota_motion_busy";
    j=countdown_service_json();const char *state=str(j,"state");bool timer_busy=strcmp(state,"idle")&&strcmp(state,"finished");cJSON_Delete(j);
    return timer_busy?"ota_timer_active":NULL;
}
cJSON *ota_service_state(void)
{
    cJSON *j=cJSON_CreateObject();const esp_partition_t *run=esp_ota_get_running_partition(),*next=esp_ota_get_next_update_partition(NULL);
    cJSON_AddBoolToObject(j,"supported",next!=NULL);cJSON_AddStringToObject(j,"version",esp_app_get_description()->version);
    cJSON_AddStringToObject(j,"running_partition",run?run->label:"");cJSON_AddStringToObject(j,"next_partition",next?next->label:"");
    cJSON_AddNumberToObject(j,"max_image_bytes",next?next->size:0);cJSON_AddBoolToObject(j,"rollback_enabled",CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE);
    cJSON_AddStringToObject(j,"minimum_version","0.17.0");cJSON_AddBoolToObject(j,"preserves_settings",true);
    esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;
    if(run)esp_ota_get_state_partition(run,&state);
    cJSON_AddStringToObject(j,"image_state",image_state(state));
    xSemaphoreTake(guard,portMAX_DELAY);
    cJSON_AddBoolToObject(j,"busy",updating);cJSON_AddStringToObject(j,"phase",phase);cJSON_AddStringToObject(j,"error",failure);
    cJSON_AddNumberToObject(j,"received_bytes",received);cJSON_AddNumberToObject(j,"total_bytes",total);
    cJSON_AddStringToObject(j,"target_version",target_version);cJSON_AddStringToObject(j,"boot_check",boot_check);bool occupied=updating;
    xSemaphoreGive(guard);
    const char *reason=occupied?"ota_in_progress":state==ESP_OTA_IMG_PENDING_VERIFY?"ota_boot_check_pending":blocked_reason();
    cJSON_AddBoolToObject(j,"ready",next&&!reason);cJSON_AddStringToObject(j,"blocked_reason",reason?reason:"");
    const esp_partition_t *bad=esp_ota_get_last_invalid_partition();esp_app_desc_t bad_desc;
    if(bad&&esp_ota_get_partition_description(bad,&bad_desc)==ESP_OK){
        cJSON_AddStringToObject(j,"last_invalid_partition",bad->label);cJSON_AddStringToObject(j,"last_invalid_version",bad_desc.version);
    }
    return j;
}
static void restart(void *arg){(void)arg;esp_restart();}
static void validate_boot(void *arg)
{
    (void)arg;const esp_partition_t *running=esp_ota_get_running_partition();esp_ota_img_states_t state;
    if(esp_ota_get_state_partition(running,&state)!=ESP_OK||state!=ESP_OTA_IMG_PENDING_VERIFY){
        xSemaphoreTake(guard,portMAX_DELAY);boot_check=running->subtype==ESP_PARTITION_SUBTYPE_APP_FACTORY?"factory":"confirmed";xSemaphoreGive(guard);vTaskDelete(NULL);return;
    }
    xSemaphoreTake(guard,portMAX_DELAY);boot_check="pending";xSemaphoreGive(guard);
    bool healthy=false;
    for(unsigned n=0;n<180;n++){
        vTaskDelay(pdMS_TO_TICKS(250));xSemaphoreTake(guard,portMAX_DELAY);bool ready=services_ready;xSemaphoreGive(guard);
        if(!ready||n<20)continue;
#ifdef DOG_OTA_SELFTEST_FAIL
        break;
#else
        cJSON *screen=display_service_state();
        cloud_display_snapshot d;cloud_dialog_display_snapshot(&d);
        healthy=d.wifi&&ble_service_ready()&&flag(screen,"initialized")&&flag(screen,"touch_initialized")&&
            cJSON_GetNumberValue(cJSON_GetObjectItem(screen,"last_error"))==0&&
            cJSON_GetNumberValue(cJSON_GetObjectItem(screen,"touch_error"))==0&&
            cJSON_GetNumberValue(cJSON_GetObjectItem(screen,"refreshes"))>2;
        cJSON_Delete(screen);if(healthy)break;
#endif
    }
    if(healthy&&esp_ota_mark_app_valid_cancel_rollback()==ESP_OK){
        xSemaphoreTake(guard,portMAX_DELAY);boot_check="confirmed";xSemaphoreGive(guard);ESP_LOGI("DOG_OTA","Boot self-check confirmed");
    }else{
        ESP_LOGE("DOG_OTA","Boot self-check failed; reverting application");
        esp_err_t err=esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE("DOG_OTA","Rollback failed: %s",esp_err_to_name(err));
        xSemaphoreTake(guard,portMAX_DELAY);boot_check="rollback_failed";xSemaphoreGive(guard);
    }
    vTaskDelete(NULL);
}
void ota_service_init(void)
{
    guard=xSemaphoreCreateMutex();assert(guard);
    esp_timer_create_args_t timer={.callback=restart,.name="ota_restart"};ESP_ERROR_CHECK(esp_timer_create(&timer,&restart_timer));
    assert(xTaskCreate(validate_boot,"ota_boot_check",4096,NULL,3,NULL)==pdPASS);
}
void ota_service_services_ready(void)
{xSemaphoreTake(guard,portMAX_DELAY);services_ready=true;xSemaphoreGive(guard);}
static esp_err_t respond(httpd_req_t *req,const char *boot,int code,const char *error)
{
    cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"boot_id",boot);
    if(error)cJSON_AddStringToObject(j,"error",error);
    else{cJSON_AddBoolToObject(j,"accepted",true);cJSON_AddStringToObject(j,"target_version",target_version);cJSON_AddBoolToObject(j,"rebooting",true);}
    char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);if(!text)return ESP_ERR_NO_MEM;
    httpd_resp_set_status(req,code==202?"202 Accepted":code==409?"409 Conflict":code==413?"413 Payload Too Large":code==422?"422 Unprocessable Entity":"503 Service Unavailable");
    httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");httpd_resp_set_hdr(req,"Connection","close");
    esp_err_t err=httpd_resp_send(req,text,HTTPD_RESP_USE_STRLEN);cJSON_free(text);return err;
}
static int hex(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
static bool digest_header(httpd_req_t *req,unsigned char *digest)
{
    char value[65];if(httpd_req_get_hdr_value_len(req,"X-Firmware-SHA256")!=64||httpd_req_get_hdr_value_str(req,"X-Firmware-SHA256",value,sizeof(value))!=ESP_OK)return false;
    for(unsigned n=0;n<32;n++){int a=hex(value[n*2]),b=hex(value[n*2+1]);if(a<0||b<0)return false;digest[n]=(a<<4)|b;}return true;
}
static bool compatible(const unsigned char *data,esp_app_desc_t *desc)
{
    esp_image_header_t header;memcpy(&header,data,sizeof(header));
    memcpy(desc,data+sizeof(header)+sizeof(esp_image_segment_header_t),sizeof(*desc));
    if(header.magic!=ESP_IMAGE_HEADER_MAGIC||header.chip_id!=ESP_CHIP_ID_ESP32S3||desc->magic_word!=ESP_APP_DESC_MAGIC_WORD||
        !memchr(desc->project_name,0,sizeof(desc->project_name))||!memchr(desc->version,0,sizeof(desc->version))||
        strcmp(desc->project_name,esp_app_get_description()->project_name))return false;
    unsigned major,minor;
    /* Earlier releases cannot confirm an OTA boot or receive a subsequent update. */
    return sscanf(desc->version,"%u.%u",&major,&minor)==2&&(major>0||minor>=17);
}
esp_err_t ota_service_upload(httpd_req_t *req,const char *boot)
{
    const esp_partition_t *slot=esp_ota_get_next_update_partition(NULL);const char *error=NULL;int code=422;
    unsigned char expected[32],actual[32];char type[40];
    if(!slot)return respond(req,boot,503,"ota_partitions_missing");
    if(req->content_len<sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)+sizeof(esp_app_desc_t))return respond(req,boot,422,"ota_invalid_image");
    if(req->content_len>slot->size)return respond(req,boot,413,"ota_image_too_large");
    if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK||strcmp(type,"application/octet-stream"))return respond(req,boot,422,"ota_binary_required");
    if(!digest_header(req,expected))return respond(req,boot,422,"ota_sha256_required");
    xSemaphoreTake(guard,portMAX_DELAY);
    if(updating){xSemaphoreGive(guard);return respond(req,boot,409,"ota_in_progress");}
    updating=true;phase="receiving";failure="";received=0;total=req->content_len;target_version[0]=0;xSemaphoreGive(guard);
    const esp_partition_t *running=esp_ota_get_running_partition();esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;esp_ota_get_state_partition(running,&state);
    error=state==ESP_OTA_IMG_PENDING_VERIFY?"ota_boot_check_pending":motion_bridge_ota_reserve();
    bool motion_reserved=!error;
    if(!error)error=blocked_reason();
    if(error)code=409;
    unsigned char *buffer=NULL;esp_ota_handle_t handle=0;mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);
    if(!error){buffer=malloc(4096);if(!buffer){error="ota_no_memory";code=503;}}
    size_t done=0,head=0;const size_t head_size=sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)+sizeof(esp_app_desc_t);
    int64_t deadline=esp_timer_get_time()+120000000;
    if(!error&&mbedtls_sha256_starts(&hash,0)!=0){error="ota_hash_failed";code=503;}
    while(!error&&done<req->content_len){
        if(esp_timer_get_time()>deadline){error="ota_upload_timeout";break;}
        size_t need=handle?4096:head_size-head;
        if(need>req->content_len-done)need=req->content_len-done;
        int count=httpd_req_recv(req,(char *)buffer+head,need);
        if(count==HTTPD_SOCK_ERR_TIMEOUT)continue;
        if(count<=0){error="ota_upload_interrupted";break;}
        done+=count;
        if(!handle){
            head+=count;if(head<head_size)continue;
            esp_app_desc_t desc;if(!compatible(buffer,&desc)){error="ota_incompatible_image";break;}
            xSemaphoreTake(guard,portMAX_DELAY);memcpy(target_version,desc.version,sizeof(target_version));xSemaphoreGive(guard);
            if(esp_ota_begin(slot,OTA_WITH_SEQUENTIAL_WRITES,&handle)!=ESP_OK){error="ota_begin_failed";code=503;break;}
            count=head;head=0;
        }
        if(esp_ota_write(handle,buffer,count)!=ESP_OK){error="ota_write_failed";code=503;break;}
        if(mbedtls_sha256_update(&hash,buffer,count)!=0){error="ota_hash_failed";code=503;break;}
        xSemaphoreTake(guard,portMAX_DELAY);received=done;xSemaphoreGive(guard);vTaskDelay(1);
    }
    free(buffer);
    if(!error){
        if(mbedtls_sha256_finish(&hash,actual)!=0||memcmp(actual,expected,32))error="ota_sha256_mismatch";
        else{
            xSemaphoreTake(guard,portMAX_DELAY);phase="verifying";xSemaphoreGive(guard);
            esp_err_t err=esp_ota_end(handle);handle=0;if(err!=ESP_OK)error="ota_image_verification_failed";
        }
    }
    mbedtls_sha256_free(&hash);if(handle)esp_ota_abort(handle);
    if(!error&&esp_ota_set_boot_partition(slot)!=ESP_OK){error="ota_select_failed";code=503;}
    if(error){
        if(motion_reserved)motion_bridge_ota_release();
        xSemaphoreTake(guard,portMAX_DELAY);updating=false;phase="failed";failure=error;xSemaphoreGive(guard);
        return respond(req,boot,code,error);
    }
    xSemaphoreTake(guard,portMAX_DELAY);phase="rebooting";xSemaphoreGive(guard);
    esp_err_t result=respond(req,boot,202,NULL);
    /* Even a lost HTTP reply must not strand an already committed update. */
    ESP_ERROR_CHECK(esp_timer_start_once(restart_timer,1500000));return result;
}
