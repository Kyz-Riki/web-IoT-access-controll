from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Depends
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse, Response
from contextlib import asynccontextmanager
from datetime import datetime
from typing import List
import json
import logging
import aiosqlite


# ============================================
#  Filter log: sembunyikan GET /favicon.ico
# ============================================

class FaviconLogFilter(logging.Filter):
    def filter(self, record: logging.LogRecord) -> bool:
        message = record.getMessage()
        if "favicon" in message.lower():
            return False
        return True

logging.getLogger("uvicorn.access").addFilter(FaviconLogFilter())

from config import HOST, PORT, HEARTBEAT_TIMEOUT_SEC
from database import init_db, get_db

# Import routers
from routers import cards, logs, schedules, doors, esp

# ============================================
#  WebSocket Manager (real-time push ke dashboard)
# ============================================

class WebSocketManager:
    def __init__(self):
        self.active_connections: List[WebSocket] = []

    async def connect(self, websocket: WebSocket):
        await websocket.accept()
        self.active_connections.append(websocket)
        print(f"[WS] Client terhubung. Total: {len(self.active_connections)}")

    def disconnect(self, websocket: WebSocket):
        self.active_connections.remove(websocket)
        print(f"[WS] Client terputus. Total: {len(self.active_connections)}")

    async def broadcast(self, data: dict):
        """Kirim data ke semua client WebSocket yang terhubung."""
        message = json.dumps(data, default=str)
        disconnected = []
        for connection in self.active_connections:
            try:
                await connection.send_text(message)
            except Exception:
                disconnected.append(connection)
        for conn in disconnected:
            self.active_connections.remove(conn)


ws_manager = WebSocketManager()

# ============================================
#  App Lifecycle
# ============================================

@asynccontextmanager
async def lifespan(app: FastAPI):
    """Startup & shutdown events."""
    # Startup
    await init_db()
    esp.set_ws_manager(ws_manager)
    print("=========================================")
    print("  IoT RFID Door Access Control — Server")
    print(f"  Running on http://{HOST}:{PORT}")
    print("  Docs: http://localhost:8000/docs")
    print("=========================================")
    yield
    # Shutdown
    print("[Server] Shutting down...")

# ============================================
#  FastAPI App
# ============================================

app = FastAPI(
    title="IoT RFID Door Access Control",
    description="Backend API untuk sistem kontrol akses pintu berbasis RFID",
    version="1.0.0",
    lifespan=lifespan
)

# Register routers
app.include_router(cards.router)
app.include_router(logs.router)
app.include_router(schedules.router)
app.include_router(doors.router)
app.include_router(esp.router)

# Tangani request favicon.ico dari browser (supaya tidak 404)
@app.get("/favicon.ico", include_in_schema=False)
async def favicon():
    return Response(status_code=204)

# ============================================
#  WebSocket endpoint
# ============================================

@app.websocket("/ws/live")
async def websocket_endpoint(websocket: WebSocket):
    await ws_manager.connect(websocket)
    try:
        while True:
            # Keep connection alive, tunggu message dari client (jika ada)
            data = await websocket.receive_text()
            # Client bisa kirim ping
            if data == "ping":
                await websocket.send_text(json.dumps({"type": "pong"}))
    except WebSocketDisconnect:
        ws_manager.disconnect(websocket)

# ============================================
#  Dashboard Stats API
# ============================================

@app.get("/api/dashboard/stats", tags=["Dashboard"])
async def get_dashboard_stats(db: aiosqlite.Connection = Depends(get_db)):
    """Statistik untuk halaman home dashboard."""
    today = datetime.now().strftime("%Y-%m-%d")

    # Total tap hari ini
    cursor = await db.execute(
        "SELECT COUNT(*) as total FROM access_logs WHERE DATE(timestamp) = ?", (today,)
    )
    total_taps = (await cursor.fetchone())["total"]

    # Granted hari ini
    cursor = await db.execute(
        "SELECT COUNT(*) as total FROM access_logs WHERE DATE(timestamp) = ? AND status = 'granted'", (today,)
    )
    granted = (await cursor.fetchone())["total"]

    # Denied hari ini
    cursor = await db.execute(
        "SELECT COUNT(*) as total FROM access_logs WHERE DATE(timestamp) = ? AND status = 'denied'", (today,)
    )
    denied = (await cursor.fetchone())["total"]

    # Total kartu
    cursor = await db.execute("SELECT COUNT(*) as total FROM cards")
    total_cards = (await cursor.fetchone())["total"]

    # Kartu aktif
    cursor = await db.execute("SELECT COUNT(*) as total FROM cards WHERE is_active = 1")
    active_cards = (await cursor.fetchone())["total"]

    # 5 log terakhir
    cursor = await db.execute(
        "SELECT * FROM access_logs ORDER BY timestamp DESC LIMIT 5"
    )
    recent_rows = await cursor.fetchall()
    recent_logs = [
        {
            "uid": row["uid"],
            "card_holder": row["card_holder"] or "Unknown",
            "status": row["status"],
            "denied_reason": row["denied_reason"],
            "door_name": row["door_name"],
            "timestamp": row["timestamp"]
        }
        for row in recent_rows
    ]

    # Status door
    cursor = await db.execute("SELECT * FROM doors")
    door_rows = await cursor.fetchall()
    doors_status = []
    for row in door_rows:
        # Cek apakah ESP32 masih online berdasarkan heartbeat terakhir
        is_online = False
        if row["last_heartbeat"]:
            try:
                last_hb = datetime.fromisoformat(row["last_heartbeat"])
                diff = (datetime.now() - last_hb).total_seconds()
                is_online = diff < HEARTBEAT_TIMEOUT_SEC
            except (ValueError, TypeError):
                is_online = False

            # Update status online di DB jika berubah
            if not is_online and row["is_online"]:
                await db.execute(
                    "UPDATE doors SET is_online = 0 WHERE id = ?", (row["id"],)
                )
                await db.commit()

        doors_status.append({
            "id": row["id"],
            "name": row["name"],
            "esp_id": row["esp_id"],
            "current_state": row["current_state"],
            "is_online": is_online,
            "wifi_rssi": row["wifi_rssi"],
            "uptime": row["uptime"],
            "last_heartbeat": row["last_heartbeat"],
            "last_activity": row["last_activity"]
        })

    return {
        "total_taps_today": total_taps,
        "granted_today": granted,
        "denied_today": denied,
        "total_cards": total_cards,
        "active_cards": active_cards,
        "recent_logs": recent_logs,
        "doors": doors_status
    }

# ============================================
#  Serve Frontend (static files)
# ============================================

# Mount static files (CSS, JS)
app.mount("/static", StaticFiles(directory="static"), name="static")

# Serve HTML pages
@app.get("/", tags=["Frontend"])
async def serve_home():
    return FileResponse("static/index.html")

@app.get("/cards", tags=["Frontend"])
async def serve_cards():
    return FileResponse("static/cards.html")

@app.get("/logs", tags=["Frontend"])
async def serve_logs():
    return FileResponse("static/logs.html")

@app.get("/schedules", tags=["Frontend"])
async def serve_schedule():
    return FileResponse("static/schedule.html")

# ============================================
#  Run with: uvicorn main:app --host 0.0.0.0 --port 8000 --reload
# ============================================

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host=HOST, port=PORT, reload=True)
