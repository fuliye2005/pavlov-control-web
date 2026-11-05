#pragma once
#include "voice_sequence.h"
/* Validate model JSON after the caller has validated/removed emotion.
 * Reject unknown/duplicate fields, nested plans, unsupported actions and
 * excessive counts/durations before any command can reach the controller. */
bool voice_plan_parse(cJSON *json,voice_intent *intent,voice_sequence *sequence);
