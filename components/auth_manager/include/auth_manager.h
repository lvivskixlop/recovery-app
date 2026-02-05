#pragma once
#include "esp_http_server.h"
#include "esp_err.h"

/**
 * @brief Initializes the Auth System.
 * Registers the POST /login route.
 */
void auth_manager_init(httpd_handle_t server);

/**
 * @brief Route Guard.
 * Call this at the start of any handler you want to protect.
 * @return ESP_OK if authorized.
 * @return ESP_FAIL if unauthorized (and sends 401 response automatically).
 */
esp_err_t auth_guard(httpd_req_t *req);