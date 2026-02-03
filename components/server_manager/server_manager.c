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

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
static const char *TAG = "SERVER";

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
    char buf[200];
    int ret = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (ret <= 0)
        return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root)
        FAIL_HTTP(req, "JSON Parse Error");

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass = cJSON_GetObjectItem(root, "password");

    esp_err_t err = ESP_FAIL;
    if (cJSON_IsString(ssid) && cJSON_IsString(pass))
    {
        err = storage_set_wifi_creds(ssid->valuestring, pass->valuestring);
    }
    cJSON_Delete(root);

    if (err == ESP_OK)
    {
        httpd_resp_sendstr(req, "Settings Saved. Rebooting...");
        trigger_restart();
        return ESP_OK;
    }
    FAIL_HTTP(req, "Failed to write Settings");
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    char buf[1024];
    esp_ota_handle_t handle = 0;
    esp_err_t err;

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part)
        FAIL_HTTP(req, "No OTA Partition found");

    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle) != ESP_OK)
        FAIL_HTTP(req, "OTA Begin Failed");

    int remaining = req->content_len;
    while (remaining > 0)
    {
        int received = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));
        if (received < 0)
        {
            if (received == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            esp_ota_end(handle);
            return ESP_FAIL;
        }
        if (received > 0)
        {
            if (esp_ota_write(handle, buf, received) != ESP_OK)
            {
                esp_ota_end(handle);
                FAIL_HTTP(req, "Flash Write Failed");
            }
            remaining -= received;
        }
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