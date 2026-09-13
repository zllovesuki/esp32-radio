#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_peer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "radio.h"
#include "signaling.h"
#include "signaling_private.h"

static const char *TAG = "signaling";
static QueueHandle_t events;
static atomic_bool transport_lost;
static RTC_DATA_ATTR unsigned recovery_attempt;
static esp_http_client_handle_t client;
static char *response_buffer;
static size_t response_used;
static bool response_overflow;
#define RESPONSE_LIMIT 24576

bool signaling_observe(const cJSON *message)
{
    if (!SIGNALING_AUTONOMOUS || !events) return false;
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(message, "event");
    if (!cJSON_IsString(kind)) return false;
    bool sdp = !strcmp(kind->valuestring, "sdp");
    bool candidate = !strcmp(kind->valuestring, "candidate");
    bool state = !strcmp(kind->valuestring, "peer_state");
    if (state) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(message, "state");
        if (cJSON_IsNumber(value) && (value->valueint == ESP_PEER_STATE_DISCONNECTED ||
            value->valueint == ESP_PEER_STATE_CONNECT_FAILED || value->valueint == ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED))
            atomic_store(&transport_lost, true);
    }
    if (sdp || state || !strcmp(kind->valuestring, "command_result")) {
        cJSON *copy = cJSON_Duplicate(message, true);
        if (copy && !xQueueSend(events, &copy, 0)) cJSON_Delete(copy);
    }
    return sdp || candidate;
}

static void drain_events(void)
{
    cJSON *event;
    while (xQueueReceive(events, &event, 0)) cJSON_Delete(event);
}

static cJSON *wait_event(const char *kind, const char *command, int state, unsigned timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        cJSON *event = NULL;
        if (!xQueueReceive(events, &event, pdMS_TO_TICKS(100))) continue;
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(event, "event");
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(event, "cmd");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(event, "state");
        if (cJSON_IsString(name) && !strcmp(name->valuestring, kind) &&
            (!command || (cJSON_IsString(cmd) && !strcmp(cmd->valuestring, command))) &&
            (state < 0 || (cJSON_IsNumber(value) && value->valueint == state))) return event;
        cJSON_Delete(event);
    }
    return NULL;
}

static bool command_ok(cJSON *command)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(command, "cmd");
    char expected[32];
    if (!cJSON_IsString(name) || strlen(name->valuestring) >= sizeof(expected)) { cJSON_Delete(command); return false; }
    strcpy(expected, name->valuestring);
    radio_command(command);
    cJSON_Delete(command);
    cJSON *reply = wait_event("command_result", expected, -1, 15000);
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(reply, "result");
    bool ok = cJSON_IsNumber(result) && result->valueint == 0;
    cJSON_Delete(reply);
    return ok;
}

static cJSON *command(const char *name)
{
    cJSON *object = cJSON_CreateObject();
    cJSON_AddStringToObject(object, "cmd", name);
    return object;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        if (response_used + event->data_len >= RESPONSE_LIMIT) response_overflow = true;
        else if (!response_overflow) {
            memcpy(response_buffer + response_used, event->data, event->data_len);
            response_used += event->data_len;
        }
    }
    return ESP_OK;
}

// Positive values are HTTP statuses, negative values are local transport errors.
// The same client owns all requests, on this task only. It reuses verified TLS.
static int post(const char *path, const cJSON *body, cJSON **reply)
{
    *reply = NULL;
    if (!radio_network_ready()) return -1;
    char url[256];
    if (snprintf(url, sizeof(url), "%s/api/device/%s", SIGNALING_URL, path) >= sizeof(url)) return -1;
    char *json = cJSON_PrintUnformatted(body);
    if (!json) return -1;
    response_used = 0;
    response_overflow = false;
    esp_err_t err = esp_http_client_set_url(client, url);
    if (err == ESP_OK) err = esp_http_client_set_post_field(client, json, strlen(json));
    if (err == ESP_OK) err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_set_post_field(client, NULL, 0);
    free(json);
    if (err != ESP_OK || response_overflow) {
        ESP_LOGW(TAG, "HTTPS %s: transport=%s oversized=%d", path, esp_err_to_name(err), response_overflow);
        esp_http_client_close(client);
        return -1;
    }
    response_buffer[response_used] = 0;
    *reply = cJSON_ParseWithLength(response_buffer, response_used);
    if (!cJSON_IsObject(*reply)) {
        cJSON_Delete(*reply);
        *reply = NULL;
        return status >= 400 ? status : -1;
    }
    ESP_LOGI(TAG, "HTTPS %s: status=%d", path, status);
    return status;
}

static cJSON *post_retry(const char *path, const cJSON *body)
{
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        cJSON *reply = NULL;
        int status = post(path, body, &reply);
        if (status >= 200 && status < 300 && reply) return reply;
        cJSON_Delete(reply);
        if (status >= 400 && status < 500 && status != 429) break;
        vTaskDelay(pdMS_TO_TICKS(1000u << attempt));
    }
    return NULL;
}

static void restart_later(const char *reason)
{
    unsigned delay = 5u << (recovery_attempt < 3 ? recovery_attempt : 3);
    recovery_attempt++;
    ESP_LOGW(TAG, "%s; restarting in %u seconds", reason, delay);
    vTaskDelay(pdMS_TO_TICKS(delay * 1000 + esp_random() % 1000));
    // The vendor peer teardown currently hangs. A restart safely resets its
    // tasks and sockets; the Worker closes the previous publisher generation.
    esp_restart();
}

static void run(void *unused)
{
    esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
        ESP_SNTP_SERVER_LIST("time.cloudflare.com", "pool.ntp.org"));
    ESP_ERROR_CHECK(esp_netif_sntp_init(&clock));
    while (time(NULL) < 1700000000) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000)) != ESP_OK)
            ESP_LOGW(TAG, "Waiting for time synchronization before verified HTTPS");
    }
    response_buffer = heap_caps_malloc(RESPONSE_LIMIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response_buffer) { restart_later("No response buffer"); return; }
    esp_http_client_config_t config = {.url = SIGNALING_URL,
        .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 12000,
        .event_handler = http_event, .buffer_size = 4096, .buffer_size_tx = 4096,
        .disable_auto_redirect = true, .keep_alive_enable = true};
    client = esp_http_client_init(&config);
    if (!client) { restart_later("HTTP initialization failed"); return; }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", "Bearer " SIGNALING_DEVICE_TOKEN);
    esp_http_client_set_header(client, "User-Agent", "PocketRadio-S3/1.0");
    ESP_LOGI(TAG, "Starting autonomous signaling at %s", SIGNALING_URL);
    if (!command_ok(command("peer_init"))) { restart_later("Radio initialization failed"); return; }
    cJSON *offer = wait_event("sdp", NULL, -1, 15000);
    const cJSON *sdp = cJSON_GetObjectItemCaseSensitive(offer, "text");
    if (!cJSON_IsString(sdp)) { cJSON_Delete(offer); restart_later("Missing radio offer"); return; }
    cJSON *start = cJSON_CreateObject();
    cJSON *description = cJSON_AddObjectToObject(start, "sessionDescription");
    cJSON_AddStringToObject(description, "type", "offer");
    cJSON_AddStringToObject(description, "sdp", sdp->valuestring);
    char boot_id[33];
    snprintf(boot_id, sizeof(boot_id), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(),
        (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
    cJSON_AddStringToObject(start, "bootId", boot_id);
    cJSON_Delete(offer);
    cJSON *started = post_retry("start", start);
    cJSON_Delete(start);
    const cJSON *generation = cJSON_GetObjectItemCaseSensitive(started, "generation");
    const cJSON *answer = cJSON_GetObjectItemCaseSensitive(started, "sessionDescription");
    const cJSON *answer_text = cJSON_GetObjectItemCaseSensitive(answer, "sdp");
    if (!cJSON_IsString(generation) || strlen(generation->valuestring) > 64 || !cJSON_IsString(answer_text)) {
        cJSON_Delete(started); restart_later("Publisher setup failed"); return;
    }
    cJSON *identity = cJSON_CreateObject();
    cJSON_AddStringToObject(identity, "generation", generation->valuestring);
    cJSON *remote = command("sdp");
    cJSON_AddStringToObject(remote, "text", answer_text->valuestring);
    cJSON_Delete(started);
    if (!command_ok(remote)) { restart_later("Answer rejected"); return; }
    cJSON *connected = wait_event("peer_state", NULL, ESP_PEER_STATE_DATA_CHANNEL_CONNECTED, 30000);
    if (!connected) { restart_later("WebRTC connection timed out"); return; }
    cJSON_Delete(connected);
    cJSON *created = post_retry("channels", identity);
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(created, "channels");
    int ids[2] = {-1, -1};
    const cJSON *channel;
    cJSON_ArrayForEach(channel, channels) {
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(channel, "dataChannelName");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(channel, "id");
        if (cJSON_IsString(label) && cJSON_IsNumber(id)) {
            if (!strcmp(label->valuestring, "robot")) ids[0] = id->valueint;
            if (!strcmp(label->valuestring, "spectrum")) ids[1] = id->valueint;
        }
    }
    cJSON_Delete(created);
    if (ids[0] != 2 || ids[1] != 4) { restart_later("Unexpected data channel allocation"); return; }
    for (unsigned i = 0; i < 2; i++) {
        if (!command_ok(command("create_channel"))) { restart_later("Local channel creation failed"); return; }
    }
    cJSON *play = command("start");
    cJSON_AddNumberToObject(play, "robot_id", ids[0]);
    cJSON_AddNumberToObject(play, "spectrum_id", ids[1]);
    if (!command_ok(play)) { restart_later("Playback could not start"); return; }
    cJSON *ready = post_retry("ready", identity);
    if (!ready) { restart_later("Publisher readiness was not acknowledged"); return; }
    cJSON_Delete(ready);
    recovery_attempt = 0;
    ESP_LOGI(TAG, "ON AIR: autonomous Wi-Fi signaling, audio and data; USB is optional");
    unsigned failures = 0;
    int64_t last_success = esp_timer_get_time();
    for (;;) {
        drain_events();
        for (unsigned i = 0; i < 50; i++) {
            if (atomic_load(&transport_lost)) { restart_later("WebRTC transport disconnected"); return; }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        cJSON *reply = NULL;
        int status = post("heartbeat", identity, &reply);
        cJSON_Delete(reply);
        if (status >= 200 && status < 300) {
            failures = 0;
            last_success = esp_timer_get_time();
        } else {
            ESP_LOGW(TAG, "Heartbeat unavailable: status=%d attempt=%u", status, ++failures);
            if ((status >= 400 && status < 500 && status != 429) ||
                esp_timer_get_time() - last_success > 55000000) {
                restart_later("Signaling session needs recovery"); return;
            }
        }
    }
}

void signaling_start(void)
{
    if (!SIGNALING_AUTONOMOUS) return;
    events = xQueueCreate(16, sizeof(cJSON *));
    if (!events || xTaskCreate(run, "signaling", 16384, NULL, 4, NULL) != pdPASS) abort();
}
