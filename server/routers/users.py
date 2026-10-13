from fastapi import APIRouter, Depends, HTTPException, status
from typing import List
import aiosqlite

from database import get_db, now_local
from models import UserCreate, UserUpdate, UserResponse
from auth import require_role, get_password_hash

# KUNCI: Seluruh endpoint di router ini hanya boleh diakses oleh 'owner'
router = APIRouter(prefix="/api/users", tags=["Users"])

@router.get("/", response_model=List[UserResponse])
async def get_users(db: aiosqlite.Connection = Depends(get_db), current_user=Depends(require_role(["owner"]))):
    """Melihat daftar seluruh akun web."""
    cursor = await db.execute("SELECT * FROM users")
    users = await cursor.fetchall()
    return [dict(u) for u in users]

@router.post("/", response_model=UserResponse)
async def create_user(user: UserCreate, db: aiosqlite.Connection = Depends(get_db), current_user=Depends(require_role(["owner"]))):
    """Menambahkan akun web baru (admin/viewer)."""
    cursor = await db.execute("SELECT * FROM users WHERE username = ?", (user.username,))
    if await cursor.fetchone():
        raise HTTPException(status_code=400, detail="Username sudah terdaftar")
        
    hashed = get_password_hash(user.password)
    try:
        cursor = await db.execute(
            "INSERT INTO users (username, hashed_password, full_name, role, is_active, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
            (user.username, hashed, user.full_name, user.role, 1, now_local(), now_local())
        )
        await db.commit()
        new_id = cursor.lastrowid
        
        cursor = await db.execute("SELECT * FROM users WHERE id = ?", (new_id,))
        new_user = await cursor.fetchone()
        return dict(new_user)
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

@router.put("/{user_id}", response_model=UserResponse)
async def update_user(
    user_id: int, 
    update_data: UserUpdate, 
    db: aiosqlite.Connection = Depends(get_db), 
    current_user=Depends(require_role(["owner"]))
):
    """Mengubah data akun pengguna (nama, role, status aktif, ganti password)."""
    cursor = await db.execute("SELECT * FROM users WHERE id = ?", (user_id,))
    target = await cursor.fetchone()
    if not target:
        raise HTTPException(status_code=404, detail="User tidak ditemukan")

    updates = []
    params = []

    if update_data.full_name is not None and update_data.full_name.strip():
        updates.append("full_name = ?")
        params.append(update_data.full_name.strip())

    if update_data.role is not None:
        if target["role"] == "owner" and update_data.role != "owner":
            raise HTTPException(status_code=400, detail="Peran akun Owner tidak dapat diubah")
        if update_data.role not in ["owner", "admin", "viewer"]:
            raise HTTPException(status_code=400, detail="Role tidak valid")
        updates.append("role = ?")
        params.append(update_data.role)

    if update_data.is_active is not None:
        if target["id"] == current_user["id"] and not update_data.is_active:
            raise HTTPException(status_code=400, detail="Tidak dapat menonaktifkan akun yang sedang digunakan")
        updates.append("is_active = ?")
        params.append(1 if update_data.is_active else 0)

    if update_data.password is not None and update_data.password.strip():
        hashed = get_password_hash(update_data.password)
        updates.append("hashed_password = ?")
        params.append(hashed)

    if not updates:
        raise HTTPException(status_code=400, detail="Tidak ada perubahan data")

    updates.append("updated_at = ?")
    params.append(now_local())
    params.append(user_id)

    query = f"UPDATE users SET {', '.join(updates)} WHERE id = ?"
    await db.execute(query, params)
    await db.commit()

    cursor = await db.execute("SELECT * FROM users WHERE id = ?", (user_id,))
    updated_user = await cursor.fetchone()
    return dict(updated_user)

@router.delete("/{user_id}")
async def delete_user(user_id: int, db: aiosqlite.Connection = Depends(get_db), current_user=Depends(require_role(["owner"]))):
    """Menghapus akun web."""
    cursor = await db.execute("SELECT role FROM users WHERE id = ?", (user_id,))
    target = await cursor.fetchone()
    
    if not target:
        raise HTTPException(status_code=404, detail="User tidak ditemukan")
        
    if target["role"] == "owner":
        raise HTTPException(status_code=400, detail="Akun Owner tidak dapat dihapus")
        
    await db.execute("DELETE FROM users WHERE id = ?", (user_id,))
    await db.commit()
    return {"message": "User berhasil dihapus"}
