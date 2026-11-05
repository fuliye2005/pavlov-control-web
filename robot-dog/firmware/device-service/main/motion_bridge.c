#include "motion_bridge.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "board_profile.h"
#include "../../shared/action_catalog.h"
#include "../../shared/rtc_calendar.h"
#include "../../shared/supply_profile.h"
#include "../../shared/sensor_profile.h"
#include "../../shared/pose_geometry.h"
#include "../../shared/power_monitor.h"
#include "../../shared/power_history.h"
#include "../../shared/compass_math.h"
#include "../../shared/handling_policy.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "wifi_provision.h"

#define HISTORY 16
typedef enum { EMPTY, RECEIVED, ACCEPTED, RUNNING, FINISHED, FAILED, CANCELLED, UNKNOWN } result_t;
typedef enum { IDLE, SYNC, BASE, ARM, COMMAND, EXECUTING } phase_t;
typedef struct {
    char id[49], action[24], error[80], profile[16];
    int argument, channel, speed, strength;
    uint32_t ttl_ms;
    uint32_t ble_session;
    int64_t received, finished;
    result_t result;
} request_t;
static SemaphoreHandle_t mutex;
static request_t history[HISTORY];
static unsigned cursor;
static int active = -1;
static bool ota_reserved;
static uint32_t user_button_events;
static int64_t aux_at,last_aux_poll;
static unsigned aux_pressed,aux_beep,aux_led,aux_sd,aux_probed,aux_eeprom,aux_rtc;
static dog_rtc_date rtc_date;
static int64_t rtc_at,last_rtc_poll,rtc_deadline;
static bool rtc_available,rtc_valid,rtc_pending;
static bool controller_uart_requested,controller_uart_owned,controller_bootloader;
static const char *rtc_sync_status="not_requested";
static bool eeprom_pending,eeprom_available,eeprom_all_ff;
static int64_t eeprom_at,eeprom_deadline;
static unsigned eeprom_non_ff;
static uint32_t eeprom_crc;
static const char *eeprom_status="not_checked";
static phase_t phase;
static int64_t phase_deadline, status_at, last_poll, stop_sent_at;
static int64_t last_version_poll;
static int64_t supply_at,last_supply_poll;
static unsigned supply_raw,supply_mv;
static bool supply_ready,supply_supported;
static dog_power_config power_config={1,DOG_POWER_UNKNOWN,0,0,7000,7200,false};
static dog_power_monitor power_monitor;
static dog_power_history power_history;
static bool power_persisted;
static int64_t power_notice_until;
static unsigned power_notifications;
static bool sensors_supported;
static dog_sensor_sample imu_sample,mag_sample;
static int64_t imu_at,mag_at,reference_at,last_sensor_poll;
static unsigned sensor_poll,reference_raw,reference_ready;
static dog_pose_mount pose_mount;
static dog_pose_wizard pose_wizard;
static int64_t pose_deadline;
static bool pose_persisted;
static uint32_t pose_owner,compass_owner;
static dog_posture_tracker posture_tracker;
typedef struct {uint32_t version,enabled,acceptance_confirmed,mount_hash;} pose_guard_config;
static pose_guard_config pose_guard={1,0,0,0};
static bool pose_guard_persisted,pose_guard_latched;
static bool pose_guard_observing,pose_guard_observed_tip;
static const char *pose_guard_reason="";
static unsigned pose_guard_stops;
static int64_t pose_guard_last_good,last_guard_poll;
typedef struct {uint32_t version,enabled,acceptance_confirmed,settings_hash;} power_limit_config;
static power_limit_config power_limit={1,0,0,0};
static dog_power_monitor power_limit_monitor;
static bool power_limit_persisted,power_limit_latched,power_limit_observing,power_limit_have_observation;
static const char *power_limit_reason="";
static unsigned power_limit_min_mv,power_limit_max_mv,power_limit_observed_updates;
static int64_t power_limit_recovered_since,last_power_limit_poll,last_power_limit_observed;
static dog_handling_tracker handling_tracker;
typedef struct {dog_compass_cal cal;uint32_t actuator_signature;} compass_record;
static compass_record compass_saved;
static dog_compass_capture compass_capture;
static int64_t compass_deadline,last_compass_reference_poll;
static bool compass_persisted;
static uint32_t compass_capture_actuators;
static bool compass_busy(void){return compass_deadline!=0;}
static void compass_cancel(const char *error)
{if(compass_capture.data)heap_caps_free(compass_capture.data);compass_capture.data=NULL;compass_capture.error=error;compass_deadline=0;compass_owner=0;}
static bool pose_busy(void){return pose_deadline!=0;}
static cJSON *pose_calibration_json(void)
{
    cJSON *j=cJSON_CreateObject();
    cJSON_AddBoolToObject(j,"calibrated",dog_pose_mount_valid(&pose_mount));
    cJSON_AddBoolToObject(j,"persisted",pose_persisted);cJSON_AddBoolToObject(j,"active",pose_busy());
    cJSON_AddBoolToObject(j,"collecting",pose_wizard.collecting);cJSON_AddNumberToObject(j,"stage",pose_wizard.stage);
    cJSON_AddNumberToObject(j,"samples",pose_wizard.count);cJSON_AddNumberToObject(j,"samples_required",10);
    cJSON_AddStringToObject(j,"error",pose_wizard.error?pose_wizard.error:"");return j;
}
static void pose_feed(bool moving)
{
    if(!pose_wizard.collecting||!imu_sample.ready||imu_sample.age>=200||moving)return;
    float a[3],g[3];for(unsigned i=0;i<3;i++){a[i]=imu_sample.axis[i]/4096.f;g[i]=imu_sample.axis[i+3]/64.f;}
    if(dog_pose_collect(&pose_wizard,imu_sample.sequence,a,g)){
        nvs_handle_t h;esp_err_t err=nvs_open("pavlov_pose",NVS_READWRITE,&h);
        if(err==ESP_OK){err=nvs_set_blob(h,"mount_v1",&pose_wizard.draft,sizeof(pose_wizard.draft));if(err==ESP_OK)err=nvs_commit(h);nvs_close(h);}
        if(err==ESP_OK){pose_mount=pose_wizard.draft;pose_persisted=true;pose_deadline=0;pose_owner=0;memset(&posture_tracker,0,sizeof(posture_tracker));}
        else{pose_wizard.stage=2;pose_wizard.error="save_failed";}
    }
}
static bool network_online, controller_baseline, controller_armed, controller_moving;
static bool stop_pending;
static uint32_t ble_session;
static unsigned stop_generation, sent_stop_generation;
static uint32_t pulses[5], tail_period;
static char last_error[96], last_line[192];
static char controller_version[40]="not_reported";
static const char *names[] = { "empty", "gateway_received", "controller_accepted", "running",
    "output_finished", "failed", "cancelled", "unknown" };
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void posture_feed(void)
{
    float a[3],r,p,v=0;for(unsigned i=0;i<3;i++)a[i]=imu_sample.axis[i]/4096.f;
    bool valid=imu_sample.ready&&imu_sample.age<200&&dog_pose_angles(&pose_mount,a,&r,&p,&v);
    dog_posture_update(&posture_tracker,valid,v,imu_sample.sequence,(uint32_t)now_ms());
}
static bool online(void) { return status_at && now_ms() - status_at < 2500; }
static uint32_t pose_guard_mount_hash(void)
{
    const unsigned char *p=(const unsigned char*)&pose_mount;uint32_t hash=2166136261u;
    for(unsigned i=0;i<sizeof(pose_mount);i++)hash=(hash^p[i])*16777619u;
    return hash?hash:1;
}
static bool pose_guard_sample_fresh(void)
{return sensors_supported&&online()&&imu_at&&imu_sample.ready&&now_ms()-imu_at+imu_sample.age<1500;}
static const char *pose_guard_admission(void)
{
    if(!pose_guard.enabled)return NULL;
    if(pose_guard_latched)return "pose_protection_latched";
    if(!dog_pose_mount_valid(&pose_mount)||pose_guard.mount_hash!=pose_guard_mount_hash())return "pose_protection_reconfirm_required";
    if(!pose_guard_sample_fresh())return "pose_protection_sensor_unavailable";
    return posture_tracker.state==DOG_POSTURE_UPRIGHT?NULL:"pose_protection_not_upright";
}
static bool handling_blocked(void)
{
    return !sensors_supported||!online()||!dog_pose_mount_valid(&pose_mount)||
        pose_busy()||compass_busy()||active>=0||phase!=IDLE||controller_moving||stop_pending||wifi_provision_busy();
}
static void handling_feed(void)
{
    uint32_t now=(uint32_t)now_ms();
    if(handling_blocked()||!imu_sample.ready||imu_sample.age>=200){dog_handling_block(&handling_tracker,now);return;}
    float a[3],gyro[3];for(unsigned i=0;i<3;i++){a[i]=imu_sample.axis[i]/4096.f;gyro[i]=imu_sample.axis[i+3]/64.f-pose_mount.bias[i];}
    dog_handling_update(&handling_tracker,a,gyro,imu_sample.sequence,now);
}
static uint32_t compass_actuators(void)
{
    uint32_t hash=controller_armed?1:0;
    for(unsigned i=0;i<5;i++)hash=(hash^pulses[i])*16777619u;
    return (hash^tail_period)*16777619u;
}
static void compass_feed(void)
{
    if(!compass_busy()||!compass_capture.data)return;
    if(compass_actuators()!=compass_capture_actuators){compass_cancel("actuator_state_changed");return;}
    if(mag_sample.range!=compass_capture.range){compass_cancel("range_changed");return;}
    int64_t imu_age=now_ms()-imu_at+imu_sample.age,mag_age=now_ms()-mag_at+mag_sample.age;
    if(!imu_sample.ready||!mag_sample.ready||imu_age>250||mag_age>250||llabs(imu_age-mag_age)>120){++compass_capture.rejected;return;}
    for(unsigned i=3;i<6;i++)if(abs(imu_sample.axis[i])>45*64){++compass_capture.rejected;return;}
    int16_t mag[3],acc[3];for(unsigned i=0;i<3;i++){mag[i]=(int16_t)mag_sample.axis[i];acc[i]=(int16_t)imu_sample.axis[i];}
    if(dog_compass_add(&compass_capture,mag_sample.sequence,mag,acc))compass_capture.error=NULL;
}
static bool power_fresh(void){return supply_supported&&supply_at&&now_ms()-supply_at<6000&&online()&&supply_ready;}
static unsigned power_voltage(void){return dog_power_calibrated(&power_config)?dog_power_mv(&power_config,supply_raw):supply_mv;}
static uint32_t power_limit_settings_hash(void)
{
    const uint32_t fields[]={(uint32_t)power_config.source,power_config.reference_raw,power_config.reference_mv,power_config.low_mv,power_config.recover_mv};
    const unsigned char *p=(const unsigned char*)fields;uint32_t hash=2166136261u;
    for(unsigned i=0;i<sizeof(fields);i++)hash=(hash^p[i])*16777619u;
    return hash?hash:1;
}
static bool power_limit_source_ready(void)
{return power_config.source==DOG_POWER_BATTERY_2S&&dog_power_calibrated(&power_config);}
static bool power_limit_recovered(void)
{unsigned mv=power_voltage();return power_limit_source_ready()&&power_fresh()&&mv>=power_config.recover_mv&&mv<=9000&&power_limit_recovered_since&&now_ms()-power_limit_recovered_since>=5000;}
static const char *power_limit_admission(void)
{
    if(!power_limit.enabled)return NULL;
    if(!power_limit_source_ready()||power_limit.settings_hash!=power_limit_settings_hash())return "power_limit_reconfirm_required";
    if(power_limit_latched)return "power_limit_latched";
    if(!power_fresh())return "power_limit_sample_unavailable";
    unsigned mv=power_voltage();
    return mv<5800||mv>9000?"power_limit_out_of_range":mv<=power_config.low_mv?"power_limit_reading_low":NULL;
}
static void power_limit_tick(int64_t now)
{
    bool ready=power_limit_source_ready()&&power_fresh();unsigned mv=power_voltage();
    if(ready&&mv>=power_config.recover_mv&&mv<=9000){if(!power_limit_recovered_since)power_limit_recovered_since=now;}else power_limit_recovered_since=0;
    if(power_limit_observing&&ready&&(active>=0||controller_moving)&&supply_at!=last_power_limit_observed){
        last_power_limit_observed=supply_at;++power_limit_observed_updates;
        if(!power_limit_have_observation){power_limit_min_mv=power_limit_max_mv=mv;power_limit_have_observation=true;}
        else{if(mv<power_limit_min_mv)power_limit_min_mv=mv;if(mv>power_limit_max_mv)power_limit_max_mv=mv;}
    }
    if(!power_limit.enabled)return;
    if(!power_limit_source_ready()||power_limit.settings_hash!=power_limit_settings_hash()){power_limit_latched=true;power_limit_reason="settings_changed";return;}
    dog_power_config monitoring=power_config;monitoring.alerts_enabled=true;
    dog_power_update(&power_limit_monitor,&monitoring,power_fresh(),supply_raw,(uint32_t)now);
    if(!power_limit_latched){
        if(!power_fresh()){power_limit_latched=true;power_limit_reason="sample_unavailable";}
        else if(mv<5800||mv>9000){power_limit_latched=true;power_limit_reason="out_of_range";}
        else if(power_limit_monitor.low){power_limit_latched=true;power_limit_reason="sustained_low";}
    }
}
static cJSON *power_json(void)
{
    bool calibrated=dog_power_calibrated(&power_config),fresh=power_fresh();unsigned mv=power_voltage();
    cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"source",power_config.source==DOG_POWER_USB?"usb":power_config.source==DOG_POWER_BATTERY_2S?"battery_2s":"unknown");
    cJSON_AddBoolToObject(j,"calibrated",calibrated);cJSON_AddBoolToObject(j,"persisted",power_persisted);
    cJSON_AddStringToObject(j,"calibration_method",calibrated?"user_measured_reference_single_point":"nominal_divider_uncalibrated");
    cJSON_AddBoolToObject(j,"reference_accuracy_verified_by_device",false);
    cJSON_AddBoolToObject(j,"alerts_enabled",power_config.alerts_enabled);cJSON_AddBoolToObject(j,"low",fresh&&power_monitor.low);
    cJSON_AddNumberToObject(j,"low_mv",power_config.low_mv);cJSON_AddNumberToObject(j,"recover_mv",power_config.recover_mv);
    if(calibrated){cJSON_AddNumberToObject(j,"reference_raw",power_config.reference_raw);cJSON_AddNumberToObject(j,"reference_millivolts",power_config.reference_mv);}
    cJSON_AddNumberToObject(j,"notifications",power_notifications);cJSON_AddBoolToObject(j,"motion_limit_enabled",power_limit.enabled!=0);
    const char *status=!fresh?"unavailable":power_config.source==DOG_POWER_UNKNOWN?"source_required":power_config.source==DOG_POWER_USB?"usb_source":!calibrated?"reference_required":!power_config.alerts_enabled?"disabled":mv<5800||mv>9000?"out_of_range":power_monitor.low?"low":"monitoring";
    cJSON_AddStringToObject(j,"status",status);return j;
}
static int walk_ms(const request_t *r) { return !strcmp(r->profile,"legacy")?r->argument*2000:r->argument*750+150; }
static const dog_action *catalog_action(const char *name)
{
    return !strncmp(name,"motion.",7)?dog_action_by_name(name+7):NULL;
}
static void tx(const char *s) { uart_write_bytes(DOG_BRIDGE_UART, s, strlen(s)); }
static cJSON *request_json(const request_t *r)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "request_id", r->id);
    cJSON_AddStringToObject(j, "action", r->action);
    cJSON_AddNumberToObject(j, "argument", r->argument);
    if(!strcmp(r->action,"motion.walk")){
        cJSON_AddStringToObject(j,"profile",r->profile);
        cJSON_AddNumberToObject(j,"duration_ms",walk_ms(r));
    }
    const dog_action *catalog=catalog_action(r->action);
    if(catalog){
        cJSON_AddNumberToObject(j,"repeats",r->argument);
        cJSON_AddStringToObject(j,"speed",r->speed==1?"slow":r->speed==3?"fast":"normal");
        cJSON_AddNumberToObject(j,"strength",r->strength);
        cJSON_AddNumberToObject(j,"duration_ms",dog_action_duration(catalog,r->argument,r->speed));
    }
    if(!strcmp(r->action,"diagnostics.leg"))cJSON_AddNumberToObject(j,"channel",r->channel);
    cJSON_AddStringToObject(j, "state", names[r->result]);
    cJSON_AddStringToObject(j, "completion_evidence", r->result == FINISHED ? "pwm_sequence" : "none");
    cJSON_AddBoolToObject(j, "physical_position_verified", false);
    if (r->error[0]) cJSON_AddStringToObject(j, "error", r->error);
    return j;
}
static void finish(result_t state, const char *error)
{
    if (active >= 0) {
        history[active].result = state;
        history[active].finished = now_ms();
        if (error) snprintf(history[active].error, sizeof(history[active].error), "%s", error);
    }
    if (error) snprintf(last_error, sizeof(last_error), "%s", error);
    active = -1;
    phase = IDLE;
}
static void begin_motion(void)
{
    if(ota_reserved||rtc_pending||eeprom_pending){finish(FAILED,"ota_in_progress");return;}
    char cmd[48];
    request_t *r = &history[active];
    const char *guard_error=pose_guard_admission();if(guard_error){pose_guard_latched=true;pose_guard_reason=guard_error;finish(FAILED,guard_error);stop_pending=true;++stop_generation;return;}
    const char *power_error=power_limit_admission();if(power_error){power_limit_latched=true;power_limit_reason="prestart_condition_changed";finish(FAILED,power_error);stop_pending=true;++stop_generation;return;}
    if(r->ble_session&&r->ble_session!=ble_session){finish(FAILED,"ble_session_closed");return;}
    if (now_ms()-r->received > r->ttl_ms) { finish(FAILED,"request_expired"); return; }
    const dog_action *catalog=catalog_action(r->action);
    if(catalog)snprintf(cmd,sizeof(cmd),"ACT %u %d %d %d\n",catalog->id,r->argument,r->speed,r->strength>DOG_ACTION_MAX_STRENGTH?DOG_ACTION_MAX_STRENGTH:r->strength);
    else if (!strcmp(r->action, "motion.walk")) {
        if(!strcmp(r->profile,"legacy"))snprintf(cmd,sizeof(cmd),"WALK %d %d\n",DOG_WALK_AMPLITUDE_US,r->argument);
        else snprintf(cmd,sizeof(cmd),"GAIT %d %d\n",!strcmp(r->profile,"vendor_raw")?1:2,r->argument);
    }
    else if(!strcmp(r->action,"diagnostics.leg"))snprintf(cmd,sizeof(cmd),"LEG %d %d\n",r->channel,r->argument);
    else if(!strcmp(r->action,"motion.home"))snprintf(cmd,sizeof(cmd),"HOME\n");
    else snprintf(cmd, sizeof(cmd), "TAIL %d\n", r->argument);
    tx(cmd); phase = COMMAND; phase_deadline = now_ms() + 1000;
}
static void pose_guard_tick(int64_t now)
{
    if(pose_guard_observing&&(active>=0||controller_moving)&&pose_guard_sample_fresh()&&(posture_tracker.state==DOG_POSTURE_SIDE||posture_tracker.state==DOG_POSTURE_INVERTED))pose_guard_observed_tip=true;
    if(!pose_guard.enabled)return;
    if(pose_guard_sample_fresh())pose_guard_last_good=imu_at-imu_sample.age;
    if((active<0&&!controller_moving)||stop_pending)return;
    const char *cause=NULL;
    if(pose_guard_latched)cause=pose_guard_reason[0]?pose_guard_reason:"latched";
    else if(!dog_pose_mount_valid(&pose_mount)||pose_guard.mount_hash!=pose_guard_mount_hash())cause="calibration_changed";
    else if(pose_guard_sample_fresh()&&posture_tracker.state==DOG_POSTURE_SIDE)cause="side";
    else if(pose_guard_sample_fresh()&&posture_tracker.state==DOG_POSTURE_INVERTED)cause="inverted";
    else if(!pose_guard_last_good||now-pose_guard_last_good>2500)cause="sensor_unavailable";
    if(cause){pose_guard_latched=true;pose_guard_reason=cause;++pose_guard_stops;finish(CANCELLED,"pose_protection_triggered");stop_pending=true;++stop_generation;}
}
static void prepare_from_state(void)
{
    if(ota_reserved||rtc_pending||eeprom_pending){finish(FAILED,"ota_in_progress");return;}
    if (controller_moving) { finish(FAILED, "controller_busy"); return; }
    const char *guard_error=pose_guard_admission();if(guard_error){finish(FAILED,guard_error);return;}
    const char *power_error=power_limit_admission();if(power_error){finish(FAILED,power_error);return;}
    if (!controller_baseline) {
        tx(DOG_BASELINE_COMMAND); phase = BASE; phase_deadline = now_ms() + 1000;
    } else if (!controller_armed) {
        tx("ARM\n"); phase = ARM; phase_deadline = now_ms() + 1000;
    } else begin_motion();
}
static void parse_line(const char *line)
{
    if(!strncmp(line,"PAVLOV_LOADER 1 READY",21)){
        controller_bootloader=true;snprintf(controller_version,sizeof(controller_version),"pavlov-loader 1");
        sensors_supported=false;supply_supported=false;return;
    }
    if(!strcmp(line,"ERR EEPROM_REQUIRES_IDLE")&&eeprom_pending){eeprom_pending=false;eeprom_at=0;eeprom_status="controller_busy";return;}
    unsigned ea,ef,en,ec;int elen=0;
    if(sscanf(line,"EEPROM available=%u all_ff=%u non_ff=%u crc32=%u%n",&ea,&ef,&en,&ec,&elen)==4&&line[elen]==0&&ea<=1&&ef<=1&&en<=256&&(!ea||ef==(en==0))){
        eeprom_pending=false;eeprom_available=ea;eeprom_all_ff=ef;eeprom_non_ff=en;eeprom_crc=ec;eeprom_at=now_ms();eeprom_status=ea?"checked_readonly":"read_failed";return;
    }
    if(!strcmp(line,"RTC_SYNC ok=1")||!strcmp(line,"RTC_SYNC ok=0")){
        rtc_pending=false;rtc_sync_status=line[12]=='1'?"confirmed":"failed";return;
    }
    unsigned ra,rv,ry,rm,rd,rh,rn,rs,rw;int rnchars=0;
    if(sscanf(line,"RTC available=%u valid=%u year=%u month=%u day=%u hour=%u minute=%u second=%u weekday=%u%n",
       &ra,&rv,&ry,&rm,&rd,&rh,&rn,&rs,&rw,&rnchars)==9&&line[rnchars]==0&&ra<=1&&rv<=1&&(!rv||ra)){
        dog_rtc_date d={ry,rm,rd,rh,rn,rs,rw};
        if(rv&&(!dog_rtc_date_valid(&d)||dog_rtc_weekday(&d)!=rw))return;
        rtc_available=ra;rtc_valid=rv;rtc_date=d;rtc_at=now_ms();return;
    }
    if(!strcmp(line,"EVENT USER_BUTTON")){
        ++user_button_events;
        if(active>=0)finish(CANCELLED,"physical_user_button");
        stop_pending=true;++stop_generation;return;
    }
    unsigned av,ae,ap,ab,al,as,ac,am,ar;int consumed=0;
    if(sscanf(line,"AUX protocol=%u events=%u pressed=%u beep=%u led=%u sd_present=%u probed=%u eeprom=%u rtc=%u%n",
        &av,&ae,&ap,&ab,&al,&as,&ac,&am,&ar,&consumed)==9&&line[consumed]==0&&av==1&&ap<=1&&ab<=1&&al<=2&&as<=1&&ac<=1&&am<=1&&ar<=1){
        aux_pressed=ap;aux_beep=ab;aux_led=al;aux_sd=as;aux_probed=ac;aux_eeprom=am;aux_rtc=ar;aux_at=now_ms();return;
    }
    snprintf(last_line, sizeof(last_line), "%s", line);
    char version[24];
    unsigned raw,mv,ready;
    dog_sensor_sample sample={0};
    if(dog_sensor_parse(line,true,&sample)){imu_sample=sample;imu_at=now_ms();pose_feed(controller_moving);posture_feed();handling_feed();return;}
    if(dog_sensor_parse(line,false,&sample)){mag_sample=sample;mag_at=now_ms();compass_feed();return;}
    if(dog_reference_parse(line,&raw,&ready)){reference_raw=raw;reference_ready=ready;reference_at=now_ms();return;}
    if(dog_supply_parse_line(line,&raw,&mv,&ready)){
        supply_raw=raw;supply_mv=mv;supply_ready=ready!=0;supply_at=now_ms();return;
    }
    if(sscanf(line,"VERSION motion-check %23s",version)==1){
        unsigned major=0,minor=0,patch=0;
        supply_supported=sscanf(version,"%u.%u.%u",&major,&minor,&patch)==3&&
            (major>0||minor>3||(minor==3&&patch>=1));
        sensors_supported=major>0||minor>=4;
        controller_bootloader=false;snprintf(controller_version,sizeof(controller_version),"motion-check %s",version);return;
    }
    unsigned b, a, m, period, p[5];
    if (sscanf(line, "STATE baseline=%u armed=%u moving=%u tail_period_us=%u pulse_us=%u,%u,%u,%u,%u",
               &b, &a, &m, &period, &p[0], &p[1], &p[2], &p[3], &p[4]) == 9) {
        status_at = now_ms(); controller_baseline = b; controller_armed = a; controller_moving = m;
        tail_period = period; memcpy(pulses, p, sizeof(pulses));
        if (active >= 0 && (phase == COMMAND || phase == EXECUTING) && (!b || !a)) {
            finish(UNKNOWN,"controller_reset_detected"); stop_pending=true; ++stop_generation;
        }
        if (active >= 0 && phase == SYNC && !stop_pending) prepare_from_state();
        if (active >= 0 && phase == EXECUTING && m) history[active].result = RUNNING;
        return;
    }
    if (strstr(line, "DOG_MOTION_CHECK")) {
        controller_bootloader=false;
        snprintf(controller_version,sizeof(controller_version),"not_reported");sensors_supported=supply_supported=false;
        imu_at=mag_at=reference_at=supply_at=0;
        memset(&power_history,0,sizeof(power_history));
        pose_deadline=0;pose_owner=0;pose_wizard.collecting=false;pose_wizard.stage=0;pose_wizard.error="controller_restarted";
        memset(&posture_tracker,0,sizeof(posture_tracker));
        dog_handling_block(&handling_tracker,(uint32_t)now_ms());
        compass_cancel("controller_restarted");
        status_at = 0;aux_at=0;rtc_at=0;rtc_pending=false;rtc_sync_status="controller_restarted";
        eeprom_at=0;eeprom_pending=false;eeprom_status="controller_restarted";
        if (active >= 0) finish(UNKNOWN, "controller_restarted");
        stop_pending = true; ++stop_generation;
        return;
    }
    if (!strcmp(line, "STOP_HOLD_PWM")) {
        if (stop_pending && sent_stop_generation == stop_generation) {
            if (active >= 0) finish(CANCELLED, NULL);
            stop_pending = false; stop_sent_at = 0;
            controller_moving = false;
        }
        return;
    }
    if (active < 0 || stop_pending) return;
    if (!strcmp(line, "BASE_STAGED") && phase == BASE) {
        controller_baseline = true;
        tx("ARM\n"); phase = ARM; phase_deadline = now_ms() + 1000;
    } else if (!strcmp(line, "ARMED_AT_BASELINE") && phase == ARM) {
        controller_armed = true; begin_motion();
    } else if (phase == COMMAND &&
               ((!strcmp(history[active].action, "motion.walk") && !strncmp(line, "ACCEPTED_WALK", 13)) ||
                (!strcmp(history[active].action, "motion.tail") && !strcmp(line, "ACCEPTED_TAIL")) ||
                (!strcmp(history[active].action, "motion.home") && !strcmp(line,"ACCEPTED_HOME")) ||
                (catalog_action(history[active].action) && !strncmp(line,"ACCEPTED_ACTION duration_ms=",sizeof("ACCEPTED_ACTION duration_ms=")-1)) ||
                (!strcmp(history[active].action, "diagnostics.leg") && !strcmp(line,"ACCEPTED_LEG")))) {
        history[active].result = ACCEPTED; phase = EXECUTING;
        int duration = !strcmp(history[active].action, "motion.walk") ? walk_ms(&history[active]) :
            !strcmp(history[active].action,"diagnostics.leg") ? 3500 : 2500;
        const dog_action *catalog=catalog_action(history[active].action);
        if(catalog)duration=(int)dog_action_duration(catalog,history[active].argument,history[active].speed)+1000;
        phase_deadline = now_ms() + duration + 1500;
    } else if (phase == EXECUTING &&
               ((!strcmp(history[active].action, "motion.walk") && !strncmp(line, "PWM_WALK_SEQUENCE_FINISHED", 26)) ||
                (!strcmp(history[active].action, "motion.tail") && !strncmp(line, "PWM_SEQUENCE_FINISHED", 21)) ||
                (!strcmp(history[active].action, "motion.home") && !strncmp(line,"PWM_HOME_SEQUENCE_FINISHED",26)) ||
                (catalog_action(history[active].action) && !strcmp(line,"PWM_ACTION_SEQUENCE_FINISHED position_unverified")) ||
                (!strcmp(history[active].action,"diagnostics.leg") && !strcmp(line,"PWM_LEG_SEQUENCE_FINISHED position_unverified")))) {
        finish(FINISHED, NULL); controller_moving = false;
    } else if (!strncmp(line, "ERR ", 4) || !strncmp(line, "TIMEOUT_", 8)) {
        finish(FAILED, line); stop_pending = true; ++stop_generation;
    }
}
static void bridge_task(void *arg)
{
    (void)arg;
    char line[192]; unsigned len = 0; bool overflow = false;
    tx("STOP\n");
    for (;;) {
        uint8_t bytes[256];
        int count = uart_read_bytes(DOG_BRIDGE_UART, bytes, sizeof(bytes), pdMS_TO_TICKS(10));
        xSemaphoreTake(mutex, portMAX_DELAY);
        for (int i = 0; i < count; ++i) {
            char c = bytes[i];
            if (c == '\n' || c == '\r') {
                if (len && !overflow) { line[len] = 0; parse_line(line); }
                len = 0; overflow = false;
            } else if (c >= 32 && c < 127) {
                if (len < sizeof(line)-1) line[len++] = c;
                else overflow = true;
            } else overflow = true;
        }
        if(controller_uart_requested){
            controller_uart_owned=true;len=0;overflow=false;status_at=0;aux_at=0;rtc_at=0;eeprom_at=0;
            controller_armed=false;controller_moving=false;controller_baseline=false;
            snprintf(controller_version,sizeof(controller_version),"not_reported");
            xSemaphoreGive(mutex);
            for(;;){vTaskDelay(pdMS_TO_TICKS(10));xSemaphoreTake(mutex,portMAX_DELAY);bool held=controller_uart_requested;xSemaphoreGive(mutex);if(!held)break;}
            xSemaphoreTake(mutex,portMAX_DELAY);controller_uart_owned=false;xSemaphoreGive(mutex);continue;
        }
        int64_t now = now_ms();
        if(eeprom_pending&&now>=eeprom_deadline){eeprom_pending=false;eeprom_at=0;eeprom_status="outcome_unconfirmed";}
        if(rtc_pending&&now>=rtc_deadline){rtc_pending=false;rtc_sync_status="outcome_unconfirmed";rtc_at=0;}
        unsigned rmajor=0,rminor=0,rpatch=0;
        bool rtc_supported=sscanf(controller_version,"motion-check %u.%u.%u",&rmajor,&rminor,&rpatch)==3&&(rmajor>0||rminor>5||(rminor==5&&rpatch>=1));
        if(rtc_supported&&online()&&active<0&&!controller_moving&&!stop_pending&&!ota_reserved&&!rtc_pending&&!eeprom_pending&&!pose_busy()&&!compass_busy()&&now-last_rtc_poll>=3000){tx("RTC_READ\n");last_rtc_poll=now;}
        if(online()&&active<0&&!controller_moving&&!stop_pending&&!ota_reserved&&now-last_aux_poll>=1500){tx("AUX_STATUS\n");last_aux_poll=now;}
        pose_guard_tick(now);
        power_limit_tick(now);
        if(handling_blocked()||!imu_at||!imu_sample.ready||now-imu_at+imu_sample.age>=900)dog_handling_block(&handling_tracker,(uint32_t)now);
        if(compass_deadline&&now>compass_deadline)compass_cancel("calibration_timed_out");
        if(dog_power_update(&power_monitor,&power_config,power_fresh(),supply_raw,(uint32_t)now)){power_notice_until=now+8000;++power_notifications;}
        dog_power_history_prune(&power_history,(uint32_t)now);
        if(power_fresh()&&active<0&&phase==IDLE&&!controller_moving&&!stop_pending&&!pose_busy()&&!compass_busy()&&!wifi_provision_busy())dog_power_history_add(&power_history,(uint32_t)supply_at,power_voltage());
        if (stop_pending && sent_stop_generation != stop_generation) {
            tx("STOP\n"); sent_stop_generation = stop_generation; stop_sent_at = now;
        }
        if (stop_pending && stop_sent_at && now - stop_sent_at > 1500) {
            if (active >= 0) finish(UNKNOWN, "stop_reply_timeout");
            snprintf(last_error, sizeof(last_error), "stop_reply_timeout; controller state requires recovery");
            /* Keep the stop barrier until an explicit fresh stop or reply; do not release new movement. */
            stop_sent_at = 0;
        }
        if (!stop_pending && active >= 0) {
            request_t *r = &history[active];
            if (phase == IDLE) {
                if (r->ble_session&&r->ble_session!=ble_session)finish(FAILED,"ble_session_closed");
                else if (!network_online&&!r->ble_session) finish(FAILED, "network_offline");
                else if (now-r->received > r->ttl_ms) finish(FAILED, "request_expired");
                else { tx("STATUS\n"); phase = SYNC; phase_deadline = now+1000; }
            } else if (now > phase_deadline) {
                finish(UNKNOWN, "controller_reply_timeout"); stop_pending = true; ++stop_generation;
            }
        }
        if (now-last_poll>=500&&(phase==IDLE||phase==EXECUTING)&&!stop_pending){
            if(active<0&&!strcmp(controller_version,"not_reported")&&now-last_version_poll>=1000){
                tx("VERSION\n");last_version_poll=now;
            }else tx("STATUS\n");
            last_poll=now;
        }
        if(supply_supported&&active<0&&phase==IDLE&&!stop_pending&&now-last_supply_poll>=2000){
            tx("SUPPLY\n");last_supply_poll=now;
        }
        if((pose_guard.enabled||pose_guard_observing)&&sensors_supported&&!stop_pending&&(active>=0||controller_moving)&&now-last_guard_poll>=250){tx("IMU\n");last_guard_poll=now;}
        if((power_limit.enabled||power_limit_observing)&&supply_supported&&!stop_pending&&(active>=0||controller_moving)&&now-last_power_limit_poll>=500){tx("SUPPLY\n");last_power_limit_poll=now;}
        if(pose_deadline&&now>pose_deadline){pose_deadline=0;pose_owner=0;pose_wizard.collecting=false;pose_wizard.stage=0;pose_wizard.error="calibration_timed_out";}
        bool compass_pair=compass_busy()||dog_compass_cal_valid(&compass_saved.cal);
        if(sensors_supported&&active<0&&phase==IDLE&&!stop_pending&&now-last_sensor_poll>=(pose_wizard.collecting?100:compass_busy()?150:compass_pair?350:250)){
            static const char *commands[]={"IMU\n","MAG\n","ADCREF\n"};
            tx(pose_wizard.collecting?"IMU\n":compass_pair?"IMU\nMAG\n":commands[sensor_poll++%3]);last_sensor_poll=now;
        }
        if(compass_pair&&sensors_supported&&active<0&&phase==IDLE&&!stop_pending&&now-last_compass_reference_poll>=2000){tx("ADCREF\n");last_compass_reference_poll=now;}
        xSemaphoreGive(mutex);
    }
}
void motion_bridge_init(void)
{
    mutex = xSemaphoreCreateMutex(); assert(mutex);
    nvs_handle_t guard_handle;if(nvs_open("pavlov_guard",NVS_READONLY,&guard_handle)==ESP_OK){
        pose_guard_config saved={0};size_t n=sizeof(saved);
        if(nvs_get_blob(guard_handle,"pose_v1",&saved,&n)==ESP_OK&&n==sizeof(saved)&&saved.version==1&&saved.enabled<=1&&saved.acceptance_confirmed<=1&&(!saved.enabled||(saved.acceptance_confirmed&&saved.mount_hash))){pose_guard=saved;pose_guard_persisted=true;}
        nvs_close(guard_handle);
    }
    if(pose_guard.enabled){pose_guard_latched=true;pose_guard_reason="boot_confirmation_required";}
    nvs_handle_t limit_handle;if(nvs_open("pavlov_guard",NVS_READONLY,&limit_handle)==ESP_OK){
        power_limit_config saved={0};size_t n=sizeof(saved);
        if(nvs_get_blob(limit_handle,"power_v1",&saved,&n)==ESP_OK&&n==sizeof(saved)&&saved.version==1&&saved.enabled<=1&&saved.acceptance_confirmed<=1&&(!saved.enabled||(saved.acceptance_confirmed&&saved.settings_hash))){power_limit=saved;power_limit_persisted=true;}
        nvs_close(limit_handle);
    }
    if(power_limit.enabled){power_limit_latched=true;power_limit_reason="boot_confirmation_required";}
    nvs_handle_t compass_handle;if(nvs_open("pavlov_compass",NVS_READONLY,&compass_handle)==ESP_OK){
        compass_record saved={0};size_t n=sizeof(saved);
        if(nvs_get_blob(compass_handle,"cal_v1",&saved,&n)==ESP_OK&&n==sizeof(saved)&&dog_compass_cal_valid(&saved.cal)){compass_saved=saved;compass_persisted=true;}
        nvs_close(compass_handle);
    }
    nvs_handle_t power_handle;if(nvs_open("pavlov_power",NVS_READONLY,&power_handle)==ESP_OK){
        dog_power_config saved={0};size_t n=sizeof(saved);
        if(nvs_get_blob(power_handle,"config_v1",&saved,&n)==ESP_OK&&n==sizeof(saved)&&dog_power_config_valid(&saved)){power_config=saved;power_persisted=true;}
        nvs_close(power_handle);
    }
    nvs_handle_t pose_handle;if(nvs_open("pavlov_pose",NVS_READONLY,&pose_handle)==ESP_OK){
        dog_pose_mount saved={0};size_t n=sizeof(saved);
        if(nvs_get_blob(pose_handle,"mount_v1",&saved,&n)==ESP_OK&&n==sizeof(saved)&&dog_pose_mount_valid(&saved)){pose_mount=saved;pose_persisted=true;}
        nvs_close(pose_handle);
    }
    uart_config_t cfg = { .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    ESP_ERROR_CHECK(uart_driver_install(DOG_BRIDGE_UART, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(DOG_BRIDGE_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(DOG_BRIDGE_UART, DOG_BRIDGE_TX, DOG_BRIDGE_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    BaseType_t created=xTaskCreate(bridge_task, "dog_motion_bridge", 6144, NULL, 8, NULL);
    assert(created==pdPASS);
}
void motion_bridge_network(bool connected)
{
    if (!mutex) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    network_online = connected;
    if (!connected) { stop_pending = true; ++stop_generation; }
    xSemaphoreGive(mutex);
}
void motion_bridge_stop(void)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if(pose_busy()){pose_deadline=0;pose_owner=0;pose_wizard.collecting=false;pose_wizard.stage=0;pose_wizard.error="cancelled";}
    if(compass_busy())compass_cancel("cancelled");
    stop_pending = true; ++stop_generation;
    xSemaphoreGive(mutex);
}
uint32_t motion_bridge_user_button_events(void)
{xSemaphoreTake(mutex,portMAX_DELAY);uint32_t n=user_button_events;xSemaphoreGive(mutex);return n;}
cJSON *motion_bridge_aux_state(void)
{
    xSemaphoreTake(mutex,portMAX_DELAY);cJSON *j=cJSON_CreateObject();bool fresh=online()&&aux_at&&now_ms()-aux_at<6000;
    cJSON_AddBoolToObject(j,"available",fresh);cJSON_AddNumberToObject(j,"received_button_events",user_button_events);
    if(fresh){cJSON_AddBoolToObject(j,"button_pressed",aux_pressed);cJSON_AddBoolToObject(j,"beeping",aux_beep);cJSON_AddNumberToObject(j,"led_mode",aux_led);
        cJSON_AddBoolToObject(j,"stm32_sd_detect_present",aux_sd);cJSON_AddBoolToObject(j,"probed",aux_probed);
        if(aux_probed){cJSON_AddBoolToObject(j,"eeprom_acknowledged",aux_eeprom);cJSON_AddBoolToObject(j,"rtc_acknowledged",aux_rtc);}}
    xSemaphoreGive(mutex);return j;
}
cJSON *motion_bridge_aux_command(const char *command,unsigned value,int *status)
{
    xSemaphoreTake(mutex,portMAX_DELAY);const char *error=NULL;*status=202;
    if(!online()||!aux_at||now_ms()-aux_at>=6000){error="board_aux_unavailable";*status=503;}
    else if(ota_reserved||rtc_pending||eeprom_pending||active>=0||controller_moving||stop_pending||phase!=IDLE||pose_busy()||compass_busy()||wifi_provision_busy()){error="board_aux_requires_idle";*status=409;}
    else if(!strcmp(command,"probe"))tx("AUX_PROBE\n");
    else if((!strcmp(command,"beep")&&value<=300)||(!strcmp(command,"led")&&value<=2)){
        char text[32];snprintf(text,sizeof(text),!strcmp(command,"beep")?"AUX_BEEP %u\n":"AUX_LED %u\n",value);tx(text);
    }else{error="invalid_board_command";*status=422;}
    cJSON *j=cJSON_CreateObject();if(error)cJSON_AddStringToObject(j,"error",error);
    else{cJSON_AddBoolToObject(j,"accepted",true);cJSON_AddBoolToObject(j,"controller_acknowledged",false);cJSON_AddStringToObject(j,"command",command);}
    xSemaphoreGive(mutex);return j;
}
cJSON *motion_bridge_rtc_state(void)
{
    xSemaphoreTake(mutex,portMAX_DELAY);cJSON *j=cJSON_CreateObject();bool fresh=online()&&rtc_at&&now_ms()-rtc_at<10000&&!rtc_pending;
    cJSON_AddBoolToObject(j,"available",fresh&&rtc_available);cJSON_AddBoolToObject(j,"valid",fresh&&rtc_available&&rtc_valid);
    cJSON_AddBoolToObject(j,"sync_pending",rtc_pending);cJSON_AddStringToObject(j,"sync_status",rtc_sync_status);
    cJSON_AddStringToObject(j,"storage_timezone","UTC");cJSON_AddBoolToObject(j,"accuracy_verified",false);
    if(rtc_at)cJSON_AddNumberToObject(j,"age_ms",now_ms()-rtc_at);else cJSON_AddNullToObject(j,"age_ms");
    if(fresh&&rtc_available&&rtc_valid){char utc[24];snprintf(utc,sizeof(utc),"%04u-%02u-%02uT%02u:%02u:%02uZ",rtc_date.year,rtc_date.month,rtc_date.day,rtc_date.hour,rtc_date.minute,rtc_date.second);cJSON_AddStringToObject(j,"utc",utc);}
    else cJSON_AddNullToObject(j,"utc");
    xSemaphoreGive(mutex);return j;
}
void motion_bridge_rtc_snapshot(dog_rtc_snapshot *out)
{
    xSemaphoreTake(mutex,portMAX_DELAY);int64_t age=rtc_at?now_ms()-rtc_at:UINT32_MAX;
    bool fresh=online()&&rtc_at&&age<10000&&!rtc_pending;
    *out=(dog_rtc_snapshot){fresh&&rtc_available,fresh&&rtc_available&&rtc_valid,rtc_pending,rtc_date,(uint32_t)(age>UINT32_MAX?UINT32_MAX:age)};
    xSemaphoreGive(mutex);
}
cJSON *motion_bridge_rtc_sync(const char *utc,int *status)
{
    dog_rtc_date d;const char *error=NULL;*status=202;
    if(!utc||!dog_rtc_parse_iso(utc,strlen(utc),&d)){error="invalid_rtc_utc";*status=422;}
    xSemaphoreTake(mutex,portMAX_DELAY);
    if(!error){
        if(!online()||!rtc_at||now_ms()-rtc_at>=10000||!rtc_available){error="rtc_unavailable";*status=503;}
        else if(ota_reserved||rtc_pending||eeprom_pending||active>=0||controller_moving||stop_pending||phase!=IDLE||pose_busy()||compass_busy()||wifi_provision_busy()){error="rtc_requires_idle";*status=409;}
        else{char text[64];snprintf(text,sizeof(text),"RTC_SET %u %u %u %u %u %u\n",d.year,d.month,d.day,d.hour,d.minute,d.second);rtc_pending=true;rtc_sync_status="pending";rtc_deadline=now_ms()+2500;tx(text);}
    }
    cJSON *j=cJSON_CreateObject();if(error)cJSON_AddStringToObject(j,"error",error);
    else{cJSON_AddBoolToObject(j,"accepted",true);cJSON_AddBoolToObject(j,"controller_acknowledged",false);cJSON_AddStringToObject(j,"utc_requested",utc);}
    xSemaphoreGive(mutex);return j;
}
cJSON *motion_bridge_eeprom_state(void)
{
    xSemaphoreTake(mutex,portMAX_DELAY);cJSON *j=cJSON_CreateObject();bool fresh=online()&&eeprom_at&&now_ms()-eeprom_at<30000&&!eeprom_pending;
    cJSON_AddBoolToObject(j,"available",fresh&&eeprom_available);cJSON_AddBoolToObject(j,"pending",eeprom_pending);cJSON_AddStringToObject(j,"status",eeprom_status);
    cJSON_AddNumberToObject(j,"capacity_bytes",256);cJSON_AddBoolToObject(j,"write_supported",false);cJSON_AddBoolToObject(j,"raw_content_exposed",false);
    if(eeprom_at)cJSON_AddNumberToObject(j,"age_ms",now_ms()-eeprom_at);else cJSON_AddNullToObject(j,"age_ms");
    if(fresh&&eeprom_available){char crc[9];snprintf(crc,sizeof(crc),"%08lx",(unsigned long)eeprom_crc);cJSON_AddStringToObject(j,"crc32",crc);cJSON_AddBoolToObject(j,"all_ff",eeprom_all_ff);cJSON_AddNumberToObject(j,"non_ff_bytes",eeprom_non_ff);}
    else{cJSON_AddNullToObject(j,"crc32");cJSON_AddNullToObject(j,"all_ff");cJSON_AddNullToObject(j,"non_ff_bytes");}
    xSemaphoreGive(mutex);return j;
}
cJSON *motion_bridge_eeprom_inspect(int *status)
{
    xSemaphoreTake(mutex,portMAX_DELAY);const char *error=NULL;*status=202;unsigned major=0,minor=0,patch=0;
    bool supported=sscanf(controller_version,"motion-check %u.%u.%u",&major,&minor,&patch)==3&&(major>0||minor>5||(minor==5&&patch>=2));
    if(!online()){error="controller_offline";*status=503;}
    else if(!supported){error="eeprom_not_supported";*status=409;}
    else if(ota_reserved||rtc_pending||eeprom_pending||active>=0||controller_moving||stop_pending||phase!=IDLE||pose_busy()||compass_busy()||wifi_provision_busy()){error="eeprom_requires_idle";*status=409;}
    else{eeprom_pending=true;eeprom_at=0;eeprom_status="pending";eeprom_deadline=now_ms()+2500;tx("EEPROM_INFO\n");}
    cJSON *j=cJSON_CreateObject();if(error)cJSON_AddStringToObject(j,"error",error);
    else{cJSON_AddBoolToObject(j,"accepted",true);cJSON_AddBoolToObject(j,"controller_acknowledged",false);}
    xSemaphoreGive(mutex);return j;
}
const char *motion_bridge_ota_reserve(void)
{
    if(!mutex)return "ota_motion_unavailable";
    xSemaphoreTake(mutex,portMAX_DELAY);
    const char *error=ota_reserved?"ota_in_progress":pose_busy()||compass_busy()?"ota_calibration_active":
        active>=0||phase!=IDLE||controller_moving||stop_pending||rtc_pending||eeprom_pending?"ota_motion_busy":NULL;
    if(!error)ota_reserved=true;
    xSemaphoreGive(mutex);return error;
}
void motion_bridge_ota_release(void)
{
    xSemaphoreTake(mutex,portMAX_DELAY);ota_reserved=false;xSemaphoreGive(mutex);
}
static cJSON *submit(uint32_t local_session,const char *id,const char *action,int argument,int channel,const char *profile,int speed,int strength,uint32_t ttl_ms,int *http_status)
{
    if(strength>DOG_ACTION_MAX_STRENGTH)strength=DOG_ACTION_MAX_STRENGTH;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (int i = 0; i < HISTORY; ++i) {
        if (history[i].result != EMPTY && !strcmp(id, history[i].id)) {
            if (strcmp(action, history[i].action) || argument != history[i].argument || channel != history[i].channel || strcmp(profile,history[i].profile) || speed!=history[i].speed || strength!=history[i].strength || ttl_ms != history[i].ttl_ms) {
                *http_status = 409;
                cJSON *j = cJSON_CreateObject(); cJSON_AddStringToObject(j,"error","request_id_conflict");
                xSemaphoreGive(mutex); return j;
            }
            *http_status = 200; cJSON *j = request_json(&history[i]);
            cJSON_AddBoolToObject(j,"deduplicated",true); xSemaphoreGive(mutex); return j;
        }
    }
    const char *error = NULL;
    const char *guard_error=pose_guard_admission();
    const char *power_error=power_limit_admission();
    if(ota_reserved)error="ota_in_progress";
    else if(controller_bootloader)error="controller_in_bootloader";
    else if(rtc_pending)error="rtc_sync_in_progress";
    else if(eeprom_pending)error="eeprom_inspection_in_progress";
    else if(local_session&&local_session!=ble_session)error="ble_session_closed";
    else if (!network_online&&!local_session) error = "network_offline";
    else if (stop_pending) error = "stop_pending";
    else if (!online()) error = "controller_offline";
    else if(pose_busy())error="pose_calibration_active";
    else if(wifi_provision_busy())error="wifi_configuration_active";
    else if(compass_busy())error="compass_calibration_active";
    else if(guard_error)error=guard_error;
    else if(power_error)error=power_error;
    else if (active >= 0 || controller_moving) error = "motion_busy";
    if (error) {
        *http_status = ota_reserved || rtc_pending || eeprom_pending || error==guard_error || error==power_error || active >= 0 || controller_moving || pose_busy() || compass_busy() || wifi_provision_busy() ? 409 : 503;
        cJSON *j = cJSON_CreateObject(); cJSON_AddStringToObject(j,"error",error);
        xSemaphoreGive(mutex); return j;
    }
    unsigned index = cursor++ % HISTORY;
    request_t *r = &history[index]; memset(r, 0, sizeof(*r));
    snprintf(r->id,sizeof(r->id),"%s",id); snprintf(r->action,sizeof(r->action),"%s",action);
    snprintf(r->profile,sizeof(r->profile),"%s",profile);
    r->speed=speed;r->strength=strength;
    r->ble_session=local_session;
    r->argument=argument;r->channel=channel;r->ttl_ms=ttl_ms;r->received=now_ms();r->result=RECEIVED;
    active=index;phase=IDLE;*http_status=202;
    cJSON *j=request_json(r);xSemaphoreGive(mutex);return j;
}
void motion_bridge_ble_session(uint32_t session)
{
    xSemaphoreTake(mutex,portMAX_DELAY);ble_session=session;
    if(pose_owner&&pose_owner!=session){pose_owner=0;pose_deadline=0;pose_wizard.collecting=false;pose_wizard.stage=0;pose_wizard.count=0;pose_wizard.error="ble_session_closed";}
    if(compass_owner&&compass_owner!=session)compass_cancel("ble_session_closed");
    if(active>=0&&history[active].ble_session&&history[active].ble_session!=session){stop_pending=true;++stop_generation;}
    xSemaphoreGive(mutex);
}
cJSON *motion_bridge_submit(const char *id,const char *action,int argument,int channel,const char *profile,int speed,int strength,uint32_t ttl_ms,int *http_status)
{return submit(0,id,action,argument,channel,profile,speed,strength,ttl_ms,http_status);}
cJSON *motion_bridge_submit_ble(uint32_t session,const char *id,const char *action,int repeats,int speed,int strength,int *http_status)
{
    if(!session){*http_status=409;cJSON *j=cJSON_CreateObject();cJSON_AddStringToObject(j,"error","ble_session_closed");return j;}
    return submit(session,id,action,repeats,0,"legacy",speed,strength,2000,http_status);
}
cJSON *motion_bridge_request(const char *id)
{
    xSemaphoreTake(mutex,portMAX_DELAY);cJSON *j=NULL;
    for(int i=0;i<HISTORY;++i) if(history[i].result!=EMPTY&&!strcmp(id,history[i].id)){j=request_json(&history[i]);break;}
    xSemaphoreGive(mutex);return j;
}
static bool sensor_fresh(const dog_sensor_sample *s,int64_t at)
{return sensors_supported&&online()&&at&&s->ready&&now_ms()-at+s->age<1500;}
bool motion_bridge_calibration_active(void)
{
    if(!mutex)return false;
    xSemaphoreTake(mutex,portMAX_DELAY);bool active_calibration=pose_busy()||compass_busy();xSemaphoreGive(mutex);return active_calibration;
}
static const char *compass_heading_status(float *heading)
{
    if(compass_busy())return "calibration_in_progress";
    if(!dog_compass_cal_valid(&compass_saved.cal))return "calibration_required";
    if(!dog_pose_mount_valid(&pose_mount))return "mount_calibration_required";
    if(controller_moving||active>=0)return "motion_in_progress";
    if(!sensor_fresh(&imu_sample,imu_at)||!sensor_fresh(&mag_sample,mag_at))return "sample_unavailable";
    if(mag_sample.range!=compass_saved.cal.range)return "range_changed";
    if(compass_actuators()!=compass_saved.actuator_signature)return "actuator_state_changed";
    int64_t age_imu=now_ms()-imu_at+imu_sample.age,age_mag=now_ms()-mag_at+mag_sample.age;
    if(age_imu>500||age_mag>500||llabs(age_imu-age_mag)>150)return "paired_sample_required";
    int16_t mag[3],acc[3];for(unsigned i=0;i<3;i++){mag[i]=(int16_t)mag_sample.axis[i];acc[i]=(int16_t)imu_sample.axis[i];}
    return dog_compass_heading(&compass_saved.cal,&pose_mount,mag,acc,heading)?"available":"magnetic_interference_or_tilt";
}
static cJSON *compass_json(void)
{
    cJSON *j=cJSON_CreateObject();float heading=0;const char *status=compass_heading_status(&heading);
    cJSON_AddBoolToObject(j,"calibrated",dog_compass_cal_valid(&compass_saved.cal));cJSON_AddBoolToObject(j,"persisted",compass_persisted);
    cJSON_AddBoolToObject(j,"available",!strcmp(status,"available"));cJSON_AddStringToObject(j,"status",status);
    if(!strcmp(status,"available"))cJSON_AddNumberToObject(j,"heading_deg",heading);else cJSON_AddNullToObject(j,"heading_deg");
    cJSON_AddBoolToObject(j,"absolute_reference_verified",false);cJSON_AddStringToObject(j,"reference","local_magnetic_north_not_true_north");
    cJSON_AddBoolToObject(j,"active",compass_busy());cJSON_AddBoolToObject(j,"collecting",compass_busy()&&compass_capture.count<DOG_COMPASS_MAX_SAMPLES);
    cJSON_AddNumberToObject(j,"samples",compass_capture.count);cJSON_AddNumberToObject(j,"minimum_samples",96);cJSON_AddNumberToObject(j,"maximum_samples",DOG_COMPASS_MAX_SAMPLES);
    cJSON_AddNumberToObject(j,"rejected_samples",compass_capture.rejected);cJSON_AddStringToObject(j,"error",compass_capture.error?compass_capture.error:"");
    if(dog_compass_cal_valid(&compass_saved.cal)){cJSON_AddNumberToObject(j,"fit_rms",compass_saved.cal.rms);cJSON_AddNumberToObject(j,"alignment_rms",compass_saved.cal.alignment_rms);}
    return j;
}
static cJSON *compass_for_session(const char *command,uint32_t local_session,int *http_status)
{
    xSemaphoreTake(mutex,portMAX_DELAY);*http_status=200;const char *error=NULL;
    if(command){
        if(local_session&&local_session!=ble_session){error="ble_session_closed";*http_status=409;}
        else if(compass_busy()&&compass_owner!=local_session){error="calibration_owned_by_other_entry";*http_status=409;}
        else if(!strcmp(command,"cancel"))compass_cancel(NULL);
        else if(ota_reserved||rtc_pending||eeprom_pending||active>=0||controller_moving||stop_pending||phase!=IDLE||pose_busy()||wifi_provision_busy()){error="compass_requires_idle";*http_status=409;}
        else if(!strcmp(command,"start")){
            if(!sensor_fresh(&imu_sample,imu_at)||!sensor_fresh(&mag_sample,mag_at)){error="fresh_sensors_required";*http_status=409;}
            else{
                compass_cancel(NULL);memset(&compass_capture,0,sizeof(compass_capture));
                compass_capture.data=heap_caps_calloc(DOG_COMPASS_MAX_SAMPLES,sizeof(dog_compass_sample),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
                if(!compass_capture.data){error="calibration_memory_unavailable";*http_status=503;}
                else{compass_capture.range=mag_sample.range;compass_capture_actuators=compass_actuators();compass_deadline=now_ms()+180000;compass_owner=local_session;*http_status=202;}
            }
        }else if(!strcmp(command,"finish")){
            if(!compass_busy()||!compass_capture.data){error="calibration_not_started";*http_status=409;}
            else{
                compass_record record={0};
                if(!dog_compass_fit(&compass_capture,&record.cal)){error=compass_capture.error;*http_status=422;}
                else{
                    record.actuator_signature=compass_capture_actuators;nvs_handle_t h;esp_err_t err=nvs_open("pavlov_compass",NVS_READWRITE,&h);
                    if(err==ESP_OK){err=nvs_set_blob(h,"cal_v1",&record,sizeof(record));if(err==ESP_OK)err=nvs_commit(h);nvs_close(h);}
                    if(err!=ESP_OK){error="calibration_not_saved";*http_status=503;}
                    else{compass_saved=record;compass_persisted=true;compass_cancel(NULL);}
                }
            }
        }else if(!strcmp(command,"clear")){
            nvs_handle_t h;esp_err_t err=nvs_open("pavlov_compass",NVS_READWRITE,&h);
            if(err==ESP_OK){err=nvs_erase_key(h,"cal_v1");if(err==ESP_ERR_NVS_NOT_FOUND)err=ESP_OK;if(err==ESP_OK)err=nvs_commit(h);nvs_close(h);}
            if(err!=ESP_OK){error="calibration_not_cleared";*http_status=503;}
            else{compass_cancel(NULL);memset(&compass_saved,0,sizeof(compass_saved));compass_persisted=false;}
        }else{error="invalid_compass_command";*http_status=422;}
    }
    cJSON *j=compass_json();if(error){cJSON_DeleteItemFromObjectCaseSensitive(j,"error");cJSON_AddStringToObject(j,"error",error);}xSemaphoreGive(mutex);return j;
}
cJSON *motion_bridge_compass(const char *command,int *status){return compass_for_session(command,0,status);}
cJSON *motion_bridge_compass_ble(uint32_t session,const char *command,int *status){return compass_for_session(command,session,status);}
void motion_bridge_power_warning(bool *low,bool *show_message)
{
    *low=*show_message=false;if(!mutex)return;
    xSemaphoreTake(mutex,portMAX_DELAY);*low=power_fresh()&&power_config.alerts_enabled&&power_monitor.low;*show_message=*low&&now_ms()<power_notice_until;xSemaphoreGive(mutex);
}
static bool power_fields(cJSON *j,const char *const *allowed,unsigned count)
{
    if(!cJSON_IsObject(j))return false;
    for(cJSON *p=j->child;p;p=p->next){
        bool found=false;for(unsigned i=0;i<count;i++)if(p->string&&!strcmp(p->string,allowed[i]))found=true;
        if(!found)return false;
        for(cJSON *q=p->next;q;q=q->next)if(q->string&&!strcmp(p->string,q->string))return false;
    }
    return true;
}
static bool power_integer(cJSON *j,const char *key,unsigned *out)
{
    cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);
    if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||v->valuedouble<0||v->valuedouble>10000||v->valuedouble!=(unsigned)v->valuedouble)return false;
    *out=(unsigned)v->valuedouble;return true;
}
cJSON *motion_bridge_power(cJSON *request,int *http_status)
{
    xSemaphoreTake(mutex,portMAX_DELAY);*http_status=200;const char *error=NULL;
    if(request){
        dog_power_config next=power_config;cJSON *cmd=cJSON_GetObjectItemCaseSensitive(request,"command");
        const char *name=cJSON_IsString(cmd)?cmd->valuestring:"";
        if(ota_reserved||rtc_pending||eeprom_pending||active>=0||controller_moving||phase!=IDLE||stop_pending||pose_busy()||compass_busy()||wifi_provision_busy()){error="power_settings_require_idle";*http_status=409;}
        else if(!strcmp(name,"source")){
            const char *const keys[]={"command","source"};cJSON *v=cJSON_GetObjectItemCaseSensitive(request,"source");
            if(!power_fields(request,keys,2)||!cJSON_IsString(v))error="invalid_power_settings";
            else{
                unsigned source=!strcmp(v->valuestring,"unknown")?DOG_POWER_UNKNOWN:!strcmp(v->valuestring,"usb")?DOG_POWER_USB:!strcmp(v->valuestring,"battery_2s")?DOG_POWER_BATTERY_2S:99;
                if(source>2)error="invalid_power_source";
                else if(next.source!=(dog_power_source)source){next.source=(dog_power_source)source;next.reference_raw=next.reference_mv=next.alerts_enabled=0;}
            }
        }else if(!strcmp(name,"reference")){
            const char *const keys[]={"command","millivolts","reference_verified"};
            if(!power_fields(request,keys,3)||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request,"reference_verified"))||!power_integer(request,"millivolts",&next.reference_mv))error="measured_reference_required";
            else if(next.source!=DOG_POWER_BATTERY_2S||!power_fresh()){error="reference_requires_fresh_battery_sample";*http_status=409;}
            else{next.reference_raw=supply_raw;next.alerts_enabled=0;}
        }else if(!strcmp(name,"alerts")){
            const char *const keys[]={"command","enabled"};cJSON *enabled=cJSON_GetObjectItemCaseSensitive(request,"enabled");
            if(!power_fields(request,keys,2)||!cJSON_IsBool(enabled))error="invalid_power_settings";
            else{next.alerts_enabled=cJSON_IsTrue(enabled);if(next.alerts_enabled&&(!dog_power_calibrated(&next)||next.source!=DOG_POWER_BATTERY_2S)){error="power_reference_required";*http_status=409;}}
        }else if(!strcmp(name,"thresholds")){
            const char *const keys[]={"command","low_mv","recover_mv"};
            if(!power_fields(request,keys,3)||!power_integer(request,"low_mv",&next.low_mv)||!power_integer(request,"recover_mv",&next.recover_mv))error="invalid_power_thresholds";
        }else if(!strcmp(name,"clear")){
            const char *const keys[]={"command"};
            if(!power_fields(request,keys,1))error="invalid_power_settings";
            else next=(dog_power_config){1,DOG_POWER_UNKNOWN,0,0,7000,7200,0};
        }else error="invalid_power_command";
        if(!error&&!dog_power_config_valid(&next))error="invalid_power_reference_or_thresholds";
        if(!error){
            nvs_handle_t h;esp_err_t err=nvs_open("pavlov_power",NVS_READWRITE,&h);
            if(err==ESP_OK){err=nvs_set_blob(h,"config_v1",&next,sizeof(next));if(err==ESP_OK)err=nvs_commit(h);nvs_close(h);}
            if(err!=ESP_OK){error="power_settings_not_saved";*http_status=503;}
            else{
                bool scale_changed=next.source!=power_config.source||next.reference_raw!=power_config.reference_raw||next.reference_mv!=power_config.reference_mv;
                uint32_t previous_settings=power_limit_settings_hash();
                if(scale_changed){memset(&power_history,0,sizeof(power_history));power_limit_observing=false;power_limit_have_observation=false;power_limit_observed_updates=0;last_power_limit_observed=0;}
                power_config=next;power_persisted=true;memset(&power_monitor,0,sizeof(power_monitor));power_notice_until=0;
                if(previous_settings!=power_limit_settings_hash()){power_limit_recovered_since=0;memset(&power_limit_monitor,0,sizeof(power_limit_monitor));}
            }
        }
    }
    cJSON *j=power_json();if(error){if(*http_status==200)*http_status=422;cJSON_AddStringToObject(j,"error",error);}
    xSemaphoreGive(mutex);return j;
}
cJSON *motion_bridge_power_history(void)
{
    xSemaphoreTake(mutex,portMAX_DELAY);uint32_t now=(uint32_t)now_ms();dog_power_history_prune(&power_history,now);
    bool calibrated=dog_power_calibrated(&power_config),eligible=power_fresh()&&active<0&&phase==IDLE&&!controller_moving&&!stop_pending&&!pose_busy()&&!compass_busy()&&!wifi_provision_busy();
    int delta=0;const char *trend=dog_power_history_trend(&power_history,eligible,power_config.source==DOG_POWER_BATTERY_2S,calibrated,now,&delta);
    cJSON *j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"calibrated",calibrated);cJSON_AddBoolToObject(j,"recording_eligible",eligible);
    cJSON_AddStringToObject(j,"source",power_config.source==DOG_POWER_USB?"usb":power_config.source==DOG_POWER_BATTERY_2S?"battery_2s":"unknown");
    cJSON_AddStringToObject(j,"voltage_method",calibrated?"user_measured_reference":"uncalibrated_nominal_estimate");
    cJSON_AddStringToObject(j,"trend",trend);cJSON_AddBoolToObject(j,"battery_terminal_measurement",false);
    if(!strcmp(trend,"rising")||!strcmp(trend,"falling")||!strcmp(trend,"stable"))cJSON_AddNumberToObject(j,"delta_mv",delta);else cJSON_AddNullToObject(j,"delta_mv");
    cJSON_AddNumberToObject(j,"window_ms",DOG_POWER_HISTORY_WINDOW_MS);cJSON_AddNumberToObject(j,"sample_interval_ms",10000);
    cJSON *samples=cJSON_AddArrayToObject(j,"samples");
    for(unsigned i=0;i<power_history.count;i++){const dog_power_history_point *p=dog_power_history_at(&power_history,i);cJSON *point=cJSON_CreateArray();cJSON_AddItemToArray(point,cJSON_CreateNumber((uint32_t)(now-p->at)));cJSON_AddItemToArray(point,cJSON_CreateNumber(p->millivolts));cJSON_AddItemToArray(samples,point);}
    xSemaphoreGive(mutex);return j;
}
cJSON *motion_bridge_power_limit(cJSON *request,int *status)
{
    xSemaphoreTake(mutex,portMAX_DELAY);*status=200;const char *error=NULL;
    bool source_ready=power_limit_source_ready(),fresh=power_fresh(),matches=source_ready&&power_limit.settings_hash==power_limit_settings_hash();unsigned mv=power_voltage();
    bool idle=!ota_reserved&&!rtc_pending&&!eeprom_pending&&active<0&&!controller_moving&&!stop_pending&&phase==IDLE&&!pose_busy()&&!compass_busy()&&!wifi_provision_busy();
    if(request){
        cJSON *name=cJSON_GetObjectItemCaseSensitive(request,"command");const char *cmd=cJSON_IsString(name)?name->valuestring:NULL;
        if(!idle){error="power_limit_requires_idle";*status=409;}
        else if(cmd&&!strcmp(cmd,"observe")){
            const char *const keys[]={"command","enabled"};cJSON *v=cJSON_GetObjectItemCaseSensitive(request,"enabled");
            if(!power_fields(request,keys,2)||!cJSON_IsBool(v))error="invalid_power_limit_settings";
            else if(power_limit.enabled){error="power_limit_disable_first";*status=409;}
            else if(cJSON_IsTrue(v)&&(!source_ready||!fresh||mv<5800||mv>9000)){error="power_limit_reference_required";*status=409;}
            else{power_limit_observing=cJSON_IsTrue(v);power_limit_have_observation=false;power_limit_observed_updates=0;last_power_limit_observed=0;}
        }else if(cmd&&!strcmp(cmd,"clear")){
            const char *const keys[]={"command","supply_confirmed"};
            if(!power_fields(request,keys,2)||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request,"supply_confirmed")))error="supply_confirmation_required";
            else if(!power_limit.enabled||!matches){error="power_limit_reconfirm_required";*status=409;}
            else if(!power_limit_recovered()){error="power_limit_recovery_required";*status=409;}
            else{power_limit_latched=false;power_limit_reason="";memset(&power_limit_monitor,0,sizeof(power_limit_monitor));}
        }else if(cmd&&(!strcmp(cmd,"enable")||!strcmp(cmd,"disable"))){
            bool enable=!strcmp(cmd,"enable");const char *const enable_keys[]={"command","acceptance_confirmed","expected_settings"},*const disable_keys[]={"command"};
            cJSON *expected=cJSON_GetObjectItemCaseSensitive(request,"expected_settings");
            if(!power_fields(request,enable?enable_keys:disable_keys,enable?3:1))error="invalid_power_limit_settings";
            else if(enable&&!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request,"acceptance_confirmed")))error="measured_load_acceptance_required";
            else if(enable&&(!cJSON_IsNumber(expected)||!isfinite(expected->valuedouble)||expected->valuedouble<1||expected->valuedouble>UINT32_MAX||expected->valuedouble!=(uint32_t)expected->valuedouble||(uint32_t)expected->valuedouble!=power_limit_settings_hash())){error="power_limit_settings_changed";*status=409;}
            else if(enable&&(!source_ready||!fresh)){error="power_limit_reference_required";*status=409;}
            else if(enable&&!power_limit_recovered()){error="power_limit_recovery_required";*status=409;}
            else{
                power_limit_config next={1,enable,enable,enable?power_limit_settings_hash():0};nvs_handle_t h;esp_err_t err=nvs_open("pavlov_guard",NVS_READWRITE,&h);
                if(err==ESP_OK){err=nvs_set_blob(h,"power_v1",&next,sizeof(next));if(err==ESP_OK)err=nvs_commit(h);nvs_close(h);}
                if(err!=ESP_OK){error="power_limit_not_saved";*status=503;}
                else{power_limit=next;power_limit_persisted=true;power_limit_observing=false;power_limit_latched=false;power_limit_reason="";memset(&power_limit_monitor,0,sizeof(power_limit_monitor));}
            }
        }else error="invalid_power_limit_settings";
        if(error&&*status==200)*status=422;
    }
    matches=source_ready&&power_limit.settings_hash==power_limit_settings_hash();
    cJSON *j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"enabled",power_limit.enabled!=0);cJSON_AddBoolToObject(j,"observing",power_limit_observing);cJSON_AddBoolToObject(j,"persisted",power_limit_persisted);
    cJSON_AddNumberToObject(j,"settings_id",power_limit_settings_hash());
    cJSON_AddBoolToObject(j,"latched",power_limit_latched);cJSON_AddStringToObject(j,"reason",power_limit_reason);cJSON_AddBoolToObject(j,"source_reference_ready",source_ready);cJSON_AddBoolToObject(j,"sample_fresh",fresh);
    cJSON_AddBoolToObject(j,"settings_match_acceptance",matches);cJSON_AddBoolToObject(j,"recovered_ready",power_limit_recovered());
    cJSON_AddNumberToObject(j,"low_mv",power_config.low_mv);cJSON_AddNumberToObject(j,"recover_mv",power_config.recover_mv);
    if(fresh)cJSON_AddNumberToObject(j,"millivolts",mv);else cJSON_AddNullToObject(j,"millivolts");
    cJSON_AddNumberToObject(j,"observed_updates",power_limit_observed_updates);
    if(power_limit_have_observation){cJSON_AddNumberToObject(j,"minimum_observed_mv",power_limit_min_mv);cJSON_AddNumberToObject(j,"maximum_observed_mv",power_limit_max_mv);}
    else{cJSON_AddNullToObject(j,"minimum_observed_mv");cJSON_AddNullToObject(j,"maximum_observed_mv");}
    cJSON_AddBoolToObject(j,"acceptance_confirmed_by_user",power_limit.acceptance_confirmed!=0);cJSON_AddBoolToObject(j,"physical_acceptance_verified_by_device",false);cJSON_AddBoolToObject(j,"interrupts_running_motion",false);
    if(error)cJSON_AddStringToObject(j,"error",error);
    xSemaphoreGive(mutex);return j;
}
bool motion_bridge_power_limit_warning(void)
{
    if(!mutex)return false;
    xSemaphoreTake(mutex,portMAX_DELAY);bool warning=power_limit.enabled&&power_limit_latched;xSemaphoreGive(mutex);return warning;
}
cJSON *motion_bridge_pose_protection(cJSON *request,int *status)
{
    xSemaphoreTake(mutex,portMAX_DELAY);*status=200;const char *error=NULL;
    bool calibrated=dog_pose_mount_valid(&pose_mount),fresh=pose_guard_sample_fresh(),upright=fresh&&posture_tracker.state==DOG_POSTURE_UPRIGHT;
    bool idle=!ota_reserved&&!rtc_pending&&!eeprom_pending&&active<0&&!controller_moving&&!stop_pending&&phase==IDLE&&!pose_busy()&&!compass_busy()&&!wifi_provision_busy();
    if(request){
        const char *cmd=NULL;cJSON *name=cJSON_GetObjectItemCaseSensitive(request,"command");if(cJSON_IsString(name))cmd=name->valuestring;
        if(!idle){error="pose_protection_requires_idle";*status=409;}
        else if(cmd&&!strcmp(cmd,"observe")){
            const char *const keys[]={"command","enabled"};cJSON *value=cJSON_GetObjectItemCaseSensitive(request,"enabled");
            if(!power_fields(request,keys,2)||!cJSON_IsBool(value))error="invalid_pose_protection_settings";
            else if(pose_guard.enabled){error="pose_protection_disable_first";*status=409;}
            else if(cJSON_IsTrue(value)&&(!calibrated||!fresh)){error="pose_protection_requires_calibrated_upright";*status=409;}
            else{pose_guard_observing=cJSON_IsTrue(value);pose_guard_observed_tip=false;}
        }else if(cmd&&!strcmp(cmd,"clear")){
            const char *const keys[]={"command","upright_confirmed"};
            if(!power_fields(request,keys,2)||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request,"upright_confirmed")))error="upright_confirmation_required";
            else if(!pose_guard.enabled||!calibrated||pose_guard.mount_hash!=pose_guard_mount_hash()){error="pose_protection_reconfirm_required";*status=409;}
            else if(!upright){error="pose_protection_not_upright";*status=409;}
            else{pose_guard_latched=false;pose_guard_reason="";pose_guard_last_good=imu_at-imu_sample.age;}
        }else if(cmd&&(!strcmp(cmd,"enable")||!strcmp(cmd,"disable"))){
            bool enable=!strcmp(cmd,"enable");const char *const enable_keys[]={"command","acceptance_confirmed"},*const disable_keys[]={"command"};
            if(!power_fields(request,enable?enable_keys:disable_keys,enable?2:1))error="invalid_pose_protection_settings";
            else if(enable&&!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request,"acceptance_confirmed")))error="physical_acceptance_required";
            else if(enable&&(!calibrated||!upright)){error="pose_protection_requires_calibrated_upright";*status=409;}
            else{
                pose_guard_config next={1,enable,enable,enable?pose_guard_mount_hash():0};nvs_handle_t h;esp_err_t err=nvs_open("pavlov_guard",NVS_READWRITE,&h);
                if(err==ESP_OK){err=nvs_set_blob(h,"pose_v1",&next,sizeof(next));if(err==ESP_OK)err=nvs_commit(h);nvs_close(h);}
                if(err!=ESP_OK){error="pose_protection_not_saved";*status=503;}
                else{pose_guard=next;pose_guard_persisted=true;pose_guard_observing=false;pose_guard_latched=false;pose_guard_reason="";pose_guard_last_good=enable?imu_at-imu_sample.age:0;}
            }
        }else error="invalid_pose_protection_settings";
        if(error&&*status==200)*status=422;
    }
    cJSON *j=cJSON_CreateObject();cJSON_AddBoolToObject(j,"enabled",pose_guard.enabled!=0);cJSON_AddBoolToObject(j,"persisted",pose_guard_persisted);
    cJSON_AddBoolToObject(j,"observing",pose_guard_observing);cJSON_AddBoolToObject(j,"observed_side_or_inverted_during_motion",pose_guard_observed_tip);
    cJSON_AddBoolToObject(j,"latched",pose_guard_latched);cJSON_AddStringToObject(j,"reason",pose_guard_reason);cJSON_AddNumberToObject(j,"stop_requests",pose_guard_stops);
    cJSON_AddBoolToObject(j,"mount_calibrated",calibrated);cJSON_AddBoolToObject(j,"sensor_fresh",fresh);cJSON_AddBoolToObject(j,"upright",upright);
    cJSON_AddBoolToObject(j,"calibration_matches_acceptance",calibrated&&pose_guard.mount_hash==pose_guard_mount_hash());
    cJSON_AddBoolToObject(j,"acceptance_confirmed_by_user",pose_guard.acceptance_confirmed!=0);cJSON_AddBoolToObject(j,"physical_acceptance_verified_by_device",false);
    cJSON_AddBoolToObject(j,"controller_stop_reported",online()&&!controller_moving&&!stop_pending);cJSON_AddBoolToObject(j,"physical_stop_verified",false);
    if(error)cJSON_AddStringToObject(j,"error",error);
    xSemaphoreGive(mutex);return j;
}
bool motion_bridge_pose_protection_warning(void)
{
    if(!mutex)return false;
    xSemaphoreTake(mutex,portMAX_DELAY);bool warning=pose_guard.enabled&&pose_guard_latched;xSemaphoreGive(mutex);return warning;
}
dog_posture motion_bridge_posture(void)
{
    if(!mutex)return DOG_POSTURE_UNKNOWN;
    xSemaphoreTake(mutex,portMAX_DELAY);dog_posture result=!pose_busy()&&sensor_fresh(&imu_sample,imu_at)&&dog_pose_mount_valid(&pose_mount)?posture_tracker.state:DOG_POSTURE_UNKNOWN;
    xSemaphoreGive(mutex);return result;
}
bool motion_bridge_handling_hint(void)
{
    if(!mutex)return false;
    xSemaphoreTake(mutex,portMAX_DELAY);
    bool hint=!handling_blocked()&&sensor_fresh(&imu_sample,imu_at)&&now_ms()-imu_at+imu_sample.age<900&&dog_handling_hint(&handling_tracker,(uint32_t)now_ms());
    xSemaphoreGive(mutex);return hint;
}
static cJSON *pose_for_session(const char *command,uint32_t local_session,int *http_status)
{
    xSemaphoreTake(mutex,portMAX_DELAY);*http_status=200;
    if(command){
        if((local_session&&local_session!=ble_session)||(pose_busy()&&pose_owner!=local_session)){*http_status=409;}
        else if(!strcmp(command,"cancel")){pose_owner=0;pose_deadline=0;pose_wizard.collecting=false;pose_wizard.stage=0;pose_wizard.count=0;pose_wizard.error=NULL;}
        else{
            unsigned step=!strcmp(command,"level")?0:!strcmp(command,"forward")?1:!strcmp(command,"left")?2:99;
            if(step>2){*http_status=422;}
            else if(step&&!pose_busy()){*http_status=409;}
            else if(ota_reserved||rtc_pending||eeprom_pending||!sensor_fresh(&imu_sample,imu_at)||active>=0||controller_moving||stop_pending||phase!=IDLE||compass_busy()||wifi_provision_busy()){*http_status=409;}
            else if(!dog_pose_begin(&pose_wizard,step)){*http_status=409;}
            else{pose_deadline=now_ms()+600000;pose_owner=local_session;*http_status=202;}
        }
    }
    cJSON *j=pose_calibration_json();
    if(*http_status>=400)cJSON_AddStringToObject(j,"request_error",*http_status==422?"invalid_calibration_command":"calibration_not_ready");
    xSemaphoreGive(mutex);return j;
}
cJSON *motion_bridge_pose_calibration(const char *command,int *status){return pose_for_session(command,0,status);}
cJSON *motion_bridge_pose_calibration_ble(uint32_t session,const char *command,int *status){return pose_for_session(command,session,status);}
static cJSON *sensor_json(const dog_sensor_sample *s,int64_t at,bool imu)
{
    cJSON *j=cJSON_CreateObject();bool available=sensor_fresh(s,at);
    cJSON_AddBoolToObject(j,"supported",sensors_supported);cJSON_AddBoolToObject(j,"available",available);
    cJSON_AddBoolToObject(j,"calibrated",false);cJSON_AddStringToObject(j,"frame","sensor_xyz");
    cJSON_AddNumberToObject(j,"chip_id",s->id);cJSON_AddNumberToObject(j,"sequence",s->sequence);
    cJSON_AddNumberToObject(j,"age_ms",at?now_ms()-at+s->age:-1);cJSON_AddNumberToObject(j,"error",s->error);
    cJSON *raw=cJSON_AddArrayToObject(j,"raw");
    if(available)for(unsigned i=0;i<(imu?6u:3u);i++)cJSON_AddItemToArray(raw,cJSON_CreateNumber(s->axis[i]));
    if(imu){
        float x=s->axis[0]/4096.f,y=s->axis[1]/4096.f,z=s->axis[2]/4096.f;
        float norm=sqrtf(x*x+y*y+z*z);bool gravity=available&&norm>.75f&&norm<1.25f;
        cJSON_AddBoolToObject(j,"gravity_tilt_available",gravity);
        cJSON_AddStringToObject(j,"tilt_method","uncalibrated_accelerometer_gravity");
        if(gravity){cJSON_AddNumberToObject(j,"roll_deg",atan2f(y,z)*57.2957795f);cJSON_AddNumberToObject(j,"pitch_deg",atan2f(-x,sqrtf(y*y+z*z))*57.2957795f);}
        else{cJSON_AddNullToObject(j,"roll_deg");cJSON_AddNullToObject(j,"pitch_deg");}
        if(available){cJSON_AddNumberToObject(j,"acceleration_g",norm);cJSON_AddNumberToObject(j,"temperature_c",s->temperature/256.f);}
    }else{
        cJSON_AddNumberToObject(j,"range_gauss",s->range);cJSON_AddNumberToObject(j,"status",s->status);
        float heading=0;const char *status=s->error==4?"magnetic_overflow":compass_heading_status(&heading);
        if(!strcmp(status,"available"))cJSON_AddNumberToObject(j,"heading_deg",heading);else cJSON_AddNullToObject(j,"heading_deg");
        cJSON_AddStringToObject(j,"heading_status",status);
    }
    return j;
}
cJSON *motion_bridge_state(void)
{
    xSemaphoreTake(mutex,portMAX_DELAY);
    cJSON *j=cJSON_CreateObject();
    cJSON_AddBoolToObject(j,"network_online",network_online);
    cJSON_AddBoolToObject(j,"calibration_active",pose_busy()||compass_busy());
    cJSON_AddStringToObject(j,"controller_version",controller_version);
    cJSON_AddBoolToObject(j,"controller_online",online());
    cJSON_AddNumberToObject(j,"controller_status_age_ms",status_at?now_ms()-status_at:-1);
    cJSON_AddBoolToObject(j,"baseline_set",controller_baseline);
    cJSON_AddBoolToObject(j,"armed",controller_armed);
    cJSON_AddBoolToObject(j,"controller_moving",controller_moving);
    cJSON_AddBoolToObject(j,"stop_pending",stop_pending);
    cJSON *supply=cJSON_AddObjectToObject(j,"supply");
    bool fresh=power_fresh();unsigned displayed_mv=power_voltage();
    cJSON_AddBoolToObject(supply,"supported",supply_supported);
    cJSON_AddBoolToObject(supply,"available",fresh);cJSON_AddBoolToObject(supply,"calibrated",dog_power_calibrated(&power_config));
    cJSON_AddStringToObject(supply,"measurement","switched_supply_pc3_adc1");
    cJSON_AddBoolToObject(supply,"battery_terminal_measurement",false);
    cJSON_AddNullToObject(supply,"battery_percent");
    cJSON_AddNumberToObject(supply,"age_ms",supply_at?now_ms()-supply_at:-1);
    cJSON_AddStringToObject(supply,"classification",fresh?dog_supply_classification(displayed_mv):"unavailable");
    if(fresh){cJSON_AddNumberToObject(supply,"raw",supply_raw);cJSON_AddNumberToObject(supply,"millivolts",displayed_mv);}
    else {cJSON_AddNullToObject(supply,"raw");cJSON_AddNullToObject(supply,"millivolts");}
    bool ref_fresh=sensors_supported&&online()&&reference_at&&now_ms()-reference_at<3000&&reference_ready&&reference_raw>0;
    cJSON_AddBoolToObject(supply,"reference_available",ref_fresh);
    if(ref_fresh){cJSON_AddNumberToObject(supply,"reference_raw",reference_raw);cJSON_AddNumberToObject(supply,"vdda_estimate_mv",1200u*4095u/reference_raw);}
    cJSON_AddStringToObject(supply,"reference_method","nominal_1200mv_bandgap_not_factory_calibration");
    cJSON_AddItemToObject(supply,"monitor",power_json());
    cJSON *sensors=cJSON_AddObjectToObject(j,"sensors");
    cJSON_AddItemToObject(sensors,"imu",sensor_json(&imu_sample,imu_at,true));
    cJSON_AddItemToObject(sensors,"magnetometer",sensor_json(&mag_sample,mag_at,false));
    cJSON_AddItemToObject(sensors,"compass",compass_json());
    cJSON *pose=cJSON_AddObjectToObject(sensors,"pose");float roll=0,pitch=0,vertical=0;
    float acceleration[3];for(unsigned i=0;i<3;i++)acceleration[i]=imu_sample.axis[i]/4096.f;
    bool body=sensor_fresh(&imu_sample,imu_at)&&dog_pose_angles(&pose_mount,acceleration,&roll,&pitch,&vertical);
    cJSON_AddBoolToObject(pose,"available",body);cJSON_AddBoolToObject(pose,"mount_calibrated",dog_pose_mount_valid(&pose_mount));
    cJSON_AddStringToObject(pose,"frame","body_x_front_y_left_z_up");cJSON_AddBoolToObject(pose,"motion_protection_enabled",pose_guard.enabled!=0);
    cJSON_AddStringToObject(pose,"posture",dog_posture_name(body?posture_tracker.state:DOG_POSTURE_UNKNOWN));
    cJSON_AddBoolToObject(pose,"handling_feedback_supported",true);
    cJSON_AddBoolToObject(pose,"handling_hint",!handling_blocked()&&body&&now_ms()-imu_at+imu_sample.age<900&&dog_handling_hint(&handling_tracker,(uint32_t)now_ms()));
    cJSON_AddBoolToObject(pose,"handling_is_pickup_detection",false);
    if(body){cJSON_AddNumberToObject(pose,"roll_deg",roll);cJSON_AddNumberToObject(pose,"pitch_deg",pitch);cJSON_AddNumberToObject(pose,"upright_cosine",vertical);}
    else{cJSON_AddNullToObject(pose,"roll_deg");cJSON_AddNullToObject(pose,"pitch_deg");}
    cJSON_AddItemToObject(pose,"calibration",pose_calibration_json());
    cJSON_AddNumberToObject(j,"tail_period_us",tail_period);
    cJSON *array=cJSON_AddArrayToObject(j,"pulse_us");
    for(int i=0;i<5;++i)cJSON_AddItemToArray(array,cJSON_CreateNumber(pulses[i]));
    if(active>=0)cJSON_AddItemToObject(j,"active_request",request_json(&history[active]));
    if(last_error[0])cJSON_AddStringToObject(j,"last_error",last_error);
    cJSON_AddBoolToObject(j,"physical_position_verified",false);
    xSemaphoreGive(mutex);return j;
}

bool motion_bridge_controller_uart_acquire(void)
{
    xSemaphoreTake(mutex,portMAX_DELAY);bool ok=ota_reserved&&!controller_uart_requested;
    if(ok){controller_uart_requested=true;}
    xSemaphoreGive(mutex);
    if(!ok)return false;
    for(unsigned i=0;i<200;i++){vTaskDelay(1);xSemaphoreTake(mutex,portMAX_DELAY);bool ready=controller_uart_owned;xSemaphoreGive(mutex);if(ready)return true;}
    xSemaphoreTake(mutex,portMAX_DELAY);controller_uart_requested=false;xSemaphoreGive(mutex);return false;
}
void motion_bridge_controller_uart_release(void)
{
    xSemaphoreTake(mutex,portMAX_DELAY);controller_uart_requested=false;status_at=0;stop_pending=true;++stop_generation;xSemaphoreGive(mutex);
}
