#include <stdlib.h>
#include <string.h>
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "peer_probe.h"

static esp_peer_handle_t peer;
static QueueHandle_t commands;
static char channel_labels[4][33];
static unsigned channel_count;

static cJSON *event(const char *name)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "event", name);
    return msg;
}

static int on_state(esp_peer_state_t state, void *ctx)
{
    cJSON *msg = event("peer_state");
    cJSON_AddNumberToObject(msg, "state", state);
    probe_emit(msg);
    return 0;
}

static int on_signal(esp_peer_msg_t *signal, void *ctx)
{
    char *text = calloc(1, signal->size + 1);
    if (!text) return -1;
    memcpy(text, signal->data, signal->size);
    cJSON *msg = event(signal->type == ESP_PEER_MSG_TYPE_SDP ? "sdp" : "candidate");
    cJSON_AddStringToObject(msg, "text", text);
    probe_emit(msg);
    free(text);
    return 0;
}

static int on_data(esp_peer_data_frame_t *frame, void *ctx)
{
    cJSON *msg = event("data_received");
    cJSON_AddNumberToObject(msg, "stream_id", frame->stream_id);
    cJSON_AddNumberToObject(msg, "bytes", frame->size);
    char *text = calloc(1, frame->size + 1);
    if (text) {
        memcpy(text, frame->data, frame->size);
        cJSON_AddStringToObject(msg, "text", text);
        cJSON *command = cJSON_Parse(text);
        const cJSON *color = command ? cJSON_GetObjectItemCaseSensitive(command, "led") : NULL;
        if (cJSON_IsArray(color) && cJSON_GetArraySize(color) == 3) {
            cJSON *r = cJSON_GetArrayItem(color, 0);
            cJSON *g = cJSON_GetArrayItem(color, 1);
            cJSON *b = cJSON_GetArrayItem(color, 2);
            if (cJSON_IsNumber(r) && cJSON_IsNumber(g) && cJSON_IsNumber(b))
                probe_set_led(r->valueint, g->valueint, b->valueint);
        }
        cJSON_Delete(command);
        free(text);
    }
    probe_emit(msg);
    return 0;
}

static int on_channel(esp_peer_data_channel_info_t *channel, void *ctx)
{
    cJSON *msg = event("channel_open");
    cJSON_AddNumberToObject(msg, "stream_id", channel->stream_id);
    cJSON_AddStringToObject(msg, "label", channel->label ? channel->label : "");
    probe_emit(msg);
    return 0;
}

static int on_channel_close(esp_peer_data_channel_info_t *channel, void *ctx)
{
    cJSON *msg = event("channel_closed");
    cJSON_AddNumberToObject(msg, "stream_id", channel->stream_id);
    cJSON_AddStringToObject(msg, "label", channel->label ? channel->label : "");
    probe_emit(msg);
    return 0;
}

static void execute(cJSON *command)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(command, "cmd");
    if (!cJSON_IsString(name)) return;
    int result = ESP_PEER_ERR_INVALID_ARG;
    if (!strcmp(name->valuestring, "peer_init") && !peer) {
        esp_peer_default_cfg_t extra = {
            .agent_recv_timeout = 10,
            .data_ch_cfg = {.send_cache_size = 16384, .recv_cache_size = 16384},
            .rtp_cfg = {.send_pool_size = 8192, .send_queue_num = 16},
        };
        esp_peer_cfg_t config = {
            .role = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(command, "initiator"))
                ? ESP_PEER_ROLE_CONTROLLING : ESP_PEER_ROLE_CONTROLLED,
            .audio_dir = ESP_PEER_MEDIA_DIR_NONE,
            .video_dir = ESP_PEER_MEDIA_DIR_NONE,
            .enable_data_channel = true,
            .manual_ch_create = true,
            .no_auto_reconnect = true,
            .on_state = on_state,
            .on_msg = on_signal,
            .on_data = on_data,
            .on_channel_open = on_channel,
            .on_channel_close = on_channel_close,
            .extra_cfg = &extra,
            .extra_size = sizeof(extra),
        };
        result = esp_peer_open(&config, esp_peer_get_default_impl(), &peer);
        channel_count = 0;
        if (!result) result = esp_peer_new_connection(peer);
    } else if (!strcmp(name->valuestring, "sdp") && peer) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(command, "text");
        if (cJSON_IsString(text)) {
            esp_peer_msg_t signal = {.type = ESP_PEER_MSG_TYPE_SDP,
                .data = (uint8_t *)text->valuestring, .size = strlen(text->valuestring)};
            result = esp_peer_send_msg(peer, &signal);
        }
    } else if (!strcmp(name->valuestring, "send") && peer) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(command, "text");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(command, "stream_id");
        if (cJSON_IsString(text) && cJSON_IsNumber(id) && id->valueint >= 0 && id->valueint < 65535) {
            esp_peer_data_frame_t frame = {.type = ESP_PEER_DATA_CHANNEL_STRING,
                .stream_id = id->valueint, .data = (uint8_t *)text->valuestring, .size = strlen(text->valuestring)};
            result = esp_peer_send_data(peer, &frame);
        }
    } else if (!strcmp(name->valuestring, "create_channel") && peer) {
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(command, "label");
        const cJSON *ordered = cJSON_GetObjectItemCaseSensitive(command, "ordered");
        const cJSON *retransmits = cJSON_GetObjectItemCaseSensitive(command, "max_retransmits");
        const char *text = cJSON_IsString(label) ? label->valuestring : "robot";
        if (channel_count < 4 && strlen(text) <= 32 &&
            (!retransmits || (cJSON_IsNumber(retransmits) && retransmits->valueint >= 0 && retransmits->valueint <= 65535))) {
            strcpy(channel_labels[channel_count], text);
            esp_peer_data_channel_cfg_t channel = {
                .type = retransmits ? ESP_PEER_DATA_CHANNEL_PARTIAL_RELIABLE_RETX : ESP_PEER_DATA_CHANNEL_RELIABLE,
                .ordered = !cJSON_IsFalse(ordered),
                .label = channel_labels[channel_count],
                .max_retransmit_count = retransmits ? retransmits->valueint : 0,
            };
            result = esp_peer_create_data_channel(peer, &channel);
            if (!result) channel_count++;
        }
    } else if (!strcmp(name->valuestring, "close_channel") && peer) {
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(command, "label");
        if (cJSON_IsString(label)) result = esp_peer_close_data_channel(peer, label->valuestring);
    } else if (!strcmp(name->valuestring, "peer_close") && peer) {
        result = esp_peer_close(peer);
        peer = NULL;
    } else if (!strcmp(name->valuestring, "led")) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(command, "r");
        const cJSON *g = cJSON_GetObjectItemCaseSensitive(command, "g");
        const cJSON *b = cJSON_GetObjectItemCaseSensitive(command, "b");
        if (cJSON_IsNumber(r) && cJSON_IsNumber(g) && cJSON_IsNumber(b)) {
            probe_set_led(r->valueint, g->valueint, b->valueint);
            result = 0;
        }
    } else if (!strcmp(name->valuestring, "ping")) result = 0;
    cJSON *reply = event("command_result");
    cJSON_AddStringToObject(reply, "cmd", name->valuestring);
    cJSON_AddNumberToObject(reply, "result", result);
    probe_emit(reply);
}

static void run(void *arg)
{
    for (;;) {
        cJSON *command = NULL;
        if (xQueueReceive(commands, &command, peer ? 0 : pdMS_TO_TICKS(20))) {
            execute(command);
            cJSON_Delete(command);
        }
        if (peer) esp_peer_main_loop(peer);
        vTaskDelay(1);
    }
}

void probe_command(const cJSON *command)
{
    if (!commands) {
        commands = xQueueCreate(8, sizeof(cJSON *));
        if (!commands || xTaskCreate(run, "peer_probe", 16384, NULL, 5, NULL) != pdPASS) abort();
    }
    cJSON *copy = cJSON_Duplicate(command, true);
    if (copy && !xQueueSend(commands, &copy, pdMS_TO_TICKS(100))) cJSON_Delete(copy);
}
