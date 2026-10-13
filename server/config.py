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

# ============================================
#  Auth & Security
# ============================================
SECRET_KEY = os.getenv("SECRET_KEY", "super-secret-key-door-access-12345")
ALGORITHM = "HS256"
ACCESS_TOKEN_EXPIRE_MINUTES = int(os.getenv("TOKEN_EXPIRE_MINUTES", "480")) # 8 jam

# Default Owner (Superadmin)
DEFAULT_OWNER_USER = os.getenv("OWNER_USER", "superadmin")
DEFAULT_OWNER_PASS = os.getenv("OWNER_PASS", "owner123")
