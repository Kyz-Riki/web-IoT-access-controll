# 📈 Laporan Progres Proyek: IoT RFID Door Access Control

**Tanggal Laporan:** 12 Oktober 2026  
**Status Proyek:** Siap Digunakan (Tahap Firmware, Backend, Frontend & Sistem Keamanan Selesai 100%)

Dokumen ini mencatat ringkasan progres pengerjaan sistem kontrol akses pintu berbasis IoT RFID hingga iterasi terbaru.

---

## 1. 📄 Dokumentasi & Blueprint Sistem (Selesai)

Dokumentasi proyek telah diperbarui secara menyeluruh agar selaras dengan arsitektur terkini:
- **`SKEMA_PROJECT.md`**: Skema rangkaian perangkat keras, diagram wiring ESP32, modul RFID MFRC522, relay dropbolt active-LOW, dan alur firmware.
- **`ARSITEKTUR_SISTEM.md`**: Arsitektur lengkap sistem terdistribusi, skema database 5 tabel, diagram alur online/offline, spesifikasi lengkap REST API & WebSocket, serta hierarki peran pengguna.
- **`PRD.md`**: Dokumen persyaratan produk mencakup seluruh kebutuhan fungsional dan non-fungsional sistem kontrol akses pintu.
- **`migrasi-auth-rbac.md`**: Blueprint migrasi autentikasi JWT dan Role-Based Access Control (RBAC).
- **`migrasi-postgresql.md`**: Blueprint perencanaan migrasi database dari SQLite ke PostgreSQL menggunakan Docker.

---

## 2. 🔧 Firmware ESP32 (Selesai)

Kode firmware mikrokontroler telah diorganisasi secara modular di dalam folder **`code esp32/`**:
- **`config.h`**: Konfigurasi terpusat (Pin GPIO, kredensial WiFi, IP server, interval delay).
- **`wifi_manager.h/.cpp`**: Menangani koneksi WiFi, mekanisme auto-reconnect, dan sinkronisasi waktu jam presisi via NTP.
- **`rfid_handler.h/.cpp`**: Driver inisialisasi modul MFRC522 via bus SPI dan pembacaan UID kartu.
- **`relay_control.h/.cpp`**: Kontrol solenoid dropbolt dengan durasi unlock 2 detik (mendukung active-LOW).
- **`access_control.h/.cpp`**: Penyimpan cache whitelist di RAM lokal ESP32 untuk mendukung **validasi offline** otomatis apabila koneksi jaringan ke server terputus.
- **`api_client.h/.cpp`**: Klien HTTP REST terintegrasi untuk sinkronisasi whitelist, pelaporan event tap kartu, pelaporan status pintu, dan pengiriman heartbeat berkala.
- **`test_dropbolt_rfid.ino`**: File program utama penghubung seluruh modul.

---

## 3. 🖥️ Backend Server (FastAPI + Async SQLite) (Selesai)

Layanan backend dibangun menggunakan framework Python **FastAPI** dengan database **SQLite** (`aiosqlite`) di dalam folder **`server/`**:
- **`config.py`**: Konfigurasi variabel lingkungan, timezone Jakarta (WIB), parameter heartbeat, dan kunci enkripsi JWT.
- **`database.py`**: Pembuatan skema database otomatis 5 tabel (`cards`, `schedules`, `doors`, `access_logs`, `users`) beserta pembuatan otomatis (*auto-seed*) akun awal Super Administrator.
- **`models.py`**: Validasi skema Request dan Response menggunakan Pydantic v2.
- **Pembersihan Log Bawaan**: Implementasi `FaviconLogFilter` dan endpoint `/favicon.ico` (204 No Content) pada `main.py` sehingga log terminal terbebas dari pesan error 404 browser.
- **WebSocket Manager**: Penyedia saluran WebSocket `/ws/live` untuk menyiarkan log ketukan kartu dan heartbeat status pintu secara seketika (*real-time*) ke dashboard browser.

---

## 4. 🔐 Sistem Keamanan & Multi-Role RBAC (Selesai)

Sistem autentikasi dan otorisasi telah diimplementasikan penuh untuk melindungi seluruh endpoint sistem:
- **Core Autentikasi (`server/auth.py`)**:
  - Hashing password menggunakan library native **`bcrypt`** (kompatibel penuh dengan Python 3.12).
  - Pembuatan dan validasi token stateless **JWT** (`python-jose`) dengan masa berlaku 8 jam.
  - Dependency FastAPI `get_current_user` dan `require_role(allowed_roles)`.
- **Tiga Tingkat Hak Akses (RBAC)**:
  1. **👑 Owner**: Memiliki kontrol penuh terhadap sistem serta menjadi satu-satunya peran yang berhak mengelola akun web pengguna (menambah, mengubah, dan menghapus akun Admin/Viewer).
  2. **🛠️ Admin**: Memiliki hak operasional penuh untuk IoT (membuka pintu remote, menambah/mengedit kartu RFID, mengatur jadwal akses).
  3. **👁️ Viewer**: Hak akses read-only (hanya dapat melihat dashboard status dan riwayat log akses, tombol aksi kritis disembunyikan).
- **Seeding Akun Bawaan**:
  - Username: `superadmin`
  - Password: `owner123`
  - Peran: `owner`
- **Router Pengguna (`server/routers/users.py`)**: Endpoint CRUD khusus bagi Owner untuk mengelola akun dashboard.

---

## 5. 🌐 Web Dashboard & Antarmuka Pengguna (Selesai)

Antarmuka web modern berbasis **Tailwind CSS**, **Alpine.js**, dan **FontAwesome** di dalam folder **`server/static/`**:
- **Halaman Login (`login.html`)**:
  - Desain 2 kolom (*Split Card layout*):
    - Sisi Kiri: Menampilkan logo resmi `LOGO SEKUNDER.png`, nama brand **"Valvindo IoT"** dengan warna resmi `#002062`, dan deskripsi sistem.
    - Sisi Kanan: Form input username, password, penanganan pesan error, dan tombol login dengan tema warna brand.
- **Halaman Manajemen Akun (`users.html`)**:
  - Khusus untuk peran Owner.
  - Tabel daftar pengguna, badge peran berwarna, modal penambahan pengguna baru, dan tombol hapus akun.
- **Halaman Dashboard Utama (`index.html`)**:
  - Widget ringkasan statistik (Total tap, akses diterima, akses ditolak, kartu aktif).
  - Status pintu & hardware (Online/Offline, sinyal WiFi dBm, status lock).
  - Tombol **Remote Unlock** (dilindungi hak akses).
  - Tabel aktivitas live feed real-time via WebSocket.
- **Halaman Manajemen Kartu (`cards.html`)**:
  - Tabel kartu RFID terdaftar, pencarian, dan filter level akses.
  - Dukungan pendaftaran kartu baru via input manual atau *Register Scan Mode* langsung dari perangkat ESP32.
- **Halaman Jadwal Akses (`schedule.html`)**:
  - Konfigurasi aturan akses berdasarkan hari dan jam aktif untuk setiap level akses.
- **Halaman Log Akses (`logs.html`)**:
  - Tabel audit seluruh ketukan kartu dengan filter tanggal dan status.
  - Fitur ekspor riwayat akses ke format file CSV dengan header token otomatis.
- **Standardisasi Tampilan Global**:
  - **Font Open Sans**: Diterapkan secara seragam di seluruh halaman melalui Google Fonts.
  - **Auth Guard**: Script verifikasi token di setiap halaman HTML yang secara otomatis mengalihkan browser ke `/login` jika sesi tidak ditemukan.
  - **Reaktivitas Alpine.js (`x-show`)**: Menyembunyikan navigasi atau tombol aksi sensitif berdasarkan nilai `user_role` pada `localStorage`.
  - **Navigasi Sidebar Terpadu**: Dilengkapi tombol **Logout** yang membersihkan token dan sesi pengguna.

---

## 6. 🚀 Rencana Pengembangan Selanjutnya (Next Steps)

1. **Dockerisasi (Opsional/Tahap Lanjutan)**:
   - Membuat `Dockerfile` untuk layanan server FastAPI.
   - Membuat `docker-compose.yml` untuk mempermudah orkestrasi aplikasi.
2. **Evaluasi Migrasi Database ke PostgreSQL**:
   - Menjalankan rencana migrasi dari SQLite ke PostgreSQL (mengacu pada panduan `migrasi-postgresql.md`) apabila sistem ditargetkan melayani puluhan pintu atau ribuan transaksi tap harian secara konkuren.
3. **Uji Coba Lapangan (Field Testing)**:
   - Pengujian stabilitas koneksi ESP32 di jaringan WiFi aktual dengan dropbolt fisik dan catu daya 12V independen.
