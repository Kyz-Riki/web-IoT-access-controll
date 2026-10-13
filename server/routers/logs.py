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
        query += " AND (card_holder LIKE ? OR uid LIKE ?)"
        params.extend([f"%{search}%", f"%{search}%"])

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

    return [
        AccessLogResponse(
            id=row["id"],
            uid=row["uid"],
            card_holder=row["card_holder"],
            access_level=row["access_level"],
            status=row["status"],
            denied_reason=row["denied_reason"],
            door_name=row["door_name"],
            timestamp=row["timestamp"]
        ) for row in rows
    ]

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
        query += " AND (card_holder LIKE ? OR uid LIKE ?)"
        params.extend([f"%{search}%", f"%{search}%"])

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
    date_from: Optional[str] = Query(None),
    date_to: Optional[str] = Query(None),
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin', 'viewer']))
):
    """Export log akses dalam format CSV."""
    from fastapi.responses import StreamingResponse
    import io

    query = "SELECT * FROM access_logs WHERE 1=1"
    params = []

    if date_from:
        query += " AND DATE(timestamp) >= ?"
        params.append(date_from)

    if date_to:
        query += " AND DATE(timestamp) <= ?"
        params.append(date_to)

    query += " ORDER BY timestamp DESC"

    cursor = await db.execute(query, params)
    rows = await cursor.fetchall()

    # Buat CSV
    output = io.StringIO()
    output.write("Timestamp,UID,Nama,Access Level,Status,Alasan Denied,Pintu\n")

    for row in rows:
        output.write(
            f'"{row["timestamp"]}","{row["uid"]}","{row["card_holder"] or ""}","'
            f'{row["access_level"] or ""}","{row["status"]}","'
            f'{row["denied_reason"] or ""}","{row["door_name"]}"\n'
        )

    output.seek(0)
    return StreamingResponse(
        iter([output.getvalue()]),
        media_type="text/csv",
        headers={"Content-Disposition": "attachment; filename=access_logs.csv"}
    )
