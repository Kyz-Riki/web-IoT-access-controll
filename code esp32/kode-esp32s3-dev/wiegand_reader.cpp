#include "wiegand_reader.h"
#include "config.h"

// ============================================
//  ISR State (volatile — diakses dari interrupt)
// ============================================

static volatile uint64_t  _wgData    = 0;
static volatile uint8_t   _wgBits    = 0;
static volatile uint32_t  _wgLastUs  = 0;
static volatile uint32_t  _fall0     = 0;   // Timestamp falling edge D0
static volatile uint32_t  _fall1     = 0;   // Timestamp falling edge D1
static volatile uint32_t  _rejected  = 0;   // Counter pulsa noise
static portMUX_TYPE       _mux       = portMUX_INITIALIZER_UNLOCKED;

// ============================================
//  ISR — Interrupt Service Routines
//  (Logic dari template.ino v10: CHANGE mode + pulse width filter)
// ============================================

/// ISR D0 — bit = 0
static void IRAM_ATTR _isrD0() {
  uint32_t now = micros();

  if (digitalRead(PIN_D0) == LOW) {
    // Falling edge → catat timestamp
    _fall0 = now;
  } else if (_fall0) {
    // Rising edge → hitung lebar pulsa
    uint32_t width = now - _fall0;
    _fall0 = 0;

    // Cek apakah D1 sedang LOW (overlap = noise)
    bool overlap = (digitalRead(PIN_D1) == LOW);

    if (width >= MIN_PULSE_US && width <= MAX_PULSE_US && !overlap) {
      portENTER_CRITICAL_ISR(&_mux);
      if (_wgBits < WG_MAX_BITS) {
        _wgData <<= 1;    // Shift left, bit baru = 0 (default)
        _wgBits++;
      }
      _wgLastUs = now;
      portEXIT_CRITICAL_ISR(&_mux);
    } else {
      _rejected++;
    }
  }
}

/// ISR D1 — bit = 1
static void IRAM_ATTR _isrD1() {
  uint32_t now = micros();

  if (digitalRead(PIN_D1) == LOW) {
    _fall1 = now;
  } else if (_fall1) {
    uint32_t width = now - _fall1;
    _fall1 = 0;

    bool overlap = (digitalRead(PIN_D0) == LOW);

    if (width >= MIN_PULSE_US && width <= MAX_PULSE_US && !overlap) {
      portENTER_CRITICAL_ISR(&_mux);
      if (_wgBits < WG_MAX_BITS) {
        _wgData = (_wgData << 1) | 1;   // Shift left, bit baru = 1
        _wgBits++;
      }
      _wgLastUs = now;
      portEXIT_CRITICAL_ISR(&_mux);
    } else {
      _rejected++;
    }
  }
}

// ============================================
//  Parity Check — Wiegand26
// ============================================

/// Cek even parity (bit 0-12) dan odd parity (bit 13-25)
static bool _parity26Check(uint32_t data) {
  // Even parity: bit 25 (MSB parity) + bit 24..13 (12 data bits) → harus genap
  uint8_t firstHalf = __builtin_popcount((data >> 13) & 0xFFF);
  bool evenOk = ((firstHalf + ((data >> 25) & 1)) % 2) == 0;

  // Odd parity: bit 12..1 (12 data bits) + bit 0 (LSB parity) → harus ganjil
  uint8_t lastHalf = __builtin_popcount((data >> 1) & 0xFFF);
  bool oddOk = ((lastHalf + (data & 1)) % 2) == 1;

  return (evenOk && oddOk);
}

// ============================================
//  Public Functions
// ============================================

void wiegandInit() {
  pinMode(PIN_D0, INPUT_PULLUP);
  pinMode(PIN_D1, INPUT_PULLUP);

  delay(300);  // Tunggu reader stabil setelah power-on
  wiegandFlush();

  // Attach interrupt pada CHANGE (bukan FALLING) → bisa ukur lebar pulsa
  attachInterrupt(digitalPinToInterrupt(PIN_D0), _isrD0, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_D1), _isrD1, CHANGE);

  // Setup LED & Buzzer (opsional, active LOW)
  if (RFID_LED_PIN >= 0) {
    pinMode(RFID_LED_PIN, OUTPUT);
    digitalWrite(RFID_LED_PIN, HIGH);   // LED mati
  }
  if (RFID_BUZZER_PIN >= 0) {
    pinMode(RFID_BUZZER_PIN, OUTPUT);
    digitalWrite(RFID_BUZZER_PIN, HIGH); // Buzzer diam
  }

  Serial.println("[Wiegand] ✅ Reader aktif (filter pulsa mode)");
  Serial.printf("[Wiegand]    D0=GPIO%d  D1=GPIO%d\n", PIN_D0, PIN_D1);
  Serial.printf("[Wiegand]    Pulse filter: %d-%d µs\n", MIN_PULSE_US, MAX_PULSE_US);
}

bool wiegandCheckCard(String &uidOut, uint8_t &bitsOut) {
  // 1. Ambil snapshot data dari ISR (critical section)
  uint64_t data = 0;
  uint8_t  bits = 0;
  bool     ready = false;

  portENTER_CRITICAL(&_mux);
  if (_wgBits > 0 && (micros() - _wgLastUs) > WG_TIMEOUT_US) {
    data  = _wgData;
    bits  = _wgBits;
    _wgData = 0;
    _wgBits = 0;
    ready = true;
  }
  portEXIT_CRITICAL(&_mux);

  if (!ready) return false;

  // 2. Filter frame terlalu pendek (noise)
  if (bits < 20) {
    return false;
  }

  bitsOut = bits;

  // 3. Log frame mentah
  Serial.printf("[Wiegand] Frame: %u bit -> ", bits);
  for (int i = bits - 1; i >= 0; i--) {
    Serial.print((int)((data >> i) & 1));
  }
  Serial.println();

  // 4. Parse berdasarkan jumlah bit
  uint32_t cardData = 0;
  int dataBytes = 0;

  if (bits == 26) {
    // Wiegand26: 1 parity + 8 facility + 16 ID + 1 parity
    if (REQUIRE_PARITY && !_parity26Check((uint32_t)data)) {
      Serial.println("[Wiegand] ❌ Parity check gagal, frame dibuang");
      return false;
    }

    // Buang 2 bit parity → 24 bit data
    cardData = (uint32_t)((data >> 1) & 0x00FFFFFF);
    dataBytes = 3;

    bool evenOk, oddOk;
    uint8_t firstHalf = __builtin_popcount(((uint32_t)data >> 13) & 0xFFF);
    evenOk = ((firstHalf + (((uint32_t)data >> 25) & 1)) % 2) == 0;
    uint8_t lastHalf = __builtin_popcount(((uint32_t)data >> 1) & 0xFFF);
    oddOk = ((lastHalf + ((uint32_t)data & 1)) % 2) == 1;

    Serial.printf("[Wiegand]   W26 raw: %u | parity E=%s O=%s\n",
                  cardData, evenOk ? "OK" : "FAIL", oddOk ? "OK" : "FAIL");

  } else if (bits == 34) {
    // Wiegand34: 1 parity + 32 data + 1 parity
    cardData = (uint32_t)((data >> 1) & 0xFFFFFFFFULL);
    dataBytes = 4;
    Serial.printf("[Wiegand]   W34 raw: %u\n", cardData);

  } else {
    Serial.printf("[Wiegand] ⚠️ Frame %u bit — tidak cocok W26/W34, dibuang\n", bits);
    return false;
  }

  // 5. Konversi ke hex string "XX XX XX" atau "XX XX XX XX"
  uidOut = "";
  for (int i = dataBytes - 1; i >= 0; i--) {
    uint8_t b = (cardData >> (i * 8)) & 0xFF;
    if (uidOut.length() > 0) uidOut += " ";
    if (b < 0x10) uidOut += "0";
    uidOut += String(b, HEX);
  }
  uidOut.toUpperCase();

  Serial.printf("[Wiegand] ✅ UID: %s\n", uidOut.c_str());
  return true;
}

void wiegandFlush() {
  portENTER_CRITICAL(&_mux);
  _wgData   = 0;
  _wgBits   = 0;
  _fall0    = 0;
  _fall1    = 0;
  portEXIT_CRITICAL(&_mux);
}

uint32_t wiegandGetRejectedCount() {
  uint32_t count = _rejected;
  _rejected = 0;
  return count;
}
