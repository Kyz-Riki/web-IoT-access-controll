#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>

/// Inisialisasi WiFi dan NTP time sync
void wifiInit();

/// Coba connect ke WiFi (blocking sampai timeout)
/// @return true jika berhasil connect
bool wifiConnect();

/// Cek apakah WiFi masih terhubung
bool wifiIsConnected();

/// Reconnect WiFi jika terputus (non-blocking, pakai interval)
void wifiReconnectIfNeeded();

/// Dapatkan IP address saat ini sebagai string
String wifiGetIP();

/// Sinkronisasi waktu via NTP
void ntpSync();

/// Cek apakah waktu sudah di-sync via NTP
bool ntpIsSynced();

#endif // WIFI_MANAGER_H
