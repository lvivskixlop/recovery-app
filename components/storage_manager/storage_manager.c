#include "storage_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "STORAGE_MANAGER";

#define NVS_NAMESPACE "app_settings"
#define KEY_WIFI_SSID CONFIG_WIFI_SSID
#define KEY_WIFI_PASS CONFIG_WIFI_PASSWORD

// Helper to check error and break the do-while loop
#define CHECK_BREAK(x)         \
    if ((err = (x)) != ESP_OK) \
    break

/* --- Private Helper --- */
/* Reads a string key safely, enforcing buffer limits */
static esp_err_t nvs_read_str_helper(nvs_handle_t handle, const char *key, char *buf, size_t max_len)
{
    size_t required_size = 0;
    esp_err_t err;

    // 1. Get size
    err = nvs_get_str(handle, key, NULL, &required_size);
    if (err != ESP_OK)
        return err;

    // 2. Validate buffer
    if (required_size > max_len)
        return ESP_ERR_NVS_INVALID_LENGTH;

    // 3. Read data
    return nvs_get_str(handle, key, buf, &max_len);
}

/* --- Public API --- */

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS Recovery: Erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t storage_get_wifi_creds(char *ssid_buf, size_t ssid_len,
                                 char *pass_buf, size_t pass_len)
{
    if (!ssid_buf || !pass_buf || ssid_len == 0 || pass_len == 0)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
        return err;

    // "Do-While-0" block for safe resource cleanup
    do
    {
        // 1. Read SSID
        CHECK_BREAK(nvs_read_str_helper(handle, KEY_WIFI_SSID, ssid_buf, ssid_len));

        // 2. Read Password
        err = nvs_read_str_helper(handle, KEY_WIFI_PASS, pass_buf, pass_len);
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            pass_buf[0] = '\0';
            err = ESP_OK;
        }
    } while (0);

    // Cleanup always happens here
    nvs_close(handle);
    return err;
}

esp_err_t storage_set_wifi_creds(const char *ssid, const char *pass)
{
    if (!ssid || !pass)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;

    do
    {
        CHECK_BREAK(nvs_set_str(handle, KEY_WIFI_SSID, ssid));
        CHECK_BREAK(nvs_set_str(handle, KEY_WIFI_PASS, pass));
        CHECK_BREAK(nvs_commit(handle));
    } while (0);

    nvs_close(handle);
    return err;
}