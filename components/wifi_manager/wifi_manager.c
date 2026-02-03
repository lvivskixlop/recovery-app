#include "wifi_manager.h"
#include "storage_manager.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"

static const char *TAG = "WIFI_MANAGER";

// --- Constants ---
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define MAX_STA_RETRIES 5

// --- Static State ---
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

// --- Event Handler ---
static void sta_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_wifi_connect failed in handler: %s", esp_err_to_name(err));
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGE(TAG, "Disconnect Reason: %d", d->reason);

        if (s_retry_num < MAX_STA_RETRIES)
        {
            s_retry_num++;
            ESP_LOGI(TAG, "Retry %d/%d", s_retry_num, MAX_STA_RETRIES);

            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Retry connect failed: %s", esp_err_to_name(err));
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// --- Public Functions ---

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK)
        return err;

    err = esp_event_loop_create_default();
    // ESP_ERR_INVALID_STATE means loop already created (safe to ignore usually,
    // but strictly we check for OK)
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return err;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK)
        return err;

    return ESP_OK;
}

esp_err_t wifi_manager_try_connect_sta(bool *out_connected)
{
    *out_connected = false;
    esp_err_t err = ESP_OK;

    // 1. Get Credentials
    char ssid[33] = {0};
    char pass[65] = {0};

    if (storage_get_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK)
    {
        ESP_LOGW(TAG, "Storage empty or failed.");
        return ESP_OK; // Logic success, but connection false
    }

    ESP_LOGW(TAG, "SSID: '%s' (Len: %d)", ssid, strlen(ssid));
    ESP_LOGW(TAG, "Password: '%s' (Len: %d)", pass, strlen(pass));

    // 2. Create Netif (Check Pointer)
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (netif == NULL)
    {
        ESP_LOGE(TAG, "Failed to create STA netif");
        return ESP_FAIL;
    }

    // 3. Register Handlers (Check Returns)
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, NULL, &instance_any_id);
    if (err != ESP_OK)
        return err;

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL, &instance_got_ip);
    if (err != ESP_OK)
    {
        // Cleanup previous register before returning
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
        return err;
    }

    // 4. Configure & Start (Check Returns)
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
        return err;

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK)
        return err;

    err = esp_wifi_start();
    if (err != ESP_OK)
        return err;

    // 5. Wait for Result
    ESP_LOGI(TAG, "Waiting for WiFi...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(15000));

    // 6. Cleanup
    // We try to unregister. If this fails, it's a logic bug, but we can't do much
    // other than log it. It doesn't affect the 'connected' status.
    esp_err_t unreg_err = esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
    if (unreg_err != ESP_OK)
        ESP_LOGE(TAG, "Failed to unregister IP handler");

    unreg_err = esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
    if (unreg_err != ESP_OK)
        ESP_LOGE(TAG, "Failed to unregister WiFi handler");

    if (bits & WIFI_CONNECTED_BIT)
    {
        *out_connected = true;
    }
    else
    {
        ESP_LOGW(TAG, "STA Failed. Stopping WiFi.");
        *out_connected = false;

        // Ensure we stop WiFi so we can restart in AP mode cleanly
        err = esp_wifi_stop();
        if (err != ESP_OK)
            ESP_LOGE(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));

        // Note: We don't return 'err' here because the PRIMARY job (try connect)
        // finished. The failure to stop is a secondary error.
    }

    return ESP_OK;
}

esp_err_t wifi_manager_start_ap(void)
{
    // Check Pointer
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    if (netif == NULL)
        return ESP_FAIL;

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP_RECOVERY",
            .ssid_len = strlen("ESP_RECOVERY"),
            .channel = 1,
            .password = "",
            .max_connection = 2,
            .authmode = WIFI_AUTH_OPEN},
    };

    esp_err_t err;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK)
        return err;

    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK)
        return err;

    err = esp_wifi_start();
    if (err != ESP_OK)
        return err;

    ESP_LOGI(TAG, "AP Started.");
    return ESP_OK;
}