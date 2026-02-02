#include "storage_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "STORAGE";

// --- Configuration Constants ---
// These MUST match your Main App's configuration
#define NVS_NAMESPACE "app_settings"
#define KEY_WIFI_SSID CONFIG_WIFI_SSID
#define KEY_WIFI_PASS CONFIG_WIFI_PASSWORD

esp_err_t storage_init(void)
{
    // 1. Try to Init
    esp_err_t err = nvs_flash_init();

    // 2. Check for Corruption / No Space
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS Corrupt or Full. Erasing...");

        // 3. Self-Heal: Erase
        err = nvs_flash_erase();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(err));
            return err;
        }

        // 4. Retry Init
        err = nvs_flash_init();
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to init NVS: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t storage_get_wifi_creds(char *ssid_buf, size_t ssid_buf_len,
                                 char *pass_buf, size_t pass_buf_len)
{
    // Rule 7: Check params validity
    if (ssid_buf == NULL || pass_buf == NULL || ssid_buf_len == 0 || pass_buf_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t my_handle;
    esp_err_t err;

    // 1. Open
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK)
        return err;

    // 2. Read SSID
    // We use a temporary size variable because nvs_get_str updates it with actual length
    size_t required_size = 0;

    // 2a. Check if key exists and get length
    err = nvs_get_str(my_handle, KEY_WIFI_SSID, NULL, &required_size);
    if (err != ESP_OK)
    {
        nvs_close(my_handle);
        return err; // Likely ESP_ERR_NVS_NOT_FOUND
    }

    // 2b. Check buffer safety
    if (required_size > ssid_buf_len)
    {
        nvs_close(my_handle);
        return ESP_ERR_NVS_INVALID_LENGTH;
    }

    // 2c. Actually read
    err = nvs_get_str(my_handle, KEY_WIFI_SSID, ssid_buf, &ssid_buf_len);
    if (err != ESP_OK)
    {
        nvs_close(my_handle);
        return err;
    }

    // 3. Read Password (Optional? No, we treat as pair)
    required_size = 0;
    err = nvs_get_str(my_handle, KEY_WIFI_PASS, NULL, &required_size);
    if (err == ESP_OK)
    {
        if (required_size <= pass_buf_len)
        {
            err = nvs_get_str(my_handle, KEY_WIFI_PASS, pass_buf, &pass_buf_len);
        }
        else
        {
            err = ESP_ERR_NVS_INVALID_LENGTH;
        }
    }

    // Note: If Password is missing (ESP_ERR_NVS_NOT_FOUND), we might still want to
    // return Success if open networks are allowed.
    // For this robust implementation, we treat a missing password as an empty string.
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        pass_buf[0] = '\0'; // Empty string
        err = ESP_OK;       // Mask the error
    }

    // 4. Close
    nvs_close(my_handle);
    return err;
}

esp_err_t storage_set_wifi_creds(const char *ssid, const char *pass)
{
    if (ssid == NULL || pass == NULL)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t my_handle;
    esp_err_t err;

    // 1. Open (Read/Write)
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK)
        return err;

    // 2. Write SSID
    err = nvs_set_str(my_handle, KEY_WIFI_SSID, ssid);
    if (err != ESP_OK)
    {
        nvs_close(my_handle);
        return err;
    }

    // 3. Write Password
    err = nvs_set_str(my_handle, KEY_WIFI_PASS, pass);
    if (err != ESP_OK)
    {
        nvs_close(my_handle);
        return err;
    }

    // 4. Commit (Flash writes happen here)
    err = nvs_commit(my_handle);

    // 5. Close
    nvs_close(my_handle);

    return err;
}