#include "camera_service.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "soc/usb_dwc_struct.h"
#include "esp_jpeg_enc.h"
#include "usb/uvc_host.h"

#define PHOTO_LIMIT (256*1024)
#define FORMAT_LIMIT 48
static SemaphoreHandle_t guard,frame_ready;
static QueueHandle_t commands,connections;
static usb_host_client_handle_t observer;
static bool host_started,busy,capturing,connected;
static esp_err_t last_error;
static uint8_t address,stream_index;
static uint16_t vid,pid;
static uvc_host_frame_info_t formats[FORMAT_LIMIT];
static size_t format_count,photo_size;
static size_t captured_size;
static enum uvc_host_stream_format captured_format;
static uint8_t *photo;
static unsigned photo_width,photo_height,photo_sequence;
static int64_t photo_us;
static const char *phase="disabled";
// Keep USB diagnostics reachable over Wi-Fi after the shared USB PHY leaves J12.
// Only USB driver lines are retained; cloud, Wi-Fi and authentication logs are excluded.
#define USB_LOG_LINES 8
#define USB_LOG_LENGTH 192
static portMUX_TYPE diagnostic_mux=portMUX_INITIALIZER_UNLOCKED;
static char usb_logs[USB_LOG_LINES][USB_LOG_LENGTH];
static unsigned usb_log_count,usb_enum_failures;
static vprintf_like_t previous_log;
static uint32_t root_register;
static unsigned root_samples,root_changes,root_connect_samples,observer_events;
static bool root_was_connected;
static int diagnostic_log(const char *format,va_list args)
{
    char line[USB_LOG_LENGTH];va_list copy;va_copy(copy,args);
    vsnprintf(line,sizeof(line),format,copy);va_end(copy);
    if(strstr(line,"USB HOST:")||strstr(line,"USBH:")||strstr(line,"HUB:")||
       strstr(line,"ENUM:")||strstr(line,"HCD DWC:")||strstr(line,"usb_phy:")||
       strstr(line,"uvc:")||strstr(line,"uvc-isoc:")||strstr(line,"uvc-bulk:")||
       strstr(line,"uvc-frame:")||strstr(line,"uvc-control:")){
        portENTER_CRITICAL(&diagnostic_mux);
        memcpy(usb_logs[usb_log_count%USB_LOG_LINES],line,sizeof(line));++usb_log_count;
        if(strstr(line,"ENUM:")&&(strstr(line,"FAILED")||strstr(line,"larger than")))++usb_enum_failures;
        portEXIT_CRITICAL(&diagnostic_mux);
    }
    return previous_log?previous_log(format,args):vprintf(format,args);
}
static void sample_root_port(void)
{
    // Read-only HPRT snapshot: never acknowledge interrupts or modify the USB driver.
    usb_dwc_hprt_reg_t port;port.val=USB_DWC.hprt_reg.val;
    xSemaphoreTake(guard,portMAX_DELAY);
    root_register=port.val;++root_samples;
    if(port.prtconnsts)++root_connect_samples;
    if((bool)port.prtconnsts!=root_was_connected){++root_changes;root_was_connected=port.prtconnsts;}
    xSemaphoreGive(guard);
}
static const char *format_name(enum uvc_host_stream_format f)
{
    switch(f){case UVC_VS_FORMAT_MJPEG:return "MJPEG";case UVC_VS_FORMAT_YUY2:return "YUY2";
    case UVC_VS_FORMAT_H264:return "H264";case UVC_VS_FORMAT_H265:return "H265";
    case UVC_VS_FORMAT_NV12:return "NV12";default:return "unknown";}
}
static void driver_event(const uvc_host_driver_event_data_t *e,void *ctx)
{
    (void)ctx;
    if(e->type==UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED)xQueueSend(connections,e,0);
}
static void observer_event(const usb_host_client_event_msg_t *e,void *ctx)
{
    (void)e;(void)ctx;
    xSemaphoreTake(guard,portMAX_DELAY);++observer_events;xSemaphoreGive(guard);
}
static void stream_event(const uvc_host_stream_event_data_t *e,void *ctx)
{
    (void)ctx;
    if(e->type==UVC_HOST_DEVICE_DISCONNECTED||e->type==UVC_HOST_TRANSFER_ERROR){
        xSemaphoreTake(guard,portMAX_DELAY);
        last_error=e->type==UVC_HOST_DEVICE_DISCONNECTED?ESP_ERR_NOT_FOUND:ESP_FAIL;
        capturing=false;
        xSemaphoreGive(guard);xSemaphoreGive(frame_ready);
    }
}
static bool frame_received(const uvc_host_frame_t *f,void *ctx)
{
    (void)ctx;
    xSemaphoreTake(guard,portMAX_DELAY);
    bool jpeg=f->vs_format.format==UVC_VS_FORMAT_MJPEG&&f->data_len>=4&&
        f->data[0]==0xff&&f->data[1]==0xd8&&f->data[f->data_len-2]==0xff&&f->data[f->data_len-1]==0xd9;
    bool yuy2=f->vs_format.format==UVC_VS_FORMAT_YUY2&&f->data_len==f->vs_format.h_res*f->vs_format.v_res*2;
    if(capturing&&f->data_len<=PHOTO_LIMIT&&(jpeg||yuy2)){
        memcpy(photo,f->data,f->data_len);captured_size=f->data_len;captured_format=f->vs_format.format;
        photo_width=f->vs_format.h_res;photo_height=f->vs_format.v_res;
        photo_us=esp_timer_get_time();capturing=false;
        xSemaphoreGive(frame_ready);
    }
    xSemaphoreGive(guard);return true;
}
static void host_events(void *arg)
{
    (void)arg;
    for(;;){uint32_t flags=0;usb_host_lib_handle_events(portMAX_DELAY,&flags);
        if(flags&USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)usb_host_device_free_all();}
}
static esp_err_t start_host(void)
{
    // Explicitly requested over Wi-Fi only: the internal PHY is shared with J12.
    previous_log=esp_log_set_vprintf(diagnostic_log);
    usb_host_config_t cfg={.intr_flags=ESP_INTR_FLAG_LEVEL1};
    esp_err_t err=usb_host_install(&cfg);if(err!=ESP_OK)return err;
    if(xTaskCreate(host_events,"camera_usb",4096,NULL,5,NULL)!=pdPASS){usb_host_uninstall();return ESP_ERR_NO_MEM;}
    usb_host_client_config_t client={.max_num_event_msg=5,.async={.client_event_callback=observer_event}};
    err=usb_host_client_register(&client,&observer);if(err!=ESP_OK)return err;
    uvc_host_driver_config_t uvc={.driver_task_stack_size=6144,.driver_task_priority=5,
        .xCoreID=tskNO_AFFINITY,.create_background_task=true,.event_cb=driver_event};
    return uvc_host_install(&uvc);
}
static void enumerate_device(void)
{
    uint8_t addresses[1];int count=0;
    if(usb_host_device_addr_list_fill(1,addresses,&count)!=ESP_OK)return;
    xSemaphoreTake(guard,portMAX_DELAY);
    bool changed=(count>0)!=(connected)||(count&&address!=addresses[0]);
    if(changed){
        connected=count>0;address=count?addresses[0]:0;format_count=0;vid=pid=0;photo_size=0;
        phase=count?"enumerating":"waiting_device";
    }
    xSemaphoreGive(guard);
    if(count&&changed){
        usb_device_handle_t dev;
        if(usb_host_device_open(observer,addresses[0],&dev)==ESP_OK){
            const usb_device_desc_t *desc;
            if(usb_host_get_device_descriptor(dev,&desc)==ESP_OK){
                xSemaphoreTake(guard,portMAX_DELAY);vid=desc->idVendor;pid=desc->idProduct;xSemaphoreGive(guard);
            }
            usb_host_device_close(observer,dev);
        }
    }
    uvc_host_driver_event_data_t e;
    while(xQueueReceive(connections,&e,0)==pdTRUE){
        uvc_host_frame_info_t list[FORMAT_LIMIT];size_t n=FORMAT_LIMIT;
        esp_err_t err=uvc_host_get_frame_list(e.device_connected.dev_addr,e.device_connected.uvc_stream_index,&list,&n);
        xSemaphoreTake(guard,portMAX_DELAY);
        if(err==ESP_OK){
            address=e.device_connected.dev_addr;stream_index=e.device_connected.uvc_stream_index;
            format_count=n;memcpy(formats,list,n*sizeof(*list));phase="ready";
        }else{last_error=err;phase="descriptor_error";}
        xSemaphoreGive(guard);
    }
}
static esp_err_t encode_photo(void)
{
    size_t encoded_size=captured_size;
    if(captured_format==UVC_VS_FORMAT_YUY2){
        jpeg_enc_config_t cfg=DEFAULT_JPEG_ENC_CONFIG();
        cfg.width=photo_width;cfg.height=photo_height;cfg.src_type=JPEG_PIXEL_FORMAT_YCbYCr;
        cfg.subsampling=JPEG_SUBSAMPLE_422;cfg.quality=75;cfg.task_enable=false;
        jpeg_enc_handle_t encoder=NULL;
        jpeg_error_t result=jpeg_enc_open(&cfg,&encoder);
        if(result!=JPEG_ERR_OK)return result==JPEG_ERR_NO_MEM?ESP_ERR_NO_MEM:ESP_FAIL;
        uint8_t *encoded=heap_caps_aligned_alloc(16,PHOTO_LIMIT,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!encoded){jpeg_enc_close(encoder);return ESP_ERR_NO_MEM;}
        int length=0;
        result=jpeg_enc_process(encoder,photo,captured_size,encoded,PHOTO_LIMIT,&length);
        jpeg_enc_close(encoder);
        if(result==JPEG_ERR_OK&&length>=4&&length<=PHOTO_LIMIT){memcpy(photo,encoded,length);encoded_size=length;}
        else encoded_size=0;
        free(encoded);if(!encoded_size)return ESP_FAIL;
    }
    xSemaphoreTake(guard,portMAX_DELAY);photo_size=encoded_size;++photo_sequence;xSemaphoreGive(guard);
    return ESP_OK;
}
static esp_err_t take_photo(void)
{
    uvc_host_stream_config_t cfg={.event_cb=stream_event,.frame_cb=frame_received,
        .advanced={.number_of_frame_buffers=2,.frame_size=PHOTO_LIMIT,.frame_heap_caps=MALLOC_CAP_SPIRAM,
            .number_of_urbs=3,.urb_size=0}};
    xSemaphoreTake(guard,portMAX_DELAY);
    unsigned best=~0u;int chosen=-1;
    for(size_t i=0;i<format_count;++i){
        unsigned pixels=formats[i].h_res*formats[i].v_res;
        bool jpeg=formats[i].format==UVC_VS_FORMAT_MJPEG;
        bool yuy2=formats[i].format==UVC_VS_FORMAT_YUY2&&pixels*2<=PHOTO_LIMIT;
        unsigned rank=pixels+(jpeg?0:10000000);
        if((jpeg||yuy2)&&formats[i].h_res<=640&&formats[i].v_res<=480&&rank<best){best=rank;chosen=i;}
    }
    photo_size=0;captured_size=0;
    if(chosen<0){xSemaphoreGive(guard);return ESP_ERR_NOT_SUPPORTED;}
    cfg.usb.dev_addr=address;cfg.usb.uvc_stream_index=stream_index;
    uvc_host_frame_info_t f=formats[chosen];
    cfg.vs_format=(uvc_host_stream_format_t){.h_res=f.h_res,.v_res=f.v_res,.format=f.format,.fps=0};
    // Prefer the slowest advertised interval, lowering bandwidth on full-speed S3.
    uint32_t interval=f.interval_type?f.interval[f.interval_type>CONFIG_UVC_INTERVAL_ARRAY_SIZE?CONFIG_UVC_INTERVAL_ARRAY_SIZE-1:f.interval_type-1]:f.interval_max;
    if(interval)cfg.vs_format.fps=10000000.0f/interval;
    if(!photo)photo=heap_caps_aligned_alloc(16,PHOTO_LIMIT,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    bool allocated=photo!=NULL;xSemaphoreGive(guard);
    if(!allocated)return ESP_ERR_NO_MEM;
    uvc_host_stream_hdl_t stream=NULL;
    esp_err_t err=uvc_host_stream_open(&cfg,pdMS_TO_TICKS(1500),&stream);
    if(err!=ESP_OK)return err;
    xSemaphoreTake(frame_ready,0);
    xSemaphoreTake(guard,portMAX_DELAY);capturing=true;xSemaphoreGive(guard);
    err=uvc_host_stream_start(stream);
    if(err==ESP_OK){
        if(xSemaphoreTake(frame_ready,pdMS_TO_TICKS(6000))!=pdTRUE)err=ESP_ERR_TIMEOUT;
        xSemaphoreTake(guard,portMAX_DELAY);
        if(err==ESP_OK&&!captured_size)err=last_error?last_error:ESP_FAIL;
        xSemaphoreGive(guard);
        esp_err_t stop=uvc_host_stream_stop(stream);if(err==ESP_OK)err=stop;
    }
    xSemaphoreTake(guard,portMAX_DELAY);capturing=false;xSemaphoreGive(guard);
    esp_err_t close=uvc_host_stream_close(stream);if(err==ESP_OK)err=close;
    if(err==ESP_OK)err=encode_photo();
    return err;
}
static void camera_task(void *arg)
{
    (void)arg;
    for(;;){
        int command;
        if(xQueueReceive(commands,&command,pdMS_TO_TICKS(30))==pdTRUE){
            esp_err_t err=ESP_OK;
            if(command==1)err=start_host();
            else err=take_photo();
            xSemaphoreTake(guard,portMAX_DELAY);last_error=err;busy=false;
            if(command==1)host_started=err==ESP_OK;
            phase=err!=ESP_OK?"error":command==1?"waiting_device":"photo_ready";
            xSemaphoreGive(guard);
        }
        if(host_started){sample_root_port();usb_host_client_handle_events(observer,0);enumerate_device();}
    }
}
void camera_service_init(void)
{
    guard=xSemaphoreCreateMutex();frame_ready=xSemaphoreCreateBinary();
    commands=xQueueCreate(1,sizeof(int));connections=xQueueCreate(4,sizeof(uvc_host_driver_event_data_t));
    assert(guard&&frame_ready&&commands&&connections);
    // USB interrupt allocation belongs to the installing core; keep it off the Wi-Fi core.
    assert(xTaskCreatePinnedToCore(camera_task,"camera",8192,NULL,3,NULL,1)==pdPASS);
}
bool camera_service_command(const char *command)
{
    int cmd=!strcmp(command,"start")?1:!strcmp(command,"capture")?2:0;
    xSemaphoreTake(guard,portMAX_DELAY);
    if(cmd==1&&host_started){xSemaphoreGive(guard);return true;}
    bool accepted=cmd&&!busy&&((cmd==1&&!host_started&&!strcmp(phase,"disabled"))||(cmd==2&&host_started&&connected&&format_count));
    if(accepted){busy=true;phase=cmd==1?"starting":"capturing";last_error=ESP_OK;if(cmd==2)photo_size=0;
        accepted=xQueueSend(commands,&cmd,0)==pdTRUE;if(!accepted)busy=false;}
    xSemaphoreGive(guard);return accepted;
}
cJSON *camera_service_state(void)
{
    cJSON *j=cJSON_CreateObject();xSemaphoreTake(guard,portMAX_DELAY);
    unsigned enum_failures;
    portENTER_CRITICAL(&diagnostic_mux);enum_failures=usb_enum_failures;portEXIT_CRITICAL(&diagnostic_mux);
    cJSON_AddStringToObject(j,"phase",host_started&&!connected&&enum_failures?"enumeration_failed":phase);
    cJSON_AddBoolToObject(j,"host_started",host_started);
    cJSON_AddBoolToObject(j,"busy",busy);cJSON_AddBoolToObject(j,"connected",connected);
    cJSON_AddBoolToObject(j,"streaming",capturing);cJSON_AddBoolToObject(j,"cloud_upload",false);
    cJSON_AddNumberToObject(j,"vid",vid);cJSON_AddNumberToObject(j,"pid",pid);
    cJSON_AddNumberToObject(j,"last_error",last_error);cJSON_AddStringToObject(j,"error_name",esp_err_to_name(last_error));
    cJSON_AddNumberToObject(j,"photo_bytes",photo_size);cJSON_AddNumberToObject(j,"photo_sequence",photo_sequence);
    cJSON_AddNumberToObject(j,"photo_uptime_ms",photo_us/1000);
    cJSON_AddNumberToObject(j,"photo_width",photo_width);cJSON_AddNumberToObject(j,"photo_height",photo_height);
    cJSON_AddStringToObject(j,"capture_format",format_name(captured_format));
    cJSON_AddStringToObject(j,"photo_content_type","image/jpeg");
    usb_dwc_hprt_reg_t port;port.val=root_register;
    cJSON *diag=cJSON_AddObjectToObject(j,"usb_diagnostics");
    cJSON_AddNumberToObject(diag,"root_register",root_register);
    cJSON_AddBoolToObject(diag,"port_connected",port.prtconnsts);
    cJSON_AddBoolToObject(diag,"port_enabled",port.prtena);
    cJSON_AddBoolToObject(diag,"port_power_bit",port.prtpwr);
    cJSON_AddBoolToObject(diag,"voltage_measured",false);
    cJSON_AddNumberToObject(diag,"line_state",port.prtlnsts);
    cJSON_AddNumberToObject(diag,"speed_bits",port.prtspd);
    cJSON_AddNumberToObject(diag,"samples",root_samples);
    cJSON_AddNumberToObject(diag,"connected_samples",root_connect_samples);
    cJSON_AddNumberToObject(diag,"connection_changes",root_changes);
    cJSON_AddNumberToObject(diag,"device_events",observer_events);
    cJSON_AddNumberToObject(diag,"enumeration_errors",enum_failures);
    char logs[USB_LOG_LINES][USB_LOG_LENGTH];unsigned log_count;
    portENTER_CRITICAL(&diagnostic_mux);
    log_count=usb_log_count;memcpy(logs,usb_logs,sizeof(logs));
    portEXIT_CRITICAL(&diagnostic_mux);
    cJSON *log=cJSON_AddArrayToObject(diag,"logs");
    unsigned begin=log_count>USB_LOG_LINES?log_count-USB_LOG_LINES:0;
    for(unsigned i=begin;i<log_count;++i)cJSON_AddItemToArray(log,cJSON_CreateString(logs[i%USB_LOG_LINES]));
    cJSON *a=cJSON_AddArrayToObject(j,"formats");
    for(size_t i=0;i<format_count;++i){cJSON *f=cJSON_CreateObject();
        cJSON_AddStringToObject(f,"format",format_name(formats[i].format));
        cJSON_AddNumberToObject(f,"width",formats[i].h_res);cJSON_AddNumberToObject(f,"height",formats[i].v_res);
        cJSON_AddNumberToObject(f,"default_interval",formats[i].default_interval);cJSON_AddItemToArray(a,f);}
    xSemaphoreGive(guard);return j;
}
esp_err_t camera_service_photo(uint8_t **data,size_t *length)
{
    xSemaphoreTake(guard,portMAX_DELAY);*length=photo_size;*data=NULL;
    if(photo_size){*data=heap_caps_malloc(photo_size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(*data)memcpy(*data,photo,photo_size);}
    xSemaphoreGive(guard);
    return !*length?ESP_ERR_NOT_FOUND:!*data?ESP_ERR_NO_MEM:ESP_OK;
}
