#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "radio.h"

static esp_peer_handle_t peer;
static QueueHandle_t commands;
static atomic_bool connected;
static bool playing, paused;
static uint16_t robot_id, spectrum_id;
static uint8_t *music;
static uint32_t *offsets, frame_count, frame_index, elapsed_frames;
static int64_t next_frame, next_telemetry;
static uint32_t audio_errors, data_errors, skipped_frames, telemetry_seq;
static unsigned led_rgb[3] = {0, 12, 4};
static char channel_labels[2][16];
static unsigned channel_count;

static cJSON *event(const char *name)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "event", name);
    return msg;
}

static uint32_t read32(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static int load_music(void)
{
    if (music) return 0;
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "music");
    uint8_t header[64];
    if (!part || esp_partition_read(part, 0, header, sizeof(header))) return -1;
    frame_count = read32(header + 12);
    if (memcmp(header, "S3MUSIC\0", 8) || read32(header + 8) != 1 || !frame_count || frame_count > 30000 ||
        read32(header + 16) != 48000 || read32(header + 20) != 2 || read32(header + 24) != 20) return -1;
    uint8_t *buffer = heap_caps_malloc(part->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t *index = heap_caps_malloc(frame_count * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer || !index || esp_partition_read(part, 0, buffer, part->size)) {
        free(buffer); free(index); return -1;
    }
    uint32_t cursor = 64;
    for (uint32_t i = 0; i < frame_count; i++) {
        if (cursor + 34 > part->size) goto invalid;
        uint16_t size = buffer[cursor] | buffer[cursor + 1] << 8;
        if (!size || size > 1275 || cursor + 34 + size > part->size) goto invalid;
        index[i] = cursor;
        cursor += 34 + size;
    }
    if (esp_rom_crc32_le(0, buffer + 64, cursor - 64) != read32(header + 28)) goto invalid;
    music = buffer;
    offsets = index;
    return 0;
invalid:
    free(buffer); free(index); return -1;
}

static int send_json(cJSON *message)
{
    char *text = cJSON_PrintUnformatted(message);
    int result = -1;
    if (text) {
        esp_peer_data_frame_t frame = {.type = ESP_PEER_DATA_CHANNEL_STRING, .stream_id = robot_id,
            .data = (uint8_t *)text, .size = strlen(text)};
        result = esp_peer_send_data(peer, &frame);
        free(text);
    }
    cJSON_Delete(message);
    if (result) data_errors++;
    return result;
}

static int on_state(esp_peer_state_t state, void *ctx)
{
    if (state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED) connected = true;
    if (state == ESP_PEER_STATE_DISCONNECTED || state == ESP_PEER_STATE_CLOSED || state == ESP_PEER_STATE_CONNECT_FAILED)
        connected = false;
    cJSON *msg = event("peer_state");
    cJSON_AddNumberToObject(msg, "state", state);
    radio_emit(msg);
    return 0;
}

static int on_signal(esp_peer_msg_t *signal, void *ctx)
{
    char *text = calloc(1, signal->size + 1);
    if (!text) return -1;
    memcpy(text, signal->data, signal->size);
    cJSON *msg = event(signal->type == ESP_PEER_MSG_TYPE_SDP ? "sdp" : "candidate");
    cJSON_AddStringToObject(msg, "text", text);
    radio_emit(msg);
    free(text);
    return 0;
}

static int on_data(esp_peer_data_frame_t *frame, void *ctx)
{
    if (frame->stream_id != robot_id || frame->size <= 0 || frame->size > 512) return 0;
    cJSON *command = cJSON_ParseWithLength((const char *)frame->data, frame->size);
    if (!cJSON_IsObject(command)) { cJSON_Delete(command); return 0; }
    // Network commands have only the documented application fields. The USB
    // maintenance protocol is not exposed through a DataChannel.
    cJSON *safe = cJSON_CreateObject();
    const char *keys[] = {"led", "action", "command_id"};
    for (unsigned i = 0; i < 3; i++) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(command, keys[i]);
        if (value) cJSON_AddItemToObject(safe, keys[i], cJSON_Duplicate(value, true));
    }
    cJSON_Delete(command);
    command = safe;
    // Callbacks can arrive on the transport task. Execute commands on the peer
    // task so playback and LED state have one owner and never race.
    if (!xQueueSend(commands, &command, 0)) cJSON_Delete(command);
    return 0;
}

static void control(cJSON *command)
{
    const cJSON *color = cJSON_GetObjectItemCaseSensitive(command, "led");
    int result = -1;
    if (cJSON_IsArray(color) && cJSON_GetArraySize(color) == 3) {
        bool valid = true;
        unsigned rgb[3];
        for (unsigned i = 0; i < 3; i++) {
            const cJSON *v = cJSON_GetArrayItem(color, i);
            if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 255 || floor(v->valuedouble) != v->valuedouble)
                valid = false;
            rgb[i] = cJSON_IsNumber(v) ? v->valueint : 0;
        }
        if (valid) { result = radio_set_led(rgb[0], rgb[1], rgb[2]); if (!result) memcpy(led_rgb, rgb, sizeof(rgb)); }
    }
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(command, "action");
    if (cJSON_IsString(action)) {
        if (!strcmp(action->valuestring, "pause")) { paused = true; result = 0; }
        else if (!strcmp(action->valuestring, "play")) { paused = false; result = 0; }
        else if (!strcmp(action->valuestring, "restart")) { frame_index = 0; paused = false; result = 0; }
    }
    cJSON *ack = event("ack");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(command, "command_id");
    if (cJSON_IsString(id) && strlen(id->valuestring) <= 64) cJSON_AddStringToObject(ack, "command_id", id->valuestring);
    cJSON_AddNumberToObject(ack, "result", result);
    cJSON_AddItemToObject(ack, "led", cJSON_CreateIntArray((int *)led_rgb, 3));
    cJSON_AddBoolToObject(ack, "paused", paused);
    send_json(ack);
}

static void execute(cJSON *command)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(command, "cmd");
    if (!cJSON_IsString(name)) { if (playing) control(command); return; }
    int result = ESP_PEER_ERR_INVALID_ARG;
    if (!strcmp(name->valuestring, "peer_init") && !peer) {
        result = load_music();
        if (!result) {
            esp_peer_default_cfg_t extra = {.agent_recv_timeout = 10,
                .data_ch_cfg = {.send_cache_size = 16384, .recv_cache_size = 16384},
                .rtp_cfg = {.send_pool_size = 32768, .send_queue_num = 32}};
            esp_peer_cfg_t config = {.role = ESP_PEER_ROLE_CONTROLLING,
                .audio_info = {.codec = ESP_PEER_AUDIO_CODEC_OPUS, .sample_rate = 48000, .channel = 2},
                .audio_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY, .video_dir = ESP_PEER_MEDIA_DIR_NONE,
                .enable_data_channel = true, .manual_ch_create = true, .no_auto_reconnect = true,
                .on_state = on_state, .on_msg = on_signal, .on_data = on_data,
                .extra_cfg = &extra, .extra_size = sizeof(extra)};
            result = esp_peer_open(&config, esp_peer_get_default_impl(), &peer);
            if (!result) result = esp_peer_new_connection(peer);
        }
    } else if (!strcmp(name->valuestring, "sdp") && peer) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(command, "text");
        if (cJSON_IsString(text)) {
            esp_peer_msg_t signal = {.type = ESP_PEER_MSG_TYPE_SDP,
                .data = (uint8_t *)text->valuestring, .size = strlen(text->valuestring)};
            result = esp_peer_send_msg(peer, &signal);
        }
    } else if (!strcmp(name->valuestring, "create_channel") && peer && connected && channel_count < 2) {
        const char *label = channel_count == 0 ? "robot" : "spectrum";
        strcpy(channel_labels[channel_count], label);
        esp_peer_data_channel_cfg_t channel = {.label = channel_labels[channel_count],
            .type = channel_count == 0 ? ESP_PEER_DATA_CHANNEL_RELIABLE : ESP_PEER_DATA_CHANNEL_PARTIAL_RELIABLE_RETX,
            .ordered = channel_count == 0, .max_retransmit_count = 0};
        result = esp_peer_create_data_channel(peer, &channel);
        if (!result) channel_count++;
    } else if (!strcmp(name->valuestring, "start") && peer && connected && channel_count == 2 && !playing) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(command, "robot_id");
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(command, "spectrum_id");
        // The tested fixed SDK allocation is 2, then 4. Fail clearly if the
        // SFU ever returns another allocation instead of silently misrouting.
        if (cJSON_IsNumber(r) && cJSON_IsNumber(s) && r->valueint == 2 && s->valueint == 4) {
            robot_id = r->valueint; spectrum_id = s->valueint;
            playing = true; next_frame = esp_timer_get_time(); next_telemetry = next_frame;
            result = 0;
        }
    } else if (!strcmp(name->valuestring, "ping")) result = 0;
    else if (!strcmp(name->valuestring, "restart_device")) { vTaskDelay(pdMS_TO_TICKS(100)); esp_restart(); }
    cJSON *reply = event("command_result");
    cJSON_AddStringToObject(reply, "cmd", name->valuestring);
    cJSON_AddNumberToObject(reply, "result", result);
    radio_emit(reply);
}

static void tick(void)
{
    if (!playing || !connected) return;
    int64_t now = esp_timer_get_time();
    if (now >= next_frame) {
        // Keep real time through stalls: skip old frames, never burst a backlog.
        uint32_t late = (now - next_frame) / 20000;
        if (late) { elapsed_frames += late; if (!paused) frame_index = (frame_index + late) % frame_count; skipped_frames += late; }
        next_frame += (late + 1) * 20000;
        uint8_t *record = music + offsets[frame_index];
        uint16_t size = record[0] | record[1] << 8;
        // RFC 6716 Opus silence (20 ms), preserving the RTP clock while paused.
        static uint8_t silence[] = {0xf8, 0xff, 0xfe};
        esp_peer_audio_frame_t audio = {.pts = elapsed_frames * 20,
            .data = paused ? silence : record + 34, .size = paused ? sizeof(silence) : size};
        if (esp_peer_send_audio(peer, &audio)) audio_errors++;
        if (elapsed_frames % 2 == 0) {
            // v1: version, paused, reserved[2], RTP-relative milliseconds,
            // song milliseconds, then 32 unsigned normalized spectrum bands.
            uint8_t packet[44] = {1, paused ? 1 : 0};
            uint32_t pts = elapsed_frames * 20, position = frame_index * 20;
            memcpy(packet + 4, &pts, 4); memcpy(packet + 8, &position, 4);
            if (!paused) memcpy(packet + 12, record + 2, 32);
            esp_peer_data_frame_t spectrum = {.type = ESP_PEER_DATA_CHANNEL_DATA, .stream_id = spectrum_id,
                .data = packet, .size = sizeof(packet)};
            if (esp_peer_send_data(peer, &spectrum)) data_errors++;
        }
        elapsed_frames++;
        if (!paused) frame_index = (frame_index + 1) % frame_count;
    }
    if (now >= next_telemetry) {
        next_telemetry = now + 500000;
        wifi_ap_record_t ap = {0};
        esp_wifi_sta_get_ap_info(&ap);
        cJSON *msg = event("telemetry");
        cJSON_AddNumberToObject(msg, "sequence", ++telemetry_seq);
        cJSON_AddNumberToObject(msg, "random", esp_random() % 1000);
        cJSON_AddNumberToObject(msg, "uptimeMs", now / 1000);
        cJSON_AddNumberToObject(msg, "positionMs", frame_index * 20);
        cJSON_AddNumberToObject(msg, "durationMs", frame_count * 20);
        cJSON_AddBoolToObject(msg, "paused", paused);
        cJSON_AddNumberToObject(msg, "rssi", ap.rssi);
        cJSON_AddNumberToObject(msg, "heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(msg, "audioErrors", audio_errors);
        cJSON_AddNumberToObject(msg, "dataErrors", data_errors);
        cJSON_AddNumberToObject(msg, "skippedFrames", skipped_frames);
        cJSON_AddItemToObject(msg, "led", cJSON_CreateIntArray((int *)led_rgb, 3));
        send_json(msg);
    }
}

static void run(void *arg)
{
    for (;;) {
        cJSON *command = NULL;
        if (xQueueReceive(commands, &command, 0)) { execute(command); cJSON_Delete(command); }
        if (peer) esp_peer_main_loop(peer);
        tick();
        vTaskDelay(1);
    }
}

void radio_init(void)
{
    if (!commands) {
        commands = xQueueCreate(16, sizeof(cJSON *));
        if (!commands || xTaskCreate(run, "radio", 16384, NULL, 5, NULL) != pdPASS) abort();
    }
}

void radio_command(const cJSON *command)
{
    cJSON *copy = cJSON_Duplicate(command, true);
    if (copy && !xQueueSend(commands, &copy, pdMS_TO_TICKS(100))) cJSON_Delete(copy);
}
