import aiosqlite
import os
from datetime import datetime, timezone, timedelta
from config import DATABASE_PATH

# ============================================
#  Timezone Helper — WIB (UTC+7)
# ============================================

WIB = timezone(timedelta(hours=7))

def now_local() -> str:
    """Dapatkan waktu sekarang dalam WIB, format ISO untuk SQLite."""
    return datetime.now(WIB).strftime("%Y-%m-%d %H:%M:%S")

# ============================================
#  Database Initialization
# ============================================

async def get_db() -> aiosqlite.Connection:
    """Dapatkan koneksi database. Digunakan sebagai FastAPI dependency."""
    db = await aiosqlite.connect(DATABASE_PATH)
    db.row_factory = aiosqlite.Row  # Agar bisa akses kolom by name
    try:
        yield db
    finally:
        await db.close()


async def init_db():
    """Buat tabel-tabel jika belum ada."""
    os.makedirs(os.path.dirname(DATABASE_PATH) or ".", exist_ok=True)

    async with aiosqlite.connect(DATABASE_PATH) as db:
        await db.executescript("""
            -- Daftar kartu RFID yang terdaftar
            CREATE TABLE IF NOT EXISTS cards (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                uid TEXT UNIQUE NOT NULL,
                card_holder TEXT NOT NULL,
                access_level TEXT NOT NULL DEFAULT 'level_1',
                is_active INTEGER NOT NULL DEFAULT 1,
                created_at DATETIME NOT NULL,
                updated_at DATETIME NOT NULL
            );

            -- Log setiap tap kartu
            CREATE TABLE IF NOT EXISTS access_logs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                uid TEXT NOT NULL,
                card_holder TEXT,
                access_level TEXT,
                status TEXT NOT NULL,
                denied_reason TEXT,
                door_name TEXT DEFAULT 'Pintu Utama',
                timestamp DATETIME NOT NULL
            );

            -- Rule jadwal akses per access level
            CREATE TABLE IF NOT EXISTS schedules (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                access_level TEXT NOT NULL,
                days TEXT NOT NULL,
                time_start TEXT NOT NULL,
                time_end TEXT NOT NULL,
                is_active INTEGER NOT NULL DEFAULT 1,
                created_at DATETIME NOT NULL,
                updated_at DATETIME NOT NULL
            );

            -- Data pintu & status ESP32
            CREATE TABLE IF NOT EXISTS doors (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT UNIQUE NOT NULL,
                esp_id TEXT UNIQUE,
                current_state TEXT NOT NULL DEFAULT 'locked',
                is_online INTEGER NOT NULL DEFAULT 0,
                wifi_rssi INTEGER,
                uptime INTEGER,
                last_heartbeat DATETIME,
                last_activity DATETIME
            );

            -- Index untuk query yang sering dipakai
            CREATE INDEX IF NOT EXISTS idx_access_logs_timestamp
                ON access_logs(timestamp DESC);
            CREATE INDEX IF NOT EXISTS idx_access_logs_uid
                ON access_logs(uid);
            CREATE INDEX IF NOT EXISTS idx_cards_uid
                ON cards(uid);
            CREATE INDEX IF NOT EXISTS idx_schedules_access_level
                ON schedules(access_level);
        """)
        await db.commit()

    print("[Database] ✅ Database siap:", DATABASE_PATH)

