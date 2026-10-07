from pydantic import BaseModel, Field
from typing import Optional
from datetime import datetime

# ============================================
#  Card Models
# ============================================

class CardCreate(BaseModel):
    uid: str = Field(..., description="UID kartu RFID (hex)", examples=["A3 B2 C1 D0"])
    card_holder: str = Field(..., description="Nama pemegang kartu", examples=["Budi Santoso"])
    access_level: str = Field(default="level_1", description="Level akses", examples=["admin", "level_1", "level_2", "level_3"])

class CardUpdate(BaseModel):
    card_holder: Optional[str] = None
    access_level: Optional[str] = None
    is_active: Optional[bool] = None

class CardResponse(BaseModel):
    id: int
    uid: str
    card_holder: str
    access_level: str
    is_active: bool
    created_at: str
    updated_at: str

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
    access_level: Optional[str]
    status: str
    denied_reason: Optional[str]
    door_name: str
    timestamp: str

# ============================================
#  Door Models
# ============================================

class DoorResponse(BaseModel):
    id: int
    name: str
    esp_id: Optional[str]
    current_state: str
    is_online: bool
    wifi_rssi: Optional[int]
    uptime: Optional[int]
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
