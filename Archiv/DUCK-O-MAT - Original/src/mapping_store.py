from __future__ import annotations

import sqlite3
import threading
from typing import Optional

from src.storage import get_conn
from src.util import normalize_uid


class MappingStore:
    def __init__(self) -> None:
        self.conn = get_conn()
        self._lock = threading.Lock()

    def get_number(self, uid: str) -> Optional[int]:
        uid = normalize_uid(uid)
        with self._lock:
            cur = self.conn.execute("SELECT number FROM mapping WHERE uid = ?", (uid,))
            row = cur.fetchone()
        return int(row[0]) if row else None

    def get_uid(self, number: int) -> Optional[str]:
        with self._lock:
            cur = self.conn.execute("SELECT uid FROM mapping WHERE number = ?", (int(number),))
            row = cur.fetchone()
        return str(row[0]) if row else None

    def is_number_taken(self, number: int) -> bool:
        with self._lock:
            cur = self.conn.execute("SELECT 1 FROM mapping WHERE number = ? LIMIT 1", (int(number),))
            return cur.fetchone() is not None

    def next_free_number(self, expected_count: int) -> Optional[int]:
        expected_count = int(expected_count)
        with self._lock:
            cur = self.conn.execute(
                "SELECT number FROM mapping WHERE number BETWEEN 1 AND ? ORDER BY number ASC",
                (expected_count,),
            )
            taken = {int(r[0]) for r in cur.fetchall()}
        for n in range(1, expected_count + 1):
            if n not in taken:
                return n
        return None

    def list_mappings(self, limit: int = 200, offset: int = 0, q: str = "") -> list[dict]:
        limit = max(1, min(int(limit), 2000))
        offset = max(0, int(offset))
        q = (q or "").strip()

        with self._lock:
            if q:
                try:
                    n = int(q)
                    cur = self.conn.execute(
                        "SELECT number, uid FROM mapping WHERE number = ? ORDER BY number ASC LIMIT ? OFFSET ?",
                        (n, limit, offset),
                    )
                except ValueError:
                    like = f"%{q.upper()}%"
                    cur = self.conn.execute(
                        "SELECT number, uid FROM mapping WHERE UPPER(uid) LIKE ? ORDER BY number ASC LIMIT ? OFFSET ?",
                        (like, limit, offset),
                    )
            else:
                cur = self.conn.execute(
                    "SELECT number, uid FROM mapping ORDER BY number ASC LIMIT ? OFFSET ?",
                    (limit, offset),
                )
            rows = cur.fetchall()

        return [{"number": int(r[0]), "uid": str(r[1])} for r in rows]

    def list_all(self) -> list[dict]:
        with self._lock:
            cur = self.conn.execute("SELECT number, uid FROM mapping ORDER BY number ASC")
            rows = cur.fetchall()
        return [{"number": int(r[0]), "uid": str(r[1])} for r in rows]

    def count_mappings(self) -> int:
        with self._lock:
            cur = self.conn.execute("SELECT COUNT(*) FROM mapping")
            return int(cur.fetchone()[0])

    def assign(self, uid: str, number: int) -> None:
        uid = normalize_uid(uid)
        with self._lock:
            try:
                self.conn.execute(
                    "INSERT INTO mapping(uid, number) VALUES (?, ?)",
                    (uid, int(number)),
                )
                self.conn.commit()
            except sqlite3.IntegrityError as e:
                raise ValueError(f"Mapping nicht möglich (UID oder Nummer existiert schon). Details: {e}")

    def reassign_number(self, number: int, new_uid: str) -> None:
        number = int(number)
        new_uid = normalize_uid(new_uid)

        with self._lock:
            old_uid = self.get_uid(number)
            if old_uid is None:
                raise ValueError(f"Nummer {number} ist aktuell keiner UID zugeordnet.")

            existing_num = self.get_number(new_uid)
            if existing_num is not None and existing_num != number:
                raise ValueError(f"Neue UID {new_uid} ist bereits als Nummer {existing_num} registriert.")

            try:
                self.conn.execute("BEGIN")
                self.conn.execute("DELETE FROM mapping WHERE uid = ?", (old_uid,))
                self.conn.execute("INSERT OR REPLACE INTO mapping(uid, number) VALUES (?, ?)", (new_uid, number))
                self.conn.commit()
            except Exception:
                self.conn.rollback()
                raise

    def clear_all(self) -> None:
        with self._lock:
            self.conn.execute("DELETE FROM mapping")
            self.conn.commit()