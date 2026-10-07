from fastapi import APIRouter, Depends, HTTPException, Query
from typing import Optional
import aiosqlite

from database import get_db, now_local
from models import CardCreate, CardUpdate, CardResponse

router = APIRouter(prefix="/api/cards", tags=["Cards"])

# ============================================
#  GET — List semua kartu
# ============================================

@router.get("", response_model=list[CardResponse])
async def list_cards(
    search: Optional[str] = Query(None, description="Cari berdasarkan nama atau UID"),
    access_level: Optional[str] = Query(None),
    is_active: Optional[bool] = Query(None),
    page: int = Query(1, ge=1),
    per_page: int = Query(25, ge=1, le=100),
    db: aiosqlite.Connection = Depends(get_db)
):
    """Dapatkan daftar semua kartu terdaftar."""
    query = "SELECT * FROM cards WHERE 1=1"
    params = []

    if search:
        query += " AND (card_holder LIKE ? OR uid LIKE ?)"
        params.extend([f"%{search}%", f"%{search}%"])

    if access_level:
        query += " AND access_level = ?"
        params.append(access_level)

    if is_active is not None:
        query += " AND is_active = ?"
        params.append(1 if is_active else 0)

    query += " ORDER BY created_at DESC LIMIT ? OFFSET ?"
    params.extend([per_page, (page - 1) * per_page])

    cursor = await db.execute(query, params)
    rows = await cursor.fetchall()

    return [
        CardResponse(
            id=row["id"],
            uid=row["uid"],
            card_holder=row["card_holder"],
            access_level=row["access_level"],
            is_active=bool(row["is_active"]),
            created_at=row["created_at"],
            updated_at=row["updated_at"]
        ) for row in rows
    ]

# ============================================
#  GET — Total kartu (untuk pagination)
# ============================================

@router.get("/count")
async def count_cards(
    search: Optional[str] = Query(None),
    access_level: Optional[str] = Query(None),
    is_active: Optional[bool] = Query(None),
    db: aiosqlite.Connection = Depends(get_db)
):
    """Dapatkan total jumlah kartu."""
    query = "SELECT COUNT(*) as total FROM cards WHERE 1=1"
    params = []

    if search:
        query += " AND (card_holder LIKE ? OR uid LIKE ?)"
        params.extend([f"%{search}%", f"%{search}%"])

    if access_level:
        query += " AND access_level = ?"
        params.append(access_level)

    if is_active is not None:
        query += " AND is_active = ?"
        params.append(1 if is_active else 0)

    cursor = await db.execute(query, params)
    row = await cursor.fetchone()
    return {"total": row["total"]}

# ============================================
#  POST — Tambah kartu baru
# ============================================

@router.post("", response_model=CardResponse, status_code=201)
async def create_card(
    card: CardCreate,
    db: aiosqlite.Connection = Depends(get_db)
):
    """Daftarkan kartu RFID baru."""
    # Cek apakah UID sudah terdaftar
    cursor = await db.execute("SELECT id FROM cards WHERE uid = ?", (card.uid.upper(),))
    existing = await cursor.fetchone()
    if existing:
        raise HTTPException(status_code=409, detail=f"Kartu dengan UID '{card.uid}' sudah terdaftar")

    # Validasi access level
    if not card.access_level.strip():
        raise HTTPException(status_code=400, detail="Access level tidak boleh kosong.")
    
    # Simpan dengan strip agar rapi
    level_to_save = card.access_level.strip()

    await db.execute(
        """INSERT INTO cards (uid, card_holder, access_level, created_at, updated_at)
           VALUES (?, ?, ?, ?, ?)""",
        (card.uid.upper(), card.card_holder, level_to_save, now_local(), now_local())
    )
    await db.commit()

    # Ambil data yang baru dibuat
    cursor = await db.execute("SELECT * FROM cards WHERE uid = ?", (card.uid.upper(),))
    row = await cursor.fetchone()

    return CardResponse(
        id=row["id"],
        uid=row["uid"],
        card_holder=row["card_holder"],
        access_level=row["access_level"],
        is_active=bool(row["is_active"]),
        created_at=row["created_at"],
        updated_at=row["updated_at"]
    )

# ============================================
#  PUT — Edit kartu
# ============================================

@router.put("/{card_id}", response_model=CardResponse)
async def update_card(
    card_id: int,
    card: CardUpdate,
    db: aiosqlite.Connection = Depends(get_db)
):
    """Update data kartu (nama, access level, status aktif)."""
    # Cek kartu ada
    cursor = await db.execute("SELECT * FROM cards WHERE id = ?", (card_id,))
    existing = await cursor.fetchone()
    if not existing:
        raise HTTPException(status_code=404, detail="Kartu tidak ditemukan")

    # Build update query secara dinamis
    updates = []
    params = []

    if card.card_holder is not None:
        updates.append("card_holder = ?")
        params.append(card.card_holder)

    if card.access_level is not None:
        if not card.access_level.strip():
            raise HTTPException(status_code=400, detail="Access level tidak boleh kosong.")
        updates.append("access_level = ?")
        params.append(card.access_level.strip())

    if card.is_active is not None:
        updates.append("is_active = ?")
        params.append(1 if card.is_active else 0)

    if not updates:
        raise HTTPException(status_code=400, detail="Tidak ada field yang diupdate")

    updates.append("updated_at = ?")
    params.append(now_local())
    params.append(card_id)

    query = f"UPDATE cards SET {', '.join(updates)} WHERE id = ?"
    await db.execute(query, params)
    await db.commit()

    # Ambil data yang sudah diupdate
    cursor = await db.execute("SELECT * FROM cards WHERE id = ?", (card_id,))
    row = await cursor.fetchone()

    return CardResponse(
        id=row["id"],
        uid=row["uid"],
        card_holder=row["card_holder"],
        access_level=row["access_level"],
        is_active=bool(row["is_active"]),
        created_at=row["created_at"],
        updated_at=row["updated_at"]
    )

# ============================================
#  DELETE — Hapus kartu
# ============================================

@router.delete("/{card_id}")
async def delete_card(
    card_id: int,
    db: aiosqlite.Connection = Depends(get_db)
):
    """Hapus kartu dari sistem."""
    cursor = await db.execute("SELECT id FROM cards WHERE id = ?", (card_id,))
    existing = await cursor.fetchone()
    if not existing:
        raise HTTPException(status_code=404, detail="Kartu tidak ditemukan")

    await db.execute("DELETE FROM cards WHERE id = ?", (card_id,))
    await db.commit()

    return {"message": "Kartu berhasil dihapus", "id": card_id}
