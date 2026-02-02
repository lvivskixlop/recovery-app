#pragma once

#include "esp_err.h"

/**
 * @brief Starts the HTTP Server on Port 80.
 * Registers handlers for:
 * - POST /ota      (Firmware upload)
 * - POST /settings (WiFi Credentials update)
 * * @return ESP_OK on success.
 */
esp_err_t server_start(void);