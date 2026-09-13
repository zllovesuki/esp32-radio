#pragma once
#include <stdbool.h>
#include "cJSON.h"

// Returns true for private SDP/candidate events that must not be printed.
bool signaling_observe(const cJSON *event);
void signaling_start(void);
