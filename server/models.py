from pydantic import BaseModel, Field
from typing import Optional
from datetime import datetime

# ============================================
#  Card & Employee Models
# ============================================

class CardCreate(BaseModel):
    uid: Optional[str] = Field(None, description="UID kartu RFID (hex), boleh kosong jika tambah karyawan tanpa kartu dulu", examples=["A3 B2 C1 D0"])
    card_holder: Optional[str] = Field(None, description="Nama karyawan (backward-compatible alias)", examples=["Budi Santoso"])
    employee_name: Optional[str] = Field(None, description="Nama karyawan", examples=["Budi Santoso"])
    division: Optional[str] = Field(None, description="Divisi / departemen karyawan", examples=["Engineering"])
    access_level: str = Field(..., description="Level akses", examples=["admin", "Engineering", "Produksi"])

class CardUpdate(BaseModel):
    uid: Optional[str] = None
    card_holder: Optional[str] = None
    employee_name: Optional[str] = None
    division: Optional[str] = None
    access_level: Optional[str] = None
    is_active: Optional[bool] = None

class CardResponse(BaseModel):
    id: int
    uid: Optional[str] = None
    card_holder: str
    employee_name: str
    division: Optional[str] = None
    access_level: str
    is_active: bool
    created_at: str
    updated_at: str

class CardImportItem(BaseModel):
    employee_name: str = Field(..., description="Nama karyawan")
    division: Optional[str] = Field(None, description="Divisi karyawan")
    access_level: str = Field(..., description="Level akses / jadwal")

class CardImportCommitRequest(BaseModel):
    items: list[CardImportItem]

class CardImportResult(BaseModel):
    imported: int
    skipped_duplicate: int
    skipped_invalid: int
    errors: list[str] = []

class BulkLevelUpdate(BaseModel):
    division: str = Field(..., description="Nama divisi (atau '__NONE__' untuk tanpa divisi)")
    access_level: str = Field(..., description="Level akses baru")

# ============================================
#  Schedule Models
# ============================================

class ScheduleCreate(BaseModel):
    access_level: str = Field(..., examples=["level_1"])
    days: str = Field(..., description="Hari (comma separated)", examples=["mon,tue,wed,thu,fri"])
    time_start: str = Field(..., description="Jam mulai", examples=["07:00"])
    time_end: str = Field(..., description="Jam selesai", examples=["18:00"])

class ScheduleUpdate(BaseModel):
    access_level: Optional[str] = None
    days: Optional[str] = None
    time_start: Optional[str] = None
    time_end: Optional[str] = None
    is_active: Optional[bool] = None

class ScheduleResponse(BaseModel):
    id: int
    access_level: str
    days: str
    time_start: str
    time_end: str
    is_active: bool
    created_at: str
    updated_at: str

# ============================================
#  ESP32 Models (request dari hardware)
# ============================================

class EspTapRequest(BaseModel):
    esp_id: str
    uid: str
    door_name: str = "Pintu Utama"
    # Jika validasi sudah dilakukan di ESP (offline mode)
    validated_locally: bool = False
    status: Optional[str] = None       # "granted" / "denied"
    reason: Optional[str] = None
    access_level: Optional[str] = None

class EspTapResponse(BaseModel):
    action: str  # "grant" atau "deny"
    reason: str = ""
    card_holder: str = ""

class EspHeartbeatRequest(BaseModel):
    esp_id: str
    door_name: str = "Pintu Utama"
    door_state: str = "locked"
    uptime: int = 0
    wifi_rssi: int = 0
    mac_address: Optional[str] = None
    free_heap: Optional[int] = None
    max_alloc_heap: Optional[int] = None

class EspDoorStatusRequest(BaseModel):
    esp_id: str
    state: str  # "locked" / "unlocked"

# ============================================
#  Access Log Models
# ============================================

class AccessLogResponse(BaseModel):
    id: int
    uid: str
    card_holder: Optional[str]
    employee_name: Optional[str] = None
    division: Optional[str] = None
    access_level: Optional[str]
    status: str
    denied_reason: Optional[str]
    door_name: str
    timestamp: str

# ============================================
#  Door Models
# ============================================

class DoorUpdate(BaseModel):
    name: str = Field(..., description="Nama baru pintu", examples=["Pintu Lobby"])

class DoorResponse(BaseModel):
    id: int
    name: str
    esp_id: Optional[str]
    mac_address: Optional[str] = None
    current_state: str
    is_online: bool
    wifi_rssi: Optional[int]
    uptime: Optional[int]
    free_heap: Optional[int] = None
    max_alloc_heap: Optional[int] = None
    last_heartbeat: Optional[str]
    last_activity: Optional[str]

# ============================================
#  Dashboard Models
# ============================================

class DashboardStats(BaseModel):
    total_taps_today: int = 0
    granted_today: int = 0
    denied_today: int = 0
    total_cards: int = 0
    active_cards: int = 0

# ============================================
#  Auth & User Models
# ============================================

class UserLogin(BaseModel):
    username: str = Field(..., examples=["admin"])
    password: str = Field(..., examples=["admin123"])

class UserCreate(BaseModel):
    username: str = Field(..., examples=["satpam1"])
    password: str = Field(..., examples=["rahasia123"])
    full_name: str = Field(..., examples=["Pak Satpam"])
    role: str = Field(default="viewer", examples=["viewer", "admin", "owner"])

class UserUpdate(BaseModel):
    full_name: Optional[str] = None
    role: Optional[str] = None
    is_active: Optional[bool] = None
    password: Optional[str] = None

class UserResponse(BaseModel):
    id: int
    username: str
    full_name: str
    role: str
    is_active: bool
    created_at: str

class Token(BaseModel):
    access_token: str
    token_type: str = "bearer"
    role: str

class TokenData(BaseModel):
    username: Optional[str] = None
