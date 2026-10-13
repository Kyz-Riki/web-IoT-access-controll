from fastapi import APIRouter, Depends
from datetime import datetime
import aiosqlite
from auth import require_role

from database import get_db, now_local
from models import (
    EspTapRequest, EspTapResponse,
    EspHeartbeatRequest, EspDoorStatusRequest
)
from config import HEARTBEAT_TIMEOUT_SEC

router = APIRouter(prefix="/api/esp", tags=["ESP32"])

# ============================================
#  Shared: WebSocket manager reference
# ============================================
# Akan di-inject dari main.py
ws_manager = None

def set_ws_manager(manager):
    global ws_manager
    ws_manager = manager

# ============================================
#  Register Mode State (in-memory)
# ============================================
# Saat True, ESP32 akan masuk mode scan kartu
# untuk registrasi (bukan validasi akses)
_register_mode = False

# ============================================
#  POST — Aktifkan mode register
# ============================================

@router.post("/register-mode")
async def activate_register_mode():
    """Aktifkan mode register. ESP32 akan scan kartu berikutnya untuk didaftarkan."""
    global _register_mode
    _register_mode = True
    return {"status": "ok", "mode": "register", "message": "ESP32 siap scan kartu baru"}

# ============================================
#  POST — Batalkan mode register
# ============================================

@router.post("/cancel-register")
async def cancel_register_mode():
    """Batalkan mode register, kembali ke mode normal."""
    global _register_mode
    _register_mode = False
    
    # Broadcast ke dashboard bahwa mode register dibatalkan
    if ws_manager:
        await ws_manager.broadcast({"type": "register_cancelled"})
    
    return {"status": "ok", "mode": "normal"}

# ============================================
#  GET — ESP32 cek mode saat ini
# ============================================

@router.get("/mode")
async def get_current_mode():
    """ESP32 polling endpoint untuk cek apakah sedang mode register."""
    return {"mode": "register" if _register_mode else "normal"}

# ============================================
#  POST — ESP32 kirim hasil scan registrasi
# ============================================

@router.post("/register-scan")
async def handle_register_scan(
    req: dict,
    db: aiosqlite.Connection = Depends(get_db)
):
    """ESP32 kirim UID kartu yang di-scan saat mode register."""
    global _register_mode
    uid = req.get("uid", "").upper()
    esp_id = req.get("esp_id", "")

    if not uid:
        return {"status": "error", "message": "UID kosong"}

    # Cek apakah kartu sudah terdaftar
    cursor = await db.execute("SELECT uid, card_holder FROM cards WHERE uid = ?", (uid,))
    existing = await cursor.fetchone()

    already_registered = False
    existing_holder = ""
    if existing:
        already_registered = True
        existing_holder = existing["card_holder"]

    # Matikan mode register (1 scan saja)
    _register_mode = False

    # Broadcast UID ke dashboard via WebSocket
    if ws_manager:
        await ws_manager.broadcast({
            "type": "register_scan",
            "uid": uid,
            "already_registered": already_registered,
            "existing_holder": existing_holder
        })

    return {
        "status": "ok",
        "uid": uid,
        "already_registered": already_registered
    }

# ============================================
#  GET — Whitelist + Schedules untuk ESP32
# ============================================

@router.get("/whitelist")
async def get_whitelist(
    esp_id: str = "esp-01",
    db: aiosqlite.Connection = Depends(get_db)
):
    """ESP32 ambil daftar kartu aktif + jadwal."""
    # Ambil kartu aktif
    cursor = await db.execute(
        "SELECT uid, access_level, is_active FROM cards WHERE is_active = 1"
    )
    card_rows = await cursor.fetchall()

    cards = [
        {
            "uid": row["uid"],
            "access_level": row["access_level"],
            "is_active": bool(row["is_active"])
        }
        for row in card_rows
    ]

    # Ambil jadwal aktif
    cursor = await db.execute(
        "SELECT access_level, days, time_start, time_end, is_active FROM schedules WHERE is_active = 1"
    )
    schedule_rows = await cursor.fetchall()

    schedules = [
        {
            "access_level": row["access_level"],
            "days": row["days"],
            "time_start": row["time_start"],
            "time_end": row["time_end"],
            "is_active": bool(row["is_active"])
        }
        for row in schedule_rows
    ]

    return {"cards": cards, "schedules": schedules}

# ============================================
#  POST — Tap event dari ESP32
# ============================================

@router.post("/tap", response_model=EspTapResponse)
async def handle_tap(
    tap: EspTapRequest,
    db: aiosqlite.Connection = Depends(get_db)
):
    """
    ESP32 kirim tap event.
    - Jika validated_locally=false → server validasi dan respond grant/deny
    - Jika validated_locally=true → ESP sudah validasi, server hanya log
    """
    uid = tap.uid.upper()

    # Jika ESP sudah validasi sendiri (offline mode), cukup simpan log
    if tap.validated_locally:
        card_holder = None
        cursor = await db.execute("SELECT card_holder FROM cards WHERE uid = ?", (uid,))
        row = await cursor.fetchone()
        if row:
            card_holder = row["card_holder"]

        await _save_log(db, uid, card_holder, tap.access_level, tap.status, tap.reason, tap.door_name)

        # Push ke WebSocket
        if ws_manager:
            await ws_manager.broadcast({
                "type": "tap",
                "uid": uid,
                "card_holder": card_holder or "Unknown",
                "status": tap.status,
                "reason": tap.reason or "",
                "door_name": tap.door_name,
                "timestamp": now_local()
            })

        return EspTapResponse(action=tap.status or "deny", reason=tap.reason or "")

    # === Server validasi ===

    # 1. Cek apakah kartu terdaftar
    cursor = await db.execute(
        "SELECT * FROM cards WHERE uid = ?", (uid,)
    )
    card = await cursor.fetchone()

    if not card:
        await _save_log(db, uid, None, None, "denied", "unregistered", tap.door_name)
        await _broadcast_tap(uid, None, "denied", "unregistered", tap.door_name)
        return EspTapResponse(action="deny", reason="unregistered")

    card_holder = card["card_holder"]
    access_level = card["access_level"]

    # 2. Cek apakah kartu aktif
    if not card["is_active"]:
        await _save_log(db, uid, card_holder, access_level, "denied", "blocked", tap.door_name)
        await _broadcast_tap(uid, card_holder, "denied", "blocked", tap.door_name)
        return EspTapResponse(action="deny", reason="blocked", card_holder=card_holder)

    # 3. Cek jadwal (admin selalu granted)
    if access_level != "admin":
        in_schedule = await _check_schedule(db, access_level)
        if not in_schedule:
            await _save_log(db, uid, card_holder, access_level, "denied", "out_of_schedule", tap.door_name)
            await _broadcast_tap(uid, card_holder, "denied", "out_of_schedule", tap.door_name)
            return EspTapResponse(action="deny", reason="out_of_schedule", card_holder=card_holder)

    # 4. Granted!
    await _save_log(db, uid, card_holder, access_level, "granted", None, tap.door_name)
    await _broadcast_tap(uid, card_holder, "granted", "", tap.door_name)

    # Update last_activity di door
    await db.execute(
        "UPDATE doors SET last_activity = ? WHERE esp_id = ?",
        (now_local(), tap.esp_id)
    )
    await db.commit()

    return EspTapResponse(action="grant", card_holder=card_holder)

# ============================================
#  POST — Heartbeat dari ESP32
# ============================================

@router.post("/heartbeat")
async def handle_heartbeat(
    hb: EspHeartbeatRequest,
    db: aiosqlite.Connection = Depends(get_db)
):
    """ESP32 kirim heartbeat periodik."""
    # Upsert door record
    cursor = await db.execute("SELECT id FROM doors WHERE esp_id = ?", (hb.esp_id,))
    existing = await cursor.fetchone()

    if existing:
        await db.execute(
            """UPDATE doors SET
                current_state = ?, is_online = 1, wifi_rssi = ?,
                uptime = ?, last_heartbeat = ?
               WHERE esp_id = ?""",
            (hb.door_state, hb.wifi_rssi, hb.uptime, now_local(), hb.esp_id)
        )
    else:
        await db.execute(
            """INSERT INTO doors (name, esp_id, current_state, is_online, wifi_rssi, uptime, last_heartbeat)
               VALUES (?, ?, ?, 1, ?, ?, ?)""",
            (hb.door_name, hb.esp_id, hb.door_state, hb.wifi_rssi, hb.uptime, now_local())
        )

    await db.commit()

    # Push status ke WebSocket
    if ws_manager:
        await ws_manager.broadcast({
            "type": "heartbeat",
            "esp_id": hb.esp_id,
            "door_name": hb.door_name,
            "door_state": hb.door_state,
            "is_online": True,
            "wifi_rssi": hb.wifi_rssi,
            "uptime": hb.uptime
        })

    return {"status": "ok"}

# ============================================
#  POST — Door status update
# ============================================

@router.post("/door-status")
async def handle_door_status(
    req: EspDoorStatusRequest,
    db: aiosqlite.Connection = Depends(get_db)
):
    """ESP32 update status pintu (locked/unlocked)."""
    await db.execute(
        "UPDATE doors SET current_state = ?, last_activity = ? WHERE esp_id = ?",
        (req.state, now_local(), req.esp_id)
    )
    await db.commit()

    # Push ke WebSocket
    if ws_manager:
        await ws_manager.broadcast({
            "type": "door_status",
            "esp_id": req.esp_id,
            "state": req.state
        })

    return {"status": "ok"}

# ============================================
#  Private Helpers
# ============================================

async def _save_log(db, uid, card_holder, access_level, status, reason, door_name):
    """Simpan log akses ke database dengan timestamp WIB."""
    await db.execute(
        """INSERT INTO access_logs (uid, card_holder, access_level, status, denied_reason, door_name, timestamp)
           VALUES (?, ?, ?, ?, ?, ?, ?)""",
        (uid, card_holder, access_level, status, reason, door_name, now_local())
    )
    await db.commit()


async def _check_schedule(db, access_level):
    """Cek apakah waktu sekarang ada dalam jadwal untuk access level tertentu."""
    now = datetime.now()
    current_day_map = {0: "mon", 1: "tue", 2: "wed", 3: "thu", 4: "fri", 5: "sat", 6: "sun"}
    current_day = current_day_map[now.weekday()]
    current_time = now.strftime("%H:%M")

    cursor = await db.execute(
        "SELECT * FROM schedules WHERE access_level = ? AND is_active = 1",
        (access_level,)
    )
    rows = await cursor.fetchall()

    # Jika tidak ada rule untuk level ini, default: izinkan
    if not rows:
        return True

    for row in rows:
        days = row["days"].lower().split(",")
        if current_day in days:
            if row["time_start"] <= current_time <= row["time_end"]:
                return True

    return False


async def _broadcast_tap(uid, card_holder, status, reason, door_name):
    """Broadcast tap event ke WebSocket."""
    if ws_manager:
        await ws_manager.broadcast({
            "type": "tap",
            "uid": uid,
            "card_holder": card_holder or "Unknown",
            "status": status,
            "reason": reason,
            "door_name": door_name,
            "timestamp": now_local()
        })
