#include "controller_ota.h"
#include "motion_bridge.h"
#include "board_profile.h"
#include "ota_service.h"
#include "interaction.h"
#include "cloud_dialog.h"
#include "audio_service.h"
#include "camera_service.h"
#include "countdown_service.h"
#include "wifi_provision.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define APP 0x08008000u
#define TARGET 0xf1030512u
#define SLOT 229376u
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static bool busy;static const char *phase="idle",*failure="";static unsigned progress,total;
static char version[16];static uint8_t *image;static uint32_t crc;
static uint32_t word(const uint8_t *p){return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint32_t checksum(const uint8_t *p,unsigned n){uint32_t c=0xffffffff;for(unsigned i=0;i<n;i++){c^=p[i];for(unsigned j=0;j<8;j++)c=(c>>1)^((0u-(c&1))&0xedb88320u);}return c^0xffffffff;}
bool controller_ota_busy(void){portENTER_CRITICAL(&mux);bool b=busy;portEXIT_CRITICAL(&mux);return b;}
static void state(const char *p,const char *f){portENTER_CRITICAL(&mux);phase=p;failure=f;portEXIT_CRITICAL(&mux);}
cJSON *controller_ota_state(void){
 cJSON *j=cJSON_CreateObject();char v[16];portENTER_CRITICAL(&mux);bool b=busy;const char *p=phase,*f=failure;unsigned n=progress,t=total;memcpy(v,version,16);portEXIT_CRITICAL(&mux);
 cJSON_AddBoolToObject(j,"busy",b);cJSON_AddStringToObject(j,"phase",p);cJSON_AddStringToObject(j,"error",f);cJSON_AddStringToObject(j,"target_version",v);cJSON_AddNumberToObject(j,"sent_bytes",n);cJSON_AddNumberToObject(j,"total_bytes",t);cJSON_AddNumberToObject(j,"max_image_bytes",SLOT);cJSON_AddBoolToObject(j,"automatic_boot_failure_rollback",false);return j;
}
static bool line(const char *prefix,char *answer,size_t capacity,unsigned timeout){
 int64_t end=esp_timer_get_time()+(int64_t)timeout*1000;size_t used=0;bool overflow=false;
 while(esp_timer_get_time()<end){uint8_t c;if(uart_read_bytes(DOG_BRIDGE_UART,&c,1,pdMS_TO_TICKS(10))<=0)continue;
  if(c=='\n'||c=='\r'){if(used&&!overflow){answer[used]=0;if(!strncmp(answer,prefix,strlen(prefix)))return true;if(!strncmp(answer,"BL_ERR",6))return false;}used=0;overflow=false;}
  else if(c>=32&&c<127&&used<capacity-1)answer[used++]=(char)c;else overflow=true;
 }return false;
}
static bool exchange(const char *command,const char *prefix,char *out,size_t cap,unsigned ms){return uart_write_bytes(DOG_BRIDGE_UART,command,strlen(command))==(int)strlen(command)&&line(prefix,out,cap,ms);}
static void worker(void *unused){
 (void)unused;const char *error=NULL;bool owned=motion_bridge_controller_uart_acquire();char reply[192],command[128];
 if(!owned)error="controller_uart_handoff_failed";
 if(!error){
  uart_flush_input(DOG_BRIDGE_UART);state("entering_loader","");
  if(!exchange("BL_INFO\n","BL_INFO protocol=1",reply,sizeof(reply),500)){
   if(!exchange("BOOT_UPDATE\n","BOOT_UPDATE accepted=1",reply,sizeof(reply),2000)||!line("PAVLOV_LOADER 1 READY",reply,sizeof(reply),5000))error="loader_not_available";
  }
  if(!error&&(!exchange("BL_INFO\n","BL_INFO protocol=1",reply,sizeof(reply),3000)||!strstr(reply,"target=4043506962")||!strstr(reply,"app=134250496")||!strstr(reply,"slot=229376")))error="loader_incompatible";
 }
 if(!error){state("staging","");snprintf(command,sizeof(command),"BL_BEGIN %u %lu\n",total,(unsigned long)crc);if(!exchange(command,"BL_BEGIN ok=1",reply,sizeof(reply),15000))error="stage_begin_failed";}
 for(unsigned offset=0;!error&&offset<total;){
  unsigned n=total-offset;if(n>48)n=48;int used=snprintf(command,sizeof(command),"BL_DATA %u ",offset);const char hex[]="0123456789abcdef";
  for(unsigned j=0;j<n;j++){unsigned b=image[64+offset+j];command[used++]=hex[b>>4];command[used++]=hex[b&15];}command[used++]='\n';command[used]=0;
  if(!exchange(command,"BL_DATA next=",reply,sizeof(reply),3000)){error="stage_transfer_unconfirmed";break;}
  unsigned next;if(sscanf(reply,"BL_DATA next=%u",&next)!=1||next!=offset+n){error="stage_offset_mismatch";break;}
  offset=next;portENTER_CRITICAL(&mux);progress=offset;portEXIT_CRITICAL(&mux);vTaskDelay(1);
 }
 if(!error){state("committing","");if(!exchange("BL_COMMIT\n","BL_COMMIT ok=1",reply,sizeof(reply),5000))error="stage_crc_or_commit_failed";}
 if(!error){state("installing","");if(!exchange("BL_RUN\n","BL_RUN accepted=1",reply,sizeof(reply),30000))error="application_install_unconfirmed";}
 if(!error){state("waiting_for_application","");char expected[96];snprintf(expected,sizeof(expected),"DOG_MOTION_CHECK %s READY outputs=OFF",version);if(!line(expected,reply,sizeof(reply),15000))error="application_startup_unconfirmed";}
 if(owned){motion_bridge_controller_uart_release();}
 motion_bridge_ota_release();free(image);image=NULL;
 state(error?"failed":"startup_confirmed",error?error:"");portENTER_CRITICAL(&mux);busy=false;portEXIT_CRITICAL(&mux);vTaskDelete(NULL);
}
static esp_err_t respond(httpd_req_t *r,int code,const char *error){
 cJSON *j=controller_ota_state();if(error)cJSON_AddStringToObject(j,"request_error",error);char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);
 httpd_resp_set_status(r,code==202?"202 Accepted":code==409?"409 Conflict":code==422?"422 Unprocessable Entity":"503 Service Unavailable");httpd_resp_set_type(r,"application/json");httpd_resp_set_hdr(r,"Cache-Control","no-store");httpd_resp_set_hdr(r,"Connection","close");esp_err_t e=httpd_resp_send(r,text,HTTPD_RESP_USE_STRLEN);free(text);return e;
}
static bool flag(cJSON *j,const char *s){return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j,s));}
esp_err_t controller_ota_upload(httpd_req_t *req,const char *boot){
 char boot_header[17],hash[65];
 if(httpd_req_get_hdr_value_str(req,"X-Device-Boot-ID",boot_header,sizeof(boot_header))!=ESP_OK||strcmp(boot_header,boot))return respond(req,409,"boot_id_mismatch");
 if(req->content_len<368||req->content_len>SLOT+64||httpd_req_get_hdr_value_len(req,"X-Firmware-SHA256")!=64||httpd_req_get_hdr_value_str(req,"X-Firmware-SHA256",hash,sizeof(hash))!=ESP_OK)return respond(req,422,"controller_image_headers_invalid");
 if(ota_service_busy()||wifi_provision_busy()||interaction_mode()!=PAVLOV_REST||!cloud_dialog_can_update()||audio_service_busy())return respond(req,409,"controller_update_requires_idle_rest");
 cJSON *j=camera_service_state();bool occupied=flag(j,"busy")||flag(j,"streaming");cJSON_Delete(j);j=countdown_service_json();const char *timer=cJSON_GetStringValue(cJSON_GetObjectItem(j,"state"));occupied|=!timer||(strcmp(timer,"idle")&&strcmp(timer,"finished"));cJSON_Delete(j);
 j=motion_bridge_state();occupied|=flag(j,"armed");cJSON_Delete(j);if(occupied)return respond(req,409,"controller_update_requires_outputs_off");
 portENTER_CRITICAL(&mux);bool used=busy;if(!used){busy=true;phase="receiving";failure="";total=progress=0;version[0]=0;}portEXIT_CRITICAL(&mux);if(used)return respond(req,409,"controller_update_busy");
 const char *error=motion_bridge_ota_reserve();bool reserved=!error;unsigned n=req->content_len;uint8_t *data=NULL;
 if(!error){data=heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!data)error="controller_no_memory";}
 unsigned got=0;int64_t deadline=esp_timer_get_time()+60000000;
 while(!error&&got<n){if(esp_timer_get_time()>deadline){error="controller_upload_timeout";break;}int count=httpd_req_recv(req,(char *)data+got,n-got);if(count==HTTPD_SOCK_ERR_TIMEOUT)continue;if(count<=0){error="controller_upload_interrupted";break;}got+=(unsigned)count;}
 if(!error){uint8_t digest[32];char actual[65];if(mbedtls_sha256(data,n,digest,0)!=0){error="controller_hash_failed";}
  for(unsigned i=0;i<32;i++){snprintf(actual+i*2,3,"%02x",digest[i]);}
  if(strcmp(hash,actual))error="controller_sha256_mismatch";}
 if(!error){unsigned size=word(data+8);uint32_t sp=word(data+64),pc=word(data+68);bool tag=false;const char label[]="PAVLOV_STM_APP_V1";
  for(unsigned i=64;i+sizeof(label)-1<=n;i++)if(!memcmp(data+i,label,sizeof(label)-1)){tag=true;break;}
  char expected_version[64];snprintf(expected_version,sizeof(expected_version),"VERSION motion-check %.15s\r\n",(const char *)data+24);
  bool version_matches=false;
  if(memchr(data+24,0,16))for(unsigned i=64;i+strlen(expected_version)<=n;i++)if(!memcmp(data+i,expected_version,strlen(expected_version))){version_matches=true;break;}
  if(!version_matches||memcmp(data,"PVSTM01",8)||size!=n-64||size>SLOT||(size&1)||word(data+16)!=APP||word(data+20)!=TARGET||!memchr(data+24,0,16)||!data[24]||!tag||(sp&7)||sp<=0x20000000||sp>0x20010000||!(pc&1)||(pc&~1u)<APP||(pc&~1u)>=APP+size||checksum(data+64,size)!=word(data+12))error="controller_image_incompatible";
  for(unsigned i=40;!error&&i<64;i++)if(data[i])error="controller_image_reserved_fields";
  if(!error){portENTER_CRITICAL(&mux);total=size;crc=word(data+12);memcpy(version,data+24,16);image=data;portEXIT_CRITICAL(&mux);}
 }
 if(!error&&xTaskCreate(worker,"stm32_update",6144,NULL,5,NULL)!=pdPASS)error="controller_worker_unavailable";
 if(error){free(data);image=NULL;if(reserved)motion_bridge_ota_release();state("failed",error);portENTER_CRITICAL(&mux);busy=false;portEXIT_CRITICAL(&mux);return respond(req,422,error);}
 return respond(req,202,NULL);
}
