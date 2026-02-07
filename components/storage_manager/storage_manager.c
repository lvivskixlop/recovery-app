#include "storage_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "STORAGE_MANAGER";

#define NVS_NAMESPACE "app_settings"
#define KEY_WIFI_SSID CONFIG_WIFI_SSID
#define KEY_WIFI_PASS CONFIG_WIFI_PASSWORD
#define KEY_MASTER_PASS "master_pass"
#define DEFAULT_MASTER_PASS CONFIG_APP_MASTER_PASSWORD
#define KEY_SESSION_TOKEN "auth_token"

// Helper to check error and break the do-while loop
#define CHECK_BREAK(x)         \
    if ((err = (x)) != ESP_OK) \
    break

/* --- Private Helper --- */
/* Reads a string key safely, enforcing buffer limits */
static esp_err_t nvs_read_str_helper(nvs_handle_t handle, const char* key, char* buf, size_t max_len)
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

esp_err_t storage_get_wifi_creds(char* ssid_buf, size_t ssid_len, char* pass_buf, size_t pass_len)
{
    // Catch logic errors in the caller during dev.
    // These enforce the contract that the caller MUST provide valid buffers.
    assert(ssid_buf != NULL);
    assert(pass_buf != NULL);
    assert(ssid_len > 0);
    assert(pass_len > 0);

    if (!ssid_buf || !pass_buf || ssid_len == 0 || pass_len == 0)
        return ESP_ERR_INVALID_ARG;


    // Initialize defaults to empty (Safe initialization)
    memset(ssid_buf, 0, ssid_len);
    memset(pass_buf, 0, pass_len);

    esp_err_t err = ESP_OK;

    nvs_handle_t handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err != ESP_OK)
    {
        // Fallback to Kconfig
        ESP_LOGW("STORAGE", "NVS empty. Using Kconfig defaults.");
        strncpy(ssid_buf, CONFIG_WIFI_SSID, ssid_len - 1);
        strncpy(pass_buf, CONFIG_WIFI_PASSWORD, pass_len - 1);
        return ESP_OK;
    }

    do
    {
        size_t actual_len = ssid_len;

        // 1. Read SSID
        CHECK_BREAK(nvs_get_str(handle, KEY_WIFI_SSID, ssid_buf, &actual_len));

        // 2. Read Password
        actual_len = pass_len;
        err = nvs_get_str(handle, KEY_WIFI_PASS, pass_buf, &actual_len);

        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            pass_buf[0] = '\0';
            err = ESP_OK;
        }
    } while (0);

    nvs_close(handle);

    if (err != ESP_OK)
    {
        // If NVS read failed mid-way, fallback to Kconfig
        ESP_LOGW("STORAGE", "NVS read error. Using Kconfig defaults.");
        strncpy(ssid_buf, CONFIG_WIFI_SSID, ssid_len - 1);
        strncpy(pass_buf, CONFIG_WIFI_PASSWORD, pass_len - 1);
        return ESP_OK;
    }

    return err;
}

esp_err_t storage_set_wifi_creds(const char* ssid, const char* pass)
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

esp_err_t storage_get_master_password(char* buf, size_t max_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

    // If NVS fails or key missing, fallback to Kconfig immediately
    if (err != ESP_OK)
    {
        strncpy(buf, DEFAULT_MASTER_PASS, max_len);
        return ESP_OK; // Not an error, just using default
    }

    // Try to read from NVS
    size_t required_size = 0;
    err = nvs_get_str(handle, KEY_MASTER_PASS, NULL, &required_size);

    if (err == ESP_OK && required_size <= max_len)
    {
        err = nvs_get_str(handle, KEY_MASTER_PASS, buf, &max_len);
    }
    else
    {
        // Fallback if key missing or invalid
        strncpy(buf, DEFAULT_MASTER_PASS, max_len);
        err = ESP_OK;
    }

    nvs_close(handle);
    return err;
}

esp_err_t storage_get_session_token(char* buf, size_t max_len)
{
    if (!buf || max_len == 0)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
        return err; // If NVS fails, we just won't have a session

    // Use our helper from before (safe read)
    // Note: If key doesn't exist (ESP_ERR_NVS_NOT_FOUND), buf stays empty.
    err = nvs_read_str_helper(handle, KEY_SESSION_TOKEN, buf, max_len);

    nvs_close(handle);
    return err;
}

esp_err_t storage_set_session_token(const char* token)
{
    if (!token)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;

    do
    {
        // We write the token string
        if ((err = nvs_set_str(handle, KEY_SESSION_TOKEN, token)) != ESP_OK)
            break;
        if ((err = nvs_commit(handle)) != ESP_OK)
            break;
    } while (0);

    nvs_close(handle);
    return err;
}