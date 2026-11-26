from fastapi import APIRouter, Depends
from datetime import datetime
import aiosqlite
import hashlib
import json
from auth import require_role

from database import get_db, now_local, WIB
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
#  Register Mode & Pending Commands State (in-memory)
# ============================================
# Saat True, ESP32 akan masuk mode scan kartu
# untuk registrasi (bukan validasi akses)
_register_mode = False

# Antrean perintah remote (misal remote unlock) per ESP ID
_pending_commands = {}

def set_pending_command(esp_id: str, command: str):
    global _pending_commands
    if esp_id:
        _pending_commands[esp_id.strip().lower()] = command

@router.get("/pending-command")
async def get_pending_command(esp_id: str = "esp-01"):
    """ESP32 polling endpoint untuk cek apakah ada perintah tertunda (misal remote unlock)."""
    global _pending_commands
    cmd = _pending_commands.pop(esp_id.strip().lower(), "none")
    return {"command": cmd}

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
    cursor = await db.execute("SELECT uid, card_holder, division FROM cards WHERE uid = ?", (uid,))
    existing = await cursor.fetchone()

    already_registered = False
    existing_holder = ""
    existing_division = ""
    if existing:
        already_registered = True
        existing_holder = existing["card_holder"]
        existing_division = (existing["division"] if "division" in existing.keys() else None) or ""

    # Matikan mode register (1 scan saja)
    _register_mode = False

    # Broadcast UID ke dashboard via WebSocket
    if ws_manager:
        await ws_manager.broadcast({
            "type": "register_scan",
            "uid": uid,
            "already_registered": already_registered,
            "existing_holder": existing_holder,
            "existing_division": existing_division
        })

    return {
        "status": "ok",
        "uid": uid,
        "already_registered": already_registered
    }

# ============================================
#  GET — Whitelist + Schedules untuk ESP32
# ============================================

async def _build_whitelist_payload(db: aiosqlite.Connection) -> dict:
    """
    Bangun payload whitelist kompak untuk ESP32 beserta hash versi deterministik.
    Nama level akses non-admin dipetakan otomatis ke kode singkat ("S1", "S2", ...)
    agar aman dari batas buffer 11 karakter di ESP32 dan menghemat RAM/NVS.
    """
    cursor = await db.execute(
        """SELECT uid, access_level, is_active FROM cards
           WHERE is_active = 1 AND uid IS NOT NULL AND TRIM(uid) != ''
           ORDER BY uid ASC"""
    )
    card_rows = await cursor.fetchall()

    cursor = await db.execute(
        """SELECT access_level, days, time_start, time_end, is_active FROM schedules
           WHERE is_active = 1
           ORDER BY LOWER(access_level) ASC, days ASC, time_start ASC, time_end ASC"""
    )
    schedule_rows = await cursor.fetchall()

    unique_levels = sorted({
        (r["access_level"] or "").strip().lower()
        for r in (*schedule_rows, *card_rows)
        if (r["access_level"] or "").strip() and (r["access_level"] or "").strip().lower() != "admin"
    })
    level_alias = {"admin": "admin"}
    for idx, lvl_key in enumerate(unique_levels, start=1):
        level_alias[lvl_key] = f"S{idx}"

    def _to_esp_level(raw_level: str) -> str:
        key = (raw_level or "").strip().lower()
        return level_alias.get(key, "S0")

    cards = [
        {
            "uid": row["uid"],
            "access_level": _to_esp_level(row["access_level"]),
            "is_active": bool(row["is_active"])
        }
        for row in card_rows
    ]

    schedules = [
        {
            "access_level": _to_esp_level(row["access_level"]),
            "days": row["days"],
            "time_start": row["time_start"],
            "time_end": row["time_end"],
            "is_active": bool(row["is_active"])
        }
        for row in schedule_rows
    ]

    canonical = json.dumps({"cards": cards, "schedules": schedules}, sort_keys=True, separators=(",", ":"))
    version = hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:12]

    return {"version": version, "cards": cards, "schedules": schedules}


@router.get("/whitelist")
async def get_whitelist(
    esp_id: str = "esp-01",
    db: aiosqlite.Connection = Depends(get_db)
):
    """ESP32 ambil daftar kartu aktif yang sudah memiliki UID + jadwal + hash versi."""
    return await _build_whitelist_payload(db)

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

    # Jika ESP sudah validasi sendiri (offline mode / push button), cukup simpan log
    if tap.validated_locally:
        card_holder = None
        division = None
        real_access_level = tap.access_level

        if uid == "BUTTON" or (tap.reason or "").strip().lower() == "manual_unlock":
            card_holder = "Push Button (Tombol Keluar)"
            division = "Tombol Fisik"
            real_access_level = "Manual"
        else:
            cursor = await db.execute(
                "SELECT card_holder, division, access_level FROM cards WHERE uid = ?", (uid,)
            )
            row = await cursor.fetchone()
            if row:
                card_holder = row["card_holder"]
                division = row["division"] if "division" in row.keys() else None
                real_access_level = row["access_level"]

        await _save_log(db, uid, card_holder, division, real_access_level, tap.status, tap.reason, tap.door_name)

        # Update last_activity pada pintu
        await db.execute(
            "UPDATE doors SET last_activity = ? WHERE esp_id = ?",
            (now_local(), tap.esp_id)
        )
        await db.commit()

        # Push ke WebSocket
        await _broadcast_tap(uid, card_holder, division, tap.status, tap.reason or "", tap.door_name)

        return EspTapResponse(action=tap.status or "deny", reason=tap.reason or "", card_holder=card_holder or "")

    # === Server validasi ===

    # 1. Cek apakah kartu terdaftar
    cursor = await db.execute(
        "SELECT * FROM cards WHERE uid = ?", (uid,)
    )
    card = await cursor.fetchone()

    if not card:
        await _save_log(db, uid, None, None, None, "denied", "unregistered", tap.door_name)
        await _broadcast_tap(uid, None, None, "denied", "unregistered", tap.door_name)
        return EspTapResponse(action="deny", reason="unregistered")

    card_holder = card["card_holder"]
    division = card["division"] if "division" in card.keys() else None
    access_level = card["access_level"]

    # 2. Cek apakah kartu aktif
    if not card["is_active"]:
        await _save_log(db, uid, card_holder, division, access_level, "denied", "blocked", tap.door_name)
        await _broadcast_tap(uid, card_holder, division, "denied", "blocked", tap.door_name)
        return EspTapResponse(action="deny", reason="blocked", card_holder=card_holder)

    # 3. Cek jadwal (admin selalu granted — case-insensitive)
    if (access_level or "").strip().lower() != "admin":
        in_schedule = await _check_schedule(db, access_level)
        if not in_schedule:
            await _save_log(db, uid, card_holder, division, access_level, "denied", "out_of_schedule", tap.door_name)
            await _broadcast_tap(uid, card_holder, division, "denied", "out_of_schedule", tap.door_name)
            return EspTapResponse(action="deny", reason="out_of_schedule", card_holder=card_holder)

    # 4. Granted!
    await _save_log(db, uid, card_holder, division, access_level, "granted", None, tap.door_name)
    await _broadcast_tap(uid, card_holder, division, "granted", "", tap.door_name)

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
    # Cek door record berdasarkan mac_address atau esp_id
    existing = None
    if hb.mac_address:
        cursor = await db.execute("SELECT id, name FROM doors WHERE mac_address = ?", (hb.mac_address,))
        existing = await cursor.fetchone()

    if not existing:
        cursor = await db.execute("SELECT id, name FROM doors WHERE esp_id = ?", (hb.esp_id,))
        existing = await cursor.fetchone()

    if existing:
        await db.execute(
            """UPDATE doors SET
                esp_id = ?, mac_address = COALESCE(?, mac_address),
                current_state = ?, is_online = 1, wifi_rssi = ?,
                uptime = ?, free_heap = ?, max_alloc_heap = ?, last_heartbeat = ?
               WHERE id = ?""",
            (hb.esp_id, hb.mac_address, hb.door_state, hb.wifi_rssi, hb.uptime, hb.free_heap, hb.max_alloc_heap, now_local(), existing["id"])
        )
    else:
        # Cegah bentrok nama jika dua ESP menggunakan DOOR_NAME default yang sama
        door_name = hb.door_name
        name_check = await db.execute("SELECT id FROM doors WHERE name = ?", (door_name,))
        if await name_check.fetchone():
            door_name = f"{hb.door_name} ({hb.esp_id})"

        await db.execute(
            """INSERT INTO doors (name, esp_id, mac_address, current_state, is_online, wifi_rssi, uptime, free_heap, max_alloc_heap, last_heartbeat)
               VALUES (?, ?, ?, ?, 1, ?, ?, ?, ?, ?)""",
            (door_name, hb.esp_id, hb.mac_address, hb.door_state, hb.wifi_rssi, hb.uptime, hb.free_heap, hb.max_alloc_heap, now_local())
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
            "uptime": hb.uptime,
            "free_heap": hb.free_heap,
            "max_alloc_heap": hb.max_alloc_heap
        })

    # Hitung hash versi whitelist terbaru agar ESP32 hanya unduh jika ada perubahan
    wl_payload = await _build_whitelist_payload(db)

    return {"status": "ok", "whitelist_version": wl_payload["version"]}

# ============================================
#  POST — Door status update
# ============================================

@router.post("/door-status")
async def handle_door_status(
    req: EspDoorStatusRequest,
    db: aiosqlite.Connection = Depends(get_db)
):
    """ESP32 update status pintu secara real-time (locked/unlocked)."""
    state_clean = (req.state or "locked").strip().lower()
    await db.execute(
        "UPDATE doors SET current_state = ?, is_online = 1, last_activity = ?, last_heartbeat = ? WHERE esp_id = ?",
        (state_clean, now_local(), now_local(), req.esp_id)
    )
    await db.commit()

    # Sinkronkan timer fallback auto-relock di doors router
    from routers import doors as doors_router
    if state_clean == "locked":
        doors_router.cancel_auto_relock(req.esp_id)
    elif state_clean == "unlocked":
        cur = await db.execute("SELECT id FROM doors WHERE esp_id = ?", (req.esp_id,))
        drow = await cur.fetchone()
        if drow:
            # Safety net 15 detik jika ESP32 mendadak mati di tengah kondisi unlocked
            doors_router.schedule_auto_relock(drow["id"], req.esp_id, 15.0)

    # Push ke WebSocket
    if ws_manager:
        await ws_manager.broadcast({
            "type": "door_status",
            "esp_id": req.esp_id,
            "state": state_clean
        })

    return {"status": "ok"}

# ============================================
#  Private Helpers
# ============================================

async def _save_log(db, uid, card_holder, division, access_level, status, reason, door_name):
    """Simpan log akses ke database dengan timestamp WIB."""
    await db.execute(
        """INSERT INTO access_logs (uid, card_holder, division, access_level, status, denied_reason, door_name, timestamp)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?)""",
        (uid, card_holder, division, access_level, status, reason, door_name, now_local())
    )
    await db.commit()


async def _check_schedule(db, access_level):
    """Cek apakah waktu sekarang ada dalam jadwal untuk access level tertentu (case-insensitive)."""
    now = datetime.now(WIB)
    current_day_map = {0: "mon", 1: "tue", 2: "wed", 3: "thu", 4: "fri", 5: "sat", 6: "sun"}
    current_day = current_day_map[now.weekday()]
    current_time = now.strftime("%H:%M")

    cursor = await db.execute(
        "SELECT * FROM schedules WHERE LOWER(access_level) = LOWER(?) AND is_active = 1",
        ((access_level or "").strip(),)
    )
    rows = await cursor.fetchall()

    # Jika tidak ada rule untuk level ini, default: izinkan
    if not rows:
        return True

    for row in rows:
        days = row["days"].lower().split(",")
        if current_day in days:
            t_start = row["time_start"]
            t_end = row["time_end"]
            if t_start <= t_end:
                if t_start <= current_time <= t_end:
                    return True
            else:
                # Rentang melewati tengah malam (contoh: 22:00 - 06:00)
                if current_time >= t_start or current_time <= t_end:
                    return True

    return False


async def _broadcast_tap(uid, card_holder, division, status, reason, door_name):
    """Broadcast tap event ke WebSocket."""
    if ws_manager:
        holder_label = card_holder or "Unknown"
        await ws_manager.broadcast({
            "type": "tap",
            "uid": uid,
            "card_holder": holder_label,
            "employee_name": holder_label,
            "division": division or "",
            "status": status,
            "reason": reason,
            "door_name": door_name,
            "timestamp": now_local()
        })
