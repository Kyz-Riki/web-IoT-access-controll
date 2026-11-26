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

static char             _whitelistVersion[16] = {0};

static Preferences       _prefs;

// Mutex untuk thread-safety antara Core 0 (Network) dan Core 1 (Hardware)
static SemaphoreHandle_t _accessMutex = NULL;

// ============================================
//  Private Helpers
// ============================================

/// Hitung checksum FNV-1a 32-bit dari data RAM whitelist (untuk deteksi perubahan tanpa ekstra RAM)
static uint32_t _computeMemoryHash() {
  uint32_t hash = 2166136261u;
  auto mixBytes = [&](const uint8_t* ptr, size_t len) {
    for (size_t i = 0; i < len; i++) {
      hash ^= ptr[i];
      hash *= 16777619u;
    }
  };
  mixBytes((const uint8_t*)&_cardCount, sizeof(_cardCount));
  if (_cardCount > 0) {
    mixBytes((const uint8_t*)_cards, sizeof(CardEntry) * _cardCount);
  }
  mixBytes((const uint8_t*)&_scheduleCount, sizeof(_scheduleCount));
  if (_scheduleCount > 0) {
    mixBytes((const uint8_t*)_schedules, sizeof(ScheduleRule) * _scheduleCount);
  }
  return hash;
}

/// Simpan ke NVS internal (pemanggil wajib memegang _accessMutex)
static void _saveToNVSInternal() {
  _prefs.begin("access_ctrl", false);

  // Hapus blob lama dulu agar partisi NVS 20KB tidak kehabisan ruang saat menimpa hingga 300 kartu
  _prefs.remove("cards");
  _prefs.remove("schedules");

  _prefs.putInt("card_count", _cardCount);
  size_t cardBytes = 0;
  if (_cardCount > 0) {
    size_t want = sizeof(CardEntry) * _cardCount;
    cardBytes = _prefs.putBytes("cards", _cards, want);
    if (cardBytes != want) {
      Serial.printf("[Access] ⚠️ Gagal simpan seluruh kartu ke NVS (%u/%u byte)\n", (unsigned)cardBytes, (unsigned)want);
    }
  }

  _prefs.putInt("sched_count", _scheduleCount);
  size_t schedBytes = 0;
  if (_scheduleCount > 0) {
    size_t want = sizeof(ScheduleRule) * _scheduleCount;
    schedBytes = _prefs.putBytes("schedules", _schedules, want);
    if (schedBytes != want) {
      Serial.printf("[Access] ⚠️ Gagal simpan seluruh jadwal ke NVS (%u/%u byte)\n", (unsigned)schedBytes, (unsigned)want);
    }
  }

  if (_whitelistVersion[0] != '\0') {
    _prefs.putString("wl_ver", _whitelistVersion);
  } else {
    _prefs.remove("wl_ver");
  }

  _prefs.end();

  Serial.printf("[Access] 💾 Whitelist disimpan ke NVS Flash (%d kartu, %d jadwal, %u byte, ver=%s)\n",
                _cardCount, _scheduleCount, (unsigned)(cardBytes + schedBytes),
                _whitelistVersion[0] ? _whitelistVersion : "-");
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

    if (startMin <= endMin) {
      if (currentMinutes >= startMin && currentMinutes <= endMin) {
        return true;
      }
    } else {
      // Rentang melewati tengah malam (contoh: 22:00 - 06:00)
      if (currentMinutes >= startMin || currentMinutes <= endMin) {
        return true;
      }
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
static void _parseTime(const char* timeStr, uint8_t &hourOut, uint8_t &minuteOut) {
  hourOut = 0;
  minuteOut = 0;
  if (!timeStr) return;
  int h = 0, m = 0;
  if (sscanf(timeStr, "%d:%d", &h, &m) >= 1) {
    if (h < 0) h = 0;
    if (h > 23) h = 23;
    if (m < 0) m = 0;
    if (m > 59) m = 59;
    hourOut = (uint8_t)h;
    minuteOut = (uint8_t)m;
  }
}

static int _loadFromDoc(DynamicJsonDocument &doc) {
  if (_accessMutex) xSemaphoreTake(_accessMutex, portMAX_DELAY);

  // Snapshot hash & versi sebelum update agar hanya tulis NVS jika ada perubahan
  uint32_t oldHash = _computeMemoryHash();
  int oldCardCount = _cardCount;
  int oldSchedCount = _scheduleCount;
  char oldVer[16] = {0};
  strncpy(oldVer, _whitelistVersion, sizeof(oldVer) - 1);

  // Zero-init buffer agar padding struct konsisten untuk checksum
  memset(_cards, 0, sizeof(_cards));
  memset(_schedules, 0, sizeof(_schedules));

  // Parse cards
  _cardCount = 0;
  JsonArray cards = doc["cards"].as<JsonArray>();
  for (JsonObject card : cards) {
    if (_cardCount >= MAX_CARDS) {
      Serial.printf("[Access] ⚠️ Batas MAX_CARDS (%d) tercapai!\n", MAX_CARDS);
      break;
    }

    const char* uid = card["uid"] | "";
    const char* lvl = card["access_level"] | "S0";

    strncpy(_cards[_cardCount].uid, uid, UID_MAX_LENGTH);
    _cards[_cardCount].uid[UID_MAX_LENGTH] = '\0';

    strncpy(_cards[_cardCount].accessLevel, lvl, sizeof(_cards[_cardCount].accessLevel) - 1);
    _cards[_cardCount].accessLevel[sizeof(_cards[_cardCount].accessLevel) - 1] = '\0';

    _cards[_cardCount].isActive = card["is_active"] | true;
    _cardCount++;
  }

  // Parse schedules
  _scheduleCount = 0;
  JsonArray schedules = doc["schedules"].as<JsonArray>();
  for (JsonObject sched : schedules) {
    if (_scheduleCount >= MAX_SCHEDULES) break;

    const char* lvl = sched["access_level"] | "";
    strncpy(_schedules[_scheduleCount].accessLevel, lvl, sizeof(_schedules[_scheduleCount].accessLevel) - 1);
    _schedules[_scheduleCount].accessLevel[sizeof(_schedules[_scheduleCount].accessLevel) - 1] = '\0';

    _parseDays(sched["days"] | "", _schedules[_scheduleCount].days);
    _parseTime(sched["time_start"] | "00:00",
               _schedules[_scheduleCount].startHour,
               _schedules[_scheduleCount].startMinute);
    _parseTime(sched["time_end"] | "23:59",
               _schedules[_scheduleCount].endHour,
               _schedules[_scheduleCount].endMinute);

    _schedules[_scheduleCount].isActive = sched["is_active"] | true;
    _scheduleCount++;
  }

  // Simpan versi whitelist dari server (jika tersedia)
  const char* ver = doc["version"] | "";
  strncpy(_whitelistVersion, ver, sizeof(_whitelistVersion) - 1);
  _whitelistVersion[sizeof(_whitelistVersion) - 1] = '\0';

  // Tulis ke NVS Flash HANYA jika data kartu/jadwal/versi benar-benar berubah
  uint32_t newHash = _computeMemoryHash();
  bool changed = (newHash != oldHash) ||
                 (_cardCount != oldCardCount) ||
                 (_scheduleCount != oldSchedCount) ||
                 (strcmp(_whitelistVersion, oldVer) != 0);

  if (changed) {
    _saveToNVSInternal();
  } else {
    Serial.printf("[Access] ℹ️ Whitelist tidak berubah (%d kartu, %d jadwal) — skip tulis NVS Flash\n",
                  _cardCount, _scheduleCount);
  }

  int loadedCount = _cardCount;
  if (_accessMutex) xSemaphoreGive(_accessMutex);

  Serial.printf("[Access] ✅ Whitelist dimuat: %d kartu, %d jadwal (ver=%s)\n",
                loadedCount, _scheduleCount, _whitelistVersion[0] ? _whitelistVersion : "-");

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
  _whitelistVersion[0] = '\0';
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
  bool nvsValid = true;

  if (cCount > 0 && cCount <= MAX_CARDS) {
    size_t expectedLen = sizeof(CardEntry) * cCount;
    size_t readLen = _prefs.getBytes("cards", _cards, expectedLen);
    if (readLen == expectedLen) {
      _cardCount = cCount;
    } else {
      Serial.printf("[Access] ⚠️ Ukuran data kartu di NVS tidak cocok (expected %u, got %u)\n", (unsigned)expectedLen, (unsigned)readLen);
      _cardCount = 0;
      nvsValid = false;
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
      Serial.printf("[Access] ⚠️ Ukuran data jadwal di NVS tidak cocok (expected %u, got %u)\n", (unsigned)expectedLen, (unsigned)readLen);
      _scheduleCount = 0;
      nvsValid = false;
    }
  } else {
    _scheduleCount = 0;
  }

  if (nvsValid) {
    _prefs.getString("wl_ver", _whitelistVersion, sizeof(_whitelistVersion));
  } else {
    _whitelistVersion[0] = '\0';
  }

  _prefs.end();

  int loaded = _cardCount;
  if (_accessMutex) xSemaphoreGive(_accessMutex);

  if (loaded > 0) {
    Serial.printf("[Access] 💾 Whitelist berhasil dimuat dari NVS Flash: %d kartu, %d jadwal (ver=%s)\n",
                  loaded, _scheduleCount, _whitelistVersion[0] ? _whitelistVersion : "-");
  } else {
    Serial.println("[Access] ℹ️ Belum ada whitelist tersimpan di NVS Flash");
  }

  return loaded;
}

int accessControlLoadFromStream(Stream &stream) {
  DynamicJsonDocument doc(49152); // 48 KB — muat hingga 300 kartu + 40 jadwal
  DeserializationError error = deserializeJson(doc, stream);
  if (error) {
    Serial.printf("[Access] ❌ Gagal parse JSON Stream: %s\n", error.c_str());
    return -1;
  }
  return _loadFromDoc(doc);
}

int accessControlLoadFromJson(const String &jsonStr) {
  DynamicJsonDocument doc(49152); // 48 KB — muat hingga 300 kartu + 40 jadwal
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

const char* accessControlGetVersion() {
  return _whitelistVersion;
}

bool accessControlNeedsSync(const char* serverVersion) {
  if (!serverVersion || serverVersion[0] == '\0') {
    // Jika server tidak mengirim versi (server lama), fallback ke perilaku biasa
    return false;
  }
  if (_accessMutex) xSemaphoreTake(_accessMutex, portMAX_DELAY);
  bool diff = (strcmp(_whitelistVersion, serverVersion) != 0);
  if (_accessMutex) xSemaphoreGive(_accessMutex);
  return diff;
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
