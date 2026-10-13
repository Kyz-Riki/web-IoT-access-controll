from fastapi import APIRouter, Depends, HTTPException, status
from datetime import timedelta
import aiosqlite

from database import get_db
from models import UserLogin, Token, UserResponse
from auth import verify_password, create_access_token, get_current_user
from config import ACCESS_TOKEN_EXPIRE_MINUTES

router = APIRouter(prefix="/api/auth", tags=["Auth"])

@router.post("/login", response_model=Token)
async def login(form: UserLogin, db: aiosqlite.Connection = Depends(get_db)):
    """Login untuk mendapatkan JWT Token."""
    cursor = await db.execute("SELECT * FROM users WHERE username = ?", (form.username,))
    user = await cursor.fetchone()

    if not user or not verify_password(form.password, user["hashed_password"]):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Username atau password salah",
        )

    if not user["is_active"]:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail="Akun dinonaktifkan",
        )

    access_token = create_access_token(
        data={"sub": user["username"]},
        expires_delta=timedelta(minutes=ACCESS_TOKEN_EXPIRE_MINUTES),
    )

    return {"access_token": access_token, "token_type": "bearer", "role": user["role"]}

@router.get("/me", response_model=UserResponse)
async def get_me(user=Depends(get_current_user)):
    """Mendapatkan profil user yang sedang login."""
    return dict(user)
