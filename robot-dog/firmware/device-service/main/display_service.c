#include "display_service.h"
#include "display_labels.h"
#include "cloud_dialog.h"
#include "camera_service.h"
#include "ota_service.h"
#include "ble_service.h"
#include "board_i2c.h"
#include "dog_face.h"
#include "face_ui.h"
#include "interaction.h"
#include "user_settings.h"
#include "motion_bridge.h"
#include "rtc_service.h"
#include "esp_lcd_touch_ft5x06.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#define WIDTH 320
#define HEIGHT 240
#define STRIP 16
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t transferred,guard;
static uint16_t *canvas,*strip;
static bool initialized;
static esp_err_t last_error;
static unsigned refreshes;
static esp_lcd_touch_handle_t touch;
static esp_err_t touch_error;
static unsigned touch_reads,touch_points;
static int touch_x,touch_y;
static face_ui ui;
static dog_face_state expression;
static dog_face_emotion emotion;
static unsigned temporary_error;
static uint32_t temporary_until;
static const char *last_touch_action="none";
static uint16_t rgb(unsigned r,unsigned g,unsigned b)
{
    uint16_t c=((r>>3)<<11)|((g>>2)<<5)|(b>>3);return (c>>8)|(c<<8);
}
static void rect(int x,int y,int w,int h,uint16_t color)
{
    for(int j=y;j<y+h&&j<HEIGHT;++j)for(int i=x;i<x+w&&i<WIDTH;++i)
        if(i>=0&&j>=0)canvas[j*WIDTH+i]=color;
}
static void label(unsigned index,int x,int y,uint16_t color)
{
    const display_label_t *l=&display_labels[index];
    if(x<0)x=(WIDTH-l->width)/2;
    for(int j=0;j<l->height;++j)for(int i=0;i<l->width;++i)
        if(l->bits[j*l->stride+i/8]&(0x80>>(i%8)))rect(x+i,y+j,1,1,color);
}
static bool finished(esp_lcd_panel_io_handle_t io,esp_lcd_panel_io_event_data_t *e,void *ctx)
{
    (void)io;(void)e;(void)ctx;BaseType_t wake=pdFALSE;
    xSemaphoreGiveFromISR(transferred,&wake);return wake==pdTRUE;
}
static esp_err_t setup(void)
{
    transferred=xSemaphoreCreateBinary();if(!transferred)return ESP_ERR_NO_MEM;
    canvas=heap_caps_malloc(WIDTH*HEIGHT*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    strip=heap_caps_malloc(WIDTH*STRIP*2,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    if(!canvas||!strip)return ESP_ERR_NO_MEM;
    gpio_config_t backlight={.pin_bit_mask=1ULL<<10,.mode=GPIO_MODE_OUTPUT};
    esp_err_t err=gpio_config(&backlight);if(err!=ESP_OK)return err;
    gpio_set_level(10,0);
    spi_bus_config_t bus={.sclk_io_num=18,.mosi_io_num=3,.miso_io_num=8,
        .quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=WIDTH*STRIP*2};
    err=spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO);if(err!=ESP_OK)return err;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t cfg={.cs_gpio_num=-1,.dc_gpio_num=17,.spi_mode=0,
        .pclk_hz=20000000,.trans_queue_depth=1,.lcd_cmd_bits=8,.lcd_param_bits=8,
        .on_color_trans_done=finished};
    err=esp_lcd_new_panel_io_spi(SPI2_HOST,&cfg,&io);if(err!=ESP_OK)return err;
    esp_lcd_panel_dev_config_t dev={.reset_gpio_num=9,.rgb_ele_order=LCD_RGB_ELEMENT_ORDER_BGR,.bits_per_pixel=16};
    err=esp_lcd_new_panel_ili9341(io,&dev,&panel);if(err!=ESP_OK)return err;
    if((err=esp_lcd_panel_reset(panel))!=ESP_OK)return err;
    if((err=esp_lcd_panel_init(panel))!=ESP_OK)return err;
    const uint8_t positive[]={0x0f,0x2a,0x28,0x08,0x0e,0x08,0x54,0xa9,0x43,0x0a,0x0f,0,0,0,0};
    const uint8_t negative[]={0,0x15,0x17,0x07,0x11,0x06,0x2b,0x56,0x3c,0x05,0x10,0x0f,0x3f,0x3f,0x0f};
    if((err=esp_lcd_panel_io_tx_param(io,0xe0,positive,sizeof(positive)))!=ESP_OK)return err;
    if((err=esp_lcd_panel_io_tx_param(io,0xe1,negative,sizeof(negative)))!=ESP_OK)return err;
    if((err=esp_lcd_panel_invert_color(panel,true))!=ESP_OK)return err;
    if((err=esp_lcd_panel_swap_xy(panel,true))!=ESP_OK)return err;
    if((err=esp_lcd_panel_mirror(panel,true,true))!=ESP_OK)return err;
    return esp_lcd_panel_disp_on_off(panel,true);
}
static esp_err_t flush(void)
{
    for(int y=0;y<HEIGHT;y+=STRIP){
        memcpy(strip,canvas+y*WIDTH,WIDTH*STRIP*2);
        esp_err_t err=esp_lcd_panel_draw_bitmap(panel,0,y,WIDTH,y+STRIP,strip);
        if(err!=ESP_OK)return err;
        if(xSemaphoreTake(transferred,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
static bool pending(uint32_t now,uint32_t until){return until&&(int32_t)(until-now)>0;}
static void touch_action(face_ui_action action)
{
    if(ota_service_busy()&&(action==FACE_UI_LISTEN||action==FACE_UI_REST||action==FACE_UI_QUIETER||action==FACE_UI_LOUDER||action==FACE_UI_PAIR))return;
    bool accepted=true;const char *name="none";
    if(action==FACE_UI_LISTEN){
        cloud_display_snapshot d;cloud_dialog_display_snapshot(&d);
        accepted=cloud_dialog_command(d.listening?"stop":"start",NULL,false);name="listening_toggle";
    }else if(action==FACE_UI_REST){
        accepted=cloud_dialog_change_mode(interaction_mode()==PAVLOV_REST?"companion":"rest")==ESP_OK;name="rest_toggle";
    }else if(action==FACE_UI_QUIETER||action==FACE_UI_LOUDER){
        int volume=user_settings_get().volume+(action==FACE_UI_LOUDER?10:-10);
        if(volume<0)volume=0;
        if(volume>100)volume=100;
        cJSON *patch=cJSON_CreateObject();cJSON_AddNumberToObject(patch,"volume",volume);
        accepted=user_settings_update(patch)==ESP_OK;cJSON_Delete(patch);name="volume";
    }else if(action==FACE_UI_PAIR){accepted=ble_service_pair_window();name="ble_pairing";}
    else if(action==FACE_UI_PAT)name="pat";
    else if(action==FACE_UI_STATUS)name="status_peek";
    else if(action==FACE_UI_CLOSE)name="menu_close";
    if(action!=FACE_UI_NONE){
        xSemaphoreTake(guard,portMAX_DELAY);last_touch_action=name;
        if(!accepted){temporary_error=25;temporary_until=esp_timer_get_time()/1000+4000;}
        xSemaphoreGive(guard);
    }
}
static void touch_task(void *arg)
{
    face_ui confirmation={0};bool confirming=false;
    (void)arg;
    for(;;){
        esp_err_t err=esp_lcd_touch_read_data(touch);
        if(err==ESP_OK){
            esp_lcd_touch_point_data_t point={0};uint8_t count=0;
            esp_err_t read=esp_lcd_touch_get_data(touch,&point,&count,1);
            bool pressed=read==ESP_OK&&count>0;uint16_t x=point.x,y=point.y;
            xSemaphoreTake(guard,portMAX_DELAY);touch_error=ESP_OK;++touch_reads;
            if(pressed){touch_x=x;touch_y=y;++touch_points;}
            uint32_t now=esp_timer_get_time()/1000;
            xSemaphoreGive(guard);dog_ble_snapshot b;ble_service_snapshot(&b);xSemaphoreTake(guard,portMAX_DELAY);
            face_ui_action action=FACE_UI_NONE;
            if(b.pending){
                if(!confirming){confirmation=(face_ui){.menu=true,.down=pressed,.long_fired=pressed};ui.down=false;confirming=true;}
                confirmation.menu_at=now;action=face_ui_update(&confirmation,pressed,x,y,now);
            }else{confirming=false;action=face_ui_update(&ui,pressed,x,y,now);}
            xSemaphoreGive(guard);
            if(b.pending){if(action==FACE_UI_QUIETER)ble_service_confirm(true);else if(action==FACE_UI_LOUDER||action==FACE_UI_CLOSE||action==FACE_UI_PAIR)ble_service_confirm(false);}
            else touch_action(action);
        }else{xSemaphoreTake(guard,portMAX_DELAY);touch_error=err;xSemaphoreGive(guard);}
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
static esp_err_t setup_touch(void)
{
    esp_lcd_panel_io_handle_t io=NULL;
    esp_lcd_panel_io_i2c_config_t cfg=ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    esp_err_t err=esp_lcd_new_panel_io_i2c_v2(board_i2c_bus(),&cfg,&io);if(err!=ESP_OK)return err;
    esp_lcd_touch_config_t tp={.x_max=240,.y_max=320,.rst_gpio_num=GPIO_NUM_16,.int_gpio_num=GPIO_NUM_NC,
        .levels={.reset=0,.interrupt=0},.flags={.swap_xy=1,.mirror_y=1}};
    err=esp_lcd_touch_new_i2c_ft5x06(io,&tp,&touch);
    if(err!=ESP_OK){esp_lcd_panel_io_del(io);return err;}
    // Creation alone isn't proof of a responsive touch controller.
    err=esp_lcd_touch_read_data(touch);if(err!=ESP_OK)return err;
    return xTaskCreate(touch_task,"face_touch",6144,NULL,3,NULL)==pdPASS?ESP_OK:ESP_ERR_NO_MEM;
}
static uint16_t status_color(dog_face_state state)
{
    switch(state){case DOG_FACE_LISTEN:return rgb(97,225,170);case DOG_FACE_THINK:case DOG_FACE_RECOGNIZE:return rgb(190,155,250);
    case DOG_FACE_SPEAK:return rgb(255,208,113);case DOG_FACE_CAMERA:return rgb(112,219,248);
    case DOG_FACE_SLEEP:return rgb(124,149,182);case DOG_FACE_MOTION:return rgb(245,166,109);
    case DOG_FACE_OFFLINE:return rgb(245,110,111);default:return rgb(99,169,219);}
}
static void button(int x,int y,int w,unsigned text)
{
    rect(x,y,w,52,rgb(29,49,65));
    const display_label_t *l=&display_labels[text];label(text,x+(w-l->width)/2,y+(52-l->height)/2,rgb(226,238,249));
}
static void clock_detail(void)
{
    dog_rtc_date local;bool available;
    if(!rtc_service_local_time(&local,&available)){label(available?DISPLAY_LABEL_RTC_INVALID:DISPLAY_LABEL_RTC_UNAVAILABLE,-1,38,rgb(226,238,249));return;}
    char text[6];snprintf(text,sizeof(text),"%02u:%02u",local.hour%24,local.minute%60);
    int width=display_labels[DISPLAY_LABEL_RTC_ZONE].width+12;
    for(unsigned i=0;i<5;i++)width+=display_labels[text[i]==':'?DISPLAY_LABEL_RTC_COLON:138+(unsigned)(text[i]-'0')].width+2;
    int x=(WIDTH-width)/2;label(DISPLAY_LABEL_RTC_ZONE,x,38,rgb(156,186,204));x+=display_labels[DISPLAY_LABEL_RTC_ZONE].width+12;
    for(unsigned i=0;i<5;i++){unsigned n=text[i]==':'?DISPLAY_LABEL_RTC_COLON:138+(unsigned)(text[i]-'0');label(n,x,38,rgb(226,238,249));x+=display_labels[n].width+2;}
}
static void display_task(void *arg)
{
    (void)arg;esp_err_t err=setup();
    xSemaphoreTake(guard,portMAX_DELAY);last_error=err;initialized=err==ESP_OK;xSemaphoreGive(guard);
    if(err!=ESP_OK){ESP_LOGE("DOG_DISPLAY","init: %s",esp_err_to_name(err));vTaskDelete(NULL);return;}
    esp_err_t tp=setup_touch();
    xSemaphoreTake(guard,portMAX_DELAY);touch_error=tp;xSemaphoreGive(guard);
    unsigned last_turn=0;char last_reply[256]={0};uint32_t feeling_until=0;dog_face_emotion feeling=DOG_EMOTION_CALM;
    for(;;){
        uint32_t now=esp_timer_get_time()/1000;
        cloud_display_snapshot d;cloud_dialog_display_snapshot(&d);
        cJSON *c=camera_service_state();
        bool camera_busy=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"busy"));
        bool host=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"host_started"));
        bool camera_connected=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"connected"));
        cJSON *ce=cJSON_GetObjectItemCaseSensitive(c,"last_error");
        bool camera_error=cJSON_IsNumber(ce)&&ce->valueint;
        cJSON *phase=cJSON_GetObjectItemCaseSensitive(c,"phase");
        if(cJSON_IsString(phase)&&(!strcmp(phase->valuestring,"enumeration_failed")||!strcmp(phase->valuestring,"descriptor_error")))camera_error=true;
        bool rest=interaction_mode()==PAVLOV_REST;
        dog_face_state face=!d.wifi?DOG_FACE_OFFLINE:rest?DOG_FACE_SLEEP:camera_busy?DOG_FACE_CAMERA:
            !strcmp(d.stage,"playing")||!strcmp(d.stage,"timer_alerting")?DOG_FACE_SPEAK:
            !strcmp(d.stage,"thinking")||!strcmp(d.stage,"synthesizing")?DOG_FACE_THINK:
            !strcmp(d.stage,"recognizing")?DOG_FACE_RECOGNIZE:!strcmp(d.stage,"executing_motion")?DOG_FACE_MOTION:
            d.listening?DOG_FACE_LISTEN:DOG_FACE_IDLE;
        if(d.turns!=last_turn||strcmp(last_reply,d.reply)){last_turn=d.turns;snprintf(last_reply,sizeof(last_reply),"%s",d.reply);
            feeling=!strcmp(d.emotion,"happy")?DOG_EMOTION_HAPPY:!strcmp(d.emotion,"curious")?DOG_EMOTION_CURIOUS:
                !strcmp(d.emotion,"puzzled")?DOG_EMOTION_PUZZLED:!strcmp(d.emotion,"low")?DOG_EMOTION_LOW:
                !strcmp(d.emotion,"calm")?DOG_EMOTION_CALM:dog_face_emotion_from_text(d.reply);
            feeling_until=now+6000;}
        face_ui current;unsigned local_error;uint32_t local_until;esp_err_t current_touch_error;
        xSemaphoreTake(guard,portMAX_DELAY);current=ui;local_error=temporary_error;local_until=temporary_until;current_touch_error=touch_error;xSemaphoreGive(guard);
        dog_face_emotion mood=face_ui_patted(&current,now)?DOG_EMOTION_HAPPY:pending(now,feeling_until)?feeling:DOG_EMOTION_CALM;
        dog_posture posture=motion_bridge_posture();
        bool power_low,power_message;motion_bridge_power_warning(&power_low,&power_message);
        bool tipped=posture==DOG_POSTURE_SIDE||posture==DOG_POSTURE_INVERTED;
        bool guard_warning=motion_bridge_pose_protection_warning();
        bool limit_warning=motion_bridge_power_limit_warning();
        if(!tipped&&!face_ui_patted(&current,now)&&!pending(now,feeling_until)&&
           (face==DOG_FACE_IDLE||face==DOG_FACE_LISTEN)&&motion_bridge_handling_hint())mood=DOG_EMOTION_CURIOUS;
        if(tipped){mood=DOG_EMOTION_PUZZLED;if(face==DOG_FACE_SLEEP)face=DOG_FACE_IDLE;}
        if(guard_warning){mood=DOG_EMOTION_PUZZLED;if(face==DOG_FACE_SLEEP)face=DOG_FACE_IDLE;}
        if(limit_warning){mood=DOG_EMOTION_PUZZLED;if(face==DOG_FACE_SLEEP)face=DOG_FACE_IDLE;}
        dog_face_render_emotion(canvas,face,mood,now);
        unsigned message=0;
        if(!d.wifi&&now>12000)message=21;
        else if(camera_error)message=23;
        else if(current_touch_error!=ESP_OK)message=24;
        else if(!strcmp(d.stage,"retry_wait")||!strcmp(d.stage,"memory_unavailable"))message=22;
        else if(tipped)message=posture==DOG_POSTURE_SIDE?DISPLAY_LABEL_POSE_SIDE:DISPLAY_LABEL_POSE_INVERTED;
        else if(guard_warning)message=DISPLAY_LABEL_POSE_GUARD_STOP;
        else if(limit_warning)message=DISPLAY_LABEL_POSE_POWER_LIMIT;
        else if(power_message)message=DISPLAY_LABEL_POSE_POWER_LOW;
        if(pending(now,local_until))message=local_error;
        rect(282,12,24,20,message?rgb(245,110,111):power_low?rgb(255,208,113):status_color(face));
        if(message){rect(0,201,WIDTH,39,rgb(42,25,35));label(message,-1,211,rgb(255,220,222));}
        else if(face_ui_peeking(&current,now)){
            unsigned text=face==DOG_FACE_OFFLINE?1:face==DOG_FACE_SLEEP?12:face==DOG_FACE_CAMERA?10:
                face==DOG_FACE_SPEAK?6:face==DOG_FACE_THINK?5:face==DOG_FACE_RECOGNIZE?26:
                face==DOG_FACE_MOTION?27:d.listening?4:3;
            label(text,-1,207,rgb(226,238,249));
            // A second tiny block reports the camera only while details are requested.
            rect(12,14,12,12,camera_error?rgb(245,110,111):!host?rgb(80,95,112):camera_connected?rgb(97,225,170):rgb(255,208,113));
        }
        dog_ble_snapshot b;ble_service_snapshot(&b);
        if(b.authenticated||b.pairing_window)rect(262,14,12,12,b.authenticated?rgb(100,180,255):rgb(255,208,113));
        if(face_ui_peeking(&current,now))label(b.authenticated?131:b.pairing_window?132:137,-1,178,rgb(100,180,255));
        if(b.pairing_window&&!b.pending&&!current.menu)label(132,-1,178,rgb(100,180,255));
        if(current.menu){
            rect(16,6,288,224,rgb(13,26,40));label(pending(now,local_until)?local_error:20,28,18,rgb(226,238,249));
            int volume=user_settings_get().volume;label(29+volume,230,18,rgb(100,220,246));
            button(24,48,132,d.listening?14:13);button(164,48,132,rest?16:15);
            button(24,108,132,17);button(164,108,132,18);button(24,168,132,130);button(164,168,132,19);
        }
        if(b.pending){
            rect(16,6,288,224,rgb(13,26,40));label(130,-1,20,rgb(100,180,255));label(133,-1,48,rgb(226,238,249));
            char digits[7];snprintf(digits,sizeof(digits),"%06lu",(unsigned long)(b.passkey%1000000));
            for(unsigned n=0;n<6;n++)label(138+digits[n]-'0',100+20*n,78,rgb(226,238,249));
            button(24,108,132,134);button(164,108,132,135);
        }
        /* Details are explicitly requested. Keep the clock above the face even
           when the independent bottom warning reports unavailable Wi-Fi. */
        if(face_ui_peeking(&current,now)&&!current.menu&&!b.pending&&!b.pairing_window)clock_detail();
        cJSON_Delete(c);err=flush();
        xSemaphoreTake(guard,portMAX_DELAY);last_error=err;expression=face;emotion=mood;if(err==ESP_OK)++refreshes;xSemaphoreGive(guard);
        if(err!=ESP_OK){gpio_set_level(10,0);vTaskDelete(NULL);return;}
        gpio_set_level(10,1);vTaskDelay(pdMS_TO_TICKS(70));
    }
}
void display_service_init(void)
{
    guard=xSemaphoreCreateMutex();assert(guard);
    assert(xTaskCreate(display_task,"display",8192,NULL,2,NULL)==pdPASS);
}
cJSON *display_service_state(void)
{
    cJSON *j=cJSON_CreateObject();xSemaphoreTake(guard,portMAX_DELAY);
    cJSON_AddStringToObject(j,"controller","ILI9341");cJSON_AddBoolToObject(j,"initialized",initialized);
    cJSON_AddNumberToObject(j,"width",WIDTH);cJSON_AddNumberToObject(j,"height",HEIGHT);
    cJSON_AddNumberToObject(j,"spi_hz",20000000);cJSON_AddNumberToObject(j,"refreshes",refreshes);
    cJSON_AddNumberToObject(j,"last_error",last_error);cJSON_AddStringToObject(j,"error_name",esp_err_to_name(last_error));
    cJSON_AddStringToObject(j,"expression",dog_face_name(expression));
    cJSON_AddStringToObject(j,"emotion",dog_face_emotion_name(emotion));
    cJSON_AddStringToObject(j,"text_policy","errors_or_requested_details");
    cJSON_AddBoolToObject(j,"touch_initialized",touch!=NULL&&touch_reads>0);
    cJSON_AddNumberToObject(j,"touch_error",touch_error);cJSON_AddNumberToObject(j,"touch_reads",touch_reads);
    cJSON_AddNumberToObject(j,"touch_points",touch_points);cJSON_AddNumberToObject(j,"touch_x",touch_x);cJSON_AddNumberToObject(j,"touch_y",touch_y);
    cJSON_AddNumberToObject(j,"face_pats",ui.pats);cJSON_AddNumberToObject(j,"long_presses",ui.long_presses);
    cJSON_AddBoolToObject(j,"menu_open",ui.menu);cJSON_AddStringToObject(j,"last_touch_action",last_touch_action);
    xSemaphoreGive(guard);return j;
}
