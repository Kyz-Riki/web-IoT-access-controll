from fastapi import APIRouter, Depends, HTTPException
from datetime import datetime
import asyncio
import aiosqlite
from auth import require_role
from config import HEARTBEAT_TIMEOUT_SEC, DATABASE_PATH

from database import get_db, now_local, WIB
from models import DoorResponse, DoorUpdate
from routers import esp

router = APIRouter(prefix="/api/doors", tags=["Doors"])

# Background auto-relock fallback tasks (jika ESP32 versi lama belum kirim /door-status atau mendadak offline)
_relock_tasks: dict[str, asyncio.Task] = {}
REMOTE_UNLOCK_FALLBACK_SEC = 7.5


def cancel_auto_relock(esp_id: str):
    """Batalkan timer fallback auto-relock jika ESP32 sudah melaporkan status locked secara real-time."""
    if not esp_id:
        return
    key = esp_id.strip().lower()
    task = _relock_tasks.pop(key, None)
    if task and not task.done():
        task.cancel()


def schedule_auto_relock(door_id: int, esp_id: str, delay_sec: float = REMOTE_UNLOCK_FALLBACK_SEC):
    """Jadwalkan fallback pengembalian status ke locked apabila ESP32 belum melaporkan locked."""
    key = (esp_id or f"door-{door_id}").strip().lower()
    old_task = _relock_tasks.pop(key, None)
    if old_task and not old_task.done():
        old_task.cancel()

    async def _relock_worker():
        try:
            await asyncio.sleep(delay_sec)
            async with aiosqlite.connect(DATABASE_PATH) as conn:
                conn.row_factory = aiosqlite.Row
                cur = await conn.execute("SELECT id, esp_id, current_state FROM doors WHERE id = ?", (door_id,))
                row = await cur.fetchone()
                if row and row["current_state"] != "locked":
                    await conn.execute(
                        "UPDATE doors SET current_state = 'locked' WHERE id = ?",
                        (door_id,)
                    )
                    await conn.commit()
                    if esp.ws_manager:
                        await esp.ws_manager.broadcast({
                            "type": "door_status",
                            "esp_id": row["esp_id"] or esp_id,
                            "state": "locked"
                        })
        except asyncio.CancelledError:
            pass
        except Exception as e:
            print(f"[Doors] ⚠️ Auto-relock fallback error: {e}")
        finally:
            if _relock_tasks.get(key) is asyncio.current_task():
                _relock_tasks.pop(key, None)

    _relock_tasks[key] = asyncio.create_task(_relock_worker())

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
                now_wib = datetime.now(WIB).replace(tzinfo=None)
                diff = (now_wib - last_hb).total_seconds()
                is_online = diff < HEARTBEAT_TIMEOUT_SEC
            except (ValueError, TypeError):
                is_online = False

        effective_state = row["current_state"]
        if not is_online:
            effective_state = "locked"
            if row["is_online"] or row["current_state"] != "locked":
                await db.execute(
                    "UPDATE doors SET is_online = 0, current_state = 'locked' WHERE id = ?",
                    (row["id"],)
                )
                await db.commit()

        doors.append(
            DoorResponse(
                id=row["id"],
                name=row["name"],
                esp_id=row["esp_id"],
                mac_address=row["mac_address"] if "mac_address" in row.keys() else None,
                current_state=effective_state,
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

    # Cek online secara real-time berdasarkan last_heartbeat
    is_online = False
    if door["last_heartbeat"]:
        try:
            last_hb = datetime.fromisoformat(door["last_heartbeat"])
            now_wib = datetime.now(WIB).replace(tzinfo=None)
            is_online = (now_wib - last_hb).total_seconds() < HEARTBEAT_TIMEOUT_SEC
        except (ValueError, TypeError):
            is_online = False

    if not is_online:
        await db.execute("UPDATE doors SET is_online = 0, current_state = 'locked' WHERE id = ?", (door_id,))
        await db.commit()
        raise HTTPException(status_code=503, detail="ESP32 sedang offline, tidak bisa remote unlock")

    esp_id = door["esp_id"]
    if esp_id:
        esp.set_pending_command(esp_id, "unlock")

    # Update state di database
    await db.execute(
        "UPDATE doors SET current_state = 'unlocked', last_activity = ? WHERE id = ?",
        (now_local(), door_id)
    )

    # Catat ke log akses
    admin_name = current_user.get("full_name") or current_user.get("username") or "Web Admin"
    remote_holder = f"{admin_name} (Remote Web)"
    await db.execute(
        """INSERT INTO access_logs (uid, card_holder, division, access_level, status, denied_reason, door_name, timestamp)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?)""",
        ("REMOTE", remote_holder, "Remote Web", "admin", "granted", "remote_unlock", door["name"], now_local())
    )
    await db.commit()

    # Broadcast status ke dashboard via WebSocket jika tersedia
    if esp.ws_manager:
        await esp.ws_manager.broadcast({
            "type": "door_status",
            "esp_id": esp_id,
            "state": "unlocked"
        })
        await esp.ws_manager.broadcast({
            "type": "tap",
            "uid": "REMOTE",
            "card_holder": remote_holder,
            "employee_name": remote_holder,
            "division": "Remote Web",
            "status": "granted",
            "reason": "remote_unlock",
            "door_name": door["name"],
            "timestamp": now_local()
        })

    # Jadwalkan fallback auto-relock agar tidak nyangkut 'unlocked' jika ESP32 offline atau belum kirim /door-status
    schedule_auto_relock(door_id, esp_id or "", REMOTE_UNLOCK_FALLBACK_SEC)

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
            now_wib = datetime.now(WIB).replace(tzinfo=None)
            diff = (now_wib - last_hb).total_seconds()
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
