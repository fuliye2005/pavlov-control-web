#pragma once
#include <stdbool.h>
#include "cJSON.h"
void cloud_dialog_init(void);
void cloud_dialog_network(bool online);
bool cloud_dialog_command(const char *command,const char *text,bool execute_motion);
bool cloud_dialog_can_update(void);
cJSON *cloud_dialog_state(void);
void cloud_dialog_display_status(bool *wifi,bool *listening,int *status);
typedef struct {bool wifi,listening;int error;unsigned turns;char stage[32],reply[256],emotion[16];} cloud_display_snapshot;
void cloud_dialog_display_snapshot(cloud_display_snapshot *snapshot);
int cloud_dialog_change_mode(const char *mode);
int cloud_dialog_profile_update(cJSON *patch);
int cloud_dialog_memory_update(cJSON *patch);
int cloud_dialog_set_transport(bool tcp_nodelay,int frame_interval_ms);
