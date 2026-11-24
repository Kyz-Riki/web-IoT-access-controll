from fastapi import APIRouter, Depends, HTTPException
from datetime import datetime
import aiosqlite
from auth import require_role
from config import HEARTBEAT_TIMEOUT_SEC

from database import get_db, now_local
from models import DoorResponse, DoorUpdate
from routers.esp import set_pending_command, ws_manager

router = APIRouter(prefix="/api/doors", tags=["Doors"])

# ============================================
#  GET — List semua pintu
# ============================================

@router.get("", response_model=list[DoorResponse])
async def list_doors(db: aiosqlite.Connection = Depends(get_db), current_user = Depends(require_role(['owner', 'admin', 'viewer']))):
    """Dapatkan status semua pintu."""
    cursor = await db.execute("SELECT * FROM doors ORDER BY name")
    rows = await cursor.fetchall()

    doors = []
    for row in rows:
        is_online = False
        if row["last_heartbeat"]:
            try:
                last_hb = datetime.fromisoformat(row["last_heartbeat"])
                diff = (datetime.now() - last_hb).total_seconds()
                is_online = diff < HEARTBEAT_TIMEOUT_SEC
            except (ValueError, TypeError):
                is_online = False

        doors.append(
            DoorResponse(
                id=row["id"],
                name=row["name"],
                esp_id=row["esp_id"],
                mac_address=row["mac_address"] if "mac_address" in row.keys() else None,
                current_state=row["current_state"],
                is_online=is_online,
                wifi_rssi=row["wifi_rssi"],
                uptime=row["uptime"],
                free_heap=row["free_heap"] if "free_heap" in row.keys() else None,
                max_alloc_heap=row["max_alloc_heap"] if "max_alloc_heap" in row.keys() else None,
                last_heartbeat=row["last_heartbeat"],
                last_activity=row["last_activity"]
            )
        )

    return doors

# ============================================
#  POST — Remote unlock
# ============================================

@router.post("/{door_id}/unlock")
async def remote_unlock(
    door_id: int,
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin']))
):
    """Remote unlock pintu dari web dashboard (emergency)."""
    cursor = await db.execute("SELECT * FROM doors WHERE id = ?", (door_id,))
    door = await cursor.fetchone()
    if not door:
        raise HTTPException(status_code=404, detail="Pintu tidak ditemukan")

    if not door["is_online"]:
        raise HTTPException(status_code=503, detail="ESP32 sedang offline, tidak bisa remote unlock")

    esp_id = door["esp_id"]
    if esp_id:
        set_pending_command(esp_id, "unlock")

    # Update state di database
    await db.execute(
        "UPDATE doors SET current_state = 'unlocked', last_activity = ? WHERE id = ?",
        (now_local(), door_id)
    )

    # Catat ke log akses
    admin_name = current_user.get("full_name") or current_user.get("username") or "Web Admin"
    await db.execute(
        """INSERT INTO access_logs (uid, card_holder, access_level, status, denied_reason, door_name, timestamp)
           VALUES (?, ?, ?, ?, ?, ?, ?)""",
        ("REMOTE", f"{admin_name} (Remote Web)", "admin", "granted", "remote_unlock", door["name"], now_local())
    )
    await db.commit()

    # Broadcast status ke dashboard via WebSocket jika tersedia
    if ws_manager:
        await ws_manager.broadcast({
            "type": "door_status",
            "esp_id": esp_id,
            "state": "unlocked"
        })
        await ws_manager.broadcast({
            "type": "tap",
            "uid": "REMOTE",
            "card_holder": f"{admin_name} (Remote Web)",
            "status": "granted",
            "reason": "remote_unlock",
            "door_name": door["name"],
            "timestamp": now_local()
        })

    return {"message": f"Perintah unlock berhasil dikirim ke {door['name']}", "door_id": door_id}

# ============================================
#  PUT — Update nama pintu
# ============================================

@router.put("/{door_id}", response_model=DoorResponse)
async def update_door(
    door_id: int,
    door_update: DoorUpdate,
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin']))
):
    """Ubah nama pintu."""
    name = door_update.name.strip()
    if not name:
        raise HTTPException(status_code=400, detail="Nama pintu tidak boleh kosong")

    cursor = await db.execute("SELECT id FROM doors WHERE id = ?", (door_id,))
    target = await cursor.fetchone()
    if not target:
        raise HTTPException(status_code=404, detail="Pintu tidak ditemukan")

    name_check = await db.execute("SELECT id FROM doors WHERE name = ? AND id != ?", (name, door_id))
    if await name_check.fetchone():
        raise HTTPException(status_code=400, detail=f"Pintu dengan nama '{name}' sudah digunakan")

    await db.execute("UPDATE doors SET name = ? WHERE id = ?", (name, door_id))
    await db.commit()

    cursor = await db.execute("SELECT * FROM doors WHERE id = ?", (door_id,))
    row = await cursor.fetchone()

    is_online = False
    if row["last_heartbeat"]:
        try:
            last_hb = datetime.fromisoformat(row["last_heartbeat"])
            diff = (datetime.now() - last_hb).total_seconds()
            is_online = diff < HEARTBEAT_TIMEOUT_SEC
        except (ValueError, TypeError):
            is_online = False

    return DoorResponse(
        id=row["id"],
        name=row["name"],
        esp_id=row["esp_id"],
        mac_address=row["mac_address"] if "mac_address" in row.keys() else None,
        current_state=row["current_state"],
        is_online=is_online,
        wifi_rssi=row["wifi_rssi"],
        uptime=row["uptime"],
        free_heap=row["free_heap"] if "free_heap" in row.keys() else None,
        max_alloc_heap=row["max_alloc_heap"] if "max_alloc_heap" in row.keys() else None,
        last_heartbeat=row["last_heartbeat"],
        last_activity=row["last_activity"]
    )

# ============================================
#  DELETE — Hapus pintu / perangkat ESP
# ============================================

@router.delete("/{door_id}")
async def delete_door(
    door_id: int,
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin']))
):
    """Hapus data pintu / perangkat ESP dari sistem."""
    cursor = await db.execute("SELECT id, name, esp_id FROM doors WHERE id = ?", (door_id,))
    target = await cursor.fetchone()
    if not target:
        raise HTTPException(status_code=404, detail="Pintu tidak ditemukan")

    await db.execute("DELETE FROM doors WHERE id = ?", (door_id,))
    await db.commit()

    return {"message": f"Pintu '{target['name']}' ({target['esp_id']}) berhasil dihapus", "id": door_id}
