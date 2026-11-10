#include "relay_control.h"
#include "config.h"

static bool _isUnlocked = false;

void relayInit() {
  pinMode(RELAY_PIN, OUTPUT);
  relayOff();  // Default: terkunci
  Serial.println("[Relay] ✅ Inisialisasi selesai, dropbolt TERKUNCI");
}

void relayOn() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? LOW : HIGH);
  _isUnlocked = true;
}

void relayOff() {
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  _isUnlocked = false;
}

void relayUnlockTimed(unsigned long durationMs) {
  Serial.print("[Relay] 🔓 Unlock dropbolt (");
  Serial.print(durationMs / 1000.0, 1);
  Serial.println(" detik)...");

  relayOn();
  delay(durationMs);
  relayOff();

  Serial.println("[Relay] 🔒 Dropbolt terkunci kembali");
}

String relayGetState() {
  return _isUnlocked ? "unlocked" : "locked";
}

bool relayIsUnlocked() {
  return _isUnlocked;
}

void doorSensorInit() {
  if (DOOR_SENSOR_PIN >= 0) {
    pinMode(DOOR_SENSOR_PIN, INPUT_PULLUP);
    Serial.println("[Relay] ✅ Sensor pintu aktif di GPIO " + String(DOOR_SENSOR_PIN));
  }
}

String doorGetPhysicalState() {
  if (DOOR_SENSOR_PIN < 0) return "unknown";
  
  // Asumsi: Sensor magnetik (NO ke GND)
  // Magnet dekat (pintu tertutup) -> switch menutup -> tersambung ke GND -> LOW
  // Magnet jauh (pintu terbuka) -> switch membuka -> PULLUP internal -> HIGH
  if (digitalRead(DOOR_SENSOR_PIN) == LOW) {
    return "locked";
  } else {
    return "unlocked";
  }
}

