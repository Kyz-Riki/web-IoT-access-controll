#include "relay_control.h"
#include "config.h"

// ============================================
//  State (Safe Rollover Arithmetic)
// ============================================

static bool          _isUnlocked         = false;
static unsigned long _unlockStartMs      = 0;
static unsigned long _unlockDurationMs   = 0;

static bool          _isIgnoring         = false;
static unsigned long _ignoreStartMs      = 0;
static unsigned long _ignoreDurationMs   = 0;

// ============================================
//  Public Functions
// ============================================

void relayInit() {
  // Pastikan relay terkunci sebelum set OUTPUT
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(PIN_RELAY, OUTPUT);
  _isUnlocked = false;
  _unlockDurationMs = 0;
  _isIgnoring = false;
  _ignoreDurationMs = 0;

  Serial.println("[Relay] ✅ Inisialisasi selesai, dropbolt TERKUNCI");
  Serial.printf("[Relay]    Relay di GPIO%d (active %s)\n",
                PIN_RELAY, RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
}

void relayUnlock() {
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? LOW : HIGH);
  _isUnlocked = true;
}

void relayLock() {
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);
  _isUnlocked = false;
  _unlockDurationMs = 0;
  relaySetIgnore(IGNORE_MS);
  Serial.println("[Relay] 🔒 Dropbolt terkunci kembali");
}

void relayUnlockTimed(unsigned long durationMs) {
  Serial.printf("[Relay] 🔓 Unlock dropbolt (%.1f detik)...\n", durationMs / 1000.0);
  relayUnlock();
  _unlockStartMs = millis();
  _unlockDurationMs = durationMs;
  relaySetIgnore(IGNORE_MS);
}

void relayUpdate() {
  // Auto-lock setelah durasi habis (Aman dari millis() rollover)
  if (_isUnlocked && _unlockDurationMs > 0) {
    if (millis() - _unlockStartMs >= _unlockDurationMs) {
      relayLock();
    }
  }

  // Auto-expire ignore noise period
  if (_isIgnoring && _ignoreDurationMs > 0) {
    if (millis() - _ignoreStartMs >= _ignoreDurationMs) {
      _isIgnoring = false;
    }
  }
}

bool relayIsUnlocked() {
  return _isUnlocked;
}

bool relayIsIgnoring() {
  if (!_isIgnoring) return false;
  // Validasi batas waktu ignore secara safe unsigned
  if (millis() - _ignoreStartMs >= _ignoreDurationMs) {
    _isIgnoring = false;
    return false;
  }
  return true;
}

void relaySetIgnore(unsigned long ms) {
  _isIgnoring = (ms > 0);
  _ignoreStartMs = millis();
  _ignoreDurationMs = ms;
}

const char* relayGetState() {
  return _isUnlocked ? "unlocked" : "locked";
}

void doorSensorInit() {
  if (DOOR_SENSOR_PIN >= 0) {
    pinMode(DOOR_SENSOR_PIN, INPUT_PULLUP);
    Serial.printf("[Relay] ✅ Sensor pintu aktif di GPIO%d\n", DOOR_SENSOR_PIN);
  }
}

const char* doorGetPhysicalState() {
  if (DOOR_SENSOR_PIN < 0) {
    // Tidak ada sensor fisik → gunakan status relay
    return relayGetState();
  }
  // Sensor dropbolt: LOW = tertutup, HIGH = terbuka
  return (digitalRead(DOOR_SENSOR_PIN) == LOW) ? "locked" : "unlocked";
}
