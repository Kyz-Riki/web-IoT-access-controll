#ifndef API_CLIENT_H
#define API_CLIENT_H

#include <Arduino.h>

// ============================================
//  API Client — Komunikasi Efisien (Zero Heap Fragmentation)
//  Menggunakan Static Buffers, Mutex Thread-Safe, & Stream Deserialization
// ============================================

/// Respon dari server untuk tap event
struct TapResponse {
  bool success;
  char action[12];      // "grant" atau "deny"
  char reason[32];      // Alasan penolakan
  char cardHolder[48];  // Nama pemegang kartu
};

/// Inisialisasi API client mutex
void apiInit();

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
/// Opsi whitelistVersionOut menerima nomor versi whitelist dari server
bool apiSendHeartbeat(const char* doorState, int *whitelistVersionOut = NULL);
bool apiSendHeartbeat(const String &doorState, int *whitelistVersionOut = NULL);

/// Kirim update status pintu ke server
bool apiSendDoorStatus(const char* state);
bool apiSendDoorStatus(const String &state);

/// Cek mode operasi dari server (normal / register)
/// Menyimpan hasil ke buffer modeOut ("normal" atau "register")
bool apiCheckMode(char* modeOut, size_t maxLen);

/// Kirim UID hasil scan registrasi ke server
bool apiSendRegisterScan(const char* uid);
bool apiSendRegisterScan(const String &uid);

#endif // API_CLIENT_H
