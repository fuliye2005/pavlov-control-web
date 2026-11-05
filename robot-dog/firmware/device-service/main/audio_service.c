#include "audio_service.h"
#include "voice_activity.h"
#include "voice_capture_policy.h"
#include "user_settings.h"
#include "board_i2c.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "cJSON.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* Vendor hqyj-esp32s3&f103 config.h; independent of motion UART GPIO12/13. */
#define AUDIO_RATE 16000
#define AUDIO_MAX_BYTES 96000
#define PA_GPIO GPIO_NUM_1
enum {TONE=1,RECORD,REPLAY,PLAY};
typedef struct {unsigned id,kind;int duration_ms;} audio_job;
static SemaphoreHandle_t guard;
static QueueHandle_t jobs;
static esp_codec_dev_handle_t codec;
static bool attempted,ready,busy,exporting;
static volatile bool cancelled;
static unsigned generation;
static audio_job current;
static const char *result="idle";
static int last_error,peak;
static int applied_volume=-1;
static int voice_noise_floor;
static bool voice_noise_ready;
static unsigned clipped;
static uint8_t *buffer;
static size_t recorded,play_bytes;
static const char *kind_name(unsigned kind)
{return kind==TONE?"tone":kind==RECORD?"record":kind==REPLAY?"replay":kind==PLAY?"play":"none";}

static int initialize_codec(void)
{
    if(attempted)return ready?0:ESP_ERR_INVALID_STATE;
    attempted=true;
    gpio_config_t pa={.pin_bit_mask=1ULL<<PA_GPIO,.mode=GPIO_MODE_OUTPUT};
    int err=gpio_config(&pa);if(err)return err;
    gpio_set_level(PA_GPIO,0);
    i2c_master_bus_handle_t bus=board_i2c_bus();
    i2s_chan_handle_t tx=NULL,rx=NULL;
    i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);
    channel.dma_desc_num=6;channel.dma_frame_num=160;channel.auto_clear=true;
    if((err=i2s_new_channel(&channel,&tx,&rx)))return err;
    i2s_std_config_t standard={.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),
        .gpio_cfg={.mclk=GPIO_NUM_48,.bclk=GPIO_NUM_42,.ws=GPIO_NUM_21,.dout=GPIO_NUM_40,.din=GPIO_NUM_38}};
    if((err=i2s_channel_init_std_mode(tx,&standard)))return err;
    if((err=i2s_channel_init_std_mode(rx,&standard)))return err;
    audio_codec_i2s_cfg_t data_cfg={.port=I2S_NUM_0,.rx_handle=rx,.tx_handle=tx};
    const audio_codec_data_if_t *data=audio_codec_new_i2s_data(&data_cfg);
    audio_codec_i2c_cfg_t control_cfg={.port=I2C_NUM_0,.addr=ES8311_CODEC_DEFAULT_ADDR,.bus_handle=bus};
    const audio_codec_ctrl_if_t *control=audio_codec_new_i2c_ctrl(&control_cfg);
    const audio_codec_gpio_if_t *gpio=audio_codec_new_gpio();
    if(!data||!control||!gpio)return ESP_ERR_NO_MEM;
    es8311_codec_cfg_t chip={.ctrl_if=control,.gpio_if=gpio,.codec_mode=ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin=GPIO_NUM_NC,.use_mclk=true,.hw_gain={.pa_voltage=5.0,.codec_dac_voltage=3.3}};
    const audio_codec_if_t *chip_if=es8311_codec_new(&chip);
    if(!chip_if)return ESP_ERR_NO_MEM;
    esp_codec_dev_cfg_t cfg={.dev_type=ESP_CODEC_DEV_TYPE_IN_OUT,.codec_if=chip_if,.data_if=data};
    codec=esp_codec_dev_new(&cfg);if(!codec)return ESP_ERR_NO_MEM;
    esp_codec_dev_sample_info_t fs={.bits_per_sample=16,.channel=1,.sample_rate=AUDIO_RATE};
    if((err=esp_codec_dev_open(codec,&fs)))return err;
    int volume=user_settings_get().volume;
    if((err=esp_codec_dev_set_out_vol(codec,volume)))return err;
    if((err=esp_codec_dev_set_in_gain(codec,30.0)))return err;
    xSemaphoreTake(guard,portMAX_DELAY);ready=true;applied_volume=volume;xSemaphoreGive(guard);
    return 0;
}
static int apply_volume(void)
{
    int volume=user_settings_get().volume;
    if(volume==applied_volume)return ESP_OK;
    int err=esp_codec_dev_set_out_vol(codec,volume);
    if(!err){xSemaphoreTake(guard,portMAX_DELAY);applied_volume=volume;xSemaphoreGive(guard);}
    return err;
}
static bool allocate_buffer(void)
{if(!buffer)buffer=malloc(AUDIO_MAX_BYTES);return buffer!=NULL;}
static void worker(void *unused)
{
    (void)unused;audio_job job;int16_t chunk[320];unsigned tone_phase=0;
    for(;;){
        xQueueReceive(jobs,&job,portMAX_DELAY);
        int err=initialize_codec();size_t total=0;int local_peak=0;unsigned local_clipped=0;
        if(!err&&job.kind!=RECORD)err=apply_volume();
        xSemaphoreTake(guard,portMAX_DELAY);result=err?"failed":"running";xSemaphoreGive(guard);
        size_t wanted=job.kind==TONE||job.kind==RECORD?(size_t)job.duration_ms*32:
            job.kind==REPLAY?recorded:play_bytes;
        if(!err&&job.kind==RECORD&&!allocate_buffer())err=ESP_ERR_NO_MEM;
        int64_t deadline=esp_timer_get_time()+((int64_t)wanted*1000000/32000)+3000000;
        if(!err&&job.kind!=RECORD)gpio_set_level(PA_GPIO,1);
        while(!err&&!cancelled&&total<wanted){
            if(esp_timer_get_time()>deadline){err=ESP_ERR_TIMEOUT;break;}
            size_t bytes=wanted-total;if(bytes>sizeof(chunk))bytes=sizeof(chunk);
            if(job.kind==RECORD){
                err=esp_codec_dev_read(codec,chunk,(int)bytes);
                if(!err){
                    memcpy(buffer+total,chunk,bytes);
                    for(size_t i=0;i<bytes/2;++i){int v=chunk[i];if(v<0)v=-v;
                        if(v>local_peak)local_peak=v;
                        if(v>=32760)++local_clipped;
                    }
                }
            }else{
                if(job.kind==TONE){
                    for(size_t i=0;i<bytes/2;++i){
                        tone_phase=(tone_phase+640)%AUDIO_RATE;
                        int v=(tone_phase<8000?tone_phase:16000-tone_phase)*2400/8000-1200;
                        chunk[i]=(int16_t)v;
                    }
                }else memcpy(chunk,buffer+total,bytes);
                err=esp_codec_dev_write(codec,chunk,(int)bytes);
            }
            if(!err)total+=bytes;
        }
        gpio_set_level(PA_GPIO,0);
        xSemaphoreTake(guard,portMAX_DELAY);
        if(job.kind==RECORD){recorded=!err&&!cancelled?total:0;peak=local_peak;clipped=local_clipped;}
        last_error=err;result=cancelled?"cancelled":err?"failed":"finished";busy=false;
        xSemaphoreGive(guard);
    }
}
void audio_service_init(void)
{
    guard=xSemaphoreCreateMutex();jobs=xQueueCreate(1,sizeof(audio_job));assert(guard&&jobs);
    assert(xTaskCreate(worker,"dog_audio",6144,NULL,5,NULL)==pdPASS);
}
void audio_service_stop(void){if(guard){xSemaphoreTake(guard,portMAX_DELAY);cancelled=true;xSemaphoreGive(guard);}}
bool audio_service_busy(void){xSemaphoreTake(guard,portMAX_DELAY);bool value=busy;xSemaphoreGive(guard);return value;}
static cJSON *snapshot(void)
{
    xSemaphoreTake(guard,portMAX_DELAY);cJSON *j=cJSON_CreateObject();
    cJSON_AddNumberToObject(j,"job_id",current.id);cJSON_AddStringToObject(j,"kind",kind_name(current.kind));
    cJSON_AddStringToObject(j,"state",result);cJSON_AddBoolToObject(j,"busy",busy);
    cJSON_AddBoolToObject(j,"initialized",ready);cJSON_AddBoolToObject(j,"hardware_audio_verified",false);
    cJSON_AddNumberToObject(j,"applied_volume",applied_volume);
    cJSON_AddNumberToObject(j,"sample_rate",AUDIO_RATE);cJSON_AddStringToObject(j,"format","pcm_s16le_mono");
    cJSON_AddNumberToObject(j,"recorded_bytes",recorded);cJSON_AddNumberToObject(j,"peak",peak);
    cJSON_AddNumberToObject(j,"clipped_samples",clipped);cJSON_AddNumberToObject(j,"last_error",last_error);
    cJSON_AddBoolToObject(j,"cancel_requested",busy&&cancelled);
    xSemaphoreGive(guard);return j;
}
static esp_err_t reply(httpd_req_t *req,const char *boot,int status,cJSON *j)
{
    cJSON_AddStringToObject(j,"boot_id",boot);char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);
    if(!text)return ESP_ERR_NO_MEM;
    httpd_resp_set_status(req,status==200?"200 OK":status==202?"202 Accepted":status==409?"409 Conflict":
        status==413?"413 Payload Too Large":status==422?"422 Unprocessable Entity":status==501?"501 Not Implemented":"503 Service Unavailable");
    httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");
    esp_err_t rc=httpd_resp_send(req,text,HTTPD_RESP_USE_STRLEN);cJSON_free(text);return rc;
}
static esp_err_t failure(httpd_req_t *req,const char *boot,int status,const char *message)
{cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"error",message);return reply(req,boot,status,j);}
static bool reserve(unsigned kind,int duration)
{
    xSemaphoreTake(guard,portMAX_DELAY);
    if(busy||exporting||(kind==REPLAY&&!recorded)){xSemaphoreGive(guard);return false;}
    current=(audio_job){.id=++generation,.kind=kind,.duration_ms=duration};
    busy=true;cancelled=false;last_error=0;result="queued";
    if(kind==RECORD||kind==PLAY)recorded=0;
    xSemaphoreGive(guard);return true;
}
static void abort_upload(int error)
{xSemaphoreTake(guard,portMAX_DELAY);last_error=error;busy=false;result="failed";xSemaphoreGive(guard);}
static void synchronous_end(int error)
{
    gpio_set_level(PA_GPIO,0);
    xSemaphoreTake(guard,portMAX_DELAY);last_error=error;busy=false;
    result=error==ESP_ERR_NOT_FOUND?"idle":error?"failed":"finished";
    xSemaphoreGive(guard);
}
esp_err_t audio_play_pcm(const uint8_t *pcm,size_t bytes,volatile bool *cancel)
{
    if(!pcm||!bytes||(bytes%2)||bytes>960000)return ESP_ERR_INVALID_SIZE;
    if(!reserve(PLAY,0))return ESP_ERR_INVALID_STATE;
    int err=initialize_codec();size_t offset=0;
    if(!err)err=apply_volume();
    if(!err)gpio_set_level(PA_GPIO,1);
    while(!err&&offset<bytes){
        if(cancelled||(cancel&&*cancel)){err=ESP_ERR_INVALID_STATE;break;}
        size_t n=bytes-offset;if(n>640)n=640;
        err=esp_codec_dev_write(codec,(void*)(pcm+offset),(int)n);offset+=n;
    }
    synchronous_end(err);return err;
}
/* Calibrate once before the greeting, rather than hiding the first 400ms of
 * every subsequent listening window. No audio is saved or uploaded here. */
esp_err_t audio_prepare_voice(volatile bool *cancel)
{
    if(voice_noise_ready)return ESP_OK;
    if(!reserve(RECORD,0))return ESP_ERR_INVALID_STATE;
    int err=initialize_codec();int16_t frame[320];unsigned energies[20];unsigned count=0;
    while(!err&&count<20){
        if(cancelled||(cancel&&*cancel)){err=ESP_ERR_INVALID_STATE;break;}
        err=esp_codec_dev_read(codec,frame,sizeof(frame));if(err)break;
        unsigned energy=0;for(unsigned i=0;i<320;++i){int v=frame[i];energy+=(unsigned)(v<0?-v:v);}
        energies[count++]=energy/320;
    }
    if(!err&&count==20){
        for(unsigned i=1;i<count;++i){unsigned x=energies[i],j=i;while(j&&energies[j-1]>x){energies[j]=energies[j-1];--j;}energies[j]=x;}
        voice_noise_floor=(int)energies[5];voice_noise_ready=true;
    }
    synchronous_end(err);return err;
}
esp_err_t audio_capture_voice(uint8_t *pcm,size_t capacity,size_t *bytes,volatile bool *cancel)
{
    *bytes=0;if(!pcm||capacity<32000||capacity>VOICE_MAX_BYTES)return ESP_ERR_INVALID_SIZE;
    if(!reserve(RECORD,0))return ESP_ERR_INVALID_STATE;
    int err=initialize_codec();int16_t frame[320];voice_activity vad=voice_activity_resume(voice_noise_floor,voice_noise_ready);
    size_t used=0;
    int64_t deadline=esp_timer_get_time()+(int64_t)VOICE_WAIT_MS*1000;
    while(!err&&esp_timer_get_time()<deadline&&used+sizeof(frame)<=capacity){
        if(cancelled||(cancel&&*cancel)){err=ESP_ERR_INVALID_STATE;break;}
        err=esp_codec_dev_read(codec,frame,sizeof(frame));if(err)break;
        unsigned energy=0;
        for(unsigned i=0;i<320;++i){int v=frame[i];energy+=(unsigned)(v<0?-v:v);}
        energy/=320;bool was_speaking=vad.speaking;
        voice_activity_feed(&vad,energy);
        if(!was_speaking){
            if(used>=VOICE_PREROLL_BYTES){memmove(pcm,pcm+640,used-640);used-=640;}
            if(vad.speaking)deadline=esp_timer_get_time()+(int64_t)VOICE_MAX_MS*1000;
        }
        memcpy(pcm+used,frame,sizeof(frame));used+=sizeof(frame);
        if(voice_capture_complete(&vad,used)){used=voice_capture_trim_tail(used,vad.quiet);break;}
    }
    if(!err&&vad.frames>=20){voice_noise_floor=vad.noise;voice_noise_ready=true;}
    if(!err&&!vad.speaking)err=ESP_ERR_NOT_FOUND;
    if(!err)*bytes=used;
    synchronous_end(err);return err;
}
static void le32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static esp_err_t recording(httpd_req_t *req,const char *boot)
{
    xSemaphoreTake(guard,portMAX_DELAY);
    if(busy||exporting||!recorded){xSemaphoreGive(guard);return failure(req,boot,409,"recording_not_ready");}
    exporting=true;size_t bytes=recorded;xSemaphoreGive(guard);
    uint8_t header[44]={0};memcpy(header,"RIFF",4);le32(header+4,bytes+36);memcpy(header+8,"WAVEfmt ",8);
    le32(header+16,16);header[20]=1;header[22]=1;le32(header+24,AUDIO_RATE);le32(header+28,AUDIO_RATE*2);
    header[32]=2;header[34]=16;memcpy(header+36,"data",4);le32(header+40,bytes);
    httpd_resp_set_type(req,"audio/wav");httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Device-Boot",boot);
    esp_err_t err=httpd_resp_send_chunk(req,(const char*)header,sizeof(header));
    for(size_t offset=0;!err&&offset<bytes;){size_t n=bytes-offset;if(n>2048)n=2048;
        err=httpd_resp_send_chunk(req,(const char*)buffer+offset,n);offset+=n;}
    if(!err)err=httpd_resp_send_chunk(req,NULL,0);
    xSemaphoreTake(guard,portMAX_DELAY);exporting=false;xSemaphoreGive(guard);return err;
}
esp_err_t audio_service_http(httpd_req_t *req,const char *boot)
{
    if(req->method==HTTP_GET&&!strcmp(req->uri,"/api/v1/audio/state"))return reply(req,boot,200,snapshot());
    if(req->method==HTTP_GET&&!strcmp(req->uri,"/api/v1/audio/recording.wav"))return recording(req,boot);
    if(req->method==HTTP_POST&&!strcmp(req->uri,"/api/v1/audio/stop")){
        if(req->content_len)return failure(req,boot,422,"empty_body_required");
        audio_service_stop();return reply(req,boot,202,snapshot());
    }
    if(req->method==HTTP_POST&&!strcmp(req->uri,"/api/v1/audio/play")){
        char content_type[48];
        if(httpd_req_get_hdr_value_str(req,"Content-Type",content_type,sizeof(content_type))!=ESP_OK||
            strcmp(content_type,"application/octet-stream"))return failure(req,boot,422,"pcm_octet_stream_required");
        if(req->content_len<2||req->content_len>AUDIO_MAX_BYTES||(req->content_len%2))return failure(req,boot,413,"pcm_body_must_be_even_and_at_most_96000_bytes");
        if(!reserve(PLAY,0))return failure(req,boot,409,"audio_busy");
        if(!allocate_buffer()){abort_upload(ESP_ERR_NO_MEM);return failure(req,boot,503,"audio_buffer_unavailable");}
        size_t received=0;int64_t deadline=esp_timer_get_time()+5000000;
        while(received<req->content_len){
            if(esp_timer_get_time()>deadline){abort_upload(ESP_ERR_TIMEOUT);return failure(req,boot,503,"pcm_upload_timeout");}
            int n=httpd_req_recv(req,(char*)buffer+received,req->content_len-received);
            if(n<=0){abort_upload(ESP_ERR_TIMEOUT);return failure(req,boot,503,"pcm_upload_failed");}received+=n;}
        play_bytes=received;
        if(xQueueSend(jobs,&current,0)!=pdPASS){abort_upload(ESP_ERR_INVALID_STATE);return failure(req,boot,503,"audio_queue_unavailable");}
        return reply(req,boot,202,snapshot());
    }
    if(req->method==HTTP_POST&&!strcmp(req->uri,"/api/v1/audio/job")){
        if(req->content_len<2||req->content_len>256)return failure(req,boot,413,"audio_job_body_limit_256");
        char text[257];int got=0;int64_t deadline=esp_timer_get_time()+2000000;
        while(got<req->content_len){
            if(esp_timer_get_time()>deadline)return failure(req,boot,422,"body_receive_timeout");
            int n=httpd_req_recv(req,text+got,req->content_len-got);
            if(n<=0)return failure(req,boot,422,"body_receive_failed");
            got+=n;
        }
        text[got]=0;
        cJSON *j=cJSON_ParseWithLengthOpts(text,got+1,NULL,true),*kind=cJSON_GetObjectItemCaseSensitive(j,"kind"),*ms=cJSON_GetObjectItemCaseSensitive(j,"duration_ms");
        bool valid=cJSON_IsObject(j)&&cJSON_IsString(kind);
        for(cJSON *p=j?j->child:NULL;p;p=p->next){
            if(!p->string||(strcmp(p->string,"kind")&&strcmp(p->string,"duration_ms")))valid=false;
            for(cJSON *q=p->next;q;q=q->next)if(p->string&&q->string&&!strcmp(p->string,q->string))valid=false;
        }
        unsigned type=valid&&!strcmp(kind->valuestring,"tone")?TONE:valid&&!strcmp(kind->valuestring,"record")?RECORD:valid&&!strcmp(kind->valuestring,"replay")?REPLAY:0;
        int duration=type==RECORD?3000:1000;
        if(ms){if(!cJSON_IsNumber(ms)||ms->valuedouble<100||ms->valuedouble>3000||(int)ms->valuedouble!=ms->valuedouble||type==REPLAY)valid=false;
            else duration=(int)ms->valuedouble;}
        cJSON_Delete(j);if(!valid||!type)return failure(req,boot,422,"invalid_audio_job");
        if(!reserve(type,duration))return failure(req,boot,409,"audio_busy_or_no_recording");
        if(xQueueSend(jobs,&current,0)!=pdPASS){abort_upload(ESP_ERR_INVALID_STATE);return failure(req,boot,503,"audio_queue_unavailable");}
        return reply(req,boot,202,snapshot());
    }
    return failure(req,boot,501,"audio_endpoint_not_implemented");
}
