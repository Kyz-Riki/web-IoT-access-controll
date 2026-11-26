from fastapi import APIRouter, Depends, Query
from typing import Optional
import aiosqlite
from auth import require_role

from database import get_db
from models import AccessLogResponse

router = APIRouter(prefix="/api/logs", tags=["Logs"])

# ============================================
#  GET — List log akses
# ============================================

@router.get("", response_model=list[AccessLogResponse])
async def list_logs(
    search: Optional[str] = Query(None, description="Cari berdasarkan nama atau UID"),
    status: Optional[str] = Query(None, description="Filter: granted / denied"),
    date_from: Optional[str] = Query(None, description="Dari tanggal (YYYY-MM-DD)"),
    date_to: Optional[str] = Query(None, description="Sampai tanggal (YYYY-MM-DD)"),
    page: int = Query(1, ge=1),
    per_page: int = Query(25, ge=1, le=100),
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin', 'viewer']))
):
    """Dapatkan history log tap kartu."""
    query = "SELECT * FROM access_logs WHERE 1=1"
    params = []

    if search:
        s = f"%{search}%"
        query += " AND (card_holder LIKE ? OR uid LIKE ? OR COALESCE(division, '') LIKE ?)"
        params.extend([s, s, s])

    if status:
        query += " AND status = ?"
        params.append(status)

    if date_from:
        query += " AND DATE(timestamp) >= ?"
        params.append(date_from)

    if date_to:
        query += " AND DATE(timestamp) <= ?"
        params.append(date_to)

    query += " ORDER BY timestamp DESC LIMIT ? OFFSET ?"
    params.extend([per_page, (page - 1) * per_page])

    cursor = await db.execute(query, params)
    rows = await cursor.fetchall()

    result = []
    for row in rows:
        is_btn = (row["uid"] or "").upper() == "BUTTON" or (row["denied_reason"] or "").lower() == "manual_unlock"
        holder = row["card_holder"] or ("Push Button (Tombol Keluar)" if is_btn else None)
        div = (row["division"] if "division" in row.keys() else None) or ("Tombol Fisik" if is_btn else None)
        lvl = "Manual" if (is_btn and (row["access_level"] or "").lower() in ("", "physical")) else row["access_level"]
        result.append(
            AccessLogResponse(
                id=row["id"],
                uid=row["uid"],
                card_holder=holder,
                employee_name=holder,
                division=div,
                access_level=lvl,
                status=row["status"],
                denied_reason=row["denied_reason"],
                door_name=row["door_name"],
                timestamp=row["timestamp"]
            )
        )
    return result

# ============================================
#  GET — Total log (untuk pagination)
# ============================================

@router.get("/count")
async def count_logs(
    search: Optional[str] = Query(None),
    status: Optional[str] = Query(None),
    date_from: Optional[str] = Query(None),
    date_to: Optional[str] = Query(None),
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin', 'viewer']))
):
    """Dapatkan total jumlah log."""
    query = "SELECT COUNT(*) as total FROM access_logs WHERE 1=1"
    params = []

    if search:
        s = f"%{search}%"
        query += " AND (card_holder LIKE ? OR uid LIKE ? OR COALESCE(division, '') LIKE ?)"
        params.extend([s, s, s])

    if status:
        query += " AND status = ?"
        params.append(status)

    if date_from:
        query += " AND DATE(timestamp) >= ?"
        params.append(date_from)

    if date_to:
        query += " AND DATE(timestamp) <= ?"
        params.append(date_to)

    cursor = await db.execute(query, params)
    row = await cursor.fetchone()
    return {"total": row["total"]}

# ============================================
#  GET — Export CSV
# ============================================

@router.get("/export")
async def export_logs_csv(
    search: Optional[str] = Query(None),
    status: Optional[str] = Query(None),
    date_from: Optional[str] = Query(None),
    date_to: Optional[str] = Query(None),
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin', 'viewer']))
):
    """Export log akses dalam format CSV (UTF-8 BOM untuk Excel)."""
    from fastapi.responses import StreamingResponse
    import csv
    import io

    query = "SELECT * FROM access_logs WHERE 1=1"
    params = []

    if search:
        s = f"%{search}%"
        query += " AND (card_holder LIKE ? OR uid LIKE ? OR COALESCE(division, '') LIKE ?)"
        params.extend([s, s, s])

    if status:
        query += " AND status = ?"
        params.append(status)

    if date_from:
        query += " AND DATE(timestamp) >= ?"
        params.append(date_from)

    if date_to:
        query += " AND DATE(timestamp) <= ?"
        params.append(date_to)

    query += " ORDER BY timestamp DESC"

    cursor = await db.execute(query, params)
    rows = await cursor.fetchall()

    # Buat CSV dengan UTF-8 BOM agar langsung rapi saat dibuka di Excel Windows
    output = io.StringIO()
    output.write("\ufeff")
    writer = csv.writer(output)
    writer.writerow(["Timestamp", "UID", "Nama Karyawan", "Divisi", "Access Level", "Status", "Alasan Denied", "Pintu"])

    for row in rows:
        is_btn = (row["uid"] or "").upper() == "BUTTON" or (row["denied_reason"] or "").lower() == "manual_unlock"
        holder = row["card_holder"] or ("Push Button (Tombol Keluar)" if is_btn else "")
        division = (row["division"] if "division" in row.keys() else None) or ("Tombol Fisik" if is_btn else "")
        lvl = "Manual" if (is_btn and (row["access_level"] or "").lower() in ("", "physical")) else (row["access_level"] or "")
        writer.writerow([
            row["timestamp"],
            row["uid"],
            holder,
            division,
            lvl,
            row["status"],
            row["denied_reason"] or "",
            row["door_name"]
        ])

    output.seek(0)
    return StreamingResponse(
        iter([output.getvalue()]),
        media_type="text/csv; charset=utf-8",
        headers={"Content-Disposition": "attachment; filename=access_logs.csv"}
    )
