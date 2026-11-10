#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>

// ============================================
//  WiFi Manager — Sederhana (tanpa Ethernet)
//  Untuk testing cepat tanpa W5500
// ============================================

/// Inisialisasi WiFi Station mode
void wifiInit();

/// Coba koneksi ke WiFi (blocking, timeout di config)
/// @return true jika berhasil terhubung
bool wifiConnect();

/// Cek apakah WiFi terhubung
bool wifiIsConnected();

/// Reconnect jika WiFi terputus
void wifiReconnectIfNeeded();

/// Dapatkan IP address WiFi
String wifiGetIP();

/// Dapatkan RSSI signal strength
int wifiGetRSSI();

/// Sinkronisasi waktu via NTP (WiFi)
void ntpSync();

/// Cek apakah waktu sudah tersinkronisasi
bool ntpIsSynced();

#endif // WIFI_MANAGER_H
