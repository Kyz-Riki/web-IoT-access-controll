// ===== Access Control v11 (test): ESP32-S3, Wiegand26 + push button + relay active-LOW =====

#define PIN_D0     4
#define PIN_D1     5
#define PIN_BTN    15   // kaki 1 ke GND, kaki 2 ke pin 15
#define PIN_RELAY  6    // pasang resistor 1k seri ke IN relay

// true  = kartu valid apa saja membuka (mode tes)
// false = hanya kartu di daftar allowedCards
#define ALLOW_ANY_CARD  true

uint32_t allowedCards[] = { 7552233 };
const int ALLOWED_COUNT = sizeof(allowedCards) / sizeof(allowedCards[0]);

#define REQUIRE_PARITY   true
#define UNLOCK_MS        3000
#define IGNORE_MS        800
#define BTN_DEBOUNCE_MS  80
#define WG_TIMEOUT_US    30000
#define WG_MAX_BITS      64
#define MIN_PULSE_US     100     // pulsa asli reader ~400us
#define MAX_PULSE_US     2500
#define HEARTBEAT        false

// Relay active-LOW, push-pull (tidak pernah dilepas)
void relayLock()   { digitalWrite(PIN_RELAY, HIGH); pinMode(PIN_RELAY, OUTPUT); }
void relayUnlock() { digitalWrite(PIN_RELAY, LOW);  pinMode(PIN_RELAY, OUTPUT); }

volatile uint64_t wgData = 0;
volatile uint8_t  wgBits = 0;
volatile uint32_t wgLastUs = 0;
volatile uint32_t fall0 = 0, fall1 = 0;
volatile uint32_t rejWidth = 0, rejOverlap = 0, missedFall = 0, lastBadW = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

unsigned long unlockUntil = 0;
unsigned long ignoreUntil = 0;
bool unlocked = false;

// ---------- ISR dengan filter lebar pulsa + penghitung penyebab ----------
void IRAM_ATTR isrD0() {
  uint32_t now = micros();
  if (digitalRead(PIN_D0) == LOW) {
    fall0 = now;
  } else if (fall0) {
    uint32_t w = now - fall0; fall0 = 0;
    bool overlap = (digitalRead(PIN_D1) == LOW);
    if (overlap) {
      rejOverlap++;
    } else if (w < MIN_PULSE_US || w > MAX_PULSE_US) {
      rejWidth++; lastBadW = w;
    } else {
      portENTER_CRITICAL_ISR(&mux);
      if (wgBits < WG_MAX_BITS) { wgData <<= 1; wgBits++; }
      wgLastUs = now;
      portEXIT_CRITICAL_ISR(&mux);
    }
  } else {
    missedFall++;   // pulsa naik tanpa pernah terlihat turun
  }
}

void IRAM_ATTR isrD1() {
  uint32_t now = micros();
  if (digitalRead(PIN_D1) == LOW) {
    fall1 = now;
  } else if (fall1) {
    uint32_t w = now - fall1; fall1 = 0;
    bool overlap = (digitalRead(PIN_D0) == LOW);
    if (overlap) {
      rejOverlap++;
    } else if (w < MIN_PULSE_US || w > MAX_PULSE_US) {
      rejWidth++; lastBadW = w;
    } else {
      portENTER_CRITICAL_ISR(&mux);
      if (wgBits < WG_MAX_BITS) { wgData = (wgData << 1) | 1; wgBits++; }
      wgLastUs = now;
      portEXIT_CRITICAL_ISR(&mux);
    }
  } else {
    missedFall++;
  }
}

void flushWiegand() {
  portENTER_CRITICAL(&mux);
  wgData = 0; wgBits = 0;
  fall0 = 0; fall1 = 0;
  portEXIT_CRITICAL(&mux);
}

void unlockDoor(const char* reason) {
  relayUnlock();
  unlocked = true;
  unlockUntil = millis() + UNLOCK_MS;
  ignoreUntil = millis() + IGNORE_MS;
  flushWiegand();
  Serial.printf("[UNLOCK] via %s\n", reason);
}

void lockDoor() {
  relayLock();
  unlocked = false;
  ignoreUntil = millis() + IGNORE_MS;
  flushWiegand();
  Serial.println("[LOCK] bolt terkunci lagi");
}

bool isAllowed(uint32_t card) {
  if (ALLOW_ANY_CARD) return true;
  for (int i = 0; i < ALLOWED_COUNT; i++) if (allowedCards[i] == card) return true;
  return false;
}

void parity26(uint32_t d, bool &evenOk, bool &oddOk) {
  uint8_t firstHalf = __builtin_popcount((d >> 13) & 0xFFF);  // bit frame 1..12
  uint8_t lastHalf  = __builtin_popcount((d >> 1)  & 0xFFF);  // bit frame 13..24
  evenOk = ((firstHalf + ((d >> 25) & 1)) % 2) == 0;
  oddOk  = ((lastHalf  + (d & 1)) % 2) == 1;
}

void printBinary(uint64_t d, uint8_t bits) {
  for (int i = bits - 1; i >= 0; i--) Serial.print((int)((d >> i) & 1));
}

void setup() {
  relayLock();                         // pertama: pastikan relay mati

#ifdef RGB_BUILTIN
  neopixelWrite(RGB_BUILTIN, 0, 0, 0); // matikan LED WS2812B onboard
#endif

  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { delay(10); }
  delay(500);
  Serial.printf("Boot, reset reason = %d\n", (int)esp_reset_reason());

  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_D0, INPUT_PULLUP);
  pinMode(PIN_D1, INPUT_PULLUP);

  delay(300);
  flushWiegand();
  ignoreUntil = millis() + 1000;

  attachInterrupt(digitalPinToInterrupt(PIN_D0), isrD0, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_D1), isrD1, CHANGE);

  Serial.printf("v11 siap. Mode: %s\n", ALLOW_ANY_CARD ? "semua kartu valid" : "whitelist");
}

void loop() {
  // ---------- 1) Kartu Wiegand ----------
  uint64_t data = 0; uint8_t bits = 0; bool ready = false;
  portENTER_CRITICAL(&mux);
  if (wgBits > 0 && (micros() - wgLastUs) > WG_TIMEOUT_US) {
    data = wgData; bits = wgBits;
    wgData = 0; wgBits = 0;
    ready = true;
  }
  portEXIT_CRITICAL(&mux);

  if (ready) {
    if (millis() < ignoreUntil) {
      // dibuang: kemungkinan noise setelah relay berpindah / boot
    } else if (bits < 20) {
      // noise pendek, dibuang tanpa log
    } else {
      Serial.printf("Frame: %u bit -> ", bits);
      printBinary(data, bits);
      Serial.println();

      bool valid = false;
      uint32_t card = 0;
      if (bits == 26) {
        bool evenOk, oddOk;
        parity26((uint32_t)data, evenOk, oddOk);
        card = (uint32_t)((data >> 1) & 0xFFFFFF);
        Serial.printf("  W26 kartu: %u | parity genap=%s ganjil=%s\n",
                      card, evenOk ? "OK" : "gagal", oddOk ? "OK" : "gagal");
        valid = REQUIRE_PARITY ? (evenOk && oddOk) : true;
        if (!valid) Serial.println("  [DITOLAK] parity gagal, tap ulang");
      } else if (bits == 34) {
        card = (uint32_t)((data >> 1) & 0xFFFFFFFFULL);
        Serial.printf("  W34 kartu: %u\n", card);
        valid = true;
      } else if (bits >= 24 && bits <= 28) {
        Serial.printf("  [DITOLAK] frame hampir benar (%u bit, ada bit hilang/lebih), tap ulang\n", bits);
      } else {
        Serial.println("  panjang frame tidak cocok, dibuang");
      }

      if (valid) {
        if (isAllowed(card)) {
          if (!unlocked) unlockDoor("kartu");
        } else {
          Serial.println("  [DITOLAK] kartu tidak ada di daftar");
        }
      }
    }
  }

  // ---------- 2) Push button ----------
  static bool lastRead = HIGH, stableBtn = HIGH;
  static unsigned long tChange = 0;

  bool r = digitalRead(PIN_BTN);
  if (r != lastRead) { lastRead = r; tChange = millis(); }

  if (millis() - tChange > BTN_DEBOUNCE_MS && r != stableBtn) {
    stableBtn = r;
    if (stableBtn == LOW && !unlocked && millis() > ignoreUntil) {
      unlockDoor("push button");
    }
  }

  // ---------- 3) Kunci lagi setelah waktu habis ----------
  if (unlocked && millis() > unlockUntil) lockDoor();

  // ---------- Laporan penyebab penolakan (hanya jika ada) ----------
  static unsigned long tRep = 0;
  if (millis() - tRep > 5000) {
    tRep = millis();
    uint32_t w = rejWidth, o = rejOverlap, m = missedFall, lb = lastBadW;
    rejWidth = 0; rejOverlap = 0; missedFall = 0;
    if (w || o || m) {
      Serial.printf("[filter] lebar=%u overlap=%u naik-tanpa-turun=%u (lebar terakhir=%uus)\n", w, o, m, lb);
    }
  }

  if (HEARTBEAT) {
    static unsigned long tHb = 0;
    if (millis() - tHb > 2000) {
      tHb = millis();
      Serial.printf("hidup %lus\n", millis() / 1000);
    }
  }
}