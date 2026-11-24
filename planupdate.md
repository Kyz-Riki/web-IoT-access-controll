# 📋 PLANNING UPDATE — Sistem Access Control IoT

**Tanggal**: 24 November 2026  
**Versi**: v1.1.0 — Perbaikan Bug Aplikasi + Arsitektur Validasi ESP32  
**Status**: 🟢 Selesai Diimplementasikan (24 Nov 2026)

---

## DAFTAR ISI

1. [Ringkasan Masalah](#1-ringkasan-masalah)
2. [UPDATE A — Bug Fix Aplikasi Server (Frontend + Backend)](#2-update-a--bug-fix-aplikasi-server)
3. [UPDATE B — Arsitektur Validasi ESP32 Online → Realtime Server](#3-update-b--arsitektur-validasi-esp32-online--realtime-server)
4. [Prioritas & Urutan Pengerjaan](#4-prioritas--urutan-pengerjaan)
5. [Checklist Pengujian](#5-checklist-pengujian)

---

## 1. Ringkasan Masalah

### Aplikasi Server (Bug)
| # | Masalah | Lokasi | Severity |
|---|---------|--------|----------|
| A1 | Schedule levels tidak muncul di datalist halaman Cards saat tambah/edit kartu | `cards.html` | 🔴 High |
| A2 | Card levels tidak muncul di datalist halaman Schedule saat tambah jadwal baru | `schedule.html` | 🔴 High |
| A3 | `_check_schedule()` tidak pakai timezone WIB — bisa salah 7 jam | `routers/esp.py` | 🔴 Critical |
| A4 | Tombol "Remote Unlock" di Dashboard tidak benar-benar kirim perintah ke ESP32 | `routers/doors.py` + `index.html` | 🟡 Medium |
| A5 | Export CSV tidak pakai auth header di URL (browser redirect) | `logs.html` | 🟡 Medium |
| A6 | Dashboard `/api/dashboard/stats` tidak pakai auth guard di backend | `main.py` | 🟡 Medium |
| A7 | Role `viewer` bisa POST/PUT/DELETE di `cards.py` — role check harusnya `['owner','admin']` saja | `routers/cards.py` | 🔴 High |
| A8 | Halaman schedule tidak realtime update saat ada perubahan dari halaman lain | `schedule.html` | 🟢 Low |

### Arsitektur ESP32 (Desain)
| # | Masalah | Lokasi | Severity |
|---|---------|--------|----------|
| B1 | ESP32 selalu validasi lokal walau online — tidak realtime terhadap database server | `main_dev.ino` | 🔴 Critical |
| B2 | Fungsi `apiSendTap()` sudah ada tapi tidak pernah dipanggil | `api_client.cpp` | 🔴 High |
| B3 | Whitelist sync delay 30 detik — perubahan tidak instan | `config.h` | 🟡 Medium |

---

## 2. UPDATE A — Bug Fix Aplikasi Server

### A1 — Schedule Levels Tidak Muncul di Halaman Cards

**File**: `server/static/cards.html`  
**Masalah**: Fungsi `getUniqueLevels()` hanya ambil level dari kartu yang sudah ada, bukan dari schedules yang sudah dibuat. Akibatnya saat user menambah kartu baru atau edit kartu, level yang sudah dijadwalkan (misal "Karyawan", "OB") tidak muncul di dropdown/datalist.

**Solusi**:
```javascript
// Tambah property baru di Alpine data
scheduleLevels: [],

// Di init(), tambahkan fetch schedule levels
async init() {
    this.fetchCards();
    this.fetchCount();
    this.fetchScheduleLevels();   // ← TAMBAH
    this.connectWebSocket();
},

// Method baru: ambil semua level dari schedules
async fetchScheduleLevels() {
    try {
        const res = await fetch('/api/schedules', { headers: this.getHeaders() });
        if (res.ok) {
            const schedules = await res.json();
            this.scheduleLevels = [...new Set(schedules.map(s => s.access_level))];
        }
    } catch (e) { console.error(e); }
},

// Ubah getUniqueLevels() — gabungkan dari cards + schedules
getUniqueLevels() {
    const levels = new Set([
        ...this.cards.map(c => c.access_level),
        ...this.scheduleLevels           // ← TAMBAH
    ]);
    levels.add('admin');
    return Array.from(levels).sort();
}
```

**Dampak**: Datalist di form register kartu dan edit kartu akan menampilkan semua level yang sudah dibuatkan jadwal.

---

### A2 — Card Levels Tidak Muncul di Halaman Schedule

**File**: `server/static/schedule.html`  
**Masalah**: Sama seperti A1 tapi kebalikan. Saat membuat schedule baru, level kartu yang sudah didaftarkan tidak muncul di datalist/filter.

**Solusi**:
```javascript
// Tambah property baru
cardLevels: [],

// Di init(), tambahkan fetch card levels
async init() {
    this.fetchSchedules();
    this.fetchCardLevels();   // ← TAMBAH
},

// Method baru
async fetchCardLevels() {
    try {
        const res = await fetch('/api/cards', { headers: this.getHeaders() });
        if (res.ok) {
            const cards = await res.json();
            this.cardLevels = [...new Set(cards.map(c => c.access_level))];
        }
    } catch (e) { console.error(e); }
},

// Ubah getUniqueLevels()
getUniqueLevels() {
    const levels = new Set([
        ...this.schedules.map(s => s.access_level),
        ...this.cardLevels                // ← TAMBAH
    ]);
    return Array.from(levels).sort();
}
```

---

### A3 — Timezone Bug di `_check_schedule()`

**File**: `server/routers/esp.py` → fungsi `_check_schedule()` baris 327-350  
**Masalah**: Menggunakan `datetime.now()` yang pakai timezone lokal server. Jika server berjalan di environment non-WIB (misalnya cloud UTC), jadwal akan salah 7 jam.

**Solusi**:
```python
async def _check_schedule(db, access_level):
    """Cek apakah waktu sekarang ada dalam jadwal untuk access level tertentu."""
    from database import WIB                    # ← TAMBAH import
    now = datetime.now(WIB)                      # ← FIX: pakai WIB eksplisit
    current_day_map = {0: "mon", 1: "tue", 2: "wed", 3: "thu", 4: "fri", 5: "sat", 6: "sun"}
    current_day = current_day_map[now.weekday()]
    current_time = now.strftime("%H:%M")
    # ... sisanya tetap
```

---

### A4 — Remote Unlock Tidak Benar-Benar Kirim ke ESP32

**File**: `server/routers/doors.py` → endpoint `POST /{door_id}/unlock` baris 55-78  
**Masalah**: Tombol "Remote Unlock" di dashboard hanya update database `current_state = 'unlocked'`, tapi **tidak ada mekanisme push ke ESP32**. ESP32 tidak pernah tau bahwa pintu harus dibuka dari web.

**Solusi**: Butuh implementasi mekanisme push. 2 opsi:

**Opsi 1 — Polling dari ESP32 (Simpel)**:
- Tambah endpoint `GET /api/esp/pending-command?esp_id=xxx`
- ESP32 cek tiap 2 detik (bersamaan dengan mode check)
- Jika ada perintah `unlock`, ESP32 buka relay

**Opsi 2 — WebSocket Bidirectional (Ideal)**:
- ESP32 membuka koneksi WebSocket ke server
- Server bisa push perintah unlock langsung ke ESP yang spesifik

**Rekomendasi**: Opsi 1 lebih realistis untuk ESP32 (resource terbatas).

```python
# Server: Tambah di routers/esp.py
_pending_commands = {}  # { "esp-01": {"command": "unlock", "created": datetime} }

@router.get("/pending-command")
async def get_pending_command(esp_id: str = "esp-01"):
    cmd = _pending_commands.pop(esp_id, None)
    if cmd:
        return {"command": cmd["command"]}
    return {"command": "none"}

# Update remote_unlock di doors.py agar set pending command
@router.post("/{door_id}/unlock")
async def remote_unlock(door_id, db):
    # ... cek door ada & online ...
    door = ...
    esp.set_pending_command(door["esp_id"], "unlock")
    return {"message": "Perintah unlock dikirim"}
```

```cpp
// ESP32: Di networkTask(), tambahkan pengecekan command
// bersamaan dengan mode check setiap 2 detik
if (online && (millis() - lastCommandCheck >= 2000)) {
    lastCommandCheck = millis();
    char cmd[16] = {0};
    if (apiCheckPendingCommand(cmd, sizeof(cmd))) {
        if (strcmp(cmd, "unlock") == 0) {
            relayUnlockTimed(UNLOCK_MS);
        }
    }
}
```

---

### A5 — Export CSV Tidak Pakai Auth

**File**: `server/static/logs.html` baris 210-215  
**Masalah**: Export CSV menggunakan `window.location.href` (browser redirect) yang tidak menyertakan JWT token. Server akan tolak dengan 401. Meskipun di backend ada `require_role`, flow ini bermasalah karena browser redirect tidak kirim Authorization header.

**Solusi**: Ubah ke fetch + blob download:
```javascript
async exportCSV() {
    let q = '?export=1';
    if (this.dateFrom) q += `&date_from=${this.dateFrom}`;
    if (this.dateTo) q += `&date_to=${this.dateTo}`;
    
    try {
        const res = await fetch(`/api/logs/export${q}`, { headers: this.getHeaders() });
        if (!res.ok) { alert('Gagal export'); return; }
        const blob = await res.blob();
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = 'access_logs.csv';
        a.click();
        URL.revokeObjectURL(url);
    } catch (e) { console.error(e); }
}
```

---

### A6 — Dashboard Stats Tidak Pakai Auth Guard

**File**: `server/main.py` → endpoint `/api/dashboard/stats` baris 109-201  
**Masalah**: Endpoint ini tidak ada `Depends(require_role(...))`. Siapa saja yang tahu URL-nya bisa akses data statistik tanpa login.

**Solusi**:
```python
from auth import require_role

@app.get("/api/dashboard/stats", tags=["Dashboard"])
async def get_dashboard_stats(
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin', 'viewer']))  # ← TAMBAH
):
```

---

### A7 — Role `viewer` Bisa CRUD Kartu

**File**: `server/routers/cards.py`  
**Masalah**: Semua endpoint (POST, PUT, DELETE) menggunakan `require_role(['owner', 'admin', 'viewer'])`. Artinya role `viewer` bisa menambah, mengedit, dan menghapus kartu. Viewer seharusnya **hanya bisa melihat** (read-only).

**Solusi**:
```python
# GET endpoints — viewer boleh lihat
@router.get("", ...)
async def list_cards(..., current_user = Depends(require_role(['owner', 'admin', 'viewer']))):

@router.get("/count")
async def count_cards(..., current_user = Depends(require_role(['owner', 'admin', 'viewer']))):

# POST, PUT, DELETE — viewer TIDAK boleh
@router.post("", ...)
async def create_card(..., current_user = Depends(require_role(['owner', 'admin']))):  # ← HAPUS 'viewer'

@router.put("/{card_id}", ...)
async def update_card(..., current_user = Depends(require_role(['owner', 'admin']))):  # ← HAPUS 'viewer'

@router.delete("/{card_id}")
async def delete_card(..., current_user = Depends(require_role(['owner', 'admin']))):  # ← HAPUS 'viewer'
```

---

### A8 — Halaman Schedule Tidak Realtime

**File**: `server/static/schedule.html`  
**Masalah**: Halaman schedule tidak terhubung ke WebSocket. Jika ada perubahan jadwal dari tab lain atau user lain, data tidak otomatis ter-refresh.

**Solusi** (prioritas rendah): Tambahkan koneksi WebSocket dengan event listener untuk refresh data saat ada broadcast perubahan. Atau minimal tambahkan auto-refresh interval.

```javascript
// Di init(), tambahkan periodic refresh
init() {
    this.fetchSchedules();
    this.fetchCardLevels();
    // Auto-refresh setiap 30 detik
    setInterval(() => this.fetchSchedules(), 30000);
},
```

---

## 3. UPDATE B — Arsitektur Validasi ESP32 Online → Realtime Server

### B1 — Ubah `handleCardTap()` ke Server-First saat Online

**File**: `code esp32/kode-esp32s3-dev/main_dev.ino` → fungsi `handleCardTap()`  
**Masalah Sekarang**: ESP32 **selalu** validasi secara lokal terhadap whitelist di RAM, bahkan ketika koneksi ke server tersedia. Fungsi `apiSendTap()` yang sudah disiapkan untuk validasi realtime ke server **tidak pernah dipanggil**.

**Arsitektur Baru**:
```
Kartu di-tap → Cek serverAvailable?
    ├── YA (Online) → POST /api/esp/tap ke Server
    │                  Server validasi realtime dari database
    │                  Server respond: grant/deny
    │                  ESP32 buka/tolak berdasarkan respons server
    │
    └── TIDAK (Offline) → Validasi LOKAL terhadap whitelist di RAM
                           Kirim log ke antrean untuk dikirim nanti
```

**Perubahan di `handleCardTap()`**:
```cpp
void handleCardTap(const char* uid, uint8_t bits) {
    Serial.println();
    Serial.println("─────────────────────────────────────────────");
    Serial.printf("[Core 1] 🪪 Kartu terdeteksi: %s (%u bit)\n", uid, bits);

    // Mode testing
    if (ALLOW_ANY_CARD) {
        Serial.println("[Core 1] 🔓 [MODE TES] Kartu diterima langsung!");
        feedbackGranted();
        relayUnlockTimed(UNLOCK_MS);
        return;
    }

    // Mode registrasi
    if (registerMode) {
        Serial.println("[Core 1] 📋 Mode Registrasi — kirim UID ke server...");
        feedbackGranted();
        TapEvent ev;
        ev.type = EVENT_REGISTER_SCAN;
        strncpy(ev.uid, uid, UID_MAX_LENGTH);
        ev.uid[UID_MAX_LENGTH] = '\0';
        if (xTapQueue) xQueueSend(xTapQueue, &ev, 0);
        return;
    }

    // ══════════════════════════════════════════════
    //  VALIDASI BARU: Online → Server, Offline → Lokal
    // ══════════════════════════════════════════════

    if (serverAvailable) {
        // ── ONLINE: Validasi realtime ke server ──
        Serial.println("[Core 1] 🌐 Online — validasi ke server...");
        
        TapResponse resp = apiSendTap(uid);
        
        if (resp.success) {
            if (strcmp(resp.action, "grant") == 0) {
                Serial.printf("[Core 1] ✅ SERVER GRANTED (%s)\n", resp.cardHolder);
                feedbackGranted();
                relayUnlockTimed(UNLOCK_MS);
            } else {
                Serial.printf("[Core 1] ❌ SERVER DENIED: %s\n", resp.reason);
                feedbackDenied();
            }
            // Server sudah simpan log → tidak perlu kirim log lagi
        } else {
            // Server gagal merespons → fallback ke validasi lokal
            Serial.println("[Core 1] ⚠️ Server tidak merespons, fallback ke lokal...");
            serverAvailable = false;
            goto local_validation;
        }
        
    } else {
local_validation:
        // ── OFFLINE: Validasi lokal ──
        Serial.println("[Core 1] 📴 Offline — validasi lokal...");

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

        // Kirim log ke antrean untuk dikirim ke server nanti
        TapEvent ev;
        ev.type = EVENT_TAP_LOG;
        strncpy(ev.uid, uid, UID_MAX_LENGTH);
        ev.uid[UID_MAX_LENGTH] = '\0';
        strncpy(ev.status, granted ? "granted" : "denied", sizeof(ev.status) - 1);
        strncpy(ev.reason, accessResultToReason(result), sizeof(ev.reason) - 1);
        strncpy(ev.accessLevel, accessLevel, sizeof(ev.accessLevel) - 1);

        if (xTapQueue) {
            xQueueSend(xTapQueue, &ev, 0);
        }
    }

    Serial.println("─────────────────────────────────────────────");
}
```

### ⚠️ Catatan Penting tentang Latency

`apiSendTap()` berjalan di **Core 1 (loop utama)**. HTTP request bisa memakan waktu **50-500ms**. Selama menunggu respons server:
- Wiegand reader tetap mendengarkan via interrupt (ISR) — tidak terpengaruh
- Relay tidak bisa di-update — tapi ini hanya 0.5 detik
- Watchdog harus cukup toleran (saat ini 15 detik — aman)

**Alternatif yang lebih elegan**: Pindahkan `apiSendTap()` ke Core 0 (network task) via queue, tapi ini memerlukan mekanisme sinkronisasi response dari Core 0 ke Core 1 (semaphore/event group). Lebih kompleks tapi Core 1 tetap responsif. Untuk tahap awal, langsung di Core 1 sudah cukup karena timeout HTTP sudah dibatasi 3 detik.

---

### B2 — Update `apiSendTap()` agar tidak kirim `validated_locally`

**File**: `code esp32/kode-esp32s3-dev/api_client.cpp`  
**Masalah**: Saat ini fungsi ini sudah benar — tidak set `validated_locally`. Yang perlu dipastikan hanya bahwa fungsi ini memang dipanggil (sudah dihandle di B1).

Fungsi `apiSendTap()` di baris 120-166 sudah mengirim tap ke server **tanpa** flag `validated_locally`, sehingga server akan melakukan validasi penuh (cek DB, cek aktif, cek schedule). ✅ Tidak perlu diubah.

---

### B3 — Whitelist Sync Tetap Dipertahankan untuk Fallback Offline

**File**: `config.h`  
**Keputusan**: Whitelist sync setiap 30 detik **tetap dipertahankan**. Ini penting sebagai backup data offline. Jika server tiba-tiba mati, ESP32 masih punya data terakhir untuk validasi lokal.

Tidak ada perubahan kode, hanya konfirmasi bahwa mekanisme ini tetap berjalan.

---

## 4. Prioritas & Urutan Pengerjaan

### 🔴 Fase 1 — Critical Fix (Harus Duluan)

| # | Task | File yang diubah | Estimasi |
|---|------|-----------------|----------|
| 1 | A3: Fix timezone `_check_schedule()` | `routers/esp.py` | 5 menit |
| 2 | A7: Fix role viewer bisa CRUD kartu | `routers/cards.py` | 5 menit |
| 3 | B1: Ubah `handleCardTap()` ke server-first | `main_dev.ino` | 30 menit |
| 4 | A1: Schedule levels muncul di halaman Cards | `cards.html` | 15 menit |
| 5 | A2: Card levels muncul di halaman Schedule | `schedule.html` | 15 menit |

### 🟡 Fase 2 — Important Fix

| # | Task | File yang diubah | Estimasi |
|---|------|-----------------|----------|
| 6 | A6: Tambah auth guard di dashboard stats | `main.py` | 5 menit |
| 7 | A5: Fix export CSV pakai fetch+blob | `logs.html` | 15 menit |
| 8 | A4: Implementasi Remote Unlock via polling | `esp.py` + `doors.py` + ESP32 | 1-2 jam |

### 🟢 Fase 3 — Enhancement

| # | Task | File yang diubah | Estimasi |
|---|------|-----------------|----------|
| 9 | A8: Auto-refresh schedule page | `schedule.html` | 10 menit |
| 10 | B1-alt: Pindah validasi server ke Core 0 | `main_dev.ino` | 2-3 jam |

---

## 5. Checklist Pengujian

### Test Validasi Realtime (Setelah Update B1)

- [ ] **Test 1**: ESP32 online → tap kartu terdaftar dalam jadwal → harus **GRANTED**
- [ ] **Test 2**: ESP32 online → tap kartu terdaftar di luar jadwal → harus **DENIED (out_of_schedule)**
- [ ] **Test 3**: ESP32 online → tap kartu tidak terdaftar → harus **DENIED (unregistered)**
- [ ] **Test 4**: ESP32 online → tap kartu yang diblokir → harus **DENIED (blocked)**
- [ ] **Test 5**: ESP32 online → tap kartu admin → harus **GRANTED** (bypass jadwal)
- [ ] **Test 6**: Blokir kartu di dashboard → tap **langsung** di-deny (tanpa tunggu 30 detik)
- [ ] **Test 7**: Tambah kartu baru di dashboard → tap **langsung** di-grant (tanpa tunggu 30 detik)
- [ ] **Test 8**: Cabut kabel jaringan → tap kartu → harus **fallback ke validasi lokal**
- [ ] **Test 9**: Server mati → tap kartu → harus pakai data **NVS Flash terakhir**

### Test Schedule di Aplikasi (Setelah Update A1 + A2)

- [ ] **Test 10**: Buat schedule "Karyawan" di halaman Schedule → buka halaman Cards → register kartu baru → level "Karyawan" harus muncul di datalist
- [ ] **Test 11**: Edit kartu yang sudah ada → level dari schedule harus muncul di datalist
- [ ] **Test 12**: Buat kartu dengan level baru "Security" → buka halaman Schedule → tambah jadwal → level "Security" harus muncul di datalist
- [ ] **Test 13**: Pastikan admin tidak bisa dibuatkan schedule (server harus tolak)

### Test Timezone (Setelah Update A3)

- [ ] **Test 14**: Jalankan server di environment UTC → schedule jam 07:00-17:00 WIB → tap kartu jam 10:00 WIB → harus **GRANTED**
- [ ] **Test 15**: Jalankan server di environment UTC → schedule jam 07:00-17:00 WIB → tap kartu jam 03:00 WIB → harus **DENIED**

### Test Security (Setelah Update A6 + A7)

- [ ] **Test 16**: Login sebagai viewer → coba POST `/api/cards` → harus **403 Forbidden**
- [ ] **Test 17**: Login sebagai viewer → coba DELETE `/api/cards/1` → harus **403 Forbidden**
- [ ] **Test 18**: Tanpa login → GET `/api/dashboard/stats` → harus **401 Unauthorized**

### Test Remote Unlock (Setelah Update A4)

- [ ] **Test 19**: Klik "Remote Unlock" di dashboard → ESP32 harus buka relay
- [ ] **Test 20**: ESP32 offline → tombol harus disabled
- [ ] **Test 21**: Remote unlock → log tercatat dengan reason "remote_unlock"

---

> **Catatan**: Seluruh update di dokumen ini sudah dianalisis berdasarkan source code aktual. Setiap item memiliki referensi file dan baris kode yang spesifik. Implementasi bisa langsung dikerjakan sesuai urutan prioritas.
