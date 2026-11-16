/*
 * =====================================================================
 *  Access Control System (test-dev) — Dual-Core FreeRTOS 24/7 Production
 *  ESP32-S3 + Wiegand Reader + Relay/Dropbolt + Push Button + WiFi Client
 * =====================================================================
 *
 *  Arsitektur Multi-Core:
 *    - Core 1 (Arduino loop): Real-Time I/O (Wiegand, Push Button, Relay, Local Validation)
 *    - Core 0 (FreeRTOS Task): Background Network Worker (WiFi, HTTP, Heartbeat, Sync)
 *    - Jembatan Komunikasi: FreeRTOS xTapQueue (Thread-Safe)
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
 *    - Dioda Flyback (1N4007):
 *        Katoda (Garis Putih) -> Terminal Relay NO/NC (Kabel + Dropbolt)
 *        Anoda (Polos Hitam)   -> WAGO GND (Kabel - Dropbolt)
 */

#include "config.h"
#include "wifi_manager.h"
#include "wiegand_reader.h"
#include "relay_control.h"
#include "access_control.h"
#include "api_client.h"

#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ============================================
//  FreeRTOS Queue Data Structures
// ============================================

enum EventType {
  EVENT_TAP_LOG,
  EVENT_REGISTER_SCAN
};

struct TapEvent {
  EventType type;
  char uid[UID_MAX_LENGTH + 1];
  char status[12];       // "granted" / "denied"
  char reason[24];       // "unregistered", "blocked", "out_of_schedule", "manual_unlock"
  char accessLevel[12];  // "admin", "level_1", "physical", etc.
};

static QueueHandle_t xTapQueue = NULL;
static TaskHandle_t  xNetTaskHandle = NULL;

// ============================================
//  Timing Trackers (Core 1)
// ============================================

static unsigned long lastTapTime     = 0;
static unsigned long lastNoiseReport = 0;
static unsigned long lastDebugHb     = 0;

// ============================================
//  Shared State (Thread-Safe)
// ============================================

static volatile bool registerMode    = false; // True jika dashboard meminta registrasi kartu
static volatile bool serverAvailable = false;

// ============================================
//  Forward Declarations
// ============================================

void checkPushButton();
void handleManualUnlock();
void handleCardTap(const char* uid, uint8_t bits);
void feedbackGranted();
void feedbackDenied();
void networkTask(void *pvParameters);
void initWatchdog();

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
  delay(200);

  Serial.println();
  Serial.println("====================================================");
  Serial.println("   ACCESS CONTROL IoT (test-dev) — ESP32-S3 (Dual-Core)");
  Serial.printf("   Device ID : %s\n", ESP_ID);
  Serial.printf("   Pintu     : %s\n", DOOR_NAME);
  Serial.printf("   Mode Tes  : %s\n", ALLOW_ANY_CARD ? "Semua Kartu Valid" : "Validasi Server/Whitelist");
  Serial.println("====================================================");

  // 1. Inisialisasi Relay (Dropbolt) & Sensor
  relayInit();
  doorSensorInit();

  // 2. Inisialisasi Push Button Exit
  if (PIN_BTN >= 0) {
    pinMode(PIN_BTN, INPUT_PULLUP);
    Serial.printf("[Setup] ✅ Push Button exit aktif di GPIO%d\n", PIN_BTN);
  }

  // 3. Inisialisasi Wiegand Reader
  wiegandInit();
  relaySetIgnore(1000); // Abaikan noise awal boot
  wiegandFlush();

  // 4. Inisialisasi Access Control Whitelist Lokal
  accessControlInit();

  // 5. Inisialisasi WiFi
  wifiInit();

  // 6. Buat FreeRTOS Queue untuk Komunikasi Core 1 -> Core 0
  xTapQueue = xQueueCreate(TAP_QUEUE_SIZE, sizeof(TapEvent));
  if (xTapQueue == NULL) {
    Serial.println("[Setup] ❌ Gagal membuat FreeRTOS xTapQueue!");
  } else {
    Serial.printf("[Setup] ✅ FreeRTOS xTapQueue siap (kapasitas %d)\n", TAP_QUEUE_SIZE);
  }

  // 7. Jalankan Network Task di Core 0 (Background)
  xTaskCreatePinnedToCore(
    networkTask,
    "NetWorker",
    NET_TASK_STACK_SIZE,
    NULL,
    1,                  // Priority normal (cooperative)
    &xNetTaskHandle,
    0                   // Pin ke Core 0!
  );

  // 8. Inisialisasi Hardware Watchdog Timer (WDT) untuk Core 1
  initWatchdog();

  Serial.println();
  Serial.println(">> Sistem siap digunakan (Dual-Core aktif). Siap tap kartu / tombol <<");
  Serial.println("====================================================");
  Serial.println();
}

// ============================================
//  MAIN LOOP (Core 1 — Real-Time I/O)
// ============================================

void loop() {
  // Feed Hardware Watchdog Timer (mencegah auto-reboot)
  esp_task_wdt_reset();

  // 1. Auto-lock relay jika durasi unlock sudah habis (safe rollover)
  relayUpdate();

  // 2. Polling Push Button manual exit (Respons instan < 5 ms)
  if (PIN_BTN >= 0) {
    checkPushButton();
  }

  // 3. Laporan filter noise berkala
  if (millis() - lastNoiseReport >= 5000) {
    lastNoiseReport = millis();
    uint32_t rj = wiegandGetRejectedCount();
    if (rj > 0) {
      Serial.printf("[Wiegand Filter] ℹ️ %u pulsa noise berhasil dibuang\n", rj);
    }
  }

  // 4. Debug Heartbeat Serial
  if (DEBUG_HEARTBEAT && millis() - lastDebugHb >= 2000) {
    lastDebugHb = millis();
    Serial.printf("[Status] Core1 Uptime: %lus | WiFi: %s | Relay: %s | FreeHeap: %u\n",
                  millis() / 1000,
                  wifiIsConnected() ? "Online" : "Offline",
                  relayGetState(),
                  ESP.getFreeHeap());
  }

  // 5. Cek apakah relay sedang dalam periode ignore noise
  if (relayIsIgnoring()) {
    wiegandFlush();
    return;
  }

  // 6. Pembacaan Kartu RFID Wiegand (Non-blocking)
  String uidStr;
  uint8_t bits = 0;
  if (!wiegandCheckCard(uidStr, bits)) {
    return;
  }

  // Anti double-tap (jeda antar pemindaian)
  if (millis() - lastTapTime < ANTI_DOUBLE_TAP_MS) {
    wiegandFlush();
    return;
  }
  lastTapTime = millis();

  // Eksekusi penanganan kartu
  handleCardTap(uidStr.c_str(), bits);
  wiegandFlush();
}

// ============================================
//  HANDLE TAP (Core 1 — Local-First Validation)
// ============================================

void handleCardTap(const char* uid, uint8_t bits) {
  Serial.println();
  Serial.println("─────────────────────────────────────────────");
  Serial.printf("[Core 1] 🪪 Kartu terdeteksi: %s (%u bit)\n", uid, bits);

  // Jika mode ALLOW_ANY_CARD aktif (testing)
  if (ALLOW_ANY_CARD) {
    Serial.println("[Core 1] 🔓 [MODE TES] Kartu diterima langsung!");
    feedbackGranted();
    relayUnlockTimed(UNLOCK_MS);
    Serial.println("─────────────────────────────────────────────");
    return;
  }

  // Jika sedang dalam Mode Registrasi Kartu Baru
  if (registerMode) {
    Serial.println("[Core 1] 📋 Mode Registrasi aktif — Antrekan UID ke Core 0...");
    feedbackGranted();

    TapEvent ev;
    ev.type = EVENT_REGISTER_SCAN;
    strncpy(ev.uid, uid, UID_MAX_LENGTH);
    ev.uid[UID_MAX_LENGTH] = '\0';
    ev.status[0] = '\0';
    ev.reason[0] = '\0';
    ev.accessLevel[0] = '\0';

    if (xTapQueue) {
      xQueueSend(xTapQueue, &ev, 0);
    }
    Serial.println("─────────────────────────────────────────────");
    return;
  }

  // Validasi lokal secepat kilat (< 5 ms) terhadap whitelist di RAM
  AccessResult result;
  char accessLevel[12] = {0};
  bool granted = accessControlValidate(uid, result, accessLevel, sizeof(accessLevel));

  Serial.printf("[Core 1] %s (Level: %s)\n", accessResultToString(result), accessLevel);

  if (granted) {
    feedbackGranted();
    relayUnlockTimed(UNLOCK_MS);
  } else {
    feedbackDenied();
  }

  // Kirim event ke antrean Core 0 untuk dilaporkan ke database server di background
  TapEvent ev;
  ev.type = EVENT_TAP_LOG;
  strncpy(ev.uid, uid, UID_MAX_LENGTH);
  ev.uid[UID_MAX_LENGTH] = '\0';
  strncpy(ev.status, granted ? "granted" : "denied", sizeof(ev.status) - 1);
  strncpy(ev.reason, accessResultToReason(result), sizeof(ev.reason) - 1);
  strncpy(ev.accessLevel, accessLevel, sizeof(ev.accessLevel) - 1);

  if (xTapQueue) {
    if (xQueueSend(xTapQueue, &ev, 0) != pdTRUE) {
      Serial.println("[Core 1] ⚠️ xTapQueue penuh! Log tap dibuang");
    }
  }

  Serial.println("─────────────────────────────────────────────");
  Serial.println();
}

// ============================================
//  PUSH BUTTON (Core 1 — Manual Exit Button)
// ============================================

void checkPushButton() {
  static bool lastRead  = HIGH;
  static bool stableBtn = HIGH;
  static unsigned long tChange = 0;

  bool r = digitalRead(PIN_BTN);
  if (r != lastRead) {
    lastRead = r;
    tChange  = millis();
  }

  // Debounce stabil
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
  Serial.println("[Core 1] 🔘 Push Button ditekan — BUKA PINTU MANUAL");

  feedbackGranted();
  relayUnlockTimed(UNLOCK_MS);

  // Antrekan log tombol ke Core 0
  TapEvent ev;
  ev.type = EVENT_TAP_LOG;
  strncpy(ev.uid, "BUTTON", sizeof(ev.uid) - 1);
  strncpy(ev.status, "granted", sizeof(ev.status) - 1);
  strncpy(ev.reason, "manual_unlock", sizeof(ev.reason) - 1);
  strncpy(ev.accessLevel, "physical", sizeof(ev.accessLevel) - 1);

  if (xTapQueue) {
    xQueueSend(xTapQueue, &ev, 0);
  }

  Serial.println("─────────────────────────────────────────────");
  Serial.println();
}

// ============================================
//  NETWORK TASK (Core 0 — Background Worker)
// ============================================

void networkTask(void *pvParameters) {
  Serial.printf("[Core 0] 🌐 Network Task aktif di Core %d\n", xPortGetCoreID());

  unsigned long lastHeartbeat     = 0;
  unsigned long lastWhitelistSync = 0;
  unsigned long lastModeCheck     = 0;
  bool wasOnline                  = false;

  // Coba koneksi awal saat boot
  wifiConnect();
  if (wifiIsConnected()) {
    wasOnline = true;
    serverAvailable = true;
    apiSyncWhitelist();
    apiSendHeartbeat(doorGetPhysicalState());
  }

  for (;;) {
    // 1. Reconnect WiFi secara non-blocking jika terputus
    wifiReconnectIfNeeded();
    bool online = wifiIsConnected();

    // 2. Deteksi transisi Offline -> Online (Pemicu Sinkronisasi Instan)
    if (!wasOnline && online) {
      Serial.println();
      Serial.println("[Core 0] 🔄 Koneksi server pulih! Memulai sinkronisasi instan...");
      wasOnline = true;
      serverAvailable = true;
      lastHeartbeat = millis();
      lastWhitelistSync = millis();
      apiSyncWhitelist();
      apiSendHeartbeat(doorGetPhysicalState());
    } else if (wasOnline && !online) {
      wasOnline = false;
      serverAvailable = false;
      Serial.println("[Core 0] ⚠️ Terputus dari server. Log tap offline akan ditahan di antrean.");
    }

    // 3. Proses antrean TapEvent (Tahan di antrean jika offline / gagal kirim)
    TapEvent ev;
    while (xTapQueue != NULL && xQueuePeek(xTapQueue, &ev, 0) == pdTRUE) {
      if (!online) {
        // Jangan buang antrean saat offline! Biarkan tersimpan untuk di-flush nanti
        break;
      }

      bool success = false;
      if (ev.type == EVENT_REGISTER_SCAN) {
        success = apiSendRegisterScan(ev.uid);
        if (success) {
          registerMode = false;
          Serial.println("[Core 0] ✅ Register scan terkirim ke dashboard web");
        }
      } else if (ev.type == EVENT_TAP_LOG) {
        success = apiSendTapLog(ev.uid, ev.status, ev.reason, ev.accessLevel);
      }

      if (success) {
        // Pop dari antrean HANYA setelah sukses terkirim ke server
        TapEvent popped;
        xQueueReceive(xTapQueue, &popped, 0);
      } else {
        // Server mungkin tidak merespon/down sementara -> hentikan pengurasan antrean
        serverAvailable = false;
        break;
      }
    }

    // 4. Polling Mode Check (Normal / Register Mode)
    if (online && (millis() - lastModeCheck >= MODE_CHECK_MS)) {
      lastModeCheck = millis();
      char modeStr[16] = {0};
      if (apiCheckMode(modeStr, sizeof(modeStr))) {
        if (strcmp(modeStr, "register") == 0 && !registerMode) {
          registerMode = true;
          Serial.println();
          Serial.println("╔═══════════════════════════════════════════╗");
          Serial.println("║  📋 MODE REGISTRASI AKTIF DARI WEB         ║");
          Serial.println("║  Tap kartu baru untuk didaftarkan...       ║");
          Serial.println("╚═══════════════════════════════════════════╝");
        } else if (strcmp(modeStr, "normal") == 0 && registerMode) {
          registerMode = false;
          Serial.println("[Core 0] ↩️ Kembali ke mode normal (Access Control)");
        }
      }
    }

    // 5. Kirim Heartbeat ke Server berkala (dilengkapi FreeHeap & MaxAllocHeap)
    if (online && (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS)) {
      lastHeartbeat = millis();
      bool ok = apiSendHeartbeat(doorGetPhysicalState());
      serverAvailable = ok;
    }

    // 6. Sinkronisasi Whitelist berkala dari Server
    if (online && (millis() - lastWhitelistSync >= WHITELIST_SYNC_MS)) {
      lastWhitelistSync = millis();
      apiSyncWhitelist();
    }

    // Beri jeda 40ms agar CPU Core 0 tidak 100% dan task FreeRTOS lain tetap jalan
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

// ============================================
//  HARDWARE WATCHDOG TIMER (WDT)
// ============================================

void initWatchdog() {
#if defined(ESP_IDF_VERSION) && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  esp_task_wdt_init(&twdt_config);
  esp_task_wdt_add(NULL); // Daftarkan loopTask (Core 1)
#else
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
  esp_task_wdt_add(NULL);
#endif
  Serial.printf("[WDT] ✅ Hardware Watchdog aktif (%d detik timeout)\n", WDT_TIMEOUT_SEC);
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
