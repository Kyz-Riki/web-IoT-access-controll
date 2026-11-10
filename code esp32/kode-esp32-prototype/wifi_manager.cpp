#include "wifi_manager.h"
#include "config.h"
#include <WiFi.h>
#include <time.h>

static unsigned long _lastReconnectAttempt = 0;
static bool _ntpSynced = false;

void wifiInit() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
}

bool wifiConnect() {
  Serial.print("[WiFi] Menghubungkan ke ");
  Serial.print(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startMs = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startMs > WIFI_TIMEOUT_MS) {
      Serial.println("\n[WiFi] ❌ Gagal connect (timeout)");
      return false;
    }
    Serial.print(".");
    delay(500);
  }

  Serial.println();
  Serial.print("[WiFi] ✅ Terhubung! IP: ");
  Serial.println(WiFi.localIP());

  // Sync waktu via NTP setelah WiFi terhubung
  ntpSync();

  return true;
}

bool wifiIsConnected() {
  return WiFi.status() == WL_CONNECTED;
}

void wifiReconnectIfNeeded() {
  if (wifiIsConnected()) return;

  unsigned long now = millis();
  if (now - _lastReconnectAttempt < WIFI_RETRY_INTERVAL_MS) return;

  _lastReconnectAttempt = now;
  Serial.println("[WiFi] ⚠️ Terputus, mencoba reconnect...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

String wifiGetIP() {
  return WiFi.localIP().toString();
}

void ntpSync() {
  Serial.println("[NTP] Sinkronisasi waktu...");
  configTime(NTP_GMT_OFFSET, NTP_DAYLIGHT, NTP_SERVER);

  // Tunggu sampai waktu valid (maks 5 detik)
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 10) {
    delay(500);
    retry++;
  }

  if (getLocalTime(&timeinfo)) {
    Serial.print("[NTP] ✅ Waktu: ");
    Serial.println(&timeinfo, "%Y-%m-%d %H:%M:%S");
    _ntpSynced = true;
  } else {
    Serial.println("[NTP] ⚠️ Gagal sync waktu");
    _ntpSynced = false;
  }
}

bool ntpIsSynced() {
  return _ntpSynced;
}
