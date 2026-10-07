#include "access_control.h"
#include <ArduinoJson.h>
#include <time.h>

// ============================================
//  Data Storage (lokal di memory ESP32)
// ============================================

static CardEntry _cards[MAX_CARDS];
static int _cardCount = 0;

static ScheduleRule _schedules[MAX_SCHEDULES];
static int _scheduleCount = 0;

// ============================================
//  Private Helper Functions
// ============================================

/// Cari kartu berdasarkan UID
/// @return index di _cards[], atau -1 jika tidak ditemukan
static int _findCard(const String &uid) {
  for (int i = 0; i < _cardCount; i++) {
    if (uid.equalsIgnoreCase(String(_cards[i].uid))) {
      return i;
    }
  }
  return -1;
}

/// Cek apakah waktu sekarang ada di dalam jadwal untuk access level tertentu
static bool _isInSchedule(const char* accessLevel) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    // Jika waktu belum di-sync, default: izinkan akses (fail-open)
    Serial.println("[Access] ⚠️ Waktu belum sync, izinkan akses (fail-open)");
    return true;
  }

  int currentDay = timeinfo.tm_wday;     // 0=Minggu, 1=Senin, ...
  int currentHour = timeinfo.tm_hour;
  int currentMinute = timeinfo.tm_min;
  int currentTimeMinutes = currentHour * 60 + currentMinute;

  // Cari schedule rules yang cocok dengan access level ini
  bool foundRule = false;
  for (int i = 0; i < _scheduleCount; i++) {
    if (!_schedules[i].isActive) continue;
    if (strcasecmp(_schedules[i].accessLevel, accessLevel) != 0) continue;

    foundRule = true;

    // Cek apakah hari ini ada di rule
    if (!_schedules[i].days[currentDay]) continue;

    // Cek apakah jam saat ini ada di range
    int startMinutes = _schedules[i].startHour * 60 + _schedules[i].startMinute;
    int endMinutes = _schedules[i].endHour * 60 + _schedules[i].endMinute;

    if (currentTimeMinutes >= startMinutes && currentTimeMinutes <= endMinutes) {
      return true;  // Ada di dalam jadwal
    }
  }

  // Jika tidak ada rule yang didefinisikan untuk level ini, default: izinkan
  if (!foundRule) {
    return true;
  }

  return false;  // Ada rule tapi tidak cocok → di luar jadwal
}

/// Parse string hari ke array boolean (misal "mon,tue,wed" → days[1]=true, days[2]=true, days[3]=true)
static void _parseDays(const char* daysStr, bool daysOut[7]) {
  memset(daysOut, false, sizeof(bool) * 7);

  String s = String(daysStr);
  s.toLowerCase();

  if (s.indexOf("sun") >= 0) daysOut[0] = true;
  if (s.indexOf("mon") >= 0) daysOut[1] = true;
  if (s.indexOf("tue") >= 0) daysOut[2] = true;
  if (s.indexOf("wed") >= 0) daysOut[3] = true;
  if (s.indexOf("thu") >= 0) daysOut[4] = true;
  if (s.indexOf("fri") >= 0) daysOut[5] = true;
  if (s.indexOf("sat") >= 0) daysOut[6] = true;
}

/// Parse time string "HH:MM" ke hour dan minute
static void _parseTime(const char* timeStr, int &hourOut, int &minuteOut) {
  hourOut = 0;
  minuteOut = 0;

  String s = String(timeStr);
  int colonIdx = s.indexOf(':');
  if (colonIdx > 0) {
    hourOut = s.substring(0, colonIdx).toInt();
    minuteOut = s.substring(colonIdx + 1).toInt();
  }
}

// ============================================
//  Public Functions
// ============================================

void accessControlInit() {
  _cardCount = 0;
  _scheduleCount = 0;
  memset(_cards, 0, sizeof(_cards));
  memset(_schedules, 0, sizeof(_schedules));
  Serial.println("[Access] ✅ Access control diinisialisasi");
}

int accessControlLoadFromJson(const String &jsonStr) {
  // Alokasi JSON document (sesuaikan size dengan kebutuhan)
  DynamicJsonDocument doc(8192);

  DeserializationError error = deserializeJson(doc, jsonStr);
  if (error) {
    Serial.print("[Access] ❌ Gagal parse JSON: ");
    Serial.println(error.c_str());
    return -1;
  }

  // Parse cards
  _cardCount = 0;
  JsonArray cards = doc["cards"].as<JsonArray>();
  for (JsonObject card : cards) {
    if (_cardCount >= MAX_CARDS) break;

    strncpy(_cards[_cardCount].uid, card["uid"] | "", UID_MAX_LENGTH);
    _cards[_cardCount].uid[UID_MAX_LENGTH] = '\0';
    strncpy(_cards[_cardCount].accessLevel, card["access_level"] | "level_1", 9);
    _cards[_cardCount].accessLevel[9] = '\0';
    _cards[_cardCount].isActive = card["is_active"] | true;

    _cardCount++;
  }

  // Parse schedules
  _scheduleCount = 0;
  JsonArray schedules = doc["schedules"].as<JsonArray>();
  for (JsonObject sched : schedules) {
    if (_scheduleCount >= MAX_SCHEDULES) break;

    strncpy(_schedules[_scheduleCount].accessLevel, sched["access_level"] | "", 9);
    _schedules[_scheduleCount].accessLevel[9] = '\0';

    _parseDays(sched["days"] | "", _schedules[_scheduleCount].days);
    _parseTime(sched["time_start"] | "00:00", _schedules[_scheduleCount].startHour, _schedules[_scheduleCount].startMinute);
    _parseTime(sched["time_end"] | "23:59", _schedules[_scheduleCount].endHour, _schedules[_scheduleCount].endMinute);

    _schedules[_scheduleCount].isActive = sched["is_active"] | true;
    _scheduleCount++;
  }

  Serial.print("[Access] ✅ Whitelist dimuat: ");
  Serial.print(_cardCount);
  Serial.print(" kartu, ");
  Serial.print(_scheduleCount);
  Serial.println(" jadwal");

  return _cardCount;
}

bool accessControlValidate(const String &uid, AccessResult &resultOut, String &accessLevelOut) {
  accessLevelOut = "";

  // 1. Cari kartu di whitelist
  int idx = _findCard(uid);
  if (idx < 0) {
    resultOut = ACCESS_DENIED_UNREGISTERED;
    return false;
  }

  accessLevelOut = String(_cards[idx].accessLevel);

  // 2. Cek apakah kartu aktif
  if (!_cards[idx].isActive) {
    resultOut = ACCESS_DENIED_BLOCKED;
    return false;
  }

  // 3. Cek access level → Admin selalu granted
  if (strcasecmp(_cards[idx].accessLevel, "admin") == 0) {
    resultOut = ACCESS_GRANTED;
    return true;
  }

  // 4. Cek jadwal untuk level 1/2/3
  if (!_isInSchedule(_cards[idx].accessLevel)) {
    resultOut = ACCESS_DENIED_OUT_OF_SCHEDULE;
    return false;
  }

  // 5. Semua lolos → granted
  resultOut = ACCESS_GRANTED;
  return true;
}

int accessControlGetCardCount() {
  return _cardCount;
}

int accessControlGetScheduleCount() {
  return _scheduleCount;
}

String accessResultToString(AccessResult result) {
  switch (result) {
    case ACCESS_GRANTED:                return "✅ GRANTED";
    case ACCESS_DENIED_UNREGISTERED:    return "❌ DENIED (tidak terdaftar)";
    case ACCESS_DENIED_BLOCKED:         return "❌ DENIED (diblokir)";
    case ACCESS_DENIED_OUT_OF_SCHEDULE: return "❌ DENIED (di luar jadwal)";
    default:                            return "❌ DENIED (unknown)";
  }
}

String accessResultToReason(AccessResult result) {
  switch (result) {
    case ACCESS_GRANTED:                return "";
    case ACCESS_DENIED_UNREGISTERED:    return "unregistered";
    case ACCESS_DENIED_BLOCKED:         return "blocked";
    case ACCESS_DENIED_OUT_OF_SCHEDULE: return "out_of_schedule";
    default:                            return "unknown";
  }
}
