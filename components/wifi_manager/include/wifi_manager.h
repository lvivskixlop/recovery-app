#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Initializes the underlying WiFi software stack (Netif, Event Loop).
 * Does NOT start the radio yet.
 * @return ESP_OK on success.
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Attempts to connect to the WiFi credentials found in Storage.
 * This function BLOCKS execution until connection succeeds or fails/times out.
 * * @param[out] out_connected  Set to true if connected, false if failed.
 * @return ESP_OK if the attempt logic ran correctly (even if connection failed).
 */
esp_err_t wifi_manager_try_connect_sta(bool *out_connected);

/**
 * @brief Starts the Emergency Access Point (SSID: ESP_RECOVERY).
 * @return ESP_OK on success.
 */
esp_err_t wifi_manager_start_ap(void);