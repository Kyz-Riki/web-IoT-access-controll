#include "relay_control.h"
#include "config.h"

// ============================================
//  State
// ============================================

static bool          _isUnlocked  = false;
static unsigned long _unlockUntil = 0;
static unsigned long _ignoreUntil = 0;

// ============================================
//  Public Functions
// ============================================

void relayInit() {
  // Pastikan relay terkunci sebelum set OUTPUT
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(PIN_RELAY, OUTPUT);
  _isUnlocked = false;

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
  _ignoreUntil = millis() + IGNORE_MS;
  Serial.println("[Relay] 🔒 Dropbolt terkunci kembali");
}

void relayUnlockTimed(unsigned long durationMs) {
  Serial.printf("[Relay] 🔓 Unlock dropbolt (%.1f detik)...\n", durationMs / 1000.0);
  relayUnlock();
  _unlockUntil = millis() + durationMs;
  _ignoreUntil = millis() + IGNORE_MS;
}

void relayUpdate() {
  // Auto-lock setelah durasi habis
  if (_isUnlocked && millis() > _unlockUntil && _unlockUntil > 0) {
    relayLock();
    _unlockUntil = 0;
  }
}

bool relayIsUnlocked() {
  return _isUnlocked;
}

bool relayIsIgnoring() {
  return (millis() < _ignoreUntil);
}

void relaySetIgnore(unsigned long ms) {
  _ignoreUntil = millis() + ms;
}

String relayGetState() {
  return _isUnlocked ? "unlocked" : "locked";
}

void doorSensorInit() {
  if (DOOR_SENSOR_PIN >= 0) {
    pinMode(DOOR_SENSOR_PIN, INPUT_PULLUP);
    Serial.printf("[Relay] ✅ Sensor pintu aktif di GPIO%d\n", DOOR_SENSOR_PIN);
  }
}

String doorGetPhysicalState() {
  if (DOOR_SENSOR_PIN < 0) {
    // Tidak ada sensor → gunakan status relay
    return relayGetState();
  }
  // Sensor dropbolt: LOW = tertutup, HIGH = terbuka
  return (digitalRead(DOOR_SENSOR_PIN) == LOW) ? "locked" : "unlocked";
}
