#pragma once
#include "voice_intent.h"
#define VOICE_SEQUENCE_MAX 6
#define VOICE_SEQUENCE_MAX_MS 30000u
typedef struct {unsigned count,duration_ms;voice_intent steps[VOICE_SEQUENCE_MAX];} voice_sequence;
/* NONE delegates a single command/chat to the existing pipeline. INVALID
 * never permits partial execution. Parsing does not issue hardware requests. */
enum {VOICE_SEQUENCE_NONE,VOICE_SEQUENCE_VALID,VOICE_SEQUENCE_INVALID};
int voice_sequence_parse(const char *text,voice_sequence *out);
