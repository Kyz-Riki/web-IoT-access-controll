#include "wifi_manager.h"
#include "config.h"
#include <WiFi.h>
#include <time.h>

// ============================================
//  State
// ============================================

static bool          _ntpSynced          = false;
static unsigned long _lastReconnectCheck = 0;
static unsigned long _lastNtpAttempt     = 0;

// ============================================
//  Public Functions
// ============================================

void wifiInit() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);  // Driver ESP32 lwIP auto-reconnect di background
  Serial.println("[WiFi] ✅ WiFi Station mode siap (Auto-Reconnect aktif)");
}

bool wifiConnect() {
  if (strlen(WIFI_SSID) == 0) {
    Serial.println("[WiFi] ⚠️ SSID kosong, tidak bisa connect");
    return false;
  }

  Serial.printf("[WiFi] Menghubungkan ke '%s'... ", WIFI_SSID);

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startMs > WIFI_TIMEOUT_MS) {
      Serial.println("❌ Timeout");
      return false;
    }
    delay(200);
    Serial.print(".");
  }

  Serial.println();
  Serial.printf("[WiFi] ✅ Terhubung! IP: %s (RSSI: %d dBm)\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  // Inisiasi background NTP sync
  ntpSync();

  return true;
}

bool wifiIsConnected() {
  return (WiFi.status() == WL_CONNECTED);
}

void wifiReconnectIfNeeded() {
  // Evaluasi tiap 5 detik secara non-blocking
  if (millis() - _lastReconnectCheck < 5000) return;
  _lastReconnectCheck = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] ⚠️ Terputus, mencoba reconnect (non-blocking)...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  } else if (!_ntpSynced && (millis() - _lastNtpAttempt > 60000)) {
    // Coba sync NTP kembali jika belum berhasil
    ntpSync();
  }
}

String wifiGetIP() {
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  return "0.0.0.0";
}

int wifiGetRSSI() {
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.RSSI();
  }
  return 0;
}

void ntpSync() {
  _lastNtpAttempt = millis();
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("[NTP] Sinkronisasi waktu via SNTP background...");
  configTime(NTP_GMT_OFFSET, NTP_DAYLIGHT, NTP_SERVER);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 500)) { // timeout 500ms
    char buf[25];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.printf("[NTP] ✅ Waktu berhasil disinkronkan: %s\n", buf);
    _ntpSynced = true;
  } else {
    Serial.println("[NTP] ℹ️ Waktu belum tersinkron, akan dicoba kembali di background.");
    _ntpSynced = false;
  }
}

bool ntpIsSynced() {
  return _ntpSynced;
}

const char* wifiGetMacAddress() {
  static char macStr[18] = {0};
  if (macStr[0] == '\0') {
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
  return macStr;
}

const char* wifiGetDeviceId() {
  static char deviceId[16] = {0};
  if (deviceId[0] == '\0') {
#if defined(ESP_ID_CUSTOM) && (sizeof(ESP_ID_CUSTOM) > 1)
    if (strlen(ESP_ID_CUSTOM) > 0) {
      strncpy(deviceId, ESP_ID_CUSTOM, sizeof(deviceId) - 1);
      deviceId[sizeof(deviceId) - 1] = '\0';
      return deviceId;
    }
#endif
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    // Format unik berbasis MAC: esp-XXXXXX (contoh: esp-9abcd1)
    snprintf(deviceId, sizeof(deviceId), "esp-%02x%02x%02x", mac[3], mac[4], mac[5]);
  }
  return deviceId;
}
