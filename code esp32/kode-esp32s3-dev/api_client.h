#ifndef API_CLIENT_H
#define API_CLIENT_H

#include <Arduino.h>

// ============================================
//  API Client — Komunikasi Efisien (Zero Heap Fragmentation)
//  Menggunakan Static Buffers & Stream Deserialization
// ============================================

/// Respon dari server untuk tap event
struct TapResponse {
  bool success;
  char action[12];      // "grant" atau "deny"
  char reason[32];      // Alasan penolakan
  char cardHolder[48];  // Nama pemegang kartu
};

/// Sinkronisasi whitelist + schedules langsung dari socket stream
/// @return true jika berhasil
bool apiSyncWhitelist();

/// Kirim tap event ke server untuk validasi (online mode)
TapResponse apiSendTap(const char* uid);
TapResponse apiSendTap(const String &uid);

/// Kirim tap event sebagai log saja (offline mode — validasi sudah lokal)
bool apiSendTapLog(const char* uid, const char* status,
                   const char* reason, const char* accessLevel);
bool apiSendTapLog(const String &uid, const String &status,
                   const String &reason, const String &accessLevel);

/// Kirim heartbeat ke server dengan metrik uptime & heap health
/// Jika whitelistVerOut diberikan, versi whitelist terbaru dari server akan diisi ke buffer tsb
bool apiSendHeartbeat(const char* doorState, char* whitelistVerOut = NULL, size_t verMaxLen = 0);
bool apiSendHeartbeat(const String &doorState);

/// Kirim update status pintu ke server
bool apiSendDoorStatus(const char* state);
bool apiSendDoorStatus(const String &state);

/// Cek mode operasi dari server (normal / register)
/// Menyimpan hasil ke buffer modeOut ("normal" atau "register")
bool apiCheckMode(char* modeOut, size_t maxLen);

/// Cek antrean perintah dari server (misal: "unlock")
/// Menyimpan perintah ke buffer cmdOut
bool apiCheckPendingCommand(char* cmdOut, size_t maxLen);

/// Kirim UID hasil scan registrasi ke server
bool apiSendRegisterScan(const char* uid);
bool apiSendRegisterScan(const String &uid);

#endif // API_CLIENT_H
