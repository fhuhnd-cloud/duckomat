# blueprints/inventur/logic.py
import threading
import time
from blueprints.mapping.db import get_number_for_uid

class SortingSession:
    BATCH_SIZE = 100
    REJECT_STOP_SIZE = 100
    MOTOR_STOP_DELAY_S = 2.0
    DEFAULT_FAST_PWM = 255
    DEFAULT_SLOW_PWM = 130

    def __init__(self, controller):
        self.controller = controller
        self.lock = threading.Lock()
        self.running = False
        self.start_direction = "LEFT"
        self.current_direction = "LEFT"
        self.batch_count = 0
        self.session_left = 0
        self.session_right = 0
        self.session_rejected = 0
        self.reject_batch_count = 0
        self.log = []
        self.stop_on_rejects = False
        self.paused_for_rejects = False
        self._last_fast_pwm = self.DEFAULT_FAST_PWM
        self._last_slow_pwm = self.DEFAULT_SLOW_PWM
        self._stop_timer = None
        controller.add_listener(self._on_event)

    def set_start_direction(self, direction):
        direction = direction.upper()
        if direction not in ("LEFT", "RIGHT"):
            return False
        with self.lock:
            self.start_direction = direction
            self.current_direction = direction
            self.batch_count = 0
        return True

    def _preposition_servo(self):
        cfg = self.controller.get_state().get("cfg") or {}
        if self.current_direction == "LEFT":
            target_us = cfg.get("posrestl", 2122)
        else:
            target_us = cfg.get("posrestr", 1050)
        self.controller.send_servo_us(target_us)

    def start(self):
        with self.lock:
            self.running = True
            self.paused_for_rejects = False
            self.batch_count = 0

        # Vor dem Start Klöppel auf Startseite
        self._preposition_servo()
        self.controller.send_nfcmode("DUCKONLY")

        # Kistengröße und Start-Seite autonom an den Nano übergeben!
        self.controller.send_config("BATCHSIZE", self.BATCH_SIZE)
        side_letter = "L" if self.start_direction == "LEFT" else "R"
        self.controller.send_arm(side_letter)

        # Motoren starten
        self.controller.send_motor(self._last_fast_pwm, self._last_slow_pwm)
        self.controller.send_motor(self._last_fast_pwm, self._last_slow_pwm)

    def stop(self):
        with self.lock:
            self.running = False
            self.paused_for_rejects = False
            self.controller.send_arm("N")
        self.controller.send_nfcmode("CONTINUOUS")
        self.controller.send_motor(0, 0)

    def reset_session(self):
        with self.lock:
            self.batch_count = 0
            self.current_direction = self.start_direction
            self.session_left = 0
            self.session_right = 0
            self.session_rejected = 0
            self.reject_batch_count = 0
            self.log = []
            self.paused_for_rejects = False
        self._preposition_servo()

    def set_stop_on_rejects(self, enabled):
        with self.lock:
            self.stop_on_rejects = bool(enabled)

    def set_reject_stop_size(self, size):
        with self.lock:
            self.REJECT_STOP_SIZE = max(1, int(size))

    def note_current_pwm(self, fast, slow):
        with self.lock:
            self._last_fast_pwm = fast
            self._last_slow_pwm = slow

    def resume_after_rejects(self):
        with self.lock:
            self.paused_for_rejects = False
            self.reject_batch_count = 0

    def _trigger_reject_stop(self):
        self.controller.send_motor(self._last_fast_pwm, 0)
        def delayed_stop_fast():
            time.sleep(self.MOTOR_STOP_DELAY_S)
            self.controller.send_motor(0, 0)
        self._stop_timer = threading.Thread(target=delayed_stop_fast, daemon=True)
        self._stop_timer.start()

    def get_status(self):
        with self.lock:
            return {
                "running": self.running,
                "start_direction": self.start_direction,
                "paused_for_rejects": self.paused_for_rejects,
                "stop_on_rejects": self.stop_on_rejects,
                "current_direction": self.current_direction,
                "batch_count": self.batch_count,
                "batch_size": self.BATCH_SIZE,
                "reject_batch_count": self.reject_batch_count,
                "reject_stop_size": self.REJECT_STOP_SIZE,
                "session_left": self.session_left,
                "session_right": self.session_right,
                "session_rejected": self.session_rejected,
                "log": list(reversed(self.log[-30:])),
            }

    def _on_event(self, event_type, payload):
        if event_type == "DUCK":
            self._handle_duck_result(payload)

    def _handle_duck_result(self, payload):
        uid = payload.get("uid")
        result = payload.get("result")
        seq = payload.get("seq")
        nummer = get_number_for_uid(uid) if uid and uid != "NONE" else None
        trigger_stop = False

        with self.lock:
            side = None
            if result == "KICK_L":
                side = "LEFT"
                self.session_left += 1
                self.batch_count += 1
            elif result == "KICK_R":
                side = "RIGHT"
                self.session_right += 1
                self.batch_count += 1
            elif result == "UNREADABLE_DROP":
                self.session_rejected += 1
                self.reject_batch_count += 1

            # UI-Synchronisation: Seite für die Anzeige anpassen
            if self.batch_count >= self.BATCH_SIZE:
                self.batch_count = 0
                self.current_direction = "RIGHT" if self.current_direction == "LEFT" else "LEFT"

            if (self.stop_on_rejects and not self.paused_for_rejects
                    and self.reject_batch_count >= self.REJECT_STOP_SIZE):
                self.paused_for_rejects = True
                trigger_stop = True

            entry = {
                "seq": seq, "uid": uid, "nummer": nummer, "side": side,
                "result": result, "ts": time.strftime("%H:%M:%S"),
            }
            self.log.append(entry)
            self.log = self.log[-200:]

        if trigger_stop:
            self._trigger_reject_stop()