#ifndef ACCESS_CONTROL_H
#define ACCESS_CONTROL_H

#include <Arduino.h>
#include <Stream.h>
#include "config.h"

// ============================================
//  Access Control — Whitelist & Jadwal (Offline)
//  Validasi lokal saat server tidak tersedia
// ============================================

/// Data kartu di whitelist
struct CardEntry {
  char uid[UID_MAX_LENGTH + 1];   // UID hex, misal "A3 B2 C1 D0"
  char accessLevel[12];           // "admin", "level_1", "level_2", "level_3"
  bool isActive;
};

/// Rule jadwal akses
struct ScheduleRule {
  char accessLevel[12];   // "level_1", "level_2", "level_3"
  bool days[7];           // 0=Minggu, 1=Senin, ..., 6=Sabtu
  int  startHour;
  int  startMinute;
  int  endHour;
  int  endMinute;
  bool isActive;
};

/// Hasil validasi akses
enum AccessResult {
  ACCESS_GRANTED,
  ACCESS_DENIED_UNREGISTERED,   // Kartu tidak terdaftar
  ACCESS_DENIED_BLOCKED,        // Kartu diblokir
  ACCESS_DENIED_OUT_OF_SCHEDULE // Di luar jadwal
};

/// Inisialisasi access control & mutex (otomatis load dari NVS Flash jika ada)
void accessControlInit();

/// Simpan data whitelist dari RAM ke Flash Memory NVS (Preferences)
void accessControlSaveToNVS();

/// Muat data whitelist dari Flash Memory NVS ke RAM
int accessControlLoadFromNVS();

/// Muat whitelist langsung dari Stream (WiFiClient) tanpa intermediate buffer
int accessControlLoadFromStream(Stream &stream);

/// Muat whitelist dari JSON string (dukungan backward compatibility)
int accessControlLoadFromJson(const String &jsonStr);

/// Validasi akses kartu secara lokal (offline)
/// @param uid - UID kartu yang di-tap
/// @param resultOut - hasil validasi
/// @param accessLevelOut - buffer output level akses (minimal 12 byte)
/// @return true jika ACCESS_GRANTED
bool accessControlValidate(const char* uid, AccessResult &resultOut, char* accessLevelOut, size_t maxLen);
bool accessControlValidate(const String &uid, AccessResult &resultOut, String &accessLevelOut);

/// Dapatkan jumlah kartu di whitelist
int accessControlGetCardCount();

/// Dapatkan jumlah schedule rules
int accessControlGetScheduleCount();

/// Konversi AccessResult ke string human-readable (zero-allocation)
const char* accessResultToString(AccessResult result);

/// Konversi AccessResult ke reason string untuk API (zero-allocation)
const char* accessResultToReason(AccessResult result);

#endif // ACCESS_CONTROL_H
