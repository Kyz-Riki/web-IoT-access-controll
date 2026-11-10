#ifndef CONFIG_H
#define CONFIG_H

// ============================================
//  TEST-DEV: Konfigurasi untuk Testing
//  Berdasarkan template.ino (Wiegand dengan filter pulsa)
//  + koneksi ke server web (FastAPI)
// ============================================

// ============================================
//  HARDWARE PINS — ESP32-S3
// ============================================

// RFID Reader Wiegand (dari template.ino)
#define PIN_D0          4    // Data 0 (kabel Hijau) — voltage divider 5V→3.3V
#define PIN_D1          5    // Data 1 (kabel Kuning) — voltage divider 5V→3.3V

// Relay — dropbolt lock
#define PIN_RELAY       6    // Relay IN (resistor 1k seri ke IN relay)
#define RELAY_ACTIVE_LOW  true   // true = relay module active LOW

// Push Button — manual unlock dari dalam (NO ke GND)
#define PIN_BTN         15   // kaki 1 ke GND, kaki 2 ke pin 15

// LED & Buzzer Reader (opsional, set -1 jika tidak pakai)
#define RFID_LED_PIN    -1   // LED reader (active LOW)
#define RFID_BUZZER_PIN -1   // Buzzer reader (active LOW)

// Door Sensor (opsional, set -1 jika tidak pakai)
#define DOOR_SENSOR_PIN -1

// ============================================
//  KONFIGURASI NETWORK — WiFi Only (untuk tes)
// ============================================

#define WIFI_SSID       "VALVINDO"
#define WIFI_PASSWORD   ""
#define WIFI_TIMEOUT_MS 10000

// ============================================
//  KONFIGURASI SERVER
// ============================================

#define SERVER_HOST     "172.16.10.255"   // IP laptop/server
#define SERVER_PORT     8000
#define SERVER_BASE_URL "http://172.16.10.255:8000"

// API Endpoints (sesuai server/routers/esp.py)
#define API_WHITELIST       "/api/esp/whitelist"
#define API_TAP             "/api/esp/tap"
#define API_HEARTBEAT       "/api/esp/heartbeat"
#define API_DOOR_STATUS     "/api/esp/door-status"
#define API_MODE            "/api/esp/mode"
#define API_REGISTER_SCAN   "/api/esp/register-scan"

// ============================================
//  IDENTITAS ESP32
// ============================================

#define ESP_ID          "esp-01"       // ID unik ESP ini
#define DOOR_NAME       "Pintu Depan Dev"    // Nama pintu

// ============================================
//  TIMING
// ============================================

#define UNLOCK_MS               3000    // Durasi unlock dropbolt (ms)
#define ANTI_DOUBLE_TAP_MS      1500    // Jeda anti double-tap (ms)
#define HEARTBEAT_INTERVAL_MS   30000   // Kirim heartbeat tiap 30 detik
#define WHITELIST_SYNC_MS       300000  // Sync whitelist tiap 5 menit
#define MODE_CHECK_MS           2000    // Cek mode register tiap 2 detik
#define HTTP_TIMEOUT_MS         3000    // Timeout HTTP request (ms)
#define BTN_DEBOUNCE_MS         200     // Debounce push button (ms)

// Wiegand timing (dari template.ino — lebih robust)
#define WG_TIMEOUT_US   30000   // Timeout antar bit terakhir (µs)
#define WG_MAX_BITS     64      // Maksimal bit yang diterima
#define MIN_PULSE_US    150     // Lebar minimum pulsa valid (µs)
#define MAX_PULSE_US    1500    // Lebar maksimum pulsa valid (µs)
#define IGNORE_MS       800     // Abaikan input setelah relay berpindah

// ============================================
//  NTP
// ============================================

#define NTP_SERVER      "pool.ntp.org"
#define NTP_GMT_OFFSET  25200   // GMT+7 (WIB) = 7 * 3600
#define NTP_DAYLIGHT    0

// ============================================
//  KAPASITAS
// ============================================

#define MAX_CARDS       200     // Maks kartu di whitelist lokal
#define MAX_SCHEDULES   20      // Maks schedule rules
#define UID_MAX_LENGTH  12      // Panjang maks string UID

// ============================================
//  SERIAL
// ============================================

#define SERIAL_BAUD     115200

// ============================================
//  MODE TESTING
// ============================================

// true  = kartu valid apa saja membuka (tanpa cek server/whitelist)
// false = validasi via server / whitelist lokal
#define ALLOW_ANY_CARD   false

// true  = harus parity OK sebelum diproses
// false = abaikan parity
#define REQUIRE_PARITY   true

// true  = cetak heartbeat Serial tiap 2 detik (debug)
#define DEBUG_HEARTBEAT  false

#endif // CONFIG_H
