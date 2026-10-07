#ifndef ACCESS_CONTROL_H
#define ACCESS_CONTROL_H

#include <Arduino.h>
#include "config.h"

// ============================================
//  Data Structures
// ============================================

/// Data kartu di whitelist
struct CardEntry {
  char uid[UID_MAX_LENGTH + 1];   // UID hex, misal "A3 B2 C1 D0"
  char accessLevel[10];           // "admin", "level_1", "level_2", "level_3"
  bool isActive;
};

/// Rule jadwal akses
struct ScheduleRule {
  char accessLevel[10];  // "level_1", "level_2", "level_3"
  bool days[7];          // 0=Minggu, 1=Senin, ..., 6=Sabtu
  int startHour;
  int startMinute;
  int endHour;
  int endMinute;
  bool isActive;
};

/// Hasil validasi akses
enum AccessResult {
  ACCESS_GRANTED,
  ACCESS_DENIED_UNREGISTERED,   // Kartu tidak terdaftar
  ACCESS_DENIED_BLOCKED,        // Kartu diblokir
  ACCESS_DENIED_OUT_OF_SCHEDULE // Di luar jadwal
};

// ============================================
//  Functions
// ============================================

/// Inisialisasi access control (kosongkan whitelist & schedule)
void accessControlInit();

/// Muat whitelist dari JSON string (dari server)
/// @param jsonStr - JSON string format: {"cards":[...], "schedules":[...]}
/// @return jumlah kartu yang berhasil dimuat
int accessControlLoadFromJson(const String &jsonStr);

/// Validasi akses kartu secara lokal (offline)
/// @param uid - UID kartu yang di-tap
/// @param resultOut - hasil validasi
/// @param accessLevelOut - access level kartu (jika terdaftar)
/// @return true jika ACCESS_GRANTED
bool accessControlValidate(const String &uid, AccessResult &resultOut, String &accessLevelOut);

/// Dapatkan jumlah kartu di whitelist
int accessControlGetCardCount();

/// Dapatkan jumlah schedule rules
int accessControlGetScheduleCount();

/// Konversi AccessResult ke string yang bisa dibaca
String accessResultToString(AccessResult result);

/// Konversi AccessResult ke denied_reason string untuk API
String accessResultToReason(AccessResult result);

#endif // ACCESS_CONTROL_H
