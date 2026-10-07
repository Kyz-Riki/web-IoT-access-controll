#ifndef RELAY_CONTROL_H
#define RELAY_CONTROL_H

#include <Arduino.h>

/// Inisialisasi pin relay dan set ke posisi locked (default)
void relayInit();

/// Aktifkan relay → dropbolt TERBUKA
void relayOn();

/// Matikan relay → dropbolt TERKUNCI
void relayOff();

/// Unlock dropbolt selama durasi tertentu, lalu lock kembali (blocking)
/// @param durationMs - durasi unlock dalam milidetik
void relayUnlockTimed(unsigned long durationMs);

/// Dapatkan status relay saat ini
/// @return "locked" atau "unlocked"
String relayGetState();

/// Cek apakah relay sedang dalam posisi unlock
bool relayIsUnlocked();

/// Inisialisasi sensor pintu magnet
void doorSensorInit();

/// Dapatkan status fisik pintu dari sensor magnet
/// @return "closed" atau "open"
String doorGetPhysicalState();

#endif // RELAY_CONTROL_H
