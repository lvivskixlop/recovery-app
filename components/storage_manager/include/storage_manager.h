#pragma once

#include "esp_err.h"
#include <stddef.h> // For size_t

/**
 * @brief Initializes NVS flash.
 * Implements "Self-Healing": If NVS is corrupt or has no free pages,
 * it erases the partition and re-initializes.
 * * @return ESP_OK on success.
 */
esp_err_t storage_init(void);

/**
 * @brief Reads WiFi credentials from NVS.
 * * @param[out] ssid_buf      Buffer to hold the SSID.
 * @param[in]  ssid_buf_len  Size of ssid_buf (should be >= 33).
 * @param[out] pass_buf      Buffer to hold the Password.
 * @param[in]  pass_buf_len  Size of pass_buf (should be >= 65).
 * * @return ESP_OK if found and loaded.
 * @return ESP_ERR_NVS_NOT_FOUND if keys don't exist.
 * @return ESP_ERR_NVS_INVALID_LENGTH if buffers are too small.
 */
esp_err_t storage_get_wifi_creds(char *ssid_buf, size_t ssid_buf_len,
                                 char *pass_buf, size_t pass_buf_len);

/**
 * @brief Writes WiFi credentials to NVS.
 * * @param[in] ssid  Null-terminated SSID string.
 * @param[in] pass  Null-terminated Password string.
 * * @return ESP_OK on success.
 */
esp_err_t storage_set_wifi_creds(const char *ssid, const char *pass);

/**
 * @brief Reads the Master Password from NVS.
 * If NVS is empty, it returns the Kconfig default.
 */
esp_err_t storage_get_master_password(char *buf, size_t max_len);