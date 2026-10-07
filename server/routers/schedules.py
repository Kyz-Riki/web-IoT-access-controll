from fastapi import APIRouter, Depends, HTTPException, Query
from typing import Optional
import aiosqlite

from database import get_db, now_local
from models import ScheduleCreate, ScheduleUpdate, ScheduleResponse

router = APIRouter(prefix="/api/schedules", tags=["Schedules"])

# ============================================
#  GET — List semua jadwal
# ============================================

@router.get("", response_model=list[ScheduleResponse])
async def list_schedules(
    access_level: Optional[str] = Query(None),
    db: aiosqlite.Connection = Depends(get_db)
):
    """Dapatkan daftar semua rule jadwal."""
    query = "SELECT * FROM schedules WHERE 1=1"
    params = []

    if access_level:
        query += " AND access_level = ?"
        params.append(access_level)

    query += " ORDER BY access_level, id"

    cursor = await db.execute(query, params)
    rows = await cursor.fetchall()

    return [
        ScheduleResponse(
            id=row["id"],
            access_level=row["access_level"],
            days=row["days"],
            time_start=row["time_start"],
            time_end=row["time_end"],
            is_active=bool(row["is_active"]),
            created_at=row["created_at"],
            updated_at=row["updated_at"]
        ) for row in rows
    ]

# ============================================
#  POST — Tambah rule jadwal baru
# ============================================

@router.post("", response_model=ScheduleResponse, status_code=201)
async def create_schedule(
    schedule: ScheduleCreate,
    db: aiosqlite.Connection = Depends(get_db)
):
    """Tambah rule jadwal akses baru."""
    # Validasi access level (admin tidak perlu jadwal)
    if schedule.access_level.strip().lower() == "admin":
        raise HTTPException(
            status_code=400,
            detail="Admin selalu punya akses 24/7 dan tidak perlu dijadwalkan."
        )
    if not schedule.access_level.strip():
        raise HTTPException(status_code=400, detail="Nama level akses tidak boleh kosong.")

    # Validasi format hari
    valid_days = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"}
    input_days = {d.strip().lower() for d in schedule.days.split(",")}
    invalid_days = input_days - valid_days
    if invalid_days:
        raise HTTPException(
            status_code=400,
            detail=f"Hari tidak valid: {invalid_days}. Gunakan: {valid_days}"
        )

    # Validasi format waktu
    for time_str in [schedule.time_start, schedule.time_end]:
        try:
            parts = time_str.split(":")
            h, m = int(parts[0]), int(parts[1])
            if not (0 <= h <= 23 and 0 <= m <= 59):
                raise ValueError
        except (ValueError, IndexError):
            raise HTTPException(status_code=400, detail=f"Format waktu tidak valid: {time_str}. Gunakan HH:MM")

    # Normalize days string
    normalized_days = ",".join(sorted(input_days, key=lambda d: ["mon","tue","wed","thu","fri","sat","sun"].index(d)))

    cursor = await db.execute(
        """INSERT INTO schedules (access_level, days, time_start, time_end, created_at, updated_at)
           VALUES (?, ?, ?, ?, ?, ?)""",
        (schedule.access_level, normalized_days, schedule.time_start, schedule.time_end, now_local(), now_local())
    )
    await db.commit()

    # Ambil data yang baru dibuat
    cursor = await db.execute("SELECT * FROM schedules WHERE id = ?", (cursor.lastrowid,))
    row = await cursor.fetchone()

    return ScheduleResponse(
        id=row["id"],
        access_level=row["access_level"],
        days=row["days"],
        time_start=row["time_start"],
        time_end=row["time_end"],
        is_active=bool(row["is_active"]),
        created_at=row["created_at"],
        updated_at=row["updated_at"]
    )

# ============================================
#  PUT — Edit rule jadwal
# ============================================

@router.put("/{schedule_id}", response_model=ScheduleResponse)
async def update_schedule(
    schedule_id: int,
    schedule: ScheduleUpdate,
    db: aiosqlite.Connection = Depends(get_db)
):
    """Update rule jadwal."""
    cursor = await db.execute("SELECT * FROM schedules WHERE id = ?", (schedule_id,))
    existing = await cursor.fetchone()
    if not existing:
        raise HTTPException(status_code=404, detail="Jadwal tidak ditemukan")

    updates = []
    params = []

    if schedule.access_level is not None:
        if schedule.access_level.strip().lower() == "admin":
            raise HTTPException(status_code=400, detail="Level Admin tidak dapat dijadwalkan.")
        if not schedule.access_level.strip():
            raise HTTPException(status_code=400, detail="Nama level akses tidak boleh kosong.")
        updates.append("access_level = ?")
        params.append(schedule.access_level.strip())

    if schedule.days is not None:
        updates.append("days = ?")
        params.append(schedule.days.lower())

    if schedule.time_start is not None:
        updates.append("time_start = ?")
        params.append(schedule.time_start)

    if schedule.time_end is not None:
        updates.append("time_end = ?")
        params.append(schedule.time_end)

    if schedule.is_active is not None:
        updates.append("is_active = ?")
        params.append(1 if schedule.is_active else 0)

    if not updates:
        raise HTTPException(status_code=400, detail="Tidak ada field yang diupdate")

    updates.append("updated_at = ?")
    params.append(now_local())
    params.append(schedule_id)

    query = f"UPDATE schedules SET {', '.join(updates)} WHERE id = ?"
    await db.execute(query, params)
    await db.commit()

    cursor = await db.execute("SELECT * FROM schedules WHERE id = ?", (schedule_id,))
    row = await cursor.fetchone()

    return ScheduleResponse(
        id=row["id"],
        access_level=row["access_level"],
        days=row["days"],
        time_start=row["time_start"],
        time_end=row["time_end"],
        is_active=bool(row["is_active"]),
        created_at=row["created_at"],
        updated_at=row["updated_at"]
    )

# ============================================
#  DELETE — Hapus rule jadwal
# ============================================

@router.delete("/{schedule_id}")
async def delete_schedule(
    schedule_id: int,
    db: aiosqlite.Connection = Depends(get_db)
):
    """Hapus rule jadwal."""
    cursor = await db.execute("SELECT id FROM schedules WHERE id = ?", (schedule_id,))
    existing = await cursor.fetchone()
    if not existing:
        raise HTTPException(status_code=404, detail="Jadwal tidak ditemukan")

    await db.execute("DELETE FROM schedules WHERE id = ?", (schedule_id,))
    await db.commit()

    return {"message": "Jadwal berhasil dihapus", "id": schedule_id}
