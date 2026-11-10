#ifndef RFID_HANDLER_H
#define RFID_HANDLER_H

#include <Arduino.h>

/// Inisialisasi modul RFID MFRC522 (SPI)
void rfidInit();

/// Cek apakah ada kartu baru yang terdeteksi dan baca UID-nya
/// @param uidOut - String output berisi UID dalam format hex (misal "A3 B2 C1 D0")
/// @return true jika kartu berhasil terbaca
bool rfidCheckCard(String &uidOut);

/// Hentikan komunikasi dengan kartu saat ini (wajib dipanggil setelah selesai proses)
void rfidHaltCard();

#endif // RFID_HANDLER_H
