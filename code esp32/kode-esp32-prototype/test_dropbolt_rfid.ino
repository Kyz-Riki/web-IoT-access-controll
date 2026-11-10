/*
 * ============================================
 *  IoT RFID Door Access Control — Main Sketch
 * ============================================
 *
 *  Hardware:
 *    - ESP32 Dev Module
 *    - MFRC522 RFID Reader (SPI)
 *    - Relay Module (Active LOW) + Dropbolt Lock
 *
 *  Library yang diperlukan (install via Library Manager):
 *    - MFRC522     by GithubCommunity
 *    - ArduinoJson by Benoit Blanchon
 *
 *  Alur kerja:
 *    1. Boot → Connect WiFi → Sync NTP → Ambil whitelist dari server
 *    2. Loop → Cek mode (normal/register) → Baca RFID → Proses
 *       - Normal:   Validasi (server/lokal) → Grant/Deny → Log
 *       - Register: Baca UID → Kirim ke server → Dashboard tampilkan form
 *    3. Periodik → Heartbeat (30 detik) → Sync whitelist (5 menit)
 *
 *  File structure:
 *    config.h           → Konfigurasi pin, WiFi, server, timing
 *    wifi_manager.h/cpp → Koneksi WiFi & NTP
 *    rfid_handler.h/cpp → Baca kartu RFID
 *    relay_control.h/cpp→ Kontrol relay / dropbolt
 *    access_control.h/cpp → Whitelist & validasi jadwal (offline)
 *    api_client.h/cpp   → HTTP komunikasi ke server
 */

#include "config.h"
#include "wifi_manager.h"
#include "rfid_handler.h"
#include "relay_control.h"
#include "access_control.h"
#include "api_client.h"

// ============================================
//  Timing Trackers
// ============================================

unsigned long lastHeartbeat = 0;
unsigned long lastWhitelistSync = 0;
unsigned long lastTapTime = 0;
unsigned long lastModeCheck = 0;
unsigned long lastButtonPress = 0;

// ============================================
//  Status Flags
// ============================================

bool serverAvailable = false;    // Apakah server bisa dijangkau?
bool registerMode = false;       // Apakah sedang mode registrasi kartu?

// ============================================
//  SETUP
// ============================================

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(500);

  Serial.println();
  Serial.println("=========================================");
  Serial.println("  IoT RFID Door Access Control");
  Serial.println("  ESP ID: " + String(ESP_ID));
  Serial.println("  Door  : " + String(DOOR_NAME));
  Serial.println("=========================================");
  Serial.println();

  // 1. Inisialisasi hardware
  relayInit();
  doorSensorInit();
  rfidInit();
  accessControlInit();

  // Push button (internal pull-up: HIGH = tidak ditekan, LOW = ditekan)
  if (BUTTON_PIN >= 0) {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    Serial.println("[Main] ✅ Push button aktif di GPIO " + String(BUTTON_PIN));
  }

  // 2. Koneksi WiFi
  wifiInit();
  if (wifiConnect()) {
    // 3. Ambil whitelist dari server
    syncWhitelist();

    // 4. Kirim heartbeat pertama
    sendHeartbeat();
  } else {
    Serial.println();
    Serial.println("[Main] ⚠️ WiFi gagal — berjalan dalam MODE OFFLINE");
    Serial.println("[Main] ⚠️ Tidak ada kartu yang bisa akses sampai WiFi terhubung");
  }

  Serial.println();
  Serial.println("=========================================");
  Serial.println("  ✅ Sistem siap — tap kartu untuk akses");
  Serial.println("=========================================");
  Serial.println();
}

// ============================================
//  MAIN LOOP
// ============================================

void loop() {
  // --- WiFi reconnect jika terputus ---
  wifiReconnectIfNeeded();

  // --- Periodic: Heartbeat ---
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    sendHeartbeat();
    lastHeartbeat = millis();
  }

  // --- Periodic: Sync whitelist ---
  if (millis() - lastWhitelistSync >= WHITELIST_SYNC_INTERVAL_MS) {
    syncWhitelist();
    lastWhitelistSync = millis();
  }

  // --- Periodic: Cek mode operasi dari server ---
  if (millis() - lastModeCheck >= MODE_CHECK_INTERVAL_MS) {
    checkOperationMode();
    lastModeCheck = millis();
  }

  // --- Cek push button manual unlock ---
  if (BUTTON_PIN >= 0) {
    checkPushButton();
  }

  // --- Cek kartu RFID ---
  String uid;
  if (!rfidCheckCard(uid)) {
    return;  // Tidak ada kartu → skip
  }

  // Anti double-tap
  if (millis() - lastTapTime < ANTI_DOUBLE_TAP_MS) {
    rfidHaltCard();
    return;
  }
  lastTapTime = millis();

  Serial.println();
  Serial.println("─────────────────────────────────────");
  Serial.print("[Main] 🪪 Kartu terdeteksi: ");
  Serial.println(uid);

  // --- Pilih aksi berdasarkan mode ---
  if (registerMode) {
    handleRegisterScan(uid);
  } else {
    handleCardTap(uid);
  }

  // --- Selesai, hentikan kartu ---
  rfidHaltCard();

  Serial.println("─────────────────────────────────────");
  Serial.println();
}

// ============================================
//  REGISTER MODE — Scan kartu untuk didaftarkan
// ============================================

/// Cek mode operasi dari server
void checkOperationMode() {
  if (!wifiIsConnected()) return;

  String mode = apiCheckMode();

  if (mode == "register" && !registerMode) {
    // Baru masuk mode register
    registerMode = true;
    Serial.println();
    Serial.println("╔═══════════════════════════════════════╗");
    Serial.println("║  📋 MODE REGISTRASI KARTU AKTIF       ║");
    Serial.println("║  Tap kartu baru di reader untuk scan  ║");
    Serial.println("╚═══════════════════════════════════════╝");
    Serial.println();

    // Feedback visual (buzzer 2x pendek)
    if (BUZZER_PIN >= 0) {
      for (int i = 0; i < 2; i++) {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(80);
        digitalWrite(BUZZER_PIN, LOW);
        delay(80);
      }
    }
  } else if (mode == "normal" && registerMode) {
    // Kembali ke mode normal
    registerMode = false;
    Serial.println("[Main] ↩️ Kembali ke mode normal (akses kontrol)");
  }
}

/// Kirim UID kartu yang di-scan ke server untuk registrasi
void handleRegisterScan(const String &uid) {
  Serial.println("[Main] 📋 Mengirim UID ke dashboard untuk registrasi...");

  if (wifiIsConnected()) {
    bool ok = apiSendRegisterScan(uid);
    if (ok) {
      Serial.println("[Main] ✅ UID terkirim ke dashboard — isi form di web");

      // Feedback sukses (buzzer 1x panjang)
      if (BUZZER_PIN >= 0) {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(300);
        digitalWrite(BUZZER_PIN, LOW);
      }

      // Otomatis kembali ke mode normal
      registerMode = false;
    } else {
      Serial.println("[Main] ❌ Gagal kirim UID — coba tap lagi");
      feedbackDenied();
    }
  } else {
    Serial.println("[Main] ⚠️ WiFi tidak terhubung — registrasi butuh koneksi");
    feedbackDenied();
  }
}

// ============================================
//  HANDLE CARD TAP (Mode Normal)
// ============================================

void handleCardTap(const String &uid) {
  if (wifiIsConnected()) {
    // === MODE ONLINE: Kirim ke server untuk validasi ===
    handleOnlineTap(uid);
  } else {
    // === MODE OFFLINE: Validasi lokal ===
    handleOfflineTap(uid);
  }
}

/// Validasi via server (online mode)
void handleOnlineTap(const String &uid) {
  TapResponse response = apiSendTap(uid);

  if (response.success) {
    // Server berhasil merespons
    if (response.action == "grant") {
      Serial.print("[Main] ✅ AKSES DIBERIKAN");
      if (response.cardHolder.length() > 0) {
        Serial.print(" → ");
        Serial.print(response.cardHolder);
      }
      Serial.println();

      relayUnlockTimed(UNLOCK_DURATION_MS);
      apiSendDoorStatus(doorGetPhysicalState());  // Setelah lock kembali, update status dari sensor fisik
    } else {
      Serial.print("[Main] ❌ AKSES DITOLAK");
      if (response.reason.length() > 0) {
        Serial.print(" (");
        Serial.print(response.reason);
        Serial.print(")");
      }
      Serial.println();
      feedbackDenied();
    }
  } else {
    // Server tidak bisa dijangkau → fallback ke validasi lokal
    Serial.println("[Main] ⚠️ Server tidak merespons, fallback ke validasi lokal");
    handleOfflineTap(uid);
  }
}

/// Validasi lokal (offline mode / fallback)
void handleOfflineTap(const String &uid) {
  AccessResult result;
  String accessLevel;

  bool granted = accessControlValidate(uid, result, accessLevel);

  Serial.print("[Main] [OFFLINE] ");
  Serial.println(accessResultToString(result));

  if (granted) {
    relayUnlockTimed(UNLOCK_DURATION_MS);
  } else {
    feedbackDenied();
  }

  // Coba kirim log ke server (best effort, tidak blocking)
  if (wifiIsConnected()) {
    String status = granted ? "granted" : "denied";
    String reason = accessResultToReason(result);
    apiSendTapLog(uid, status, reason, accessLevel);
  }
}

// ============================================
//  PERIODIC TASKS
// ============================================

/// Ambil whitelist + jadwal dari server dan simpan di memori lokal
void syncWhitelist() {
  if (!wifiIsConnected()) return;

  Serial.println("[Main] 🔄 Sync whitelist dari server...");

  String json;
  if (apiGetWhitelist(json)) {
    int count = accessControlLoadFromJson(json);
    if (count >= 0) {
      serverAvailable = true;
      Serial.print("[Main] ✅ Whitelist tersimpan: ");
      Serial.print(accessControlGetCardCount());
      Serial.print(" kartu, ");
      Serial.print(accessControlGetScheduleCount());
      Serial.println(" jadwal");
    }
  } else {
    Serial.println("[Main] ⚠️ Gagal sync whitelist, pakai data terakhir");
  }
}

/// Kirim heartbeat ke server
void sendHeartbeat() {
  if (!wifiIsConnected()) return;

  bool ok = apiSendHeartbeat(doorGetPhysicalState());
  if (ok) {
    serverAvailable = true;
  } else {
    serverAvailable = false;
    Serial.println("[Main] ⚠️ Heartbeat gagal — server mungkin offline");
  }
}

// ============================================
//  PUSH BUTTON — Manual Unlock dari dalam
// ============================================

/// Deteksi push button dengan debounce
void checkPushButton() {
  // Baca button (INPUT_PULLUP: LOW = ditekan)
  if (digitalRead(BUTTON_PIN) == LOW) {
    // Debounce: abaikan jika baru saja ditekan
    if (millis() - lastButtonPress < BUTTON_DEBOUNCE_MS) return;
    // Abaikan jika relay sedang unlock (sedang proses)
    if (relayIsUnlocked()) return;

    lastButtonPress = millis();
    handleManualUnlock();
  }
}

/// Buka pintu secara manual dan kirim log ke server
void handleManualUnlock() {
  Serial.println();
  Serial.println("─────────────────────────────────────");
  Serial.println("[Main] 🔘 Push button ditekan — MANUAL UNLOCK");

  // Buka dropbolt
  relayUnlockTimed(UNLOCK_DURATION_MS);

  // Kirim log ke server (best effort)
  if (wifiIsConnected()) {
    apiSendTapLog("BUTTON", "granted", "manual_unlock", "physical");
    apiSendDoorStatus(doorGetPhysicalState());  // Update status dari sensor fisik
  }

  Serial.println("─────────────────────────────────────");
  Serial.println();
}

// ============================================
//  FEEDBACK (Buzzer / LED)
// ============================================

/// Feedback saat akses ditolak (buzzer + LED merah jika terpasang)
void feedbackDenied() {
  if (BUZZER_PIN >= 0) {
    // Beep 3x pendek
    for (int i = 0; i < 3; i++) {
      digitalWrite(BUZZER_PIN, HIGH);
      delay(100);
      digitalWrite(BUZZER_PIN, LOW);
      delay(100);
    }
  }

  if (LED_RED_PIN >= 0) {
    digitalWrite(LED_RED_PIN, HIGH);
    delay(500);
    digitalWrite(LED_RED_PIN, LOW);
  }
}
