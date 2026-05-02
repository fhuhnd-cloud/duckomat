from __future__ import annotations

import glob
import threading
import time
from typing import Optional

import serial

from src.core import InventoryController


def auto_find_port() -> str:
    ports = sorted(glob.glob("/dev/ttyACM*")) + sorted(glob.glob("/dev/ttyUSB*"))
    if not ports:
        raise RuntimeError("Kein Arduino-Port gefunden. Erwartet /dev/ttyACM* oder /dev/ttyUSB*")
    return ports[0]


class ArduinoUSB:
    """
    Protokoll (Textzeilen, newline '\n'):

    Arduino -> Pi:
      LS,<seq>,<t_ms>

    Pi -> Arduino:
      DEC,<seq>,PASS
      DEC,<seq>,EJECT
    """

    def __init__(self, controller: InventoryController, port: str = "AUTO", baud: int = 115200) -> None:
        self.controller = controller
        self.port = auto_find_port() if str(port).upper() == "AUTO" else str(port)
        self.baud = int(baud)

        self._ser: Optional[serial.Serial] = None
        self._t: Optional[threading.Thread] = None
        self._stop = threading.Event()

        # Controller callback -> Arduino
        self.controller.on_decision = self._on_decision

    def start(self) -> None:
        if self._t and self._t.is_alive():
            return

        self._stop.clear()
        self._ser = serial.Serial(self.port, self.baud, timeout=0.2)

        # Arduino resetet oft beim Port-Open → kurze Wartezeit
        time.sleep(1.5)

        self._t = threading.Thread(target=self._rx_loop, daemon=True)
        self._t.start()

    def stop(self) -> None:
        self._stop.set()
        try:
            if self._ser:
                self._ser.close()
        except Exception:
            pass

    def _rx_loop(self) -> None:
        assert self._ser is not None
        buf = b""

        while not self._stop.is_set():
            try:
                chunk = self._ser.read(256)
                if not chunk:
                    continue

                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    s = line.decode("utf-8", errors="ignore").strip()
                    if s:
                        self._handle_line(s)
            except Exception:
                time.sleep(0.1)

    def _handle_line(self, s: str) -> None:
        # Erwartet: LS,<seq>,<t_ms>
        if not s.startswith("LS,"):
            return

        parts = s.split(",")
        if len(parts) < 3:
            return

        try:
            seq = int(parts[1])
            t_ms = int(parts[2])
        except ValueError:
            return

        self.controller.ingest_ls(seq=seq, t_ms=None)

    def _on_decision(self, seq: int, decision: str, reason: str) -> None:
        # decision kommt aus Controller: "PASS" oder "EJECT"
        if not self._ser:
            return

        try:
            msg = f"DEC,{int(seq)},{decision}\n"
            self._ser.write(msg.encode("utf-8"))
        except Exception:
            pass