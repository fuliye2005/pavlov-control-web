#pragma once
#include <stdbool.h>
#include "cJSON.h"
typedef struct {
    char action[24],reply[401];
    int repeats,speed,strength;
    bool local,expression;
} voice_intent;
void voice_intent_clear(voice_intent *intent);
bool voice_intent_parse(cJSON *object,voice_intent *intent);
bool voice_intent_local(const char *text,voice_intent *intent);
bool voice_intent_named(const char *text);
bool voice_intent_locomotion(const char *action);
bool voice_intent_denied(const char *text);
const char *voice_intent_label(const char *action);
cJSON *voice_intent_json(const voice_intent *intent);
void voice_reply_no_task_prompt(char *reply,size_t capacity);
