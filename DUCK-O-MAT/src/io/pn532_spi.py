from __future__ import annotations

import threading
import time
from typing import Optional

from src.core import RaceController

class PN532SPI:
    def __init__(self, controller: RaceController, cs_pin_name: str = "D8") -> None:
        self.c = controller
        self.cs_pin_name = cs_pin_name

        self._stop = False
        self._t: threading.Thread | None = None

        # Imports erst hier, damit Windows/PC nicht crasht
        try:
            import board
            import busio
            from digitalio import DigitalInOut
            from adafruit_pn532.spi import PN532_SPI
        except Exception as e:
            raise RuntimeError(
                "PN532 libs fehlen. Auf dem Raspberry Pi installieren:\n"
                "pip install adafruit-blinka adafruit-circuitpython-pn532\n"
                f"Original error: {e}"
            )

        self._board = __import__("board")
        self._busio = __import__("busio")
        digitalio = __import__("digitalio")
        adafruit_pn532_spi = __import__("adafruit_pn532.spi", fromlist=["PN532_SPI"])

        board = self._board
        busio = self._busio
        DigitalInOut = digitalio.DigitalInOut
        PN532_SPI = adafruit_pn532_spi.PN532_SPI

        spi = busio.SPI(board.SCK, board.MOSI, board.MISO)

        # z.B. "D8" -> board.D8
        cs_pin = getattr(board, cs_pin_name)
        cs = DigitalInOut(cs_pin)

        self.pn532 = PN532_SPI(spi, cs, debug=False)
        self.pn532.SAM_configuration()

    def start(self) -> None:
        self._stop = False
        self._t = threading.Thread(target=self._run, daemon=True)
        self._t.start()

    def stop(self) -> None:
        self._stop = True

    def _run(self) -> None:
        try:
            while not self._stop:
                uid = self.pn532.read_passive_target(timeout=0.1)
                if uid is None:
                    time.sleep(0.05)
                    continue

                uid_str = "".join(f"{b:02X}" for b in uid)
                t_ms = int(time.time() * 1000)
                self.c.ingest_rfid(uid_str, t_ms=t_ms)

                # debounce
                time.sleep(0.2)
        except Exception as e:
            print("[PN532SPI] crashed:", e)
