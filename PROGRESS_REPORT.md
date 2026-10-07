# 📈 Laporan Progres Proyek: IoT RFID Door Access Control

**Tanggal Laporan:** 23 September 2026
**Status Proyek:** Sedang Berjalan (Tahap Backend & Infrastruktur)

Berikut adalah ringkasan dari semua pekerjaan yang telah diselesaikan sejauh ini:

---

## 1. 📄 Dokumentasi & Perencanaan (Selesai)
Telah dibuat tiga dokumen utama untuk memandu pengembangan proyek:
- **`SKEMA_PROJECT.md`**: Mencatat skema hardware, wiring diagram (ESP32, MFRC522, Relay), alur kerja awal, dan catatan komponen.
- **`ARSITEKTUR_SISTEM.md`**: Menentukan arsitektur sistem secara keseluruhan (FastAPI + SQLite lokal), alasan pemilihan tech stack, struktur database, dan roadmap pengembangan.
- **`PRD.md` (Product Requirements Document)**: Menjabarkan secara detail fitur-fitur yang dibutuhkan (Admin panel, role, logika jadwal, API endpoint, alur komunikasi online/offline).

---

## 2. 🔧 Firmware ESP32 (Selesai)
Kode purwarupa Arduino awal telah di-refactor secara total menjadi arsitektur modular yang rapi di dalam folder `test_dropbolt_rfid/`:
- **`config.h`**: File pusat untuk mengatur PIN, SSID WiFi, IP Server, dan waktu delay.
- **`wifi_manager.h/.cpp`**: Menangani koneksi WiFi, auto-reconnect, dan sinkronisasi waktu via NTP.
- **`rfid_handler.h/.cpp`**: Menangani inisialisasi reader MFRC522 dan pembacaan UID kartu.
- **`relay_control.h/.cpp`**: Menangani kontrol dropbolt (Lock/Unlock) dengan dukungan active-LOW.
- **`access_control.h/.cpp`**: Logika utama untuk menyimpan whitelist secara lokal di RAM ESP32 dan melakukan validasi secara mandiri jika server sedang offline (termasuk validasi jadwal).
- **`api_client.h/.cpp`**: Menangani semua komunikasi HTTP ke server (Ambil whitelist, kirim log tap, kirim heartbeat, update status pintu).
- **`test_dropbolt_rfid.ino`**: File utama (orchestrator) yang menghubungkan semua modul dengan logika *Online Validation* (via server) dan fallback ke *Offline Validation* (lokal).

---

## 3. 🖥️ Backend Server (Python FastAPI) (Selesai 90%)
Struktur backend telah dibangun di dalam folder `server/` menggunakan FastAPI dan aiosqlite (async SQLite):
- **`requirements.txt`**: Daftar dependensi Python (FastAPI, uvicorn, aiosqlite, pydantic).
- **`config.py`**: Konfigurasi environment variables untuk server.
- **`database.py`**: Skema database SQLite otomatis (`cards`, `access_logs`, `schedules`, `doors`) dengan index untuk performa.
- **`models.py`**: Skema Pydantic untuk validasi data Request dan Response API.
- **Routers (Endpoints):**
  - **`routers/cards.py`**: CRUD untuk manajemen kartu RFID.
  - **`routers/logs.py`**: Endpoint untuk melihat history tap kartu, filter, dan export CSV.
  - **`routers/schedules.py`**: CRUD untuk pengaturan jadwal akses berdasarkan *Access Level*.
  - **`routers/doors.py`**: Endpoint untuk melihat status pintu dan fitur *remote unlock*.
  - **`routers/esp.py`**: Endpoint khusus untuk komunikasi ESP32 (Handle Tap, Heartbeat, Door Status, Whitelist Sync).
- **`main.py`**: File utama server FastAPI, mengintegrasikan semua router, menyediakan WebSocket manager untuk update *real-time* ke dashboard, dan melayani file statis frontend.

---

## 4. ⏳ Tugas yang Tertunda (Pending / Next Steps)
Beberapa task terakhir dihentikan karena keterbatasan izin tulis (permission denied). Berikut adalah apa yang harus dilanjutkan:
1. **Frontend / Web Dashboard**: Membuat file HTML, CSS, JS (`index.html`, `cards.html`, `logs.html`, `schedule.html`) di dalam folder `server/static/`.
2. **Dockerization**:
   - Membuat `Dockerfile` untuk backend FastAPI.
   - Membuat `docker-compose.yml` di root proyek untuk menjalankan server + database dengan mudah.
   - Membuat `.dockerignore`.

---

