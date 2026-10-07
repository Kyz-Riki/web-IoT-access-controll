#ifndef CONFIG_H
#define CONFIG_H

// ============================================
//  KONFIGURASI HARDWARE PINS
// ============================================

// RFID MFRC522 (SPI)
#define SS_PIN      5    // SDA RFID
#define RST_PIN     4    // RST RFID
// SCK  = GPIO 18 (default SPI)
// MOSI = GPIO 23 (default SPI)
// MISO = GPIO 19 (default SPI)

// Relay
#define RELAY_PIN   22   // IN relay module
#define RELAY_ACTIVE_LOW  true   // true jika relay module active LOW

// Buzzer (opsional, set -1 jika tidak pakai)
#define BUZZER_PIN  -1

// LED Indikator (opsional, set -1 jika tidak pakai)
#define LED_GREEN_PIN  -1
#define LED_RED_PIN    -1

// Push Button Manual Unlock (exit button dari dalam)
#define BUTTON_PIN          21      // GPIO untuk push button (NO ke GND)
#define BUTTON_DEBOUNCE_MS  200     // Debounce delay (ms)

// Door Sensor / Sensor Magnet (kabel putih dropbolt)
#define DOOR_SENSOR_PIN     25      // GPIO untuk sensor magnet (ke GND)
// Logika: pintu TERTUTUP → sensor LOW, pintu TERBUKA → sensor HIGH
// (menggunakan INPUT_PULLUP)

// ============================================
//  KONFIGURASI WIFI
// ============================================

#define WIFI_SSID       "VALVINDO"
#define WIFI_PASSWORD   ""
#define WIFI_TIMEOUT_MS 10000   // Timeout koneksi WiFi (ms)
#define WIFI_RETRY_INTERVAL_MS 30000  // Interval retry jika WiFi putus

// ============================================
//  KONFIGURASI SERVER
// ============================================

#define SERVER_HOST     "192.168.1.10"   // IP laptop/server lokal
#define SERVER_PORT     8000
#define SERVER_BASE_URL "http://192.168.1.10:8000"

// API Endpoints
#define API_WHITELIST       "/api/esp/whitelist"
#define API_TAP             "/api/esp/tap"
#define API_HEARTBEAT       "/api/esp/heartbeat"
#define API_DOOR_STATUS     "/api/esp/door-status"
#define API_MODE            "/api/esp/mode"
#define API_REGISTER_SCAN   "/api/esp/register-scan"

// ============================================
//  KONFIGURASI ESP32
// ============================================

#define ESP_ID          "esp-01"       // ID unik untuk ESP ini
#define DOOR_NAME       "Pintu Utama"  // Nama pintu yang dikontrol

// ============================================
//  TIMING (dalam milidetik)
// ============================================

#define UNLOCK_DURATION_MS       2000    // Durasi unlock dropbolt
#define ANTI_DOUBLE_TAP_MS       1500    // Jeda anti double-tap
#define HEARTBEAT_INTERVAL_MS    30000   // Kirim heartbeat tiap 30 detik
#define WHITELIST_SYNC_INTERVAL_MS 300000 // Sync whitelist tiap 5 menit
#define MODE_CHECK_INTERVAL_MS   2000    // Cek mode register tiap 2 detik
#define HTTP_TIMEOUT_MS          3000    // Timeout HTTP request

// ============================================
//  NTP (Network Time Protocol)
// ============================================

#define NTP_SERVER      "pool.ntp.org"
#define NTP_GMT_OFFSET  25200   // GMT+7 (WIB) dalam detik = 7 * 3600
#define NTP_DAYLIGHT    0       // Tidak pakai daylight saving

// ============================================
//  KAPASITAS
// ============================================

#define MAX_CARDS       200     // Maksimal kartu di whitelist lokal
#define MAX_SCHEDULES   20      // Maksimal rule jadwal
#define UID_MAX_LENGTH  12      // Panjang maksimal string UID

// ============================================
//  SERIAL MONITOR
// ============================================

#define SERIAL_BAUD     115200

#endif // CONFIG_H
