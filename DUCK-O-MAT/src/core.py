from __future__ import annotations

import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional

from src.mapping_store import MappingStore
from src.state import LiveState
from src.util import normalize_uid
from src.validator import missing_numbers
from src.storage import get_conn

DecisionCallback = Callable[[int, str, str], None]
# callback(seq, decision_str: "PASS"/"EJECT", reason_str)


@dataclass(frozen=True)
class RFIDEvent:
    uid: str
    t_ms: int


@dataclass(frozen=True)
class PendingLS:
    seq: int
    ls_t_ms: int
    deadline_ms: int


class InventoryController:
    def __init__(self, mapping: MappingStore) -> None:
        self.mapping = mapping
        self.state = LiveState()
        self._lock = threading.Lock()

        self.inventory_id: str = "unset"
        self.expected_count: int = 0
        self.window_before_ms: int = 100
        self.window_after_ms: int = 300

        self._rfid_buffer: list[RFIDEvent] = []
        self._pending_ls: list[PendingLS] = []

        self._running_evt = threading.Event()

        self.on_decision: Optional[DecisionCallback] = None

        self._db = get_conn()
        self._db_lock = threading.Lock()

        self._worker_stop = False
        self._worker = threading.Thread(target=self._worker_loop, daemon=True)
        self._worker.start()

    def configure(self, inventory_id: str, expected_count: int, wb: int, wa: int) -> None:
        with self._lock:
            self.inventory_id = inventory_id
            self.expected_count = int(expected_count)
            self.window_before_ms = int(wb)
            self.window_after_ms = int(wa)

    def start(self) -> None:
        self._running_evt.set()
        with self._lock:
            self.state.running = True

    def stop(self) -> None:
        self._running_evt.clear()
        with self._lock:
            self.state.running = False

    def is_running(self) -> bool:
        return self._running_evt.is_set()

    def reset_counts(self) -> None:
        with self._lock:
            running = self.is_running()
            self.state = LiveState(running=running)
            self._rfid_buffer.clear()
            self._pending_ls.clear()

    def ingest_rfid(self, uid: str, t_ms: Optional[int] = None) -> None:
        uid = normalize_uid(uid)
        if t_ms is None:
            t_ms = int(time.time() * 1000)
        with self._lock:
            # RFID puffern unabhängig von running, damit beim Start sofort Match möglich ist
            self._rfid_buffer.append(RFIDEvent(uid=uid, t_ms=int(t_ms)))

            cutoff = int(time.time() * 1000) - 5000
            if len(self._rfid_buffer) > 5000:
                self._rfid_buffer = [e for e in self._rfid_buffer if e.t_ms >= cutoff]

    def ingest_ls(self, seq: int, t_ms: Optional[int] = None) -> None:
        if t_ms is None:
            t_ms = int(time.time() * 1000)
        t_ms = int(t_ms)

        with self._lock:
            if not self.is_running():
                return

            self.state.ls_total += 1
            self.state.last_seq = int(seq)

            deadline = t_ms + self.window_after_ms
            self._pending_ls.append(PendingLS(seq=int(seq), ls_t_ms=t_ms, deadline_ms=deadline))

    def _worker_loop(self) -> None:
        while not self._worker_stop:
            time.sleep(0.02)
            if not self.is_running():
                continue
            self._process_pending()

    def _process_pending(self) -> None:
        now = int(time.time() * 1000)
        with self._lock:
            if not self._pending_ls:
                return
            ready = [p for p in self._pending_ls if p.deadline_ms <= now]
            if not ready:
                return
            self._pending_ls = [p for p in self._pending_ls if p.deadline_ms > now]

            for p in ready:
                self._decide_for_ls(p.seq, p.ls_t_ms)

    def _emit_decision(self, seq: int, decision: str, reason: str, uid: Optional[str], num: Optional[int], t_ms: int) -> None:
        if self.on_decision:
            self.on_decision(seq, decision, reason)

        with self._db_lock:
            self._db.execute(
                "INSERT INTO seen(inventory_id, number, uid, seq, decision, reason, t_ms) VALUES (?,?,?,?,?,?,?)",
                (self.inventory_id, num, uid, int(seq), decision, reason, int(t_ms)),
            )
            self._db.commit()

    def _decide_for_ls(self, seq: int, ls_t_ms: int) -> None:
        lo = ls_t_ms - self.window_before_ms
        hi = ls_t_ms + self.window_after_ms

        best_i = None
        best_abs_dt = None
        for i, ev in enumerate(self._rfid_buffer):
            if lo <= ev.t_ms <= hi:
                abs_dt = abs(ev.t_ms - ls_t_ms)
                if best_i is None or abs_dt < best_abs_dt:
                    best_i = i
                    best_abs_dt = abs_dt

        if best_i is None:
            self.state.timeouts += 1
            self.state.last_uid = None
            self.state.last_number = None
            self.state.last_result = "B"
            self.state.last_reason = "TIMEOUT"
            self._emit_decision(seq, "PASS", "TIMEOUT", None, None, ls_t_ms)
            return

        match = self._rfid_buffer.pop(best_i)
        uid = match.uid
        num = self.mapping.get_number(uid)

        self.state.last_uid = uid
        self.state.last_number = num

        if num is None:
            self.state.unknown_uids += 1
            self.state.last_result = "B"
            self.state.last_reason = "UNKNOWN_UID"
            self._emit_decision(seq, "PASS", "UNKNOWN_UID", uid, None, match.t_ms)
            return

        self.state.ok_reads += 1
        self.state.seen_numbers.add(num)
        self.state.last_result = "A"
        self.state.last_reason = "MATCH"
        self._emit_decision(seq, "EJECT", "MATCH", uid, num, match.t_ms)

    def snapshot(self) -> dict:
        with self._lock:
            miss = missing_numbers(self.expected_count, set(self.state.seen_numbers))
            return {
                "running": self.state.running,
                "inventory_id": self.inventory_id,
                "expected_count": self.expected_count,
                "window_before_ms": self.window_before_ms,
                "window_after_ms": self.window_after_ms,

                "ls_total": self.state.ls_total,
                "ok_reads": self.state.ok_reads,
                "timeouts": self.state.timeouts,
                "unknown_uids": self.state.unknown_uids,

                "last_seq": self.state.last_seq,
                "last_uid": self.state.last_uid,
                "last_number": self.state.last_number,
                "last_result": self.state.last_result,
                "last_reason": self.state.last_reason,

                "missing_count": len(miss),
                "missing_preview": miss[:30],
            }

