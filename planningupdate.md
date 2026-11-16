# 📋 Rencana Pembaruan & Refactoring Firmware ESP32-S3 (Access Control 24/7)

> **Dokumen Perencanaan Teknis (Engineering Plan)**  
> **Target Perangkat:** ESP32-S3 Dev Module  
> **Target Direktori:** `code esp32/kode-esp32s3-dev/`  
> **Target Operasional:** Nonstop 24/7/365 (*Mission-Critical Production Grade*)  
> **Terakhir Diperbarui:** 16 November 2026  

---

## 📑 Daftar Isi
1. [Latar Belakang & Identifikasi Masalah](#1-latar-belakang--identifikasi-masalah)
2. [Arsitektur Baru: Dual-Core FreeRTOS](#2-arsitektur-baru-dual-core-freertos)
3. [Daftar Prioritas Migrasi](#3-daftar-prioritas-migrasi)
4. [Rincian Rencana Perubahan Tiap File](#4-rincian-rencana-perubahan-tiap-file)
5. [Panduan Instalasi Proteksi Fisik (Hardware)](#5-panduan-instalasi-proteksi-fisik-hardware)
6. [Skenario Pengujian & Kriteria Kelayakan (Testing & QA)](#6-skenario-pengujian--kriteria-kelayakan-testing--qa)
7. [Checklist Progress Eksekusi](#7-checklist-progress-eksekusi)

---

## 1. Latar Belakang & Identifikasi Masalah

Berdasarkan audit teknis terhadap firmware prototipe saat ini, ditemukan **5 titik kritis (*bottlenecks / bugs*)** yang menyebabkan perangkat rentan mengalami *freeze*, respon lambat, atau *crash* mendadak saat berjalan nonstop:

1. **Blocking I/O pada Loop Utama:** Request HTTP (`GET /api/esp/mode`) tiap 2 detik dan fungsi `WiFiClient::connect()` menahan CPU selama 3–5 detik jika server mati/offline. Selama CPU tertahan, pembacaan RFID dan Push Button macet total.
2. **Blocking saat WiFi Putus:** `while (WiFi.status() != WL_CONNECTED) delay(500)` di `wifi_manager.cpp` menahan eksekusi hingga 10–15 detik saat koneksi hilang.
3. **Bug `millis()` Rollover di Hari ke-49,7:** Rumus timer di `relay_control.cpp` menggunakan perbandingan langsung `millis() > _unlockUntil`, yang akan rusak ketika register `millis()` meluap (*overflow*) kembali ke 0.
4. **Fragmentasi Memori RAM (Heap Fragmentation):** Pembacaan karakter per karakter menggunakan objek dinamis `String` di `api_client.cpp` memecah RAM ESP32 menjadi potongan-potongan kecil hingga berujung pada *Out Of Memory (OOM) Kernel Panic*.
5. **Belum Ada Watchdog Timer (WDT):** Jika driver WiFi atau stack TCP membeku, perangkat tidak memiliki mekanisme auto-reboot mandiri.

---

## 2. Arsitektur Baru: Dual-Core FreeRTOS

ESP32-S3 memiliki prosesor Dual-Core (Xtensa LX7). Arsitektur baru akan memisahkan domain **Hardware Real-Time** dengan domain **Networking/Background** ke dua core yang berbeda:

```mermaid
flowchart TB
    subgraph Core1 ["CORE 1: REAL-TIME HARDWARE TASK (Priority: HIGH)"]
        Wiegand["Wiegand RFID Reader (ISR CHANGE)"]
        Button["Push Button Exit (Debounce Polling)"]
        Relay["Relay Dropbolt Control (Safe Rollover Timer)"]
        Sensors["Door Status Sensor"]
        LocalDB["Validasi Offline Whitelist (In-Memory Struct)"]
    end

    subgraph QueueBridge ["FreeRTOS QUEUES & MUTEX (Thread-Safe Bridge)"]
        TapQueue["xTapEventQueue (Tap Event & Button Log)"]
        SyncQueue["xSyncCommandQueue (Sync / Config Commands)"]
        SharedState["Shared State Flags (Mutex Protected)"]
    end

    subgraph Core0 ["CORE 0: NETWORK WORKER TASK (Priority: LOW/NORMAL)"]
        WiFiMgr["Non-blocking WiFi Reconnect & Keepalive"]
        ApiClient["HTTP REST Client (Static char[] Buffer / Stream)"]
        NtpWorker["NTP Time Sync (Async Background)"]
        ModeCheck["Check Operation Mode (Polling / WS)"]
        HeartbeatWorker["Heartbeat + Heap Health Monitor"]
    end

    subgraph ServerBackend ["BACKEND SERVER (FastAPI)"]
        FastAPI["FastAPI REST & WebSocket Server"]
        SQLite[("SQLite Database (WAL Mode)")]
    end

    %% Wiring Core 1
    Wiegand -->|Tap Terdeteksi| LocalDB
    Button -->|Tombol Ditekan| Relay
    LocalDB -->|Akses Diterima| Relay
    LocalDB -->|Kirim Event| TapQueue

    %% Wiring Queues
    TapQueue -->|Dequeue Event| ApiClient
    SyncQueue -->|Update Whitelist| LocalDB
    SharedState <---> LocalDB

    %% Wiring Core 0 to Server
    ApiClient <--->|HTTP REST / WebSocket| FastAPI
    WiFiMgr -.->|Status Jaringan| SharedState
    FastAPI <---> SQLite
```

### Keunggulan Arsitektur Ini:
* **Zero Latency pada Hardware:** Jika server mati, kabel LAN dicabut, atau WiFi error, respon buka pintu kartu RFID & Push Button tetap instan (< 5 ms).
* **Isolasi Kegagalan (*Fault Isolation*):** Crash atau timeout pada jaringan di Core 0 tidak akan mematikan Core 1.

---

## 3. Daftar Prioritas Migrasi

Perombakan dibagi menjadi 3 tahap prioritas terstruktur:

| Prioritas | Komponen | Estimasi Dampak |
|---|---|---|
| **P1 - Kritis** | • Fix `millis()` Rollover di `relay_control.cpp`<br>• Refactor buffer statis di `api_client.cpp`<br>• Implementasi Dual-Core FreeRTOS & Queue di `test-dev.ino`<br>• Pasang Hardware Watchdog Timer (WDT) | Menghilangkan 100% resiko sistem beku (*freeze*), crash OOM, dan bug hari ke-49,7. |
| **P2 - Penting** | • Non-blocking WiFi Reconnect di `wifi_manager.cpp`<br>• Kirim metrik `getFreeHeap()` & `getMaxAllocHeap()` di Heartbeat | Menjamin pemulihan koneksi mandiri tanpa jeda dan memberikan visibilitas RAM. |
| **P3 - Opsional** | • Aktifkan SQLite WAL Mode di `server/database.py`<br>• Tangkap metrik RAM di `server/models.py` | Optimasi performa database server saat banyak tap bersamaan. |

---

## 4. Rincian Rencana Perubahan Tiap File

### 4.1. `relay_control.h` & `relay_control.cpp`
* **Masalah:** Menggunakan `millis() > _unlockUntil` dan `millis() < _ignoreUntil`.
* **Solusi Perubahan:**
  * Simpan waktu mulai (`_unlockStartMs`, `_ignoreStartMs`) dan durasi (`_unlockDurationMs`, `_ignoreDurationMs`).
  * Gunakan rumus *safe unsigned subtraction*:
    ```cpp
    bool relayIsUnlocked() {
      if (!_isUnlocked) return false;
      if (millis() - _unlockStartMs >= _unlockDurationMs) {
        relayLock();
        return false;
      }
      return true;
    }
    ```
  * Perbaiki fungsi `relayIsIgnoring()` dengan pola yang sama.

### 4.2. `api_client.h` & `api_client.cpp`
* **Masalah:** Menggunakan objek `String` untuk merakit dan membaca respons TCP karakter demi karakter.
* **Solusi Perubahan:**
  * Buang loop `while (client.available()) rawResponse += (char)client.read();`.
  * Gunakan buffer tetap `char httpBuffer[1024]` dengan `client.readBytes()`.
  * Untuk payload JSON (seperti Whitelist): gunakan *Stream Deserialization* `deserializeJson(doc, client)` langsung dari stream socket tanpa alokasi memori ganda.
  * Tetapkan timeout koneksi TCP pendek (`CONNECT_TIMEOUT_MS = 1500`).

### 4.3. `wifi_manager.h` & `wifi_manager.cpp`
* **Masalah:** Loop `while` blocking dengan `delay(500)` saat koneksi gagal.
* **Solusi Perubahan:**
  * Ubah inisialisasi koneksi menjadi asinkron (`WiFi.begin()` tanpa blocking loop).
  * Di dalam task FreeRTOS, gunakan `vTaskDelay(pdMS_TO_TICKS(500))` sebagai pengganti `delay(500)`.
  * Buat flag status koneksi yang thread-safe.

### 4.4. `test-dev.ino`
* **Masalah:** Semua alur (RFID, Tombol, Relay, WiFi, Polling HTTP) bertumpuk dalam satu `loop()` Arduino.
* **Solusi Perubahan:**
  * Buat struktur data `TapEvent` untuk antrean FreeRTOS.
  * Inisialisasi antrean: `QueueHandle_t xTapQueue = xQueueCreate(10, sizeof(TapEvent));`.
  * Buat task FreeRTOS di Core 0:
    ```cpp
    xTaskCreatePinnedToCore(networkTask, "NetWorker", 8192, NULL, 1, &netTaskHandle, 0);
    ```
  * Konfigurasikan Task Watchdog Timer (WDT) dengan batas 8 detik via ESP-IDF API:
    ```cpp
    esp_task_wdt_init(8, true);
    esp_task_wdt_add(NULL);
    ```
  * `loop()` (berjalan di Core 1) hanya berisi:
    1. `esp_task_wdt_reset();`
    2. `relayUpdate();`
    3. `checkPushButton();`
    4. `wiegandCheckCard();` (dan mengirim ke Queue jika ada tap).

### 4.5. `config.h`
* Tambahkan definisi pendukung:
  * `NETWORK_TASK_STACK_SIZE = 8192`
  * `QUEUE_SIZE = 10`
  * `WDT_TIMEOUT_SEC = 8`
  * Interval penyesuaian heartbeat dan mode check.

---

## 5. Skenario Pengujian & Kriteria Kelayakan (Testing & QA)

Setelah refactoring selesai, serangkaian pengujian wajib dilakukan:

1. **Uji Server Mati (Offline Stress Test):**
   * *Aksi:* Matikan server FastAPI / matikan laptop.
   * *Kriteria Lolos:* Push Button dan tap kartu RFID lokal tetap merespons seketika (< 10 ms) membuka dropbolt tanpa ada jeda atau freeze.
2. **Uji WiFi Putus-Nyambung (Network Resilience Test):**
   * *Aksi:* Matikan Access Point WiFi selama 1 menit, lalu nyalakan kembali.
   * *Kriteria Lolos:* ESP32 otomatis tersambung kembali (*reconnected*) dan data tap offline yang tertampung di antrean dikirim ke server.
3. **Uji Ketahanan Memori (Heap Stability Test):**
   * *Aksi:* Nyalakan perangkat selama 24–48 jam dengan ribuan tap dan polling.
   * *Kriteria Lolos:* Nilai `ESP.getMaxAllocHeap()` tidak mengalami degradasi bertahap (tidak ada memory leak).
4. **Uji Simulasi Watchdog (WDT Test):**
   * *Aksi:* Buat simulasi infinite loop buatan di `loop()`.
   * *Kriteria Lolos:* ESP32 mendeteksi hang dan me-reboot dirinya sendiri dalam 8 detik.

---

## 7. Checklist Progress Eksekusi

### Fase 1: Perbaikan Firmware ESP32
- [x] **1.1.** Revisi `relay_control.cpp` & `relay_control.h` (Fix Rollover Timer - Selesai).
- [x] **1.2.** Revisi `api_client.cpp` & `api_client.h` (Static Buffer & Stream Parsing - Selesai).
- [x] **1.3.** Revisi `wifi_manager.cpp` & `wifi_manager.h` (Non-blocking Reconnect - Selesai).
- [x] **1.4.** Implementasi FreeRTOS Task (Core 0) & Queue di `test-dev.ino` (Selesai).
- [x] **1.5.** Konfigurasi Hardware Watchdog Timer (WDT) 8 detik di `test-dev.ino` (Selesai).
- [x] **1.6.** Offline Mode Persistence (NVS Flash Storage via `Preferences.h` - Selesai).
- [x] **1.7.** Offline Queue Retention & Instant Reconnect Sync (Selesai).
- [ ] **1.8.** Compile & Verifikasi di Arduino IDE / CLI.

### Fase 2: Pengujian & Validasi
- [ ] **2.1.** Uji coba respon kartu saat server offline.
- [ ] **2.2.** Monitoring stabilitas heap RAM melalui Serial / Heartbeat.
