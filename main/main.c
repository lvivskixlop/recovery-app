/* main.c */

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage_manager.h"
#include "wifi_manager.h"
#include "server_manager.h"
#include "auth_manager.h"

static const char *TAG = "MAIN";

#define REQUIRE(condition, error_code, msg)                                  \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            ESP_LOGE(TAG, "CRITICAL FAILURE: %s. Reverting/Aborting.", msg); \
            return error_code;                                               \
        }                                                                    \
    } while (0)

static esp_err_t system_setup(void);
static void system_loop(void);

void app_main(void)
{
    ESP_LOGI(TAG, "=== RECOVERY MODE STARTING ===");

    esp_err_t status = system_setup();

    if (status != ESP_OK)
    {
        ESP_LOGE(TAG, "System Setup Failed with error: %d", status);
        // If the recovery app itself fails setup, we might force a restart
        // or hang to prevent undefined behavior.
        // For now, we log and halt.
        return;
    }

    ESP_LOGI(TAG, "System Setup Complete. Entering Main Loop.");

    // Transfer control to the infinite loop
    system_loop();
}

/**
 * @brief  Initializes all subsystems in a deterministic order.
 */
static esp_err_t system_setup(void)
{
    esp_err_t err = ESP_OK;

    // 1. Initialize Storage (NVS)
    err = storage_init();
    REQUIRE(err == ESP_OK, err, "NVS Init Failed");

    // 2. Initialize WiFi Hardware
    err = wifi_manager_init();
    REQUIRE(err == ESP_OK, err, "WiFi Init Failed");

    // 3. Attempt Connection Strategy
    // We try to connect to Station. If that fails, we MUST fall back to AP.
    // We do NOT return error here if STA fails; we recover by starting AP.
    bool is_connected = false;

    err = wifi_manager_try_connect_sta(&is_connected);

    // Check return value of the function itself (did the logic crash?)
    REQUIRE(err == ESP_OK, err, "WiFi Station Logic Failed");

    if (is_connected)
    {
        ESP_LOGI(TAG, "Connected to Router. Ready for OTA.");
    }
    else
    {
        ESP_LOGW(TAG, "Could not connect to Router. FALLBACK: Starting AP.");
        err = wifi_manager_start_ap();
        REQUIRE(err == ESP_OK, err, "WiFi AP Start Failed");
    }

    // 4. Start Web Server
    err = server_start();
    REQUIRE(err == ESP_OK, err, "Web Server Start Failed");

    return ESP_OK;
}

/**
 * @brief  Main idle loop.
 */
static void system_loop(void)
{
    const int LOOP_DELAY_MS = 1000;

    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(LOOP_DELAY_MS));
    }
}