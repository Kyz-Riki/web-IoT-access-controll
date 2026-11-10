#ifndef WIEGAND_READER_H
#define WIEGAND_READER_H

#include <Arduino.h>

// ============================================
//  Wiegand Reader — Interrupt-based dengan filter pulsa
//  (Diambil & disederhanakan dari template.ino v10)
// ============================================
//
//  Fitur:
//    - ISR berbasis CHANGE (bukan FALLING saja) → bisa ukur lebar pulsa
//    - Filter noise: buang pulsa terlalu pendek / terlalu panjang
//    - Overlap detection: buang jika D0 dan D1 rendah bersamaan
//    - Support Wiegand26 dan Wiegand34
//    - Parity check (even + odd) untuk Wiegand26
//    - Konversi ke hex string UID format "XX XX XX" atau "XX XX XX XX"

/// Inisialisasi Wiegand reader (setup pins + attach interrupts)
void wiegandInit();

/// Cek apakah ada frame Wiegand baru yang sudah selesai
/// @param uidOut  - output: UID dalam format hex "A3 B2 C1"
/// @param bitsOut - output: jumlah bit frame (26 atau 34)
/// @return true jika ada kartu valid terbaca
bool wiegandCheckCard(String &uidOut, uint8_t &bitsOut);

/// Bersihkan buffer Wiegand (setelah proses kartu / setelah unlock)
void wiegandFlush();

/// Dapatkan jumlah pulsa noise yang dibuang sejak reset terakhir
/// (reset otomatis setelah dibaca)
uint32_t wiegandGetRejectedCount();

#endif // WIEGAND_READER_H
