#pragma once
#include "cJSON.h"
void probe_emit(cJSON *message);
void probe_command(const cJSON *command);
void probe_set_led(unsigned red, unsigned green, unsigned blue);
