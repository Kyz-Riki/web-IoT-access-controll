# Panduan Menjalankan Server IoT Access Control

Dokumentasi ini berisi perintah langkah demi langkah (step-by-step) untuk menjalankan backend (FastAPI) pada sistem operasi Windows dan Linux Mint.

---

## 🪟 PENGGUNA WINDOWS (PowerShell)

**1. Membuat Virtual Environment (Hanya pertama kali)**
Jalankan perintah ini di root folder (`Aplikasi`):
```powershell
python -m venv venv
```

**2. Menghidupkan / Mengaktifkan Venv**
Wajib dilakukan setiap membuka terminal baru:
```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy RemoteSigned
.\venv\Scripts\Activate.ps1
```

**3. Menginstall Dependensi**
Masuk ke folder server dan install library yang dibutuhkan:
```powershell
cd server
pip install -r requirements.txt
```

**4. Mengaktifkan Server**
Pastikan posisi terminal ada di folder `server` dan venv aktif:
```powershell
python main.py
```

---

## 🐧 PENGGUNA LINUX MINT / UBUNTU (Terminal / Bash)

**1. Membuat Virtual Environment (Hanya pertama kali)**
Jalankan perintah ini di root folder (`Aplikasi`):
*(Jika error, pastikan sudah menjalankan `sudo apt install python3-venv`)*
```bash
python3 -m venv venv
```

**2. Menghidupkan / Mengaktifkan Venv**
Wajib dilakukan setiap membuka terminal baru:
```bash
source venv/bin/activate
```

**3. Menginstall Dependensi**
Masuk ke folder server dan install library yang dibutuhkan:
```bash
cd server
pip install -r requirements.txt
```

**4. Mengaktifkan Server**
Pastikan posisi terminal ada di folder `server` dan venv aktif:
```bash
python3 main.py
```

---

## 🌐 AKSES DASHBOARD
Jika server berhasil berjalan, buka browser dan kunjungi:
👉 **http://localhost:8000**
