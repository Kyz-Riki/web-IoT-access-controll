#ifndef API_CLIENT_H
#define API_CLIENT_H

#include <Arduino.h>
#include "access_control.h"

// ============================================
//  Respon dari server untuk tap event
// ============================================

struct TapResponse {
  bool success;         // HTTP request berhasil?
  String action;        // "grant" atau "deny"
  String reason;        // Alasan denied (kosong jika granted)
  String cardHolder;    // Nama pemegang kartu (dari server)
};

// ============================================
//  Functions
// ============================================

/// Ambil whitelist + schedules dari server
/// @param jsonOut - JSON string response dari server
/// @return true jika berhasil
bool apiGetWhitelist(String &jsonOut);

/// Kirim tap event ke server untuk divalidasi
/// @param uid - UID kartu yang di-tap
/// @return TapResponse dengan hasil dari server
TapResponse apiSendTap(const String &uid);

/// Kirim tap event sebagai log saja (validasi sudah dilakukan lokal)
/// @param uid - UID kartu
/// @param status - "granted" atau "denied"
/// @param reason - alasan denied
/// @param accessLevel - access level kartu
/// @return true jika berhasil kirim
bool apiSendTapLog(const String &uid, const String &status,
                   const String &reason, const String &accessLevel);

/// Kirim heartbeat ke server (status ESP32 & pintu)
/// @param doorState - "locked" atau "unlocked"
/// @return true jika berhasil
bool apiSendHeartbeat(const String &doorState);

/// Kirim update status pintu ke server
/// @param state - "locked" atau "unlocked"
/// @return true jika berhasil
bool apiSendDoorStatus(const String &state);

/// Cek mode operasi dari server (normal / register)
/// @return "normal" atau "register"
String apiCheckMode();

/// Kirim UID hasil scan registrasi ke server
/// @param uid - UID kartu yang di-scan
/// @return true jika berhasil
bool apiSendRegisterScan(const String &uid);

#endif // API_CLIENT_H
