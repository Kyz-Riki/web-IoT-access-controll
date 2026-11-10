#include "wifi_manager.h"
#include "config.h"
#include <WiFi.h>
#include <time.h>

// ============================================
//  State
// ============================================

static bool _ntpSynced = false;
static unsigned long _lastReconnectCheck = 0;

// ============================================
//  Public Functions
// ============================================

void wifiInit() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // Reconnect dikontrol manual
  Serial.println("[WiFi] ✅ WiFi Station mode siap");
}

bool wifiConnect() {
  if (String(WIFI_SSID).length() == 0) {
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
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.printf("[WiFi] ✅ Terhubung! IP: %s (RSSI: %d dBm)\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  // Sync waktu NTP
  ntpSync();

  return true;
}

bool wifiIsConnected() {
  return (WiFi.status() == WL_CONNECTED);
}

void wifiReconnectIfNeeded() {
  // Cek tiap 5 detik saja
  if (millis() - _lastReconnectCheck < 5000) return;
  _lastReconnectCheck = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] ⚠️ Terputus, mencoba reconnect...");
    wifiConnect();
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
  Serial.println("[NTP] Sinkronisasi waktu via WiFi...");

  configTime(NTP_GMT_OFFSET, NTP_DAYLIGHT, NTP_SERVER);

  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 10) {
    delay(500);
    retry++;
  }

  if (getLocalTime(&timeinfo)) {
    char buf[25];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.printf("[NTP] ✅ Waktu: %s\n", buf);
    _ntpSynced = true;
  } else {
    Serial.println("[NTP] ⚠️ Gagal sync waktu");
    _ntpSynced = false;
  }
}

bool ntpIsSynced() {
  return _ntpSynced;
}
