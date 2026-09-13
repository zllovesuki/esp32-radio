#pragma once
#include <stdbool.h>
#include "cJSON.h"
void radio_init(void);
bool radio_network_ready(void);
void radio_command(const cJSON *command);
void radio_emit(cJSON *message);
int radio_set_led(unsigned r, unsigned g, unsigned b);
