from __future__ import annotations

import random
import threading
import time

from src.core import RaceController

class SimIO:
    def __init__(
        self,
        controller: RaceController,
        ls_gap_ms: int,
        p_noread: float,
        p_unknown: float,
        rfid_delay_min_ms: int,
        rfid_delay_max_ms: int,
    ) -> None:
        self.c = controller
        self.ls_gap_ms = int(ls_gap_ms)
        self.p_noread = float(p_noread)
        self.p_unknown = float(p_unknown)
        self.dmin = int(rfid_delay_min_ms)
        self.dmax = int(rfid_delay_max_ms)

        self._stop = False
        self._t: threading.Thread | None = None
        self._seq = 0

    def start(self) -> None:
        self._stop = False
        self._t = threading.Thread(target=self._run, daemon=True)
        self._t.start()

    def stop(self) -> None:
        self._stop = True

    def _run(self) -> None:
        try:
            while not self._stop:
                time.sleep(self.ls_gap_ms / 1000.0)
                if not self.c.is_running():
                    continue

                self._seq += 1
                ls_t = int(time.time() * 1000)
                self.c.ingest_ls(self._seq, t_ms=ls_t)

                if random.random() < self.p_noread:
                    continue

                if self.c.expected_count <= 0:
                    continue

                if random.random() < self.p_unknown:
                    uid = "UNKNOWN" + str(random.randint(1000, 9999))
                else:
                    n = random.randint(1, self.c.expected_count)
                    uid = f"SIM{n:06d}"

                delay = random.randint(self.dmin, self.dmax)
                time.sleep(delay / 1000.0)
                self.c.ingest_rfid(uid, t_ms=int(time.time() * 1000))
        except Exception as e:
            print("[SimIO] crashed:", e)
