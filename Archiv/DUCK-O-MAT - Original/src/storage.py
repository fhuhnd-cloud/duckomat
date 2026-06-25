from __future__ import annotations

import sqlite3
from pathlib import Path

DB_PATH = Path(__file__).resolve().parent.parent / "data.db"


def get_conn() -> sqlite3.Connection:
    # Thread-tolerant für Flask + Hintergrundthreads
    conn = sqlite3.connect(str(DB_PATH), check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL;")
    conn.execute("PRAGMA synchronous=NORMAL;")
    conn.execute("PRAGMA foreign_keys=ON;")
    return conn


def init_db() -> None:
    conn = get_conn()
    try:
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS mapping (
                uid TEXT PRIMARY KEY,
                number INTEGER UNIQUE NOT NULL
            )
            """
        )
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS seen (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                inventory_id TEXT,
                number INTEGER,
                uid TEXT,
                seq INTEGER,
                decision TEXT,
                reason TEXT,
                t_ms INTEGER
            )
            """
        )
        conn.commit()
    finally:
        conn.close()

