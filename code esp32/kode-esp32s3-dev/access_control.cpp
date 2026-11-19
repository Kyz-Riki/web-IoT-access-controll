#include "access_control.h"
#include <ArduinoJson.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <Preferences.h>

// ============================================
//  Data Storage (lokal di memory ESP32 & NVS)
// ============================================

static CardEntry        _cards[MAX_CARDS];
static int              _cardCount = 0;

static ScheduleRule     _schedules[MAX_SCHEDULES];
static int              _scheduleCount = 0;

static Preferences       _prefs;

// Mutex untuk thread-safety antara Core 0 (Network) dan Core 1 (Hardware)
static SemaphoreHandle_t _accessMutex = NULL;

// ============================================
//  Private Helpers
// ============================================

/// Simpan ke NVS internal (pemanggil wajib memegang _accessMutex)
static void _saveToNVSInternal() {
  _prefs.begin("access_ctrl", false);
  _prefs.putInt("card_count", _cardCount);
  if (_cardCount > 0) {
    _prefs.putBytes("cards", _cards, sizeof(CardEntry) * _cardCount);
  } else {
    _prefs.remove("cards");
  }

  _prefs.putInt("sched_count", _scheduleCount);
  if (_scheduleCount > 0) {
    _prefs.putBytes("schedules", _schedules, sizeof(ScheduleRule) * _scheduleCount);
  } else {
    _prefs.remove("schedules");
  }
  _prefs.end();

  Serial.printf("[Access] 💾 Whitelist disimpan ke NVS Flash (%d kartu, %d jadwal)\n",
                _cardCount, _scheduleCount);
}

/// Cari kartu berdasarkan UID (zero-allocation, case-insensitive)
static int _findCard(const char* uid) {
  if (!uid) return -1;
  for (int i = 0; i < _cardCount; i++) {
    if (strcasecmp(_cards[i].uid, uid) == 0) {
      return i;
    }
  }
  return -1;
}

/// Cek apakah waktu sekarang ada di dalam jadwal untuk access level tertentu
static bool _isInSchedule(const char* accessLevel) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    // Waktu belum sync → fail-open (izinkan akses)
    Serial.println("[Access] ⚠️ Waktu belum sync, izinkan akses (fail-open)");
    return true;
  }

  int currentDay = timeinfo.tm_wday;       // 0=Minggu, 1=Senin, ...
  int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;

  bool foundRule = false;
  for (int i = 0; i < _scheduleCount; i++) {
    if (!_schedules[i].isActive) continue;
    if (strcasecmp(_schedules[i].accessLevel, accessLevel) != 0) continue;

    foundRule = true;

    if (!_schedules[i].days[currentDay]) continue;

    int startMin = _schedules[i].startHour * 60 + _schedules[i].startMinute;
    int endMin   = _schedules[i].endHour * 60 + _schedules[i].endMinute;

    if (currentMinutes >= startMin && currentMinutes <= endMin) {
      return true;
    }
  }

  // Tidak ada rule → default izinkan
  if (!foundRule) return true;

  return false;
}

/// Parse string hari ke array boolean (tanpa alokasi String)
static void _parseDays(const char* daysStr, bool daysOut[7]) {
  memset(daysOut, false, sizeof(bool) * 7);
  if (!daysStr) return;

  char temp[32];
  strncpy(temp, daysStr, sizeof(temp) - 1);
  temp[sizeof(temp) - 1] = '\0';

  // to lowercase
  for (int i = 0; temp[i]; i++) {
    temp[i] = tolower((unsigned char)temp[i]);
  }

  if (strstr(temp, "sun")) daysOut[0] = true;
  if (strstr(temp, "mon")) daysOut[1] = true;
  if (strstr(temp, "tue")) daysOut[2] = true;
  if (strstr(temp, "wed")) daysOut[3] = true;
  if (strstr(temp, "thu")) daysOut[4] = true;
  if (strstr(temp, "fri")) daysOut[5] = true;
  if (strstr(temp, "sat")) daysOut[6] = true;
}

/// Parse time string "HH:MM"
static void _parseTime(const char* timeStr, int &hourOut, int &minuteOut) {
  hourOut = 0;
  minuteOut = 0;
  if (!timeStr) return;
  int h = 0, m = 0;
  if (sscanf(timeStr, "%d:%d", &h, &m) >= 1) {
    hourOut = h;
    minuteOut = m;
  }
}

static int _loadFromDoc(DynamicJsonDocument &doc) {
  if (_accessMutex) xSemaphoreTake(_accessMutex, portMAX_DELAY);

  // Parse cards ke buffer sementara
  CardEntry newCards[MAX_CARDS];
  int newCardCount = 0;
  JsonArray cards = doc["cards"].as<JsonArray>();
  for (JsonObject card : cards) {
    if (newCardCount >= MAX_CARDS) break;

    const char* uid = card["uid"] | "";
    const char* lvl = card["access_level"] | "level_1";

    strncpy(newCards[newCardCount].uid, uid, UID_MAX_LENGTH);
    newCards[newCardCount].uid[UID_MAX_LENGTH] = '\0';

    strncpy(newCards[newCardCount].accessLevel, lvl, sizeof(newCards[newCardCount].accessLevel) - 1);
    newCards[newCardCount].accessLevel[sizeof(newCards[newCardCount].accessLevel) - 1] = '\0';

    newCards[newCardCount].isActive = card["is_active"] | true;
    newCardCount++;
  }

  // Parse schedules ke buffer sementara
  ScheduleRule newSchedules[MAX_SCHEDULES];
  int newScheduleCount = 0;
  JsonArray schedules = doc["schedules"].as<JsonArray>();
  for (JsonObject sched : schedules) {
    if (newScheduleCount >= MAX_SCHEDULES) break;

    const char* lvl = sched["access_level"] | "";
    strncpy(newSchedules[newScheduleCount].accessLevel, lvl, sizeof(newSchedules[newScheduleCount].accessLevel) - 1);
    newSchedules[newScheduleCount].accessLevel[sizeof(newSchedules[newScheduleCount].accessLevel) - 1] = '\0';

    _parseDays(sched["days"] | "", newSchedules[newScheduleCount].days);
    _parseTime(sched["time_start"] | "00:00",
               newSchedules[newScheduleCount].startHour,
               newSchedules[newScheduleCount].startMinute);
    _parseTime(sched["time_end"] | "23:59",
               newSchedules[newScheduleCount].endHour,
               newSchedules[newScheduleCount].endMinute);

    newSchedules[newScheduleCount].isActive = sched["is_active"] | true;
    newScheduleCount++;
  }

  // Cek apakah data baru berbeda dengan data yang ada di RAM
  bool isChanged = false;
  if (newCardCount != _cardCount || newScheduleCount != _scheduleCount) {
    isChanged = true;
  } else {
    if (newCardCount > 0 && memcmp(newCards, _cards, sizeof(CardEntry) * newCardCount) != 0) {
      isChanged = true;
    }
    if (newScheduleCount > 0 && memcmp(newSchedules, _schedules, sizeof(ScheduleRule) * newScheduleCount) != 0) {
      isChanged = true;
    }
  }

  // Update RAM
  _cardCount = newCardCount;
  if (_cardCount > 0) {
    memcpy(_cards, newCards, sizeof(CardEntry) * _cardCount);
  }
  _scheduleCount = newScheduleCount;
  if (_scheduleCount > 0) {
    memcpy(_schedules, newSchedules, sizeof(ScheduleRule) * _scheduleCount);
  }

  // HANYA tulis ke NVS Flash jika data benar-benar berubah! (Mencegah keausan Flash)
  if (isChanged) {
    _saveToNVSInternal();
    Serial.printf("[Access] 💾 Whitelist diperbarui dan disimpan ke NVS: %d kartu, %d jadwal\n",
                  _cardCount, _scheduleCount);
  } else {
    Serial.printf("[Access] ℹ️ Whitelist RAM terverifikasi (identik, skip NVS flash write): %d kartu, %d jadwal\n",
                  _cardCount, _scheduleCount);
  }

  int loadedCount = _cardCount;
  if (_accessMutex) xSemaphoreGive(_accessMutex);

  return loadedCount;
}

// ============================================
//  Public Functions
// ============================================

void accessControlInit() {
  if (!_accessMutex) {
    _accessMutex = xSemaphoreCreateMutex();
  }

  if (_accessMutex) xSemaphoreTake(_accessMutex, portMAX_DELAY);
  _cardCount = 0;
  _scheduleCount = 0;
  memset(_cards, 0, sizeof(_cards));
  memset(_schedules, 0, sizeof(_schedules));
  if (_accessMutex) xSemaphoreGive(_accessMutex);

  // Otomatis muat data tersimpan dari NVS Flash
  accessControlLoadFromNVS();

  Serial.println("[Access] ✅ Access control diinisialisasi (Mutex aktif & NVS siap)");
}

void accessControlSaveToNVS() {
  if (_accessMutex) xSemaphoreTake(_accessMutex, portMAX_DELAY);
  _saveToNVSInternal();
  if (_accessMutex) xSemaphoreGive(_accessMutex);
}

int accessControlLoadFromNVS() {
  if (_accessMutex) xSemaphoreTake(_accessMutex, portMAX_DELAY);

  _prefs.begin("access_ctrl", true); // Read-only
  int cCount = _prefs.getInt("card_count", 0);
  int sCount = _prefs.getInt("sched_count", 0);

  if (cCount > 0 && cCount <= MAX_CARDS) {
    size_t expectedLen = sizeof(CardEntry) * cCount;
    size_t readLen = _prefs.getBytes("cards", _cards, expectedLen);
    if (readLen == expectedLen) {
      _cardCount = cCount;
    } else {
      Serial.printf("[Access] ⚠️ Ukuran data kartu di NVS tidak cocok (expected %u, got %u)\n", expectedLen, readLen);
      _cardCount = 0;
    }
  } else {
    _cardCount = 0;
  }

  if (sCount > 0 && sCount <= MAX_SCHEDULES) {
    size_t expectedLen = sizeof(ScheduleRule) * sCount;
    size_t readLen = _prefs.getBytes("schedules", _schedules, expectedLen);
    if (readLen == expectedLen) {
      _scheduleCount = sCount;
    } else {
      _scheduleCount = 0;
    }
  } else {
    _scheduleCount = 0;
  }

  _prefs.end();

  int loaded = _cardCount;
  if (_accessMutex) xSemaphoreGive(_accessMutex);

  if (loaded > 0) {
    Serial.printf("[Access] 💾 Whitelist berhasil dimuat dari NVS Flash: %d kartu, %d jadwal\n",
                  loaded, _scheduleCount);
  } else {
    Serial.println("[Access] ℹ️ Belum ada whitelist tersimpan di NVS Flash");
  }

  return loaded;
}

int accessControlLoadFromStream(Stream &stream) {
  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, stream);
  if (error) {
    Serial.printf("[Access] ❌ Gagal parse JSON Stream: %s\n", error.c_str());
    return -1;
  }
  return _loadFromDoc(doc);
}

int accessControlLoadFromJson(const String &jsonStr) {
  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, jsonStr);
  if (error) {
    Serial.printf("[Access] ❌ Gagal parse JSON: %s\n", error.c_str());
    return -1;
  }
  return _loadFromDoc(doc);
}

bool accessControlValidate(const char* uid, AccessResult &resultOut, char* accessLevelOut, size_t maxLen) {
  if (accessLevelOut && maxLen > 0) {
    accessLevelOut[0] = '\0';
  }

  if (_accessMutex) xSemaphoreTake(_accessMutex, portMAX_DELAY);

  // 1. Cari kartu di whitelist
  int idx = _findCard(uid);
  if (idx < 0) {
    if (_accessMutex) xSemaphoreGive(_accessMutex);
    resultOut = ACCESS_DENIED_UNREGISTERED;
    return false;
  }

  if (accessLevelOut && maxLen > 0) {
    strncpy(accessLevelOut, _cards[idx].accessLevel, maxLen - 1);
    accessLevelOut[maxLen - 1] = '\0';
  }

  // 2. Cek apakah kartu aktif
  if (!_cards[idx].isActive) {
    if (_accessMutex) xSemaphoreGive(_accessMutex);
    resultOut = ACCESS_DENIED_BLOCKED;
    return false;
  }

  // 3. Admin selalu granted
  if (strcasecmp(_cards[idx].accessLevel, "admin") == 0) {
    if (_accessMutex) xSemaphoreGive(_accessMutex);
    resultOut = ACCESS_GRANTED;
    return true;
  }

  // 4. Cek jadwal
  bool inSched = _isInSchedule(_cards[idx].accessLevel);
  if (!inSched) {
    if (_accessMutex) xSemaphoreGive(_accessMutex);
    resultOut = ACCESS_DENIED_OUT_OF_SCHEDULE;
    return false;
  }

  if (_accessMutex) xSemaphoreGive(_accessMutex);

  // 5. Granted
  resultOut = ACCESS_GRANTED;
  return true;
}

bool accessControlValidate(const String &uid, AccessResult &resultOut, String &accessLevelOut) {
  char lvlBuf[16] = {0};
  bool ok = accessControlValidate(uid.c_str(), resultOut, lvlBuf, sizeof(lvlBuf));
  accessLevelOut = String(lvlBuf);
  return ok;
}

int accessControlGetCardCount() {
  if (_accessMutex) xSemaphoreTake(_accessMutex, portMAX_DELAY);
  int count = _cardCount;
  if (_accessMutex) xSemaphoreGive(_accessMutex);
  return count;
}

int accessControlGetScheduleCount() {
  if (_accessMutex) xSemaphoreTake(_accessMutex, portMAX_DELAY);
  int count = _scheduleCount;
  if (_accessMutex) xSemaphoreGive(_accessMutex);
  return count;
}

const char* accessResultToString(AccessResult result) {
  switch (result) {
    case ACCESS_GRANTED:                return "✅ GRANTED";
    case ACCESS_DENIED_UNREGISTERED:    return "❌ DENIED (tidak terdaftar)";
    case ACCESS_DENIED_BLOCKED:         return "❌ DENIED (diblokir)";
    case ACCESS_DENIED_OUT_OF_SCHEDULE: return "❌ DENIED (di luar jadwal)";
    default:                            return "❌ DENIED (unknown)";
  }
}

const char* accessResultToReason(AccessResult result) {
  switch (result) {
    case ACCESS_GRANTED:                return "";
    case ACCESS_DENIED_UNREGISTERED:    return "unregistered";
    case ACCESS_DENIED_BLOCKED:         return "blocked";
    case ACCESS_DENIED_OUT_OF_SCHEDULE: return "out_of_schedule";
    default:                            return "unknown";
  }
}
