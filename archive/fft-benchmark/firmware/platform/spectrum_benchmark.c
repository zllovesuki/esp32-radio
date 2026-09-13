/* A bounded shadow workload for measuring Opus decode + FFT during WebRTC.
 * Built only by build.py into an isolated copy of the normal firmware.
 * All codec/DSP state belongs to one worker pinned to CPU1. The radio only
 * copies compressed packets into a nonblocking queue; no Rust pointers survive.
 */
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spectrum_benchmark.h"
#include "dsps_fft2r.h"
#include "esp_audio_dec.h"
#include "esp_opus_dec.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define MAX_FFT 2048
#define PHASE_FRAMES 2500
#define QUEUE_DEPTH 8
#define SAMPLE_RATE 48000
#define PI_F 3.14159265358979323846f

typedef struct {
    uint32_t pts, size;
    uint64_t queued_us;
    uint8_t bytes[1275];
} packet;
typedef struct {
    uint64_t sum;
    uint32_t count, max;
    uint32_t histogram[401]; /* 50 us bins; final bin includes >=20 ms. */
} timing;
typedef struct {
    void *decoder;
    float ring[MAX_FFT], window[MAX_FFT];
    int16_t pcm[960 * 2];
    packet incoming;
    timing decode, float_fft, fixed_fft, analysis, latency;
    uint32_t frames, transforms, errors, gaps, over_budget, position, filled, previous_pts;
    uint32_t heap_min, nonzero;
    float window_sum;
} state;

static QueueHandle_t packets;
static atomic_bool accepting;
static atomic_uint dropped, radio_cores;
static atomic_int radio_affinity;
static float *fft;
static int16_t *fixed_fft;

static uint64_t now_us(void) { return esp_timer_get_time(); }
static uint32_t heap_free(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

static void record(timing *t, uint64_t elapsed)
{
    uint32_t us = elapsed > UINT32_MAX ? UINT32_MAX : elapsed;
    t->sum += us;
    t->count++;
    if (us > t->max) t->max = us;
    t->histogram[us / 50 < 400 ? us / 50 : 400]++;
}

static unsigned percentile95(const timing *t)
{
    unsigned remaining = (t->count * 95 + 99) / 100;
    for (unsigned i = 0; i < 401; i++) {
        if (t->histogram[i] >= remaining) return i == 400 ? t->max : (i + 1) * 50;
        remaining -= t->histogram[i];
    }
    return 20050;
}

static void print_timing(const char *name, const timing *t)
{
    printf("\"%s\":{\"mean_us\":%lu,\"p95_bin_upper_us\":%u,\"max_us\":%lu}", name,
           (unsigned long)(t->count ? t->sum / t->count : 0), percentile95(t), (unsigned long)t->max);
}

static void set_window(state *s, unsigned n)
{
    s->window_sum = 0;
    for (unsigned i = 0; i < n; i++) {
        s->window[i] = 0.5f - 0.5f * cosf(2 * PI_F * i / n);
        s->window_sum += s->window[i];
    }
}

static void self_test(state *s)
{
    for (unsigned n = 1024; n <= MAX_FFT; n *= 2) {
        set_window(s, n);
        unsigned expected = n / 32;
        for (unsigned i = 0; i < n; i++) {
            float sample = 0.5f * sinf(2 * PI_F * expected * i / n) * s->window[i];
            fft[2 * i] = sample; fft[2 * i + 1] = 0;
            fixed_fft[2 * i] = lrintf(sample * 32767); fixed_fft[2 * i + 1] = 0;
        }
        ESP_ERROR_CHECK(dsps_fft2r_fc32(fft, n));
        ESP_ERROR_CHECK(dsps_bit_rev_fc32(fft, n));
        ESP_ERROR_CHECK(dsps_fft2r_sc16(fixed_fft, n));
        ESP_ERROR_CHECK(dsps_bit_rev_sc16_ansi(fixed_fft, n));
        float peak = 0; int64_t fixed_peak = 0;
        unsigned float_bin = 0, fixed_bin = 0;
        for (unsigned k = 1; k < n / 2; k++) {
            float power = fft[2 * k] * fft[2 * k] + fft[2 * k + 1] * fft[2 * k + 1];
            int32_t real = fixed_fft[2 * k], imag = fixed_fft[2 * k + 1];
            int64_t fixed_power = (int64_t)real * real + (int64_t)imag * imag;
            if (power > peak) { peak = power; float_bin = k; }
            if (fixed_power > fixed_peak) { fixed_peak = fixed_power; fixed_bin = k; }
        }
        float amplitude = sqrtf(peak) * 2 / s->window_sum;
        bool passed = float_bin == expected && fixed_bin == expected && fabsf(amplitude - 0.5f) < 0.002f;
        printf("FFT_SELF_TEST {\"n\":%u,\"float_bin\":%u,\"fixed_bin\":%u,\"amplitude\":%.5f,\"passed\":%s}\n",
               n, float_bin, fixed_bin, amplitude, passed ? "true" : "false");
        if (!passed) abort();
    }
}

static void bands(state *s, unsigned n, uint8_t out[32])
{
    const float norm = 4 / (s->window_sum * s->window_sum);
    for (unsigned band = 0; band < 32; band++) {
        float low = 30 * powf(20000.0f / 30, band / 32.0f);
        float high = 30 * powf(20000.0f / 30, (band + 1) / 32.0f);
        unsigned first = (unsigned)ceilf(low * n / SAMPLE_RATE);
        unsigned end = (unsigned)ceilf(high * n / SAMPLE_RATE);
        if (first == end) { first = lroundf((low + high) * n / (2 * SAMPLE_RATE)); end = first + 1; }
        if (first < 1) first = 1;
        float power = 1e-12f;
        for (unsigned k = first; k < end && k < n / 2; k++) {
            float value = (fft[2 * k] * fft[2 * k] + fft[2 * k + 1] * fft[2 * k + 1]) * norm;
            if (value > power) power = value;
        }
        float level = (10 * log10f(power) + 72) * (255.0f / 66);
        out[band] = (uint8_t)fminf(255, fmaxf(0, level));
    }
}

static void summarize(state *s, unsigned n)
{
    printf("FFT_RESULT {\"n\":%u,\"sample_rate\":48000,\"channels\":2,\"frames\":%lu,\"transforms\":%lu,",
           n, (unsigned long)s->frames, (unsigned long)s->transforms);
    print_timing("decode", &s->decode); printf(",");
    print_timing("float_fft_and_reorder", &s->float_fft); printf(",");
    print_timing("fixed_fft_and_reorder", &s->fixed_fft); printf(",");
    print_timing("window_both_ffts_and_bands", &s->analysis); printf(",");
    print_timing("queue_to_done", &s->latency);
    printf(",\"decode_errors\":%lu,\"pts_gaps\":%lu,\"dropped\":%u,\"over_20ms\":%lu,\"heap_min\":%lu,"
           "\"stack_free\":%u,\"worker_core\":%d,\"radio_cores_mask\":%u,\"radio_affinity\":%d,\"nonzero_frames\":%lu}\n",
           (unsigned long)s->errors, (unsigned long)s->gaps, atomic_load(&dropped), (unsigned long)s->over_budget,
           (unsigned long)s->heap_min, (unsigned)uxTaskGetStackHighWaterMark(NULL), xPortGetCoreID(),
           atomic_load(&radio_cores), atomic_load(&radio_affinity), (unsigned long)s->nonzero);
}

static void run(void *unused)
{
    state *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    fft = heap_caps_aligned_alloc(16, 2 * MAX_FFT * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    fixed_fft = heap_caps_aligned_alloc(16, 2 * MAX_FFT * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s || !fft || !fixed_fft) abort();
    uint32_t before = heap_free();
    esp_opus_dec_cfg_t config = {.sample_rate = SAMPLE_RATE, .channel = 2,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS, .self_delimited = false};
    ESP_ERROR_CHECK(esp_opus_dec_open(&config, sizeof(config), &s->decoder));
    uint32_t codec_internal = before - heap_free();
    ESP_ERROR_CHECK(dsps_fft2r_init_fc32(NULL, MAX_FFT));
    ESP_ERROR_CHECK(dsps_fft2r_init_sc16(NULL, MAX_FFT));
    self_test(s);
    printf("FFT_READY {\"codec_version\":\"2.6.2\",\"codec_internal_bytes\":%lu,\"heap_free\":%lu,\"core\":%d}\n",
           (unsigned long)codec_internal, (unsigned long)heap_free(), xPortGetCoreID());
    atomic_store(&accepting, true);
    for (unsigned n = 1024; n <= MAX_FFT; n *= 2) {
        set_window(s, n);
        memset(&s->decode, 0, sizeof(timing) * 5);
        s->frames = s->transforms = s->errors = s->gaps = s->over_budget = s->nonzero = 0;
        s->heap_min = heap_free();
        while (s->frames < PHASE_FRAMES) {
            if (!xQueueReceive(packets, &s->incoming, portMAX_DELAY)) continue;
            packet *p = &s->incoming;
            if (s->frames && p->pts != s->previous_pts + 20) {
                s->gaps++;
                ESP_ERROR_CHECK(esp_opus_dec_reset(s->decoder));
                s->filled = s->position = 0;
                memset(s->ring, 0, sizeof(s->ring));
            }
            s->previous_pts = p->pts;
            esp_audio_dec_in_raw_t input = {.buffer = p->bytes, .len = p->size};
            esp_audio_dec_out_frame_t output = {.buffer = (uint8_t *)s->pcm, .len = sizeof(s->pcm)};
            esp_audio_dec_info_t info = {0};
            uint64_t start = now_us();
            int err = esp_opus_dec_decode(s->decoder, &input, &output, &info);
            record(&s->decode, now_us() - start);
            s->frames++;
            if (err || input.consumed != p->size || output.decoded_size != sizeof(s->pcm) ||
                info.sample_rate != SAMPLE_RATE || info.channel != 2 || info.bits_per_sample != 16) {
                s->errors++;
                if (s->errors < 3) printf("FFT_ERROR {\"code\":%d,\"consumed\":%lu,\"decoded\":%lu}\n",
                                        err, (unsigned long)input.consumed, (unsigned long)output.decoded_size);
                continue;
            }
            bool nonzero = false;
            for (unsigned i = 0; i < 960; i++) {
                int32_t value = (int32_t)s->pcm[2 * i] + s->pcm[2 * i + 1];
                s->ring[s->position] = value / 65536.0f;
                s->position = (s->position + 1) % MAX_FFT;
                if (s->filled < MAX_FFT) s->filled++;
                if (value) nonzero = true;
            }
            s->nonzero += nonzero;
            if (s->frames % 2 == 0 && s->filled >= n) {
                start = now_us();
                for (unsigned i = 0; i < n; i++) {
                    float sample = s->ring[(s->position + MAX_FFT - n + i) % MAX_FFT] * s->window[i];
                    fft[2 * i] = sample; fft[2 * i + 1] = 0;
                    fixed_fft[2 * i] = lrintf(sample * 32767); fixed_fft[2 * i + 1] = 0;
                }
                uint64_t kernel = now_us();
                ESP_ERROR_CHECK(dsps_fft2r_fc32(fft, n));
                ESP_ERROR_CHECK(dsps_bit_rev_fc32(fft, n));
                record(&s->float_fft, now_us() - kernel);
                kernel = now_us();
                ESP_ERROR_CHECK(dsps_fft2r_sc16(fixed_fft, n));
                ESP_ERROR_CHECK(dsps_bit_rev_sc16_ansi(fixed_fft, n));
                record(&s->fixed_fft, now_us() - kernel);
                uint8_t levels[32];
                bands(s, n, levels);
                record(&s->analysis, now_us() - start);
                s->transforms++;
                if (s->frames % 500 == 0) {
                    printf("FFT_SAMPLE {\"pts\":%lu,\"n\":%u,\"bands\":[", (unsigned long)p->pts, n);
                    for (unsigned i = 0; i < 32; i++) printf("%s%u", i ? "," : "", levels[i]);
                    printf("]}\n");
                }
            }
            uint64_t latency = now_us() - p->queued_us;
            record(&s->latency, latency);
            if (latency > 20000) s->over_budget++;
            uint32_t available = heap_free();
            if (available < s->heap_min) s->heap_min = available;
        }
        summarize(s, n);
    }
    atomic_store(&accepting, false);
    printf("FFT_COMPLETE\n");
    /* The isolated experiment retains its allocations until normal firmware is
     * restored. No task is deleted while it might own decoder/DSP state. */
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

void spectrum_benchmark_start(void)
{
    static StaticQueue_t queue_control;
    uint8_t *storage = heap_caps_malloc(QUEUE_DEPTH * sizeof(packet), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage) abort();
    packets = xQueueCreateStatic(QUEUE_DEPTH, sizeof(packet), storage, &queue_control);
    if (!packets || xTaskCreatePinnedToCore(run, "spectrum-bench", 16384, NULL, 4, NULL, 1) != pdPASS) abort();
}

void spectrum_benchmark_push(uint32_t pts, const uint8_t *opus, size_t size)
{
    if (!atomic_load(&accepting) || !size || size > 1275) return;
    packet p = {.pts = pts, .size = size, .queued_us = now_us()};
    memcpy(p.bytes, opus, size);
    atomic_fetch_or(&radio_cores, 1u << xPortGetCoreID());
    atomic_store(&radio_affinity, xTaskGetCoreID(NULL));
    if (!xQueueSend(packets, &p, 0)) atomic_fetch_add(&dropped, 1);
}
