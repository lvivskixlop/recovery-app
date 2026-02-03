#include "wifi_manager.h"
#include "storage_manager.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"

static const char *TAG = "WIFI_MANAGER";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define MAX_STA_RETRIES 5

// Error Check Helper
#define CHECK_RET(x)       \
    do                     \
    {                      \
        esp_err_t e = (x); \
        if (e != ESP_OK)   \
            return e;      \
    } while (0)
#define CHECK_BREAK(x)         \
    if ((err = (x)) != ESP_OK) \
    break

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

/* --- Event Handler (Unchanged logic, compacted) --- */
static void sta_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < MAX_STA_RETRIES)
        {
            s_retry_num++;
            ESP_LOGI(TAG, "Retry %d/%d", s_retry_num, MAX_STA_RETRIES);
            esp_wifi_connect();
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* --- Public API --- */

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group)
        return ESP_ERR_NO_MEM;

    CHECK_RET(esp_netif_init());

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return err;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    CHECK_RET(esp_wifi_init(&cfg));

    return ESP_OK;
}

esp_err_t wifi_manager_try_connect_sta(bool *out_connected)
{
    *out_connected = false;
    char ssid[33] = {0};
    char pass[65] = {0};
    esp_err_t err = ESP_OK;

    // 1. Load Creds
    if (storage_get_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK)
    {
        ESP_LOGW(TAG, "No credentials in NVS.");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    // 2. Prepare Resources
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (!netif)
        return ESP_FAIL;

    esp_event_handler_instance_t instance_any_id = NULL;
    esp_event_handler_instance_t instance_got_ip = NULL;

    // 3. Execution Block (Safe Cleanup Pattern)
    do
    {
        // Register Handlers
        CHECK_BREAK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &sta_event_handler, NULL, &instance_any_id));
        CHECK_BREAK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &sta_event_handler, NULL, &instance_got_ip));

        // Configure
        wifi_config_t wifi_config = {0};
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        CHECK_BREAK(esp_wifi_set_mode(WIFI_MODE_STA));
        CHECK_BREAK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        CHECK_BREAK(esp_wifi_start());

        // Wait
        ESP_LOGI(TAG, "Waiting for WiFi...");
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

        if (bits & WIFI_CONNECTED_BIT)
            *out_connected = true;

    } while (0);

    // 4. Cleanup (Always runs, keeping system clean)
    if (!*out_connected)
    {
        esp_wifi_stop(); // Ensure radio is off if we failed
    }

    // Unregister handlers (Crucial to prevent leaks if called multiple times)
    if (instance_any_id)
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
    if (instance_got_ip)
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);

    // Note: We destroy the default Netif implicitly via stop/cleanup or reuse it.
    // ESP-IDF Netif handling is tricky to destroy, but stopping WiFi is the key part.

    return err;
}

esp_err_t wifi_manager_start_ap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP_RECOVERY",
            .ssid_len = strlen("ESP_RECOVERY"),
            .channel = 1,
            .max_connection = 2,
            .authmode = WIFI_AUTH_OPEN},
    };

    CHECK_RET(esp_wifi_set_mode(WIFI_MODE_AP));
    CHECK_RET(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    CHECK_RET(esp_wifi_start());

    ESP_LOGI(TAG, "AP Started.");
    return ESP_OK;
}