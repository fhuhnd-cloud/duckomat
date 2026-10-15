# blueprints/mapping/db.py
import json
import csv
import io
import threading
from pathlib import Path
from datetime import datetime

_lock = threading.Lock()
_DB_FILE = Path(__file__).parent / "mapping_data.json"

# Verzeichnisse, die nach mapping*.csv durchsucht werden
_SEARCH_DIRS = [
    Path(__file__).parent,                          # blueprints/mapping/
    Path(__file__).parent.parent.parent,           # pi/app/
    Path(__file__).parent / "mapping_import",      # blueprints/mapping/mapping_import/
]

def _atomic_write(path: Path, data):
    tmp_path = path.with_suffix(path.suffix + ".tmp")
    tmp_path.write_text(json.dumps(data, indent=2))
    tmp_path.replace(path)

def _load():
    if _DB_FILE.exists():
        try:
            return json.loads(_DB_FILE.read_text())
        except Exception:
            return {}
    return {}

def _save(data):
    _atomic_write(_DB_FILE, data)

def get_number_for_uid(uid):
    with _lock:
        data = _load()
        entry = data.get(uid)
        return entry["nummer"] if entry else None

def list_all():
    with _lock:
        data = _load()
    rows = [{"uid": uid, "nummer": v["nummer"], "created_at": v["created_at"]}
            for uid, v in data.items()]
    rows.sort(key=lambda r: r["nummer"])
    return rows

def add_mapping(uid, nummer):
    with _lock:
        data = _load()
        data[uid] = {"nummer": int(nummer), "created_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S")}
        _save(data)

def delete_mapping(uid):
    with _lock:
        data = _load()
        if uid in data:
            del data[uid]
            _save(data)

def reset_all():
    with _lock:
        _save({})

def next_free_number():
    with _lock:
        data = _load()
    used = {v["nummer"] for v in data.values()}
    n = 1
    while n in used:
        n += 1
    return n

def count():
    with _lock:
        return len(_load())

def import_rows(rows):
    with _lock:
        data = _load()
        imported = 0
        for row in rows:
            uid = row.get("uid")
            nummer = row.get("nummer")
            if not uid or nummer is None:
                continue
            data[uid] = {
                "nummer": int(nummer),
                "created_at": row.get("created_at") or datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            }
            imported += 1
        _save(data)
    return imported

def auto_import_csv_if_present():
    """
    Sucht dynamisch nach allen 'mapping*.csv' Dateien in den Zielordnern.
    Nimmt bei mehreren Treffern automatisch die Datei mit dem neuesten Änderungsdatum.
    """
    candidate_files = []
    for d in _SEARCH_DIRS:
        if d.exists() and d.is_dir():
            for p in d.glob("mapping*.csv"):
                if p.is_file() and not p.name.endswith(".tmp"):
                    candidate_files.append(p)

    if not candidate_files:
        return 0

    # Nach Änderungszeitpunkt sortieren (neueste Datei zuerst)
    candidate_files.sort(key=lambda f: f.stat().st_mtime, reverse=True)
    target_csv = candidate_files[0]

    try:
        content = target_csv.read_text(encoding="utf-8")
        reader = csv.DictReader(io.StringIO(content))
        rows = list(reader)
        if rows:
            count_imported = import_rows(rows)
            print(f"[Mapping] Auto-Import: {count_imported} Datensätze aus '{target_csv.name}' erfolgreich eingelesen.")
            return count_imported
    except Exception as e:
        print(f"[Mapping] Fehler beim Auto-Import von '{target_csv}': {e}")

    return 0

# Wird beim Start der Flask-App ausgeführt
auto_import_csv_if_present()