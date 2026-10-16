# core.py
import serial
import serial.tools.list_ports
import threading
import time
import json
from pathlib import Path

def us_to_angle(us, us_left, us_right):
    if us_right == us_left:
        return 45.0
    ratio = (us - us_left) / (us_right - us_left)
    return round(45.0 + ratio * 90.0, 1)

def angle_to_us(angle, us_left, us_right):
    ratio = (angle - 45.0) / 90.0
    return int(round(us_left + ratio * (us_right - us_left)))

def atomic_write_json(path: Path, data: dict):
    tmp_path = path.with_suffix(path.suffix + ".tmp")
    tmp_path.write_text(json.dumps(data, indent=2))
    tmp_path.replace(path)

class DuckController:
    RECONNECT_INTERVAL_S = 3
    SERIAL_READ_TIMEOUT_S = 0.1

    @classmethod
    def find_nano_port(cls, preferred_port=None):
        """
        Sucht nach dem Arduino Nano. Prüft zuerst preferred_port, 
        dann alle verfügbaren seriellen USB-Schnittstellen.
        """
        ports = list(serial.tools.list_ports.comports())
        if not ports:
            return None

        # 1. Bevorzugten Port prüfen, falls vorhanden
        if preferred_port:
            for p in ports:
                if p.device == preferred_port:
                    return preferred_port

        # 2. Nach typischen USB-Serial-Konvertern filtern (CH340, FTDI, Arduino)
        for p in ports:
            desc = (p.description or "").lower()
            hwid = (p.hwid or "").lower()
            # Typische USB-Chips auf Nanos
            if any(k in desc or k in hwid for k in ["ch340", "ftdi", "cp210", "arduino", "usb serial"]):
                return p.device

        # 3. Fallback: Erster gefundener ttyUSB/ttyACM bzw. COM-Port
        for p in ports:
            dev = p.device.lower()
            if "ttyusb" in dev or "ttyacm" in dev or dev.startswith("com"):
                return p.device

        return ports[0].device

    @classmethod
    def _try_open_serial(cls, port, baud):
        # Falls der feste Port nicht erreichbar ist, dynamisch suchen
        target_port = port
        try:
            return serial.Serial(target_port, baud, timeout=cls.SERIAL_READ_TIMEOUT_S)
        except serial.SerialException:
            fallback = cls.find_nano_port(preferred_port=port)
            if fallback and fallback != target_port:
                try:
                    ser = serial.Serial(fallback, baud, timeout=cls.SERIAL_READ_TIMEOUT_S)
                    print(f"[core] Automatischer Portwechsel: {port} -> {fallback}")
                    return ser
                except serial.SerialException:
                    pass
            return None

    @classmethod
    def _open_serial(cls, port, baud):
        # Versuch 1: Wunsch-Port
        ser = cls._try_open_serial(port, baud)
        if ser is not None:
            return ser

        # Falls beim ersten Start noch nichts gefunden wurde, 5 Sekunden im Sekundentakt suchen
        print(f"[core] Warte auf Verbindung an '{port}' oder alternativem Port...")
        for _ in range(5):
            time.sleep(1)
            ser = cls._try_open_serial(port, baud)
            if ser is not None:
                return ser

        print("\n" + "=" * 60)
        print(f"FEHLER: Kein passender serieller Port gefunden (Ziel: '{port}').")
        print("Vorhandene Ports:")
        for p in serial.tools.list_ports.comports():
            print(f" - {p.device}: {p.description} [{p.hwid}]")
        print("Ursache: Nano nicht angeschlossen oder Berechtigung fehlt.")
        print("=" * 60 + "\n")
        raise SystemExit(1)

    @staticmethod
    def _try_open_serial(port, baud):
        try:
            return serial.Serial(port, baud, timeout=DuckController.SERIAL_READ_TIMEOUT_S)
        except serial.SerialException:
            return None

    @staticmethod
    def _open_serial(port, baud):
        try:
            return serial.Serial(port, baud, timeout=DuckController.SERIAL_READ_TIMEOUT_S)
        except serial.SerialException as e:
            msg = str(e)
            print("\n" + "=" * 60)
            print(f"FEHLER: Serieller Port '{port}' konnte nicht geoeffnet werden.")
            if "PermissionError" in msg or "Access is denied" in msg or "Zugriff verweigert" in msg:
                print("Ursache: Der Port ist bereits durch ein anderes Programm belegt.")
            elif "FileNotFoundError" in msg or "could not open port" in msg.lower():
                print("Ursache: Port existiert nicht oder Nano ist nicht verbunden.")
            else:
                print(f"Details: {msg}")
            print("=" * 60 + "\n")
            raise SystemExit(1)

    def __init__(self, port="/dev/ttyUSB0", baud=115200, stale_after_s=8):
        self.port = port
        self.baud = baud
        self.ser = self._open_serial(port, baud)
        self.lock = threading.RLock()
        self._io_lock = threading.Lock()
        self.stale_after_s = stale_after_s
        self._last_reconnect_attempt = time.time()
        self._ever_connected = False
        self.state = {
            "left_count": 0,
            "right_count": 0,
            "rejected_count": 0,
            "connected": False,
            "last_line_ms": 0,
            "last_event": None,
            "ls1": "UNKNOWN",
            "ls2": "UNKNOWN",
            "us1_cm": None,
            "us2_cm": None,
            "last_uid": "NONE",
            "fast": None,
            "slow": None,
            "servo_state": "UNKNOWN",
            "nfcmode": None,
            "nfc_ready": None,
            "cfg": {},
            "dev_mode": False,
            "log": []
        }
        self.data_file = Path(__file__).parent / "duck_data.json"
        self._load_state()
        self.running = True
        self._token_counter = 0
        self._listeners = []
        self.thread = threading.Thread(target=self._read_loop, daemon=True)
        self.thread.start()
        time.sleep(0.3)
        self._send("STATE? token=0")
        self._send("CFG? token=0")

    def add_listener(self, callback):
        self._listeners.append(callback)

    def _notify(self, event_type, payload):
        for cb in self._listeners:
            try:
                cb(event_type, payload)
            except Exception as e:
                print(f"Listener error ({event_type}): {e}")

    def _next_token(self):
        with self.lock:
            self._token_counter += 1
            return self._token_counter

    def _load_state(self):
        if self.data_file.exists():
            try:
                saved = json.loads(self.data_file.read_text())
                for k in ("left_count", "right_count", "rejected_count"):
                    if k in saved:
                        self.state[k] = saved[k]
            except Exception:
                pass

    def _save_counts(self):
        to_save = {k: self.state[k] for k in
                   ("left_count", "right_count", "rejected_count")}
        atomic_write_json(self.data_file, to_save)

    def _send(self, line):
        try:
            with self._io_lock:
                self.ser.write((line + "\n").encode())
        except Exception as e:
            print(f"[core] SCHREIB-FEHLER: {type(e).__name__}: {e}")
            with self.lock:
                self.state["connected"] = False

    def _read_loop(self):
        while self.running:
            try:
                with self._io_lock:
                    raw = self.ser.readline().decode(errors="ignore").strip()
                if not raw:
                    self._check_staleness()
                    if self._ever_connected:
                        self._maybe_reconnect()
                    continue
                with self.lock:
                    self.state["connected"] = True
                    self.state["last_line_ms"] = time.time()
                self._ever_connected = True
                self._handle_line(raw)
            except Exception as e:
                print(f"[core] READ-FEHLER: {type(e).__name__}: {e}")
                with self.lock:
                    self.state["connected"] = False
                self._maybe_reconnect()
                time.sleep(1)

    def _check_staleness(self):
        with self.lock:
            last = self.state.get("last_line_ms", 0)
            if last and (time.time() - last) > self.stale_after_s:
                self.state["connected"] = False

    def _maybe_reconnect(self):
        with self.lock:
            if self.state.get("connected"):
                return
            now = time.time()
            if now - self._last_reconnect_attempt < self.RECONNECT_INTERVAL_S:
                return
            self._last_reconnect_attempt = now

        new_ser = None
        try:
            with self._io_lock:
                try:
                    self.ser.close()
                except Exception:
                    pass
                new_ser = self._try_open_serial(self.port, self.baud)
                if new_ser is not None:
                    self.ser = new_ser
                    self.port = new_ser.port  # Aktualisiere port auf den neu gefundenen Port

            if new_ser is not None:
                print(f"[core] Serieller Port {self.port} neu verbunden.")
                self._send("STATE? token=0")
                self._send("CFG? token=0")
        except Exception as e:
            print(f"[core] Reconnect-Versuch fehlgeschlagen: {e}")

    def _handle_line(self, raw):
        with self.lock:
            self.state["log"].append(raw)
            self.state["log"] = self.state["log"][-50:]

        if raw.startswith("DUCK"):
            self._handle_duck_line(raw)
        elif raw.startswith("STATE"):
            self._handle_state_line(raw)
        elif raw.startswith("CFG"):
            self._handle_cfg_line(raw)
        elif raw.startswith("HELLO"):
            self._handle_hello_line(raw)
        elif raw.startswith("EV"):
            self._handle_event_line(raw)

    @staticmethod
    def _parse_kv(raw, skip_first=1):
        parts = raw.split(" ")[skip_first:]
        return dict(p.split("=", 1) for p in parts if "=" in p)

    def _handle_duck_line(self, raw):
        parts = self._parse_kv(raw)
        seq = parts.get("seq")
        uid = parts.get("uid")
        result = parts.get("result", "")

        with self.lock:
            if result == "UNREADABLE_DROP":
                self.state["rejected_count"] += 1
            elif result == "KICK_L":
                self.state["left_count"] += 1
            elif result == "KICK_R":
                self.state["right_count"] += 1
            self.state["last_event"] = {"seq": seq, "uid": uid, "result": result}
            self._save_counts()

        self._notify("DUCK", {"seq": seq, "uid": uid, "result": result})

    def _handle_state_line(self, raw):
        parts = self._parse_kv(raw)
        with self.lock:
            self.state["last_uid"] = parts.get("lastuid", self.state["last_uid"])
            self.state["fast"] = int(parts["fast"]) if "fast" in parts else self.state["fast"]
            self.state["slow"] = int(parts["slow"]) if "slow" in parts else self.state["slow"]
            self.state["servo_state"] = parts.get("servo", self.state["servo_state"])
            self.state["nfcmode"] = parts.get("nfcmode", self.state.get("nfcmode"))
            if "nfc" in parts:
                self.state["nfc_ready"] = parts.get("nfc") == "1"

    def _handle_hello_line(self, raw):
        parts = self._parse_kv(raw)
        with self.lock:
            if "nfc" in parts:
                self.state["nfc_ready"] = parts.get("nfc") == "1"
            self.state["nfcmode"] = parts.get("nfcmode", self.state.get("nfcmode"))

    def _handle_cfg_line(self, raw):
        parts = self._parse_kv(raw)
        with self.lock:
            self.state["cfg"] = {
                "posrestl": int(parts.get("posrestl", 0)),
                "poskickl": int(parts.get("poskickl", 0)),
                "poskickr": int(parts.get("poskickr", 0)),
                "posrestr": int(parts.get("posrestr", 0)),
                "kdelay": int(parts.get("kdelay", 0)),
                "khold": int(parts.get("khold", 0)),
                "rhold": int(parts.get("rhold", 0)),
                "rholdswitch": int(parts.get("rholdswitch", 150)),
                "invert": parts.get("invert") == "1",
                "usthreshmm": int(parts.get("usthreshmm", 100)),
                "usconfirm": int(parts.get("usconfirm", 2)),
                "usinterval": int(parts.get("usinterval", 20)),
                "nfctimeout": int(parts.get("nfctimeout", 60)),
                "nfcretries": int(parts.get("nfcretries", 5)),
            }

    def _handle_event_line(self, raw):
        parts = self._parse_kv(raw)
        ev_type = parts.get("type")
        state_val = parts.get("state")
        cm_val = parts.get("cm")

        if ev_type == "LS1" and state_val:
            with self.lock:
                self.state["ls1"] = state_val
                if cm_val is not None:
                    self.state["us1_cm"] = float(cm_val)
        elif ev_type == "LS2" and state_val:
            with self.lock:
                self.state["ls2"] = state_val
                if cm_val is not None:
                    self.state["us2_cm"] = float(cm_val)
        elif ev_type == "TAG":
            uid = parts.get("uid")
            if uid:
                with self.lock:
                    self.state["last_uid"] = uid
                self._notify("TAG", {"uid": uid, "seq": parts.get("seq")})
        elif ev_type == "LS1_DUCK":
            self._notify("LS1_DUCK", {"seq": parts.get("seq")})
        elif ev_type == "LS2_DUCK":
            self._notify("LS2_DUCK", {"seq": parts.get("seq")})
        elif ev_type in ("NFC_RECOVERED", "NFC_TIMEOUT_DETECTED"):
            with self.lock:
                self.state["nfc_ready"] = (ev_type == "NFC_RECOVERED")

    def send_motor(self, fast, slow):
        token = self._next_token()
        self._send(f"MOTOR token={token} fast={int(fast)} slow={int(slow)}")

    def send_servo_us(self, us):
        token = self._next_token()
        self._send(f"SERVOUS token={token} us={int(us)}")

    def send_nfcmode(self, mode):
        token = self._next_token()
        self._send(f"NFCMODE token={token} mode={mode}")

    def send_arm(self, side, switch=False):
        token = self._next_token()
        self._send(f"ARM token={token} side={side} switch={1 if switch else 0}")

    def send_simtag(self, uid):
        with self.lock:
            if not self.state.get("dev_mode"):
                return False
        token = self._next_token()
        self._send(f"SIMTAG token={token} uid={uid}")
        return True

    def set_dev_mode(self, enabled):
        with self.lock:
            self.state["dev_mode"] = bool(enabled)

    def send_config(self, key, value):
        token = self._next_token()
        self._send(f"CONFIG token={token} key={key} value={int(value)}")

    def request_cfg(self):
        token = self._next_token()
        self._send(f"CFG? token={token}")

    def get_state(self):
        with self.lock:
            self._check_staleness()
            st = dict(self.state)
        cfg = st.get("cfg") or {}
        if cfg:
            st["angle_rest_left"] = us_to_angle(cfg["posrestl"], cfg["posrestl"], cfg["posrestr"])
            st["angle_kick_left"] = us_to_angle(cfg["poskickl"], cfg["posrestl"], cfg["posrestr"])
            st["angle_kick_right"] = us_to_angle(cfg["poskickr"], cfg["posrestl"], cfg["posrestr"])
            st["angle_rest_right"] = us_to_angle(cfg["posrestr"], cfg["posrestl"], cfg["posrestr"])
        return st

    def reset(self):
        with self.lock:
            self.state.update({
                "left_count": 0, "right_count": 0, "rejected_count": 0
            })
            self._save_counts()
