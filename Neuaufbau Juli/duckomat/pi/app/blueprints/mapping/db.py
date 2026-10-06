# blueprints/mapping/db.py
import json
import threading
from pathlib import Path
from datetime import datetime

_lock = threading.Lock()
_DB_FILE = Path(__file__).parent / "mapping_data.json"

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
            data[uid] = {"nummer": int(nummer),
                         "created_at": row.get("created_at") or datetime.now().strftime("%Y-%m-%d %H:%M:%S")}
            imported += 1
        _save(data)
    return imported
