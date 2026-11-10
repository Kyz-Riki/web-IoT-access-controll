#ifndef RELAY_CONTROL_H
#define RELAY_CONTROL_H

#include <Arduino.h>

// ============================================
//  Relay & Dropbolt Control
//  Non-blocking unlock dengan timer
// ============================================

/// Inisialisasi relay pin → default: terkunci
void relayInit();

/// Aktifkan relay → dropbolt TERBUKA
void relayUnlock();

/// Matikan relay → dropbolt TERKUNCI
void relayLock();

/// Mulai unlock terjadwal (non-blocking)
/// Panggil relayUpdate() di loop() untuk otomatis lock kembali
/// @param durationMs - durasi unlock (ms)
void relayUnlockTimed(unsigned long durationMs);

/// Update timer relay — panggil di loop()
/// Akan otomatis lock setelah durasi habis
void relayUpdate();

/// Cek apakah relay sedang unlock
bool relayIsUnlocked();

/// Cek apakah sedang dalam periode ignore noise (setelah perpindahan relay)
bool relayIsIgnoring();

/// Set durasi ignore noise (ms)
void relaySetIgnore(unsigned long ms);

/// Dapatkan status relay: "locked" atau "unlocked"
String relayGetState();

/// Inisialisasi door sensor (opsional)
void doorSensorInit();

/// Baca status fisik pintu dari sensor
/// @return "locked", "unlocked", atau "unknown"
String doorGetPhysicalState();

#endif // RELAY_CONTROL_H
