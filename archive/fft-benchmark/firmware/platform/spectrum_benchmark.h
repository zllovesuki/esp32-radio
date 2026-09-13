#pragma once
#include <stddef.h>
#include <stdint.h>

/* Shadow analysis only. Never sends media or changes playback/LED state. */
void spectrum_benchmark_start(void);
void spectrum_benchmark_push(uint32_t pts, const uint8_t *opus, size_t size);
