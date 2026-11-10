/*
 * =====================================================================
 *  Access Control System (test-dev)
 *  ESP32-S3 + Wiegand Reader + Relay/Dropbolt + Push Button + WiFi Client
 * =====================================================================
 *
 *  Panduan Hardware & Wiring:
 *    - RFID Reader Wiegand:
 *        D0 (Hijau)  -> R 1k seri -> GPIO4 (dengan R 2k pull-down ke GND = divider 5V->3.3V)
 *        D1 (Kuning) -> R 1k seri -> GPIO5 (dengan R 2k pull-down ke GND = divider 5V->3.3V)
 *        VCC Reader  -> 12V DC (External power supply / splitter)
 *        GND Reader  -> GND bersama (ESP32 GND & 12V GND wajib nyambung!)
 *    - Relay Dropbolt (Active-LOW):
 *        IN1         -> R 1k seri -> GPIO6
 *        VCC         -> 5V
 *        GND         -> GND
 *    - Push Button Exit:
 *        Kaki 1      -> GPIO15 (internal pull-up)
 *        Kaki 2      -> GND
 *
 *  Konfigurasi & Parameter:
 *    Semua setting ada di "config.h" (SSID WiFi, IP Server, Timing, Pin, dll).
 */

#include "config.h"
#include "wifi_manager.h"
#include "wiegand_reader.h"
#include "relay_control.h"
#include "access_control.h"
#include "api_client.h"

// ============================================
//  Timing Trackers
// ============================================

static unsigned long lastHeartbeat     = 0;
static unsigned long lastWhitelistSync = 0;
static unsigned long lastModeCheck     = 0;
static unsigned long lastTapTime       = 0;
static unsigned long lastNoiseReport   = 0;
static unsigned long lastDebugHb       = 0;

// ============================================
//  State Flags
// ============================================

static bool registerMode    = false;   // True jika dashboard sedang minta registrasi kartu
static bool serverAvailable = false;

// ============================================
//  Forward Declarations
// ============================================

void checkPushButton();
void handleManualUnlock();
void checkOperationMode();
void handleRegisterScan(const String &uid);
void handleCardTap(const String &uid);
void handleOnlineTap(const String &uid);
void handleOfflineTap(const String &uid);
void syncWhitelist();
void sendHeartbeat();
void feedbackGranted();
void feedbackDenied();

// ============================================
//  SETUP
// ============================================

void setup() {
  // Matikan LED WS2812B onboard ESP32-S3 agar tidak menyilaukan / bentrok pin
#ifdef RGB_BUILTIN
  neopixelWrite(RGB_BUILTIN, 0, 0, 0);
#endif

  Serial.begin(SERIAL_BAUD);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { delay(10); }
  delay(300);

  Serial.println();
  Serial.println("====================================================");
  Serial.println("   ACCESS CONTROL IoT (test-dev) — ESP32-S3");
  Serial.printf("   Device ID : %s\n", ESP_ID);
  Serial.printf("   Pintu     : %s\n", DOOR_NAME);
  Serial.printf("   Mode Tes  : %s\n", ALLOW_ANY_CARD ? "Semua Kartu Valid" : "Validasi Server/Whitelist");
  Serial.println("====================================================");

  // 1. Inisialisasi Relay (Dropbolt)
  relayInit();
  doorSensorInit();

  // 2. Inisialisasi Push Button
  if (PIN_BTN >= 0) {
    pinMode(PIN_BTN, INPUT_PULLUP);
    Serial.printf("[Setup] ✅ Push Button exit aktif di GPIO%d\n", PIN_BTN);
  }

  // 3. Inisialisasi Wiegand Reader
  wiegandInit();

  // Abaikan input noise selama 1 detik awal setelah booting
  relaySetIgnore(1000);
  wiegandFlush();

  // 4. Inisialisasi Access Control Whitelist Lokal
  accessControlInit();

  // 5. Inisialisasi & Koneksi WiFi
  wifiInit();
  if (wifiConnect()) {
    serverAvailable = true;
    syncWhitelist();
    sendHeartbeat();
  } else {
    Serial.println("[Setup] ⚠️ WiFi belum terhubung. Beroperasi dalam mode OFFLINE.");
  }

  Serial.println();
  Serial.println(">> Sistem siap digunakan. Silakan tap kartu atau tekan tombol <<");
  Serial.println("====================================================");
  Serial.println();
}

// ============================================
//  MAIN LOOP
// ============================================

void loop() {
  // 1. Auto-lock relay jika durasi unlock sudah lewat (non-blocking)
  relayUpdate();

  // 2. Monitoring & reconnect WiFi jika terputus
  wifiReconnectIfNeeded();

  // 3. Polling Push Button manual
  if (PIN_BTN >= 0) {
    checkPushButton();
  }

  // 4. Cek mode operasi dari server (normal / registrasi kartu baru)
  if (millis() - lastModeCheck >= MODE_CHECK_MS) {
    lastModeCheck = millis();
    checkOperationMode();
  }

  // 5. Kirim Heartbeat ke server secara berkala
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = millis();
    sendHeartbeat();
  }

  // 6. Sinkronisasi Whitelist dari server secara berkala
  if (millis() - lastWhitelistSync >= WHITELIST_SYNC_MS) {
    lastWhitelistSync = millis();
    syncWhitelist();
  }

  // 7. Laporan noise filter (jika ada pulsa liar yang dibuang)
  if (millis() - lastNoiseReport >= 5000) {
    lastNoiseReport = millis();
    uint32_t rj = wiegandGetRejectedCount();
    if (rj > 0) {
      Serial.printf("[Wiegand Filter] ℹ️ %u pulsa noise berhasil dibuang\n", rj);
    }
  }

  // 8. Debug Heartbeat Serial
  if (DEBUG_HEARTBEAT && millis() - lastDebugHb >= 2000) {
    lastDebugHb = millis();
    Serial.printf("[Status] Uptime: %lus | WiFi: %s | Relay: %s\n",
                  millis() / 1000,
                  wifiIsConnected() ? "Online" : "Offline",
                  relayGetState().c_str());
  }

  // 9. Jika relay baru saja aktif/non-aktif, abaikan pembacaan noise
  if (relayIsIgnoring()) {
    wiegandFlush();
    return;
  }

  // 10. Pembacaan Kartu RFID Wiegand
  String uid;
  uint8_t bits = 0;
  if (!wiegandCheckCard(uid, bits)) {
    return;
  }

  // Anti double-tap (hindari pemindaian berulang terlalu cepat)
  if (millis() - lastTapTime < ANTI_DOUBLE_TAP_MS) {
    wiegandFlush();
    return;
  }
  lastTapTime = millis();

  Serial.println();
  Serial.println("─────────────────────────────────────────────");
  Serial.printf("[Main] 🪪 Kartu terdeteksi: %s (%u bit)\n", uid.c_str(), bits);

  // Jika ALLOW_ANY_CARD aktif (mode testing cepat)
  if (ALLOW_ANY_CARD) {
    Serial.println("[Main] 🔓 [MODE TES] Kartu diterima langsung!");
    feedbackGranted();
    relayUnlockTimed(UNLOCK_MS);
    wiegandFlush();
    Serial.println("─────────────────────────────────────────────");
    return;
  }

  // Aksi berdasarkan mode operasi
  if (registerMode) {
    handleRegisterScan(uid);
  } else {
    handleCardTap(uid);
  }

  wiegandFlush();
  Serial.println("─────────────────────────────────────────────");
  Serial.println();
}

// ============================================
//  REGISTER MODE (Scan untuk Tambah Kartu di Web)
// ============================================

void checkOperationMode() {
  if (!wifiIsConnected()) return;

  String mode = apiCheckMode();
  if (mode == "register" && !registerMode) {
    registerMode = true;
    Serial.println();
    Serial.println("╔═══════════════════════════════════════════╗");
    Serial.println("║  📋 MODE REGISTRASI AKTIF DARI WEB         ║");
    Serial.println("║  Tap kartu baru untuk didaftarkan...       ║");
    Serial.println("╚═══════════════════════════════════════════╝");

    // Beep 2x pendek penanda masuk mode registrasi
    if (RFID_BUZZER_PIN >= 0) {
      for (int i = 0; i < 2; i++) {
        digitalWrite(RFID_BUZZER_PIN, LOW);
        delay(80);
        digitalWrite(RFID_BUZZER_PIN, HIGH);
        delay(80);
      }
    }
  } else if (mode == "normal" && registerMode) {
    registerMode = false;
    Serial.println("[Main] ↩️ Kembali ke mode normal (Access Control)");
  }
}

void handleRegisterScan(const String &uid) {
  Serial.println("[Main] 📋 Mengirim UID ke dashboard web...");

  if (!wifiIsConnected()) {
    Serial.println("[Main] ❌ WiFi terputus. Registrasi membutuhkan koneksi server.");
    feedbackDenied();
    return;
  }

  bool ok = apiSendRegisterScan(uid);
  if (ok) {
    Serial.println("[Main] ✅ Berhasil kirim UID ke dashboard! Lengkapi form di web.");
    feedbackGranted();
    registerMode = false; // Otomatis kembali ke mode normal setelah 1 scan berhasil
  } else {
    Serial.println("[Main] ❌ Gagal kirim UID ke server.");
    feedbackDenied();
  }
}

// ============================================
//  HANDLE TAP (Normal Access Control)
// ============================================

void handleCardTap(const String &uid) {
  if (wifiIsConnected()) {
    handleOnlineTap(uid);
  } else {
    handleOfflineTap(uid);
  }
}

void handleOnlineTap(const String &uid) {
  TapResponse res = apiSendTap(uid);

  if (res.success) {
    serverAvailable = true;
    if (res.action == "grant") {
      Serial.printf("[Main] ✅ AKSES DIBERIKAN: %s\n", res.cardHolder.c_str());
      feedbackGranted();
      relayUnlockTimed(UNLOCK_MS);
      apiSendDoorStatus(doorGetPhysicalState());
    } else {
      Serial.printf("[Main] ❌ AKSES DITOLAK (Alasan: %s)\n", res.reason.c_str());
      feedbackDenied();
    }
  } else {
    // Server tidak merespon -> fallback ke verifikasi lokal
    Serial.println("[Main] ⚠️ Server tidak merespon, fallback ke validasi lokal (offline)");
    serverAvailable = false;
    handleOfflineTap(uid);
  }
}

void handleOfflineTap(const String &uid) {
  AccessResult result;
  String accessLevel;

  bool granted = accessControlValidate(uid, result, accessLevel);
  Serial.printf("[Main] [OFFLINE] %s\n", accessResultToString(result).c_str());

  if (granted) {
    feedbackGranted();
    relayUnlockTimed(UNLOCK_MS);
  } else {
    feedbackDenied();
  }

  // Kirim log ke server bila ada koneksi (best effort)
  if (wifiIsConnected()) {
    String status = granted ? "granted" : "denied";
    String reason = accessResultToReason(result);
    apiSendTapLog(uid, status, reason, accessLevel);
  }
}

// ============================================
//  PUSH BUTTON (Manual Exit Button)
// ============================================

void checkPushButton() {
  static bool lastRead   = HIGH;
  static bool stableBtn  = HIGH;
  static unsigned long tChange = 0;

  bool r = digitalRead(PIN_BTN);
  if (r != lastRead) {
    lastRead = r;
    tChange  = millis();
  }

  // Debounce stabil selama BTN_DEBOUNCE_MS
  if (millis() - tChange > BTN_DEBOUNCE_MS && r != stableBtn) {
    stableBtn = r;

    // Saat tombol ditekan (LOW) dan relay sedang tidak unlock & tidak dalam ignore noise
    if (stableBtn == LOW && !relayIsUnlocked() && !relayIsIgnoring()) {
      handleManualUnlock();
    }
  }
}

void handleManualUnlock() {
  Serial.println();
  Serial.println("─────────────────────────────────────────────");
  Serial.println("[Main] 🔘 Push Button ditekan — BUKA PINTU MANUAL");

  feedbackGranted();
  relayUnlockTimed(UNLOCK_MS);

  if (wifiIsConnected()) {
    apiSendTapLog("BUTTON", "granted", "manual_unlock", "physical");
    apiSendDoorStatus(doorGetPhysicalState());
  }

  Serial.println("─────────────────────────────────────────────");
  Serial.println();
}

// ============================================
//  SINKRONISASI & HEARTBEAT
// ============================================

void syncWhitelist() {
  if (!wifiIsConnected()) return;

  Serial.println("[Main] 🔄 Menyinkronkan whitelist dari server...");
  String json;
  if (apiGetWhitelist(json)) {
    int count = accessControlLoadFromJson(json);
    if (count >= 0) {
      serverAvailable = true;
    }
  }
}

void sendHeartbeat() {
  if (!wifiIsConnected()) return;

  bool ok = apiSendHeartbeat(doorGetPhysicalState());
  if (ok) {
    serverAvailable = true;
  } else {
    serverAvailable = false;
    Serial.println("[Main] ⚠️ Heartbeat gagal. Server mungkin offline.");
  }
}

// ============================================
//  FEEDBACK AUDIO / VISUAL
// ============================================

void feedbackGranted() {
  if (RFID_LED_PIN >= 0) {
    digitalWrite(RFID_LED_PIN, LOW); // LED ON
  }
  if (RFID_BUZZER_PIN >= 0) {
    digitalWrite(RFID_BUZZER_PIN, LOW);
    delay(150);
    digitalWrite(RFID_BUZZER_PIN, HIGH);
  }
  if (RFID_LED_PIN >= 0) {
    delay(200);
    digitalWrite(RFID_LED_PIN, HIGH); // LED OFF
  }
}

void feedbackDenied() {
  if (RFID_BUZZER_PIN >= 0) {
    for (int i = 0; i < 3; i++) {
      digitalWrite(RFID_BUZZER_PIN, LOW);
      delay(80);
      digitalWrite(RFID_BUZZER_PIN, HIGH);
      delay(80);
    }
  }
}
