#include "board_controls.h"
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "motion_bridge.h"
#include "cloud_dialog.h"
#include "ota_service.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
static SemaphoreHandle_t guard;
static unsigned esp_presses,stop_requests;
static bool pressed,led_on;
static bool storage_checked,storage_responded;
static esp_err_t storage_error=ESP_OK;
static const char *storage_result="not_checked";
static void controls_task(void *arg)
{
    (void)arg;bool raw=gpio_get_level(GPIO_NUM_2)==0,stable=raw,ready=false;int64_t since=esp_timer_get_time();
    uint32_t stm=motion_bridge_user_button_events();
    for(;;){
        int64_t now=esp_timer_get_time();bool down=gpio_get_level(GPIO_NUM_2)==0,local_event=false;
        if(down!=raw){raw=down;since=now;}
        if(now-since>=40000){if(!raw)ready=true;if(stable!=raw){stable=raw;local_event=stable&&ready;}}
        uint32_t n=motion_bridge_user_button_events();bool remote_event=n!=stm;stm=n;
        if(local_event||remote_event){
            motion_bridge_stop();cloud_dialog_command("stop",NULL,false);
            xSemaphoreTake(guard,portMAX_DELAY);if(local_event)++esp_presses;++stop_requests;xSemaphoreGive(guard);
        }
        cloud_display_snapshot d;cloud_dialog_display_snapshot(&d);
        bool light=ota_service_busy()?now%200000<100000:d.wifi?now%2000000<100000:now%1000000<100000;
        gpio_set_level(GPIO_NUM_11,light);
        xSemaphoreTake(guard,portMAX_DELAY);pressed=stable;led_on=light;xSemaphoreGive(guard);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
void board_controls_init(void)
{
    guard=xSemaphoreCreateMutex();assert(guard);
    gpio_config_t led={.pin_bit_mask=1ULL<<11,.mode=GPIO_MODE_OUTPUT};ESP_ERROR_CHECK(gpio_config(&led));gpio_set_level(GPIO_NUM_11,0);
    gpio_config_t button={.pin_bit_mask=1ULL<<2,.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};ESP_ERROR_CHECK(gpio_config(&button));
    assert(xTaskCreate(controls_task,"board_controls",4096,NULL,3,NULL)==pdPASS);
}
cJSON *board_controls_state(void)
{
    cJSON *j=cJSON_CreateObject();xSemaphoreTake(guard,portMAX_DELAY);
    cJSON_AddNumberToObject(j,"esp_button_presses",esp_presses);cJSON_AddBoolToObject(j,"esp_button_pressed",pressed);
    cJSON_AddNumberToObject(j,"stop_requests",stop_requests);cJSON_AddBoolToObject(j,"esp_led_on",led_on);
    cJSON *sd=cJSON_AddObjectToObject(j,"esp_sd");cJSON_AddBoolToObject(sd,"checked",storage_checked);cJSON_AddBoolToObject(sd,"responded",storage_responded);
    cJSON_AddStringToObject(sd,"result",storage_result);cJSON_AddStringToObject(sd,"error",esp_err_to_name(storage_error));
    cJSON_AddBoolToObject(sd,"mounted",false);cJSON_AddBoolToObject(sd,"files_written",false);xSemaphoreGive(guard);
    cJSON_AddStringToObject(j,"button_action","pause_listening_and_request_stop_hold_pwm");
    cJSON_AddBoolToObject(j,"physical_button_verified",false);cJSON_AddBoolToObject(j,"physical_stop_verified",false);
    cJSON_AddItemToObject(j,"stm32",motion_bridge_aux_state());cJSON_AddItemToObject(j,"rtc",motion_bridge_rtc_state());cJSON_AddItemToObject(j,"eeprom",motion_bridge_eeprom_state());return j;
}
cJSON *board_controls_probe_storage(int *status)
{
    const char *blocked=motion_bridge_ota_reserve();
    if(blocked){*status=409;cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"error",blocked);return j;}
    spi_bus_config_t bus={.mosi_io_num=4,.miso_io_num=14,.sclk_io_num=47,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=512};
    bool bus_ready=false,host_ready=false,device_ready=false;sdspi_dev_handle_t device=0;
    esp_err_t err=spi_bus_initialize(SPI3_HOST,&bus,SPI_DMA_CH_AUTO);
    if(err==ESP_OK){bus_ready=true;err=sdspi_host_init();}
    if(err==ESP_OK){host_ready=true;sdspi_device_config_t cfg=SDSPI_DEVICE_CONFIG_DEFAULT();cfg.host_id=SPI3_HOST;cfg.gpio_cs=6;err=sdspi_host_init_device(&cfg,&device);}
    if(err==ESP_OK){
        device_ready=true;sdmmc_host_t host=SDSPI_HOST_DEFAULT();host.slot=device;host.max_freq_khz=SDMMC_FREQ_PROBING;host.command_timeout_ms=300;
        sdmmc_card_t card;err=sdmmc_card_init(&host,&card);
    }
    if(device_ready)sdspi_host_remove_device(device);
    if(host_ready)sdspi_host_deinit();
    if(bus_ready)spi_bus_free(SPI3_HOST);
    xSemaphoreTake(guard,portMAX_DELAY);storage_checked=true;storage_error=err;storage_responded=err==ESP_OK;
    storage_result=err==ESP_OK?"card_responded_not_mounted":err==ESP_ERR_TIMEOUT||err==ESP_ERR_NOT_FOUND?"no_card_response_storage_skipped":"probe_error_presence_unconfirmed";
    xSemaphoreGive(guard);motion_bridge_ota_release();*status=200;return board_controls_state();
}
