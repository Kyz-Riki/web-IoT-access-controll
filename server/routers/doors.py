from fastapi import APIRouter, Depends, HTTPException
import aiosqlite

from database import get_db, now_local
from models import DoorResponse

router = APIRouter(prefix="/api/doors", tags=["Doors"])

# ============================================
#  GET — List semua pintu
# ============================================

@router.get("", response_model=list[DoorResponse])
async def list_doors(db: aiosqlite.Connection = Depends(get_db)):
    """Dapatkan status semua pintu."""
    cursor = await db.execute("SELECT * FROM doors ORDER BY name")
    rows = await cursor.fetchall()

    return [
        DoorResponse(
            id=row["id"],
            name=row["name"],
            esp_id=row["esp_id"],
            current_state=row["current_state"],
            is_online=bool(row["is_online"]),
            wifi_rssi=row["wifi_rssi"],
            uptime=row["uptime"],
            last_heartbeat=row["last_heartbeat"],
            last_activity=row["last_activity"]
        ) for row in rows
    ]

# ============================================
#  POST — Remote unlock
# ============================================

@router.post("/{door_id}/unlock")
async def remote_unlock(
    door_id: int,
    db: aiosqlite.Connection = Depends(get_db)
):
    """Remote unlock pintu dari web dashboard (emergency)."""
    cursor = await db.execute("SELECT * FROM doors WHERE id = ?", (door_id,))
    door = await cursor.fetchone()
    if not door:
        raise HTTPException(status_code=404, detail="Pintu tidak ditemukan")

    if not door["is_online"]:
        raise HTTPException(status_code=503, detail="ESP32 sedang offline, tidak bisa remote unlock")

    # Update state di database
    # Note: Implementasi sebenarnya butuh push ke ESP32 (via WebSocket atau long polling)
    # Untuk sekarang, flag di database dulu
    await db.execute(
        "UPDATE doors SET current_state = 'unlocked', last_activity = ? WHERE id = ?",
        (now_local(), door_id)
    )
    await db.commit()

    return {"message": f"Perintah unlock dikirim ke {door['name']}", "door_id": door_id}
