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

/* --- HELPER: Restart Task --- */
/* We can't restart inside the HTTP handler because the response
   wouldn't finish sending. We spawn this tiny task to wait 2s then reboot. */
static void restart_task(void *param)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

static void trigger_restart(void)
{
    xTaskCreate(restart_task, "restart_task", 2048, NULL, 5, NULL);
}

/* --- HANDLER: POST /settings --- */
/* Expects JSON: {"ssid": "MyWiFi", "password": "123"} */
static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char buf[200]; // Rule 2: Fixed bound on input size
    int ret, remaining = req->content_len;

    // 1. Safety Check: Body too large?
    if (remaining >= sizeof(buf))
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 2. Read Data
    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0)
    {
        return ESP_FAIL;
    }
    buf[ret] = '\0'; // Null-terminate

    // 3. Parse JSON
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");

    // 4. Validate & Save
    esp_err_t err = ESP_FAIL;
    if (cJSON_IsString(ssid_item) && cJSON_IsString(pass_item))
    {
        err = storage_set_wifi_creds(ssid_item->valuestring, pass_item->valuestring);
    }

    cJSON_Delete(root); // Clean up JSON object

    if (err == ESP_OK)
    {
        httpd_resp_sendstr(req, "Settings Saved. Rebooting...");
        ESP_LOGW(TAG, "New settings saved. Rebooting system.");
        trigger_restart();
        return ESP_OK;
    }
    else
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
}

/* --- HANDLER: POST /ota --- */
/* Streams binary data directly to the OTA partition */
static esp_err_t ota_post_handler(httpd_req_t *req)
{
    char buf[1024]; // 1KB Buffer (Stack)
    esp_err_t err;
    esp_ota_handle_t update_handle = 0;

    // 1. Find the target partition (ota_0)
    // In our CSV, the factory app runs from 'factory'.
    // The Update engine automatically looks for the first "app" partition that isn't the running one.
    // Since we are running 'factory', it should find 'ota_0'.
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    if (update_partition == NULL)
    {
        ESP_LOGE(TAG, "No OTA partition found!");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%lx",
             update_partition->subtype, update_partition->address);

    // 2. Begin OTA (Erase flash)
    // OTA_SIZE_UNKNOWN lets us stream without knowing exact size upfront
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 3. The STREAM LOOP (Bucket Brigade)
    int remaining = req->content_len;
    int received;

    // Rule 2: Loop is bounded by content_len.
    // If content_len is huge, we trust the user (authenticated via WiFi) won't DoS us.
    // A watchdog could optionally be fed here.
    while (remaining > 0)
    {
        // 3a. Receive from Network
        received = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));

        if (received < 0)
        {
            // Error handling for socket errors
            if (received == HTTPD_SOCK_ERR_TIMEOUT)
            {
                continue; // Retry
            }
            ESP_LOGE(TAG, "Socket Error");
            esp_ota_end(update_handle); // Abort
            return ESP_FAIL;
        }

        // 3b. Write to Flash
        if (received > 0)
        {
            err = esp_ota_write(update_handle, buf, received);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Flash Write Failed (%s)", esp_err_to_name(err));
                esp_ota_end(update_handle);
                httpd_resp_send_500(req);
                return ESP_FAIL;
            }
            remaining -= received;
        }
    }

    // 4. Finalize & Verify
    err = esp_ota_end(update_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "OTA End/Validation Failed (%s)", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 5. Activate New Partition
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Set Boot Partition Failed (%s)", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 6. Success Response
    httpd_resp_sendstr(req, "Update Success. Rebooting...");
    ESP_LOGI(TAG, "OTA Complete. Rebooting...");

    trigger_restart();
    return ESP_OK;
}

/* --- Public Init Function --- */
esp_err_t server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Increase stack size! OTA handlers use 1KB buffer + system overhead.
    // Default is usually 4096, bump to 8192 to be safe.
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK)
        return err;

    // Register OTA
    httpd_uri_t ota_uri = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_post_handler,
        .user_ctx = NULL};
    err = httpd_register_uri_handler(server, &ota_uri);
    if (err != ESP_OK)
        return err;

    // Register Settings
    httpd_uri_t settings_uri = {
        .uri = "/settings",
        .method = HTTP_POST,
        .handler = settings_post_handler,
        .user_ctx = NULL};
    err = httpd_register_uri_handler(server, &settings_uri);
    if (err != ESP_OK)
        return err;

    ESP_LOGI(TAG, "Web Server Started.");
    return ESP_OK;
}