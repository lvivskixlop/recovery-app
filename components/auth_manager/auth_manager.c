#include "auth_manager.h"
#include "storage_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "AUTH";

// Configuration
#define SESSION_TOKEN_LEN 64
#define SESSION_TIMEOUT_US (30 * 24 * 60 * 60 * 1000000LL) // 30 Days in Microseconds

// State (RAM)
static char s_session_token[SESSION_TOKEN_LEN + 1] = {0};
static int64_t s_last_activity_time = 0;

/* --- INTERNAL HELPERS --- */

static void generate_new_session(void)
{
    // Fill with random hex characters
    // We use esp_random() which uses the hardware RNG (noise buffer)
    const char charset[] = "0123456789abcdef";
    for (int i = 0; i < SESSION_TOKEN_LEN; i++)
    {
        uint32_t rnd = esp_random();
        s_session_token[i] = charset[rnd % 16];
    }
    s_session_token[SESSION_TOKEN_LEN] = '\0';
    s_last_activity_time = esp_timer_get_time();

    storage_set_session_token(s_session_token);
}

/* --- PUBLIC API --- */

esp_err_t auth_guard(httpd_req_t *req)
{
    // 1. Check if we even have a session active in RAM
    if (s_session_token[0] == '\0')
    {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "No active session. Log in first.");
        return ESP_FAIL;
    }

    // 2. Check Timeout
    int64_t now = esp_timer_get_time();
    if ((now - s_last_activity_time) > SESSION_TIMEOUT_US)
    {
        s_session_token[0] = '\0'; // Invalidate
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Session expired.");
        return ESP_FAIL;
    }

    // 3. Extract Cookie Header
    // "Cookie: access_token=abc12345..."
    char cookie_buf[256];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_buf, sizeof(cookie_buf)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Missing Cookie Header.");
        return ESP_FAIL;
    }

    // 4. Validate Token
    // We search for our token string inside the cookie header.
    // Ideally, we'd parse "access_token=...", but strictly checking for the random string
    // is cryptographically sufficient if the token is long enough (64 chars).
    if (strstr(cookie_buf, s_session_token) != NULL)
    {
        // Activity detected, extend session
        s_last_activity_time = now;
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid Token.");
    return ESP_FAIL;
}

/* --- LOGIN HANDLER --- */

static esp_err_t login_post_handler(httpd_req_t *req)
{
    char buf[256]; // Buffer for JSON body
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);

    if (ret <= 0)
    {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT)
        {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // 1. Parse Password
    cJSON *root = cJSON_Parse(buf);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *pass_item = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(pass_item))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing password field");
        return ESP_FAIL;
    }

    // 2. Verify against Storage (NVS/Kconfig)
    char stored_pass[65] = {0};
    storage_get_master_password(stored_pass, sizeof(stored_pass));

    if (strcmp(pass_item->valuestring, stored_pass) == 0)
    {
        // --- SUCCESS ---
        generate_new_session();

        // Send Cookie Header
        // Max-Age=2592000 (30 days)
        char set_cookie[200];
        snprintf(set_cookie, sizeof(set_cookie),
                 "access_token=%s; Max-Age=2592000; Path=/; HttpOnly", s_session_token);

        httpd_resp_set_hdr(req, "Set-Cookie", set_cookie);
        httpd_resp_sendstr(req, "Login Success");

        ESP_LOGI(TAG, "User logged in. Session created.");
    }
    else
    {
        // --- FAILURE ---
        ESP_LOGW(TAG, "Login failed. Wrong password.");
        // Add a small delay to prevent brute-force timing attacks
        vTaskDelay(pdMS_TO_TICKS(1000));
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Wrong Password");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

void auth_manager_init(httpd_handle_t server)
{
    // We try to fill the RAM cache. If it fails (no key), it stays empty (requires login).
    if (storage_get_session_token(s_session_token, sizeof(s_session_token)) == ESP_OK)
    {
        if (strlen(s_session_token) > 0)
        {
            ESP_LOGI(TAG, "Restored active session from NVS.");
            // Reset activity timer so they have a fresh 30 days from boot
            s_last_activity_time = esp_timer_get_time();
        }
    }

    httpd_uri_t login_uri = {
        .uri = "/login",
        .method = HTTP_POST,
        .handler = login_post_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &login_uri);
}