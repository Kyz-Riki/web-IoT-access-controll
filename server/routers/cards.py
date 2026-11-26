from fastapi import APIRouter, Depends, HTTPException, Query, UploadFile, File, Form
from fastapi.responses import StreamingResponse
from typing import Optional
import aiosqlite
import csv
import io
import openpyxl
from openpyxl.styles import Font, PatternFill

from auth import require_role
from database import get_db, now_local
from models import (
    CardCreate, CardUpdate, CardResponse,
    CardImportCommitRequest, CardImportResult, BulkLevelUpdate
)

router = APIRouter(prefix="/api/cards", tags=["Cards"])

MAX_IMPORT_BYTES = 2 * 1024 * 1024  # 2 MB
MAX_IMPORT_ROWS = 5000

# Kata kunci deteksi otomatis header & kolom
_NAME_KEYWORDS = [
    "employee_name", "nama_karyawan", "nama karyawan", "nama lengkap",
    "full name", "fullname", "nama", "employee", "karyawan", "name"
]
_DIV_KEYWORDS = [
    "division", "divisi", "departemen", "department", "dept", "bagian", "unit"
]
_LEVEL_KEYWORDS = [
    "access_level", "level_akses", "level akses", "access level", "jadwal", "schedule", "level"
]
_ALL_HEADER_HINTS = set(_NAME_KEYWORDS + _DIV_KEYWORDS + _LEVEL_KEYWORDS + [
    "no", "no.", "nik", "nip", "id", "badge", "jabatan", "posisi", "email", "status"
])


# ============================================
#  Helpers
# ============================================

def _row_to_card_response(row) -> CardResponse:
    holder = row["card_holder"]
    division = row["division"] if "division" in row.keys() else None
    return CardResponse(
        id=row["id"],
        uid=row["uid"],
        card_holder=holder,
        employee_name=holder,
        division=division,
        access_level=row["access_level"],
        is_active=bool(row["is_active"]),
        created_at=row["created_at"],
        updated_at=row["updated_at"]
    )


def _apply_card_filters(
    query: str,
    params: list,
    search: Optional[str],
    division: Optional[str],
    access_level: Optional[str],
    is_active: Optional[bool],
    card_status: Optional[str]
):
    if search:
        s = f"%{search.strip()}%"
        query += " AND (card_holder LIKE ? OR COALESCE(uid, '') LIKE ? OR COALESCE(division, '') LIKE ?)"
        params.extend([s, s, s])

    if division:
        if division == "__NONE__":
            query += " AND (division IS NULL OR TRIM(division) = '')"
        else:
            query += " AND division = ?"
            params.append(division)

    if access_level:
        query += " AND LOWER(access_level) = LOWER(?)"
        params.append(access_level)

    if is_active is not None:
        query += " AND is_active = ?"
        params.append(1 if is_active else 0)

    if card_status == "registered":
        query += " AND uid IS NOT NULL AND TRIM(uid) != ''"
    elif card_status == "unassigned":
        query += " AND (uid IS NULL OR TRIM(uid) = '')"

    return query, params


def _clean_cell(val) -> str:
    if val is None:
        return ""
    if isinstance(val, float) and val.is_integer():
        return str(int(val)).strip()
    return str(val).strip()


def _detect_header_row(raw_rows: list[list[str]]) -> int:
    """Deteksi indeks baris header (0-based) dari 15 baris pertama."""
    best_idx = 0
    best_score = -1
    first_non_empty = 0
    found_non_empty = False

    for idx, row in enumerate(raw_rows[:15]):
        non_empty = [c for c in row if c]
        if not non_empty:
            continue
        if not found_non_empty:
            first_non_empty = idx
            found_non_empty = True

        score = 0
        for cell in non_empty:
            low = cell.lower().strip()
            if low in _ALL_HEADER_HINTS:
                score += 3
            elif any(k in low for k in ("nama", "name", "divis", "depart", "bagian", "level", "jadwal")):
                score += 2
        # Bonus kecil jika punya >= 2 kolom terisi (menghindari baris kop judul tunggal)
        if len(non_empty) >= 2 and score > 0:
            score += 1

        if score > best_score:
            best_score = score
            best_idx = idx

    return best_idx if best_score > 0 else first_non_empty


def _detect_column_index(headers: list[str], keywords: list[str], exclude: set[int]) -> int:
    """Cari indeks kolom paling cocok berdasarkan daftar kata kunci."""
    lowered = [h.lower().strip() for h in headers]

    # 1. Exact match terlebih dulu
    for kw in keywords:
        for idx, h in enumerate(lowered):
            if idx in exclude:
                continue
            if h == kw:
                return idx

    # 2. Substring match
    for kw in keywords:
        for idx, h in enumerate(lowered):
            if idx in exclude:
                continue
            if kw in h:
                return idx

    return -1


def _read_xlsx_rows(content: bytes, sheet_name: Optional[str]):
    try:
        wb = openpyxl.load_workbook(io.BytesIO(content), data_only=True)
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"File Excel (.xlsx) tidak valid atau rusak: {e}")

    sheets = wb.sheetnames
    if not sheets:
        raise HTTPException(status_code=400, detail="File Excel tidak memiliki sheet.")

    active_sheet = sheet_name if (sheet_name and sheet_name in sheets) else sheets[0]
    ws = wb[active_sheet]

    # Unmerge cell: salin nilai top-left ke seluruh area merge
    merged_map = {}
    for merged_range in ws.merged_cells.ranges:
        top_left_val = ws.cell(merged_range.min_row, merged_range.min_col).value
        for r in range(merged_range.min_row, merged_range.max_row + 1):
            for c in range(merged_range.min_col, merged_range.max_col + 1):
                merged_map[(r, c)] = top_left_val

    max_row = min(ws.max_row or 0, MAX_IMPORT_ROWS + 50)
    max_col = min(ws.max_column or 0, 50)

    raw_rows = []
    for r in range(1, max_row + 1):
        row_vals = []
        for c in range(1, max_col + 1):
            val = merged_map.get((r, c), ws.cell(r, c).value)
            row_vals.append(_clean_cell(val))
        raw_rows.append(row_vals)

    wb.close()
    return sheets, active_sheet, raw_rows


def _read_csv_rows(content: bytes):
    try:
        text = content.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = content.decode("latin-1")

    lines = [ln for ln in text.splitlines() if ln.strip()]
    if not lines:
        raise HTTPException(status_code=400, detail="File CSV kosong.")

    sample = "\n".join(lines[:10])
    delimiter = ","
    try:
        dialect = csv.Sniffer().sniff(sample, delimiters=",;\t")
        delimiter = dialect.delimiter
    except csv.Error:
        first_line = lines[0]
        if first_line.count(";") > first_line.count(","):
            delimiter = ";"

    reader = csv.reader(io.StringIO(text), delimiter=delimiter)
    raw_rows = []
    for idx, row in enumerate(reader):
        if idx >= MAX_IMPORT_ROWS + 50:
            break
        raw_rows.append([_clean_cell(c) for c in row])

    return ["CSV"], "CSV", raw_rows


# ============================================
#  GET — List semua kartu & karyawan
# ============================================

@router.get("", response_model=list[CardResponse])
async def list_cards(
    search: Optional[str] = Query(None, description="Cari berdasarkan nama, divisi, atau UID"),
    division: Optional[str] = Query(None, description="Filter divisi (atau '__NONE__' untuk tanpa divisi)"),
    access_level: Optional[str] = Query(None),
    is_active: Optional[bool] = Query(None),
    card_status: Optional[str] = Query("all", description="all | registered | unassigned"),
    page: int = Query(1, ge=1),
    per_page: int = Query(25, ge=1, le=200),
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin', 'viewer']))
):
    """Dapatkan daftar semua kartu dan karyawan terdaftar."""
    query = "SELECT * FROM cards WHERE 1=1"
    params = []
    query, params = _apply_card_filters(query, params, search, division, access_level, is_active, card_status)

    query += " ORDER BY CASE WHEN uid IS NULL OR TRIM(uid) = '' THEN 1 ELSE 0 END ASC, updated_at DESC, id DESC LIMIT ? OFFSET ?"
    params.extend([per_page, (page - 1) * per_page])

    cursor = await db.execute(query, params)
    rows = await cursor.fetchall()
    return [_row_to_card_response(row) for row in rows]


# ============================================
#  GET — Total kartu (untuk pagination)
# ============================================

@router.get("/count")
async def count_cards(
    search: Optional[str] = Query(None),
    division: Optional[str] = Query(None),
    access_level: Optional[str] = Query(None),
    is_active: Optional[bool] = Query(None),
    card_status: Optional[str] = Query("all"),
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin', 'viewer']))
):
    """Dapatkan total jumlah baris sesuai filter."""
    query = "SELECT COUNT(*) as total FROM cards WHERE 1=1"
    params = []
    query, params = _apply_card_filters(query, params, search, division, access_level, is_active, card_status)

    cursor = await db.execute(query, params)
    row = await cursor.fetchone()
    return {"total": row["total"]}


# ============================================
#  GET — Ringkasan jumlah status kartu (tabs)
# ============================================

@router.get("/summary")
async def cards_summary(
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin', 'viewer']))
):
    """Ringkasan total semua baris, sudah ber-UID, dan belum ber-UID."""
    cursor = await db.execute("""
        SELECT
            COUNT(*) as total_all,
            SUM(CASE WHEN uid IS NOT NULL AND TRIM(uid) != '' THEN 1 ELSE 0 END) as registered,
            SUM(CASE WHEN uid IS NULL OR TRIM(uid) = '' THEN 1 ELSE 0 END) as unassigned
        FROM cards
    """)
    row = await cursor.fetchone()
    return {
        "all": row["total_all"] or 0,
        "registered": row["registered"] or 0,
        "unassigned": row["unassigned"] or 0
    }


# ============================================
#  GET — List level akses unik dari kartu
# ============================================

@router.get("/levels", response_model=list[str])
async def list_card_levels(
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin', 'viewer']))
):
    """Dapatkan daftar nama unique level akses dari kartu terdaftar."""
    cursor = await db.execute("SELECT DISTINCT access_level FROM cards ORDER BY access_level")
    rows = await cursor.fetchall()
    return [row["access_level"] for row in rows if row["access_level"]]


# ============================================
#  GET — List divisi unik dari kartu
# ============================================

@router.get("/divisions", response_model=list[str])
async def list_card_divisions(
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin', 'viewer']))
):
    """Dapatkan daftar nama divisi unik yang ada di tabel kartu."""
    cursor = await db.execute(
        "SELECT DISTINCT division FROM cards WHERE division IS NOT NULL AND TRIM(division) != '' ORDER BY division"
    )
    rows = await cursor.fetchall()
    return [row["division"] for row in rows if row["division"]]


# ============================================
#  GET — List karyawan belum punya kartu (UID NULL)
# ============================================

@router.get("/unassigned", response_model=list[CardResponse])
async def list_unassigned_cards(
    search: Optional[str] = Query(None),
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin']))
):
    """Dapatkan semua karyawan yang belum memiliki UID kartu (untuk pencarian cepat saat scan)."""
    query = "SELECT * FROM cards WHERE (uid IS NULL OR TRIM(uid) = '')"
    params = []
    if search:
        s = f"%{search.strip()}%"
        query += " AND (card_holder LIKE ? OR COALESCE(division, '') LIKE ?)"
        params.extend([s, s])
    query += " ORDER BY card_holder COLLATE NOCASE ASC"

    cursor = await db.execute(query, params)
    rows = await cursor.fetchall()
    return [_row_to_card_response(row) for row in rows]


# ============================================
#  GET — Unduh template Excel / CSV
# ============================================

@router.get("/template")
async def download_import_template(
    format: str = Query("xlsx", description="Format template: xlsx atau csv"),
    current_user = Depends(require_role(['owner', 'admin']))
):
    """Unduh template file import karyawan (Excel .xlsx atau .csv)."""
    sample_rows = [
        ("Budi Santoso", "Engineering"),
        ("Siti Aminah", "Finance & Accounting"),
        ("Ahmad Fauzi", "Produksi"),
        ("Dewi Lestari", "HRD"),
    ]

    if format.lower() == "csv":
        output = io.StringIO()
        output.write("\ufeff")  # UTF-8 BOM agar rapi di Excel Windows
        writer = csv.writer(output)
        writer.writerow(["employee_name", "division"])
        for row in sample_rows:
            writer.writerow(row)
        output.seek(0)
        return StreamingResponse(
            iter([output.getvalue()]),
            media_type="text/csv; charset=utf-8",
            headers={"Content-Disposition": "attachment; filename=template_karyawan.csv"}
        )

    # Default: Excel (.xlsx)
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = "Data Karyawan"

    headers = ["Nama Karyawan", "Divisi"]
    ws.append(headers)
    header_fill = PatternFill(start_color="002062", end_color="002062", fill_type="solid")
    header_font = Font(bold=True, color="FFFFFF")
    for col_idx in range(1, 3):
        cell = ws.cell(row=1, column=col_idx)
        cell.fill = header_fill
        cell.font = header_font

    for row in sample_rows:
        ws.append(list(row))

    ws.column_dimensions["A"].width = 28
    ws.column_dimensions["B"].width = 24

    buf = io.BytesIO()
    wb.save(buf)
    buf.seek(0)
    return StreamingResponse(
        buf,
        media_type="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
        headers={"Content-Disposition": "attachment; filename=template_karyawan.xlsx"}
    )


# ============================================
#  POST — Parse file Excel/CSV untuk Editable Preview
# ============================================

@router.post("/import/parse")
async def parse_import_file(
    file: UploadFile = File(...),
    sheet_name: Optional[str] = Form(None),
    header_row: Optional[int] = Form(None),
    name_col: Optional[int] = Form(None),
    division_col: Optional[int] = Form(None),
    level_col: Optional[int] = Form(None),
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin']))
):
    """
    Parse file .xlsx atau .csv, auto-detect kolom, unmerge cell,
    auto-match Divisi <-> Jadwal Akses, dan tandai duplikat untuk Editable Preview.
    """
    filename = (file.filename or "").strip()
    lower_name = filename.lower()
    if not (lower_name.endswith(".xlsx") or lower_name.endswith(".csv")):
        raise HTTPException(
            status_code=400,
            detail="Format file tidak didukung. Gunakan file Excel (.xlsx) atau CSV (.csv)."
        )

    content = await file.read()
    if not content:
        raise HTTPException(status_code=400, detail="File kosong.")
    if len(content) > MAX_IMPORT_BYTES:
        raise HTTPException(status_code=400, detail="Ukuran file terlalu besar (maksimal 2 MB).")

    if lower_name.endswith(".xlsx"):
        file_type = "xlsx"
        sheets, active_sheet, raw_rows = _read_xlsx_rows(content, sheet_name)
    else:
        file_type = "csv"
        sheets, active_sheet, raw_rows = _read_csv_rows(content)

    if not raw_rows:
        raise HTTPException(status_code=400, detail="Tidak ada data yang dapat dibaca dari file.")

    # Tentukan baris header (1-based dari user → 0-based internal)
    if header_row is not None and 1 <= header_row <= len(raw_rows):
        hdr_idx = header_row - 1
    else:
        hdr_idx = _detect_header_row(raw_rows)

    raw_header = raw_rows[hdr_idx] if hdr_idx < len(raw_rows) else []
    max_cols = max((len(r) for r in raw_rows[hdr_idx:hdr_idx + 20]), default=len(raw_header))
    headers = []
    for i in range(max_cols):
        label = raw_header[i] if i < len(raw_header) and raw_header[i] else f"Kolom {i + 1}"
        headers.append(label)

    # Deteksi / gunakan pilihan kolom
    used_cols: set[int] = set()
    if name_col is not None and 0 <= name_col < len(headers):
        col_name = name_col
    else:
        col_name = _detect_column_index(headers, _NAME_KEYWORDS, used_cols)
        if col_name == -1:
            col_name = 0 if len(headers) > 0 else -1
    if col_name >= 0:
        used_cols.add(col_name)

    if division_col is not None:
        col_div = division_col if (0 <= division_col < len(headers)) else -1
    else:
        col_div = _detect_column_index(headers, _DIV_KEYWORDS, used_cols)
    if col_div >= 0:
        used_cols.add(col_div)

    if level_col is not None:
        col_lvl = level_col if (0 <= level_col < len(headers)) else -1
    else:
        col_lvl = _detect_column_index(headers, _LEVEL_KEYWORDS, used_cols)

    # Ambil jadwal yang sudah tersedia di server untuk auto-matching
    cursor = await db.execute("SELECT DISTINCT access_level FROM schedules ORDER BY access_level")
    sched_rows = await cursor.fetchall()
    schedule_levels = [r["access_level"] for r in sched_rows if r["access_level"]]

    level_map = {"admin": "admin"}
    for lvl in schedule_levels:
        level_map[lvl.strip().lower()] = lvl.strip()

    # Ambil data existing di DB untuk deteksi duplikat
    cursor = await db.execute(
        "SELECT LOWER(TRIM(card_holder)) as nm, LOWER(TRIM(COALESCE(division, ''))) as dv FROM cards"
    )
    db_existing = {f"{r['nm']}\t{r['dv']}" for r in await cursor.fetchall()}

    parsed_rows = []
    warnings = []
    seen_in_file = set()

    for idx in range(hdr_idx + 1, len(raw_rows)):
        row = raw_rows[idx]
        if not any(c for c in row):
            continue  # Skip baris benar-benar kosong

        if len(parsed_rows) >= MAX_IMPORT_ROWS:
            warnings.append(f"Batas maksimal {MAX_IMPORT_ROWS} baris tercapai; sisa baris dilewati.")
            break

        excel_row_num = idx + 1
        emp_name = row[col_name].strip() if (0 <= col_name < len(row)) else ""
        emp_div = row[col_div].strip() if (0 <= col_div < len(row)) else ""
        raw_lvl = row[col_lvl].strip() if (0 <= col_lvl < len(row)) else ""

        if not emp_name:
            warnings.append(f"Baris {excel_row_num}: Nama karyawan kosong (dilewati).")
            continue

        # Auto-matching Level Akses
        access_level = ""
        auto_matched = False
        if raw_lvl:
            low_lvl = raw_lvl.lower()
            if low_lvl in level_map:
                access_level = level_map[low_lvl]
                auto_matched = True
            else:
                access_level = raw_lvl
        elif emp_div and emp_div.lower() in level_map:
            access_level = level_map[emp_div.lower()]
            auto_matched = True

        key = f"{emp_name.lower()}\t{emp_div.lower()}"
        duplicate_source = None
        if key in db_existing:
            duplicate_source = "database"
        elif key in seen_in_file:
            duplicate_source = "file"
        seen_in_file.add(key)

        parsed_rows.append({
            "row_num": excel_row_num,
            "employee_name": emp_name,
            "division": emp_div,
            "access_level": access_level,
            "auto_matched": auto_matched,
            "is_duplicate": duplicate_source is not None,
            "duplicate_source": duplicate_source
        })

    return {
        "filename": filename,
        "file_type": file_type,
        "sheets": sheets,
        "active_sheet": active_sheet,
        "header_row": hdr_idx + 1,
        "total_raw_rows": len(raw_rows),
        "headers": headers,
        "detected_cols": {
            "name_col": col_name,
            "division_col": col_div,
            "level_col": col_lvl
        },
        "available_levels": ["admin"] + [lvl for lvl in schedule_levels if lvl.lower() != "admin"],
        "db_existing_keys": list(db_existing),
        "rows": parsed_rows,
        "warnings": warnings[:30]
    }


# ============================================
#  POST — Commit hasil Editable Preview ke database
# ============================================

@router.post("/import/commit", response_model=CardImportResult)
async def commit_import_cards(
    payload: CardImportCommitRequest,
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin']))
):
    """Simpan daftar karyawan hasil Editable Preview ke tabel cards (dengan uid = NULL)."""
    if not payload.items:
        raise HTTPException(status_code=400, detail="Tidak ada data yang dikirim untuk diimpor.")
    if len(payload.items) > MAX_IMPORT_ROWS:
        raise HTTPException(status_code=400, detail=f"Maksimal {MAX_IMPORT_ROWS} baris per import.")

    cursor = await db.execute(
        "SELECT LOWER(TRIM(card_holder)) as nm, LOWER(TRIM(COALESCE(division, ''))) as dv FROM cards"
    )
    db_existing = {(r["nm"], r["dv"]) for r in await cursor.fetchall()}

    seen_in_batch = set()
    imported = 0
    skipped_duplicate = 0
    skipped_invalid = 0
    errors = []
    ts = now_local()

    for idx, item in enumerate(payload.items, start=1):
        name = (item.employee_name or "").strip()
        division = (item.division or "").strip() or None
        level = (item.access_level or "").strip()

        if not name:
            skipped_invalid += 1
            errors.append(f"Baris #{idx}: Nama karyawan tidak boleh kosong.")
            continue

        if not level:
            skipped_invalid += 1
            errors.append(f"Baris #{idx} ({name}): Level akses belum dipilih.")
            continue

        key = (name.lower(), (division or "").lower())
        if key in db_existing or key in seen_in_batch:
            skipped_duplicate += 1
            continue

        await db.execute(
            """INSERT INTO cards (uid, card_holder, division, access_level, is_active, created_at, updated_at)
               VALUES (NULL, ?, ?, ?, 1, ?, ?)""",
            (name, division, level, ts, ts)
        )
        seen_in_batch.add(key)
        imported += 1

    await db.commit()

    return CardImportResult(
        imported=imported,
        skipped_duplicate=skipped_duplicate,
        skipped_invalid=skipped_invalid,
        errors=errors[:30]
    )


# ============================================
#  PUT — Ubah level akses massal per divisi
# ============================================

@router.put("/bulk-level")
async def bulk_update_level_by_division(
    req: BulkLevelUpdate,
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin']))
):
    """Ubah level akses seluruh karyawan/kartu dalam satu divisi sekaligus."""
    new_level = (req.access_level or "").strip()
    if not new_level:
        raise HTTPException(status_code=400, detail="Level akses baru tidak boleh kosong.")

    ts = now_local()
    div = (req.division or "").strip()
    if not div or div == "__NONE__":
        cursor = await db.execute(
            "UPDATE cards SET access_level = ?, updated_at = ? WHERE division IS NULL OR TRIM(division) = ''",
            (new_level, ts)
        )
        label = "(Tanpa Divisi)"
    else:
        cursor = await db.execute(
            "UPDATE cards SET access_level = ?, updated_at = ? WHERE division = ?",
            (new_level, ts, div)
        )
        label = div

    await db.commit()
    return {
        "message": f"Berhasil memperbarui level akses {cursor.rowcount} data pada divisi '{label}' menjadi '{new_level}'.",
        "updated_count": cursor.rowcount
    }


# ============================================
#  POST — Tambah kartu / karyawan baru (manual)
# ============================================

@router.post("", response_model=CardResponse, status_code=201)
async def create_card(
    card: CardCreate,
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin']))
):
    """Daftarkan kartu RFID baru (atau tambah karyawan baru tanpa kartu terlebih dahulu)."""
    name = (card.employee_name if card.employee_name is not None else card.card_holder) or ""
    name = name.strip()
    if not name:
        raise HTTPException(status_code=400, detail="Nama karyawan tidak boleh kosong.")

    level_to_save = (card.access_level or "").strip()
    if not level_to_save:
        raise HTTPException(status_code=400, detail="Access level tidak boleh kosong.")

    division_to_save = (card.division or "").strip() or None
    uid_clean = (card.uid or "").strip().upper() or None

    if uid_clean:
        cursor = await db.execute("SELECT id, card_holder FROM cards WHERE uid = ?", (uid_clean,))
        existing = await cursor.fetchone()
        if existing:
            raise HTTPException(
                status_code=409,
                detail=f"Kartu dengan UID '{uid_clean}' sudah terdaftar untuk {existing['card_holder']}"
            )

    ts = now_local()
    cursor = await db.execute(
        """INSERT INTO cards (uid, card_holder, division, access_level, is_active, created_at, updated_at)
           VALUES (?, ?, ?, ?, 1, ?, ?)""",
        (uid_clean, name, division_to_save, level_to_save, ts, ts)
    )
    new_id = cursor.lastrowid
    await db.commit()

    cursor = await db.execute("SELECT * FROM cards WHERE id = ?", (new_id,))
    row = await cursor.fetchone()
    return _row_to_card_response(row)


# ============================================
#  PUT — Edit kartu / pasangkan UID ke karyawan
# ============================================

@router.put("/{card_id}", response_model=CardResponse)
async def update_card(
    card_id: int,
    card: CardUpdate,
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin']))
):
    """Update data kartu/karyawan (nama, divisi, access level, status aktif, atau pasang UID)."""
    cursor = await db.execute("SELECT * FROM cards WHERE id = ?", (card_id,))
    existing = await cursor.fetchone()
    if not existing:
        raise HTTPException(status_code=404, detail="Data kartu/karyawan tidak ditemukan")

    updates = []
    params = []

    if card.uid is not None:
        uid_clean = card.uid.strip().upper() or None
        if uid_clean:
            dup_cursor = await db.execute(
                "SELECT id, card_holder FROM cards WHERE uid = ? AND id != ?",
                (uid_clean, card_id)
            )
            dup = await dup_cursor.fetchone()
            if dup:
                raise HTTPException(
                    status_code=409,
                    detail=f"Kartu dengan UID '{uid_clean}' sudah digunakan oleh {dup['card_holder']}"
                )
        updates.append("uid = ?")
        params.append(uid_clean)

    new_name = card.employee_name if card.employee_name is not None else card.card_holder
    if new_name is not None:
        cleaned_name = new_name.strip()
        if not cleaned_name:
            raise HTTPException(status_code=400, detail="Nama karyawan tidak boleh kosong.")
        updates.append("card_holder = ?")
        params.append(cleaned_name)

    if card.division is not None:
        updates.append("division = ?")
        params.append(card.division.strip() or None)

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

    cursor = await db.execute("SELECT * FROM cards WHERE id = ?", (card_id,))
    row = await cursor.fetchone()
    return _row_to_card_response(row)


# ============================================
#  POST — Lepas kartu (Unlink UID tanpa hapus karyawan)
# ============================================

@router.post("/{card_id}/unlink", response_model=CardResponse)
async def unlink_card_uid(
    card_id: int,
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin']))
):
    """Kosongkan UID kartu (misal kartu hilang/rusak) tanpa menghapus data karyawan."""
    cursor = await db.execute("SELECT * FROM cards WHERE id = ?", (card_id,))
    existing = await cursor.fetchone()
    if not existing:
        raise HTTPException(status_code=404, detail="Data kartu/karyawan tidak ditemukan")

    await db.execute(
        "UPDATE cards SET uid = NULL, updated_at = ? WHERE id = ?",
        (now_local(), card_id)
    )
    await db.commit()

    cursor = await db.execute("SELECT * FROM cards WHERE id = ?", (card_id,))
    row = await cursor.fetchone()
    return _row_to_card_response(row)


# ============================================
#  DELETE — Hapus kartu / karyawan
# ============================================

@router.delete("/{card_id}")
async def delete_card(
    card_id: int,
    db: aiosqlite.Connection = Depends(get_db),
    current_user = Depends(require_role(['owner', 'admin']))
):
    """Hapus data kartu/karyawan dari sistem."""
    cursor = await db.execute("SELECT id FROM cards WHERE id = ?", (card_id,))
    existing = await cursor.fetchone()
    if not existing:
        raise HTTPException(status_code=404, detail="Data kartu/karyawan tidak ditemukan")

    await db.execute("DELETE FROM cards WHERE id = ?", (card_id,))
    await db.commit()

    return {"message": "Data berhasil dihapus", "id": card_id}
