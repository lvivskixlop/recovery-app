#include "server_manager.h"
#include "storage_manager.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_OTA_TIMEOUT_RETRIES 5
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
static const char *TAG = "SERVER_MANAGER";

// Macro to simplify error exits
#define FAIL_HTTP(req, msg)       \
    do                            \
    {                             \
        ESP_LOGE(TAG, "%s", msg); \
        httpd_resp_send_500(req); \
        return ESP_FAIL;          \
    } while (0)

/* --- HELPER: Restart --- */
static void restart_task(void *param)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}
static void trigger_restart(void)
{
    xTaskCreate(restart_task, "restart_task", 2048, NULL, 5, NULL);
}

/* --- HANDLERS --- */

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (req == NULL || req->aux == NULL)
        return ESP_FAIL;
    if (req->content_len <= 0 || req->content_len > 200)
    {
        FAIL_HTTP(req, "Invalid Content Length");
    }

    char buf[201];
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0)
        return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root)
        FAIL_HTTP(req, "JSON Parse Error");

    cJSON *ssid_json = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_json = cJSON_GetObjectItem(root, "password");

    esp_err_t err = ESP_FAIL;

    if (cJSON_IsString(ssid_json) && cJSON_IsString(pass_json))
    {
        const char *s_str = ssid_json->valuestring;
        const char *p_str = pass_json->valuestring;
        size_t s_len = strlen(s_str);
        size_t p_len = strlen(p_str);

        // Validate WiFi Constraints (SSID <= 32, Pass <= 64)
        if (s_len > 0 && s_len <= 32 && p_len <= 64)
        {
            err = storage_set_wifi_creds(s_str, p_str);
        }
        else
        {
            ESP_LOGE(TAG, "Invalid SSID/Pass length");
        }
    }

    cJSON_Delete(root);

    if (err == ESP_OK)
    {
        if (httpd_resp_sendstr(req, "Settings Saved. Rebooting...") != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to send response");
        }
        trigger_restart();
        return ESP_OK;
    }

    FAIL_HTTP(req, "Failed to write Settings");
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    char buf[1024];
    esp_ota_handle_t handle = 0;
    int timeout_retries = 0; // Guard for infinite timeout loop

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part)
        FAIL_HTTP(req, "No OTA Partition found");

    if (part->type != ESP_PARTITION_TYPE_APP)
    {
        FAIL_HTTP(req, "ASSERT FAIL: Target partition is not an APP partition!");
    }

    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle) != ESP_OK)
        FAIL_HTTP(req, "OTA Begin Failed");

    int remaining = req->content_len;
    while (remaining > 0)
    {
        int received = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));
        if (received < 0)
        {
            if (received == HTTPD_SOCK_ERR_TIMEOUT)
            {
                timeout_retries++;
                if (timeout_retries >= MAX_OTA_TIMEOUT_RETRIES)
                {
                    ESP_LOGE(TAG, "OTA Socket Timeout limit reached. Aborting.");
                    esp_ota_end(handle);
                    return ESP_FAIL;
                }

                ESP_LOGW(TAG, "Socket Timeout, retrying... (%d/%d)", timeout_retries, MAX_OTA_TIMEOUT_RETRIES);
                continue;
            }

            // Other socket errors are fatal
            esp_ota_end(handle);
            return ESP_FAIL;
        }
        if (received > 0)
        {
            timeout_retries = 0;
            if (received > remaining)
            {
                esp_ota_end(handle);
                FAIL_HTTP(req, "CRITICAL: OTA Buffer Overflow Logic Error");
            }

            if (esp_ota_write(handle, buf, received) != ESP_OK)
            {
                esp_ota_end(handle);
                FAIL_HTTP(req, "Flash Write Failed");
            }
            remaining -= received;
        }
    }

    if (remaining != 0)
    {
        // This implies we exited the loop but didn't finish
        esp_ota_end(handle);
        FAIL_HTTP(req, "OTA Stream Mismatch");
    }

    if (esp_ota_end(handle) != ESP_OK)
        FAIL_HTTP(req, "OTA Validation Failed");
    if (esp_ota_set_boot_partition(part) != ESP_OK)
        FAIL_HTTP(req, "Set Boot Partition Failed");

    httpd_resp_sendstr(req, "Update Success. Rebooting...");
    trigger_restart();
    return ESP_OK;
}

/* --- INIT --- */
esp_err_t server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;

    if (httpd_start(&server, &config) != ESP_OK)
        return ESP_FAIL;

    httpd_uri_t ota_uri = {.uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler};
    httpd_register_uri_handler(server, &ota_uri);

    httpd_uri_t settings_uri = {.uri = "/settings", .method = HTTP_POST, .handler = settings_post_handler};
    httpd_register_uri_handler(server, &settings_uri);

    ESP_LOGI(TAG, "Server Started.");
    return ESP_OK;
}