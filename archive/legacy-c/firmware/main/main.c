#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "radio.h"
#include "signaling.h"
#include "wifi_private.h"

static EventGroupHandle_t wifi_events;
static led_strip_handle_t led;

void radio_emit(cJSON *message)
{
    if (!message) return;
    if (signaling_observe(message)) { cJSON_Delete(message); return; }
    char *text = cJSON_PrintUnformatted(message);
    if (text) { printf("PROBE %s\n", text); free(text); }
    cJSON_Delete(message);
}

int radio_set_led(unsigned r, unsigned g, unsigned b)
{
    esp_err_t err = led_strip_set_pixel(led, 0, r, g, b);
    return err == ESP_OK ? led_strip_refresh(led) : err;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(wifi_events, BIT0);
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, BIT0);
        esp_wifi_connect();
    }
}

bool radio_network_ready(void)
{
    return wifi_events && (xEventGroupGetBits(wifi_events) & BIT0);
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    led_strip_config_t strip = {.strip_gpio_num = 38, .max_leds = 1};
    led_strip_rmt_config_t rmt = {.resolution_hz = 10000000};
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip, &rmt, &led));
    radio_set_led(8, 4, 0);
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
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, WIFI_SSID, sizeof(WIFI_SSID) - 1);
    memcpy(config.sta.password, WIFI_PASSWORD, sizeof(WIFI_PASSWORD) - 1);
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_connect());
    xEventGroupWaitBits(wifi_events, BIT0, false, false, portMAX_DELAY);
    radio_set_led(0, 12, 4);
    cJSON *ready = cJSON_CreateObject();
    cJSON_AddStringToObject(ready, "event", "hardware_ready");
    cJSON_AddBoolToObject(ready, "wifi", true);
    cJSON_AddNumberToObject(ready, "psram_bytes", esp_psram_get_size());
    cJSON_AddNumberToObject(ready, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    radio_emit(ready);
    radio_init();
    signaling_start();
    char *line = calloc(1, 16384);
    if (!line) abort();
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        int ch = getchar();
        if (ch == EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(10)); }
        else if (ch == '\n') {
            line[used] = 0;
            cJSON *command = overflow ? NULL : cJSON_Parse(line);
            if (command) { radio_command(command); cJSON_Delete(command); }
            used = 0;
            overflow = false;
        } else if (ch != '\r' && !overflow) {
            if (used < 16383) line[used++] = ch;
            else overflow = true;
        }
    }
}
