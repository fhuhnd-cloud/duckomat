from __future__ import annotations

import threading
import time
from typing import Optional

from smbus2 import SMBus, i2c_msg

from src.core import InventoryController
from src.util import normalize_uid


class PN532I2C:
    ADDR = 0x24
    ACK = bytes([0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00])

    def __init__(self, controller: InventoryController, bus_id: int = 1, poll_s: float = 0.12) -> None:
        self.controller = controller
        self.bus_id = int(bus_id)
        self.poll_s = float(poll_s)

        self._bus: Optional[SMBus] = None
        self._lock = threading.Lock()

        self._stop = threading.Event()
        self._t: Optional[threading.Thread] = None

        self._last_uid: Optional[str] = None
        self._last_uid_ts: float = 0.0

    def start(self) -> None:
        if self._t and self._t.is_alive():
            return
        self._stop.clear()
        self._t = threading.Thread(target=self._loop, daemon=True)
        self._t.start()

    def stop(self) -> None:
        self._stop.set()

    def get_last_uid(self, max_age_s: float = 30.0) -> Optional[str]:
        if not self._last_uid:
            return None
        if (time.time() - self._last_uid_ts) > float(max_age_s):
            return None
        return self._last_uid

    def _open_bus(self) -> None:
        if self._bus is None:
            self._bus = SMBus(self.bus_id)

    def _close_bus(self) -> None:
        if self._bus is not None:
            try:
                self._bus.close()
            finally:
                self._bus = None

    def _i2c_write(self, payload: bytes) -> None:
        assert self._bus is not None
        self._bus.write_i2c_block_data(self.ADDR, 0x00, list(payload))

    def _i2c_read_raw(self, n: int) -> bytes:
        assert self._bus is not None
        msg = i2c_msg.read(self.ADDR, n)
        self._bus.i2c_rdwr(msg)
        return bytes(msg)

    @staticmethod
    def _build_frame(data: bytes) -> bytes:
        ln = len(data)
        lcs = (-ln) & 0xFF
        dcs = (-sum(data)) & 0xFF
        return bytes([0x00, 0x00, 0xFF, ln, lcs]) + data + bytes([dcs, 0x00])

    @staticmethod
    def _parse_data_from_frame(frame: bytes) -> Optional[bytes]:
        if len(frame) < 8:
            return None
        if frame[0:3] != b"\x00\x00\xFF":
            return None
        ln = frame[3]
        lcs = frame[4]
        if ((ln + lcs) & 0xFF) != 0:
            return None
        need = 7 + ln
        if len(frame) < need:
            return None
        data = frame[5:5 + ln]
        dcs = frame[5 + ln]
        if ((sum(data) + dcs) & 0xFF) != 0:
            return None
        if frame[6 + ln] != 0x00:
            return None
        return data

    def _wait_ready(self, timeout_s: float = 0.6) -> bool:
        t0 = time.time()
        while (time.time() - t0) < timeout_s:
            try:
                b = self._i2c_read_raw(1)
                if b and b[0] == 0x01:
                    return True
            except OSError:
                time.sleep(0.01)
            time.sleep(0.01)
        return False

    def _read_frame(self, n: int = 64) -> bytes:
        raw = self._i2c_read_raw(n + 1)
        if not raw or raw[0] != 0x01:
            return b""
        return raw[1:]

    def _cmd(self, data: bytes, resp_len: int = 64) -> Optional[bytes]:
        self._i2c_write(self._build_frame(data))

        if not self._wait_ready(0.6):
            return None
        ack = self._read_frame(16)
        if ack[:6] != self.ACK:
            return None

        if not self._wait_ready(0.6):
            return None
        resp = self._read_frame(resp_len)
        return self._parse_data_from_frame(resp)

    def _sam_config(self) -> bool:
        resp = self._cmd(bytes([0xD4, 0x14, 0x01, 0x14, 0x01]), resp_len=32)
        return bool(resp and len(resp) >= 2 and resp[0] == 0xD5 and resp[1] == 0x15)

    def _read_uid_once(self) -> Optional[str]:
        resp = self._cmd(bytes([0xD4, 0x4A, 0x01, 0x00]), resp_len=64)
        if not resp or len(resp) < 8:
            return None
        if resp[0] != 0xD5 or resp[1] != 0x4B:
            return None
        if resp[2] < 1:
            return None
        uid_len = resp[7]
        uid = resp[8:8 + uid_len]
        if len(uid) != uid_len:
            return None
        return uid.hex().upper()

    def _loop(self) -> None:
        while not self._stop.is_set():
            try:
                with self._lock:
                    self._open_bus()
                    ok = self._sam_config()
                if not ok:
                    time.sleep(0.2)
                    continue

                uid = None
                with self._lock:
                    uid = self._read_uid_once()

                if uid:
                    uid = normalize_uid(uid)
                    self._last_uid = uid
                    self._last_uid_ts = time.time()

                    # zusätzlich in die Inventur-Logik, falls running
                    if self.controller.is_running():
                        self.controller.ingest_rfid(uid)

                    time.sleep(0.35)
                else:
                    time.sleep(self.poll_s)

            except (OSError, TimeoutError):
                with self._lock:
                    self._close_bus()
                time.sleep(0.25)
            except Exception:
                time.sleep(0.25)

