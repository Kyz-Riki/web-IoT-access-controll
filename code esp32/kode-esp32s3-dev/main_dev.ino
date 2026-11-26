#include "config.h"
#include "wifi_manager.h"
#include "wiegand_reader.h"
#include "relay_control.h"
#include "access_control.h"
#include "api_client.h"

#if ENABLE_OTA
#include <ArduinoOTA.h>
#endif

#include <esp_idf_version.h>
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

static volatile bool registerMode          = false; // True jika dashboard meminta registrasi kartu
static volatile bool serverAvailable       = false;
static volatile bool remoteUnlockRequested = false; // True jika ada remote unlock dari web

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
#if ENABLE_OTA
void setupOTA();
#endif

// ============================================
//  SETUP
// ============================================

void setup() {
  // 1. Inisialisasi Relay (Dropbolt) sedini mungkin di milidetik ke-0
  // agar pin tidak mengambang (floating) dan memicu aktivasi liar saat boot
  relayInit();

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

  // 2. Inisialisasi Sensor Pintu
  doorSensorInit();

  // 3. Inisialisasi Push Button Exit
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

#if ENABLE_OTA
  // 9. Inisialisasi ArduinoOTA (Wireless Upload)
  setupOTA();
#endif

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

  // 1b. Periksa perintah Remote Unlock dari Web Dashboard
  if (remoteUnlockRequested) {
    remoteUnlockRequested = false;
    Serial.println();
    Serial.println("─────────────────────────────────────────────");
    Serial.println("[Core 1] 🌐 REMOTE UNLOCK diterima dari Web Dashboard");
    feedbackGranted();
    relayUnlockTimed(UNLOCK_MS);
    Serial.println("─────────────────────────────────────────────");
  }

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

  // ══════════════════════════════════════════════
  //  VALIDASI: Online -> Realtime Server, Offline -> Whitelist Lokal
  // ══════════════════════════════════════════════

  if (serverAvailable && wifiIsConnected()) {
    Serial.println("[Core 1] 🌐 ESP32 Online — Mengirim tap ke server untuk validasi realtime...");
    TapResponse resp = apiSendTap(uid);

    if (resp.success) {
      if (strcmp(resp.action, "grant") == 0) {
        Serial.printf("[Core 1] ✅ SERVER GRANTED (Pemegang: %s)\n", resp.cardHolder);
        feedbackGranted();
        relayUnlockTimed(UNLOCK_MS);
      } else {
        Serial.printf("[Core 1] ❌ SERVER DENIED: %s (Pemegang: %s)\n", resp.reason, resp.cardHolder);
        feedbackDenied();
      }
      // Server sudah memvalidasi, mencatat log ke DB, dan broadcast ke web secara realtime
      Serial.println("─────────────────────────────────────────────");
      Serial.println();
      return;
    } else {
      Serial.println("[Core 1] ⚠️ Server tidak merespons (HTTP timeout/error), fallback ke validasi lokal...");
      serverAvailable = false;
      // Lanjutkan ke validasi lokal di bawah
    }
  }

  // Validasi lokal terhadap whitelist di RAM (Offline Fallback)
  Serial.println("[Core 1] 📴 Mode Offline — Validasi lokal terhadap whitelist internal RAM...");
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

  // Kirim event ke antrean Core 0 untuk dilaporkan ke database server saat koneksi kembali
  TapEvent ev;
  ev.type = EVENT_TAP_LOG;
  strncpy(ev.uid, uid, UID_MAX_LENGTH);
  ev.uid[UID_MAX_LENGTH] = '\0';
  strncpy(ev.status, granted ? "granted" : "denied", sizeof(ev.status) - 1);
  ev.status[sizeof(ev.status) - 1] = '\0';
  strncpy(ev.reason, accessResultToReason(result), sizeof(ev.reason) - 1);
  ev.reason[sizeof(ev.reason) - 1] = '\0';
  strncpy(ev.accessLevel, accessLevel, sizeof(ev.accessLevel) - 1);
  ev.accessLevel[sizeof(ev.accessLevel) - 1] = '\0';

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
  ev.uid[sizeof(ev.uid) - 1] = '\0';
  strncpy(ev.status, "granted", sizeof(ev.status) - 1);
  ev.status[sizeof(ev.status) - 1] = '\0';
  strncpy(ev.reason, "manual_unlock", sizeof(ev.reason) - 1);
  ev.reason[sizeof(ev.reason) - 1] = '\0';
  strncpy(ev.accessLevel, "physical", sizeof(ev.accessLevel) - 1);
  ev.accessLevel[sizeof(ev.accessLevel) - 1] = '\0';

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
  bool lastReportedUnlocked       = (strcmp(doorGetPhysicalState(), "unlocked") == 0);

  // Coba koneksi awal saat boot
  wifiConnect();
  if (wifiIsConnected()) {
    wasOnline = true;
    serverAvailable = true;
    char serverWlVer[16] = {0};
    bool hbOk = apiSendHeartbeat(doorGetPhysicalState(), serverWlVer, sizeof(serverWlVer));
    serverAvailable = hbOk;
    if (hbOk) {
      if (accessControlGetVersion()[0] == '\0' || accessControlNeedsSync(serverWlVer)) {
        Serial.println("[Core 0] 📥 Whitelist berubah/belum ada — mengunduh whitelist awal...");
        apiSyncWhitelist();
      } else {
        Serial.printf("[Core 0] ✅ Whitelist lokal sudah sesuai server (ver=%s) — skip unduh\n", serverWlVer);
      }
    }
  }

  // Daftarkan networkTask (Core 0) ke Task Watchdog Timer
  esp_task_wdt_add(NULL);
  Serial.printf("[Core 0] 🛡️ Network Task terdaftar di Watchdog (Core %d)\n", xPortGetCoreID());

  for (;;) {
    // Feed watchdog untuk Core 0 di setiap siklus loop
    esp_task_wdt_reset();

    // 1. Reconnect WiFi secara non-blocking jika terputus
    wifiReconnectIfNeeded();
    bool online = wifiIsConnected();

    // 2. Deteksi transisi Offline -> Online (Pemicu Sinkronisasi Instan jika ada perubahan)
    if (!wasOnline && online) {
      Serial.println();
      Serial.println("[Core 0] 🔄 Koneksi server pulih! Mengecek versi whitelist...");
      wasOnline = true;
      lastHeartbeat = millis();
      lastWhitelistSync = millis();
      char serverWlVer[16] = {0};
      bool hbOk = apiSendHeartbeat(doorGetPhysicalState(), serverWlVer, sizeof(serverWlVer));
      serverAvailable = hbOk;
      if (hbOk && (accessControlGetVersion()[0] == '\0' || accessControlNeedsSync(serverWlVer))) {
        apiSyncWhitelist();
      }
    } else if (wasOnline && !online) {
      wasOnline = false;
      serverAvailable = false;
      Serial.println("[Core 0] ⚠️ Terputus dari server. Log tap offline akan ditahan di antrean.");
    }

    // 2b. Laporkan perubahan status relay/pintu secara real-time (unlocked <-> locked)
    bool currentUnlocked = (strcmp(doorGetPhysicalState(), "unlocked") == 0);
    if (online && currentUnlocked != lastReportedUnlocked) {
      if (apiSendDoorStatus(currentUnlocked ? "unlocked" : "locked")) {
        lastReportedUnlocked = currentUnlocked;
        Serial.printf("[Core 0] 🚪 Status pintu dilaporkan ke server: %s\n",
                      currentUnlocked ? "unlocked" : "locked");
      }
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

      // Cek antrean perintah tertunda dari dashboard (misal Remote Unlock)
      char cmdStr[16] = {0};
      if (apiCheckPendingCommand(cmdStr, sizeof(cmdStr))) {
        if (strcmp(cmdStr, "unlock") == 0) {
          Serial.println("[Core 0] 🔓 Menerima perintah Remote Unlock dari web dashboard!");
          remoteUnlockRequested = true;
        }
      }
    }

    // 5. Kirim Heartbeat ke Server berkala + cek apakah versi whitelist berubah
    if (online && (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS)) {
      lastHeartbeat = millis();
      char serverWlVer[16] = {0};
      bool ok = apiSendHeartbeat(doorGetPhysicalState(), serverWlVer, sizeof(serverWlVer));
      serverAvailable = ok;

      // Unduh whitelist HANYA jika ada perubahan data di server (hemat RAM/jaringan/NVS Flash)
      if (ok && (accessControlGetVersion()[0] == '\0' || accessControlNeedsSync(serverWlVer))) {
        Serial.printf("[Core 0] 🔔 Perubahan whitelist terdeteksi (lokal=%s -> server=%s), menyinkronkan...\n",
                      accessControlGetVersion()[0] ? accessControlGetVersion() : "-",
                      serverWlVer[0] ? serverWlVer : "-");
        apiSyncWhitelist();
        lastWhitelistSync = millis();
      }
    }

    // 6. Fallback sync berkala HANYA jika belum punya versi (misal server versi lama)
    if (online && accessControlGetVersion()[0] == '\0' && (millis() - lastWhitelistSync >= WHITELIST_SYNC_MS)) {
      lastWhitelistSync = millis();
      apiSyncWhitelist();
    }

    // 7. Handle OTA (Wireless Upload) jika terhubung ke WiFi
#if ENABLE_OTA
    if (online) {
      ArduinoOTA.handle();
    }
#endif

    // Beri jeda 40ms agar CPU Core 0 tidak 100% dan task FreeRTOS lain tetap jalan
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

// ============================================
//  HARDWARE WATCHDOG TIMER (WDT)
// ============================================

void initWatchdog() {
#if defined(ESP_IDF_VERSION) && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
  // Arduino-ESP32 3.x (ESP-IDF 5.x):
  // TWDT biasanya sudah di-init otomatis oleh Arduino core saat boot.
  // Gunakan esp_task_wdt_reconfigure agar tidak terjadi error ESP_ERR_INVALID_STATE.
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  esp_err_t err = esp_task_wdt_reconfigure(&twdt_config);
  if (err == ESP_ERR_INVALID_STATE) {
    // Jika belum diinisialisasi oleh framework, lakukan init
    esp_task_wdt_init(&twdt_config);
  }
  esp_task_wdt_add(NULL); // Daftarkan loopTask (Core 1)
#else
  // Arduino-ESP32 2.x (ESP-IDF 4.x)
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
  esp_task_wdt_add(NULL); // Daftarkan loopTask (Core 1)
#endif
  Serial.printf("[WDT] ✅ Hardware Watchdog aktif (%d detik timeout, memantau Core 1 & Core 0)\n", WDT_TIMEOUT_SEC);
}

// ============================================
//  OTA (Over-The-Air) WIRELESS UPLOAD
// ============================================

#if ENABLE_OTA
void setupOTA() {
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (strlen(OTA_PASSWORD) > 0) {
    ArduinoOTA.setPassword(OTA_PASSWORD);
  }

  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] 🚀 Menerima pembaruan firmware nirkabel...");
    relayLock(); // Kunci pintu demi keamanan saat flashing
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] ✅ Update sukses! ESP32 akan me-reboot...");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    esp_task_wdt_reset(); // Feed watchdog agar tidak memotong proses flashing
    Serial.printf("[OTA] Flashing: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] ❌ Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed (Password salah)");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.printf("[OTA] ✅ Siap menerima upload wireless (mDNS: %s.local, Port %d)\n",
                OTA_HOSTNAME, OTA_PORT);
}
#endif

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
