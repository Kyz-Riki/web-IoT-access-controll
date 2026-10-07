import os

# ============================================
#  Konfigurasi Server
# ============================================

# Database
DATABASE_PATH = os.getenv("DATABASE_PATH", "data/door_access.db")

# Server
HOST = os.getenv("HOST", "0.0.0.0")
PORT = int(os.getenv("PORT", "8000"))

# ESP32
HEARTBEAT_TIMEOUT_SEC = int(os.getenv("HEARTBEAT_TIMEOUT_SEC", "60"))
# Jika ESP32 tidak kirim heartbeat dalam 60 detik, dianggap offline

# Timezone (untuk log)
TIMEZONE = os.getenv("TIMEZONE", "Asia/Jakarta")
