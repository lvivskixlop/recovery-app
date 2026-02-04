# ESP32 Fail-Safe Recovery Firmware

A fail-safe factory firmware for ESP32. This application resides in the `factory` partition and provides a robust safety net for IoT devices. Its sole purpose is to rescue the device from bad updates, corrupted credentials, or boot loops by providing a reliable HTTP interface for OTA updates and settings management.

## ✨ Features

* **Robust State Machine:** Automatically falls back to Access Point mode (`ESP_RECOVERY`) if the configured WiFi is unavailable.
* **Self-Healing NVS:** Detects and repairs corrupted NVS partitions automatically on boot.
* **Streamed OTA Updates:** Supports uploading large firmware binaries (`.bin`) via HTTP POST, regardless of RAM limitations.
* **JSON Settings API:** Simple REST API to update WiFi credentials without reflashing.

## 💾 Partition Table

**Crucial:** Both your Recovery App and Main App **MUST** use the exact same `partitions.csv` to ensure memory layout compatibility.

```csv
# Name, Type, SubType, Offset, Size, Flags
nvs, data, nvs, , 0x4000,
otadata, data, ota, , 0x2000,
phy_init, data, phy, , 0x1000,
factory, app, factory, , 1M,
ota_0, app, ota_0, , 2900K,
```

## 📡 API Reference

### 1. Update WiFi Credentials

**Endpoint:** `POST /settings`

**Content-Type:** `application/json`

Updates the stored WiFi credentials. The device will reboot and attempt to connect immediately.

**Request Body:**

```json
{
  "ssid": "MyHomeNetwork",
  "password": "SuperSecretPassword"
}
```

### 2. Upload New Firmware (OTA)

**Endpoint:** `POST /ota`

**Content-Type:** `application/octet-stream` (Binary Body)

Streams a compiled `.bin` file directly to the `ota_0` partition. Upon success, the device reboots into the new Main App.

**Example (cURL):**

```bash
curl -X POST --data-binary @my_main_app.bin http://<ESP_IP>/ota
```

## 📘 Guidelines for the "Main App"

To fully utilize this recovery architecture, your Main App must implement specific "Lifecycle Safety" features.

### 1. NVS Synchronization (Source of Truth)

The Recovery App relies on specific NVS keys (`ssid`, `password`) in the `app_settings` namespace. Your Main App must sync its config to these keys so the Recovery App can connect to the same network.

**Required Logic in Main App Init:**

1. Read Main App config (e.g., from Kconfig or internal storage).
2. Read NVS keys `ssid` and `password`.
3. If they differ, overwrite NVS with Main App config.

### 2. Boot Loop Protection (The "Three Strikes" Rule)

To prevent a buggy update from permanently bricking the device (by looping forever in a crashing Main App), add this logic to the very top of `app_main()`:

```c
static RTC_NOINIT_ATTR int boot_crash_count;

void app_main(void) {
  // 1. Check Crash History
  if (esp_reset_reason() != ESP_RST_POWERON) {
    boot_crash_count++;
  } else {
    boot_crash_count = 0;
  }

  // 2. Emergency Fallback
  if (boot_crash_count >= 3) {
    // Switch boot partition back to FACTORY (Recovery App)
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    esp_ota_set_boot_partition(factory);
    esp_restart();
  }

  // ... Rest of initialization ...

  // 3. Reset Counter (after successful init)
  boot_crash_count = 0;
}
```

### 3. Manual Recovery Trigger

Allow users to manually force the device into Recovery Mode (e.g., via a physical button hold or a specific API call).

```c
void reboot_to_recovery(void) {
  const esp_partition_t *factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
  esp_ota_set_boot_partition(factory);
  esp_restart();
}
```
