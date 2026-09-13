#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "peer_probe.h"
#include "wifi_private.h"

static const char *TAG = "hardware_check";
static EventGroupHandle_t wifi_events;
static led_strip_handle_t led;
static unsigned reconnects;
static bool want_connection;

void probe_emit(cJSON *message)
{
    if (!message) return;
    char *text = cJSON_PrintUnformatted(message);
    if (text) {
        printf("PROBE %s\n", text);
        fflush(stdout);
        free(text);
    }
    cJSON_Delete(message);
}

void probe_set_led(unsigned red, unsigned green, unsigned blue)
{
    if (!led) return;
    esp_err_t err = led_strip_set_pixel(led, 0, red & 255, green & 255, blue & 255);
    if (err == ESP_OK) err = led_strip_refresh(led);
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "event", "led");
    cJSON_AddNumberToObject(msg, "result", err);
    cJSON_AddNumberToObject(msg, "r", red & 255);
    cJSON_AddNumberToObject(msg, "g", green & 255);
    cJSON_AddNumberToObject(msg, "b", blue & 255);
    probe_emit(msg);
}

static bool test_psram(void)
{
    const size_t bytes = 12 * 1024 * 1024;
    volatile uint32_t *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) {
        ESP_LOGE(TAG, "PSRAM test: allocation of %u bytes failed", (unsigned)bytes);
        return false;
    }
    for (unsigned pass = 0; pass < 4; pass++) {
        for (size_t i = 0; i < bytes / 4; i++) {
            uint32_t value = pass == 0 ? 0 : pass == 1 ? UINT32_MAX : (uint32_t)i * 2654435761u;
            memory[i] = pass == 3 ? ~value : value;
            if ((i & 0x3ffff) == 0) vTaskDelay(1);
        }
        for (size_t i = 0; i < bytes / 4; i++) {
            uint32_t value = pass == 0 ? 0 : pass == 1 ? UINT32_MAX : (uint32_t)i * 2654435761u;
            if (pass == 3) value = ~value;
            if (memory[i] != value) {
                ESP_LOGE(TAG, "PSRAM mismatch: pass=%u word=%u", pass, (unsigned)i);
                free((void *)memory);
                return false;
            }
            if ((i & 0x3ffff) == 0) vTaskDelay(1);
        }
        ESP_LOGI(TAG, "PSRAM pass %u/4: 12 MiB verified", pass + 1);
    }
    free((void *)memory);
    return heap_caps_check_integrity_all(true);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, BIT0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && want_connection) {
        wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected: reason=%u", event->reason);
        if (reconnects++ < 3) esp_wifi_connect();
        else xEventGroupSetBits(wifi_events, BIT1);
    }
}

static bool test_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_events = xEventGroupCreate();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = 0;
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_scan_config_t scan = {.scan_type = WIFI_SCAN_TYPE_PASSIVE};
    esp_err_t scan_result = esp_wifi_scan_start(&scan, true);
    uint16_t count = 0;
    if (scan_result == ESP_OK) esp_wifi_scan_get_ap_num(&count);
    ESP_LOGI(TAG, "Wi-Fi passive scan: result=%s networks=%u", esp_err_to_name(scan_result), count);
    esp_wifi_clear_ap_list();

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, WIFI_SSID, sizeof(WIFI_SSID) - 1);
    memcpy(config.sta.password, WIFI_PASSWORD, sizeof(WIFI_PASSWORD) - 1);
    config.sta.threshold.authmode = sizeof(WIFI_PASSWORD) > 1 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    want_connection = true;
    ESP_ERROR_CHECK(esp_wifi_connect());
    EventBits_t bits = xEventGroupWaitBits(wifi_events, BIT0 | BIT1, false, false, pdMS_TO_TICKS(35000));
    if (!(bits & BIT0)) return false;
    wifi_ap_record_t ap = {0};
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap));
    ESP_LOGI(TAG, "Wi-Fi associated and DHCP complete: RSSI=%d dBm channel=%u auth=%u", ap.rssi, ap.primary, ap.authmode);
    return true;
}

static bool test_https(void)
{
    esp_sntp_config_t time_config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
        ESP_SNTP_SERVER_LIST("time.cloudflare.com", "pool.ntp.org"));
    ESP_ERROR_CHECK(esp_netif_sntp_init(&time_config));
    esp_err_t sync = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(12000));
    ESP_LOGI(TAG, "NTP synchronization: %s", esp_err_to_name(sync));
    if (sync != ESP_OK) return false;
    esp_http_client_config_t config = {
        .url = "https://www.cloudflare.com/cdn-cgi/trace",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 12000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_err_t result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTPS with certificate verification: result=%s status=%d", esp_err_to_name(result), status);
    esp_http_client_cleanup(client);
    return result == ESP_OK && status == 200;
}

static void heartbeat(void *arg)
{
    bool down = false;
    unsigned count = 0;
    for (;;) {
        bool now = gpio_get_level(GPIO_NUM_0) == 0;
        if (now && !down) ESP_LOGI(TAG, "BOOT button pressed: count=%u", ++count);
        down = now;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_bytes = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_bytes));
    ESP_LOGI(TAG, "Chip cores=%u revision=%u flash=%" PRIu32 " PSRAM=%u", chip.cores, chip.revision,
        flash_bytes, (unsigned)esp_psram_get_size());
    bool ram_ok = test_psram();
    led_strip_config_t strip_config = {.strip_gpio_num = 38, .max_leds = 1};
    led_strip_rmt_config_t rmt_config = {.resolution_hz = 10000000};
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led));
    probe_set_led(16, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(600));
    probe_set_led(0, 16, 0);
    vTaskDelay(pdMS_TO_TICKS(600));
    probe_set_led(0, 0, 16);
    gpio_config_t button = {.pin_bit_mask = 1ULL << GPIO_NUM_0, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&button));
    bool wifi_ok = test_wifi();
    bool https_ok = wifi_ok && test_https();
    probe_set_led(0, ram_ok && https_ok ? 16 : 0, ram_ok && https_ok ? 0 : 16);
    cJSON *summary = cJSON_CreateObject();
    cJSON_AddStringToObject(summary, "event", "hardware_ready");
    cJSON_AddBoolToObject(summary, "psram_test", ram_ok);
    cJSON_AddBoolToObject(summary, "wifi", wifi_ok);
    cJSON_AddBoolToObject(summary, "https", https_ok);
    cJSON_AddNumberToObject(summary, "flash_bytes", flash_bytes);
    cJSON_AddNumberToObject(summary, "psram_bytes", esp_psram_get_size());
    cJSON_AddNumberToObject(summary, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    probe_emit(summary);
    xTaskCreate(heartbeat, "button", 2048, NULL, 2, NULL);
    char *line = calloc(1, 16384);
    if (!line) abort();
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        int ch = getchar();
        if (ch == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if (ch == '\n') {
            line[used] = 0;
            cJSON *cmd = overflow ? NULL : cJSON_Parse(line);
            if (cmd) {
                probe_command(cmd);
                cJSON_Delete(cmd);
            }
            used = 0;
            overflow = false;
        } else if (ch != '\r' && !overflow) {
            if (used < 16383) line[used++] = ch;
            else overflow = true;
        }
    }
}
