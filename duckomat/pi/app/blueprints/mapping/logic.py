# blueprints/mapping/logic.py
import threading
from . import db

class AutoAssigner:
    def __init__(self, controller):
        self.controller = controller
        self.lock = threading.Lock()
        self.active = False
        self.last_assigned = None
        controller.add_listener(self._on_event)

    def start(self, reset=False):
        with self.lock:
            if reset:
                db.reset_all()
            self.active = True
            self.last_assigned = None

    def stop(self):
        with self.lock:
            self.active = False

    def _on_event(self, event_type, payload):
        if event_type != "TAG":
            return
        with self.lock:
            if not self.active:
                return
            uid = payload.get("uid")
            if not uid or uid == "NONE":
                return
            if db.get_number_for_uid(uid) is not None:
                return
            nummer = db.next_free_number()
            db.add_mapping(uid, nummer)
            self.last_assigned = {"uid": uid, "nummer": nummer}

    def get_status(self):
        with self.lock:
            return {"auto_active": self.active, "last_assigned": self.last_assigned}
