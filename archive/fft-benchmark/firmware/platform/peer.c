#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "radio_bridge.h"
#include "spectrum_benchmark.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define SDP_LIMIT 16000
#define COMMAND_LIMIT 512
typedef struct { uint32_t length; uint8_t bytes[COMMAND_LIMIT]; } command;

/* Process-lifetime ownership: the vendor close path hangs on the pinned SDK.
 * No C callback calls Rust or touches Rust memory. Queue elements are copies.
 */
struct radio_peer {
    esp_peer_handle_t handle;
    esp_peer_cfg_t config;
    esp_peer_default_cfg_t extra;
    esp_peer_data_channel_cfg_t channels[2];
    QueueHandle_t commands;
    SemaphoreHandle_t offer_lock;
    atomic_int state;
    atomic_uint dropped;
    bool channels_created;
    size_t offer_length;
    char offer[SDP_LIMIT + 1];
    char answer[SDP_LIMIT + 1];
    uint8_t audio[1275];
    uint8_t data[1024];
};
static atomic_bool taken;

static int on_state(esp_peer_state_t state, void *ctx)
{
    radio_peer *p = ctx;
    if (state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED) atomic_store(&p->state, 1);
    if (state == ESP_PEER_STATE_DISCONNECTED || state == ESP_PEER_STATE_CLOSED ||
        state == ESP_PEER_STATE_CONNECT_FAILED || state == ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED)
        atomic_store(&p->state, -1);
    return 0;
}

static int on_signal(esp_peer_msg_t *msg, void *ctx)
{
    radio_peer *p = ctx;
    if (msg->type != ESP_PEER_MSG_TYPE_SDP) return 0;
    if (!msg->data || msg->size <= 0 || msg->size > SDP_LIMIT) { atomic_store(&p->state, -1); return -1; }
    xSemaphoreTake(p->offer_lock, portMAX_DELAY);
    memcpy(p->offer, msg->data, msg->size);
    p->offer[msg->size] = 0;
    p->offer_length = msg->size;
    xSemaphoreGive(p->offer_lock);
    return 0;
}

static int on_data(esp_peer_data_frame_t *frame, void *ctx)
{
    radio_peer *p = ctx;
    if (frame->stream_id != 2 || !frame->data || frame->size <= 0 || frame->size > COMMAND_LIMIT) return 0;
    command msg = {.length = frame->size};
    memcpy(msg.bytes, frame->data, frame->size);
    if (!xQueueSend(p->commands, &msg, 0)) atomic_fetch_add(&p->dropped, 1);
    return 0;
}

radio_peer *radio_peer_open(void)
{
    if (atomic_exchange(&taken, true)) return NULL;
    spectrum_benchmark_start();
    radio_peer *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->commands = xQueueCreate(16, sizeof(command));
    p->offer_lock = xSemaphoreCreateMutex();
    if (!p->commands || !p->offer_lock) goto early_error;
    atomic_init(&p->state, 0);
    atomic_init(&p->dropped, 0);
    /* 1 ms caused DTLS receive timeouts to be interpreted as a peer close.
     * Retain the measured 10 ms setting and the validated transport buffers. */
    p->extra = (esp_peer_default_cfg_t){.agent_recv_timeout = 10,
        .data_ch_cfg = {.send_cache_size = 16384, .recv_cache_size = 16384},
        .rtp_cfg = {.send_pool_size = 32768, .send_queue_num = 32}};
    p->config = (esp_peer_cfg_t){.role = ESP_PEER_ROLE_CONTROLLING,
        .audio_info = {.codec = ESP_PEER_AUDIO_CODEC_OPUS, .sample_rate = 48000, .channel = 2},
        .audio_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY, .video_dir = ESP_PEER_MEDIA_DIR_NONE,
        .enable_data_channel = true, .manual_ch_create = true, .no_auto_reconnect = true,
        .on_state = on_state, .on_msg = on_signal, .on_data = on_data, .ctx = p,
        .extra_cfg = &p->extra, .extra_size = sizeof(p->extra)};
    /* All configuration and context remain live even if initialization fails. */
    if (esp_peer_open(&p->config, esp_peer_get_default_impl(), &p->handle) || esp_peer_new_connection(p->handle)) return NULL;
    return p;
early_error:
    if (p->commands) vQueueDelete(p->commands);
    if (p->offer_lock) vSemaphoreDelete(p->offer_lock);
    free(p);
    return NULL;
}

int32_t radio_peer_poll(radio_peer *p) { return esp_peer_main_loop(p->handle); }
int32_t radio_peer_state(radio_peer *p) { return atomic_load(&p->state); }
uint32_t radio_peer_dropped(radio_peer *p) { return atomic_load(&p->dropped); }
int32_t radio_peer_offer(radio_peer *p, uint8_t *out, size_t capacity)
{
    xSemaphoreTake(p->offer_lock, portMAX_DELAY);
    int32_t size = p->offer_length;
    if ((size_t)size > capacity) size = -1;
    else if (size) { memcpy(out, p->offer, size); p->offer_length = 0; }
    xSemaphoreGive(p->offer_lock);
    return size;
}
int32_t radio_peer_answer(radio_peer *p, const uint8_t *bytes, size_t length)
{
    if (!length || length > SDP_LIMIT || p->answer[0]) return -1;
    memcpy(p->answer, bytes, length);
    p->answer[length] = 0;
    esp_peer_msg_t msg = {.type = ESP_PEER_MSG_TYPE_SDP, .data = (uint8_t *)p->answer, .size = length};
    return esp_peer_send_msg(p->handle, &msg);
}
int32_t radio_peer_channels(radio_peer *p)
{
    if (p->channels_created || radio_peer_state(p) != 1) return -1;
    p->channels[0] = (esp_peer_data_channel_cfg_t){.label = "robot", .type = ESP_PEER_DATA_CHANNEL_RELIABLE, .ordered = true};
    p->channels[1] = (esp_peer_data_channel_cfg_t){.label = "spectrum", .type = ESP_PEER_DATA_CHANNEL_PARTIAL_RELIABLE_RETX,
        .ordered = false, .max_retransmit_count = 0};
    for (unsigned i = 0; i < 2; i++) { int err = esp_peer_create_data_channel(p->handle, &p->channels[i]); if (err) return err; }
    p->channels_created = true;
    return 0;
}
int32_t radio_peer_command(radio_peer *p, uint8_t *out, size_t capacity)
{
    command msg;
    if (!xQueueReceive(p->commands, &msg, 0)) return 0;
    if (msg.length > capacity) return -1;
    memcpy(out, msg.bytes, msg.length);
    return msg.length;
}
int32_t radio_peer_audio(radio_peer *p, uint32_t pts, const uint8_t *bytes, size_t length)
{
    if (!length || length > sizeof(p->audio)) return -1;
    spectrum_benchmark_push(pts, bytes, length);
    memcpy(p->audio, bytes, length);
    esp_peer_audio_frame_t frame = {.pts = pts, .data = p->audio, .size = length};
    return esp_peer_send_audio(p->handle, &frame);
}
int32_t radio_peer_data(radio_peer *p, uint16_t stream, int32_t binary, const uint8_t *bytes, size_t length)
{
    if (!length || length > sizeof(p->data) || (stream != 2 && stream != 4)) return -1;
    memcpy(p->data, bytes, length);
    esp_peer_data_frame_t frame = {.type = binary ? ESP_PEER_DATA_CHANNEL_DATA : ESP_PEER_DATA_CHANNEL_STRING,
        .stream_id = stream, .data = p->data, .size = length};
    return esp_peer_send_data(p->handle, &frame);
}
