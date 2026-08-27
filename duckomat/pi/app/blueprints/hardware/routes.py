from flask import Blueprint, render_template, jsonify, request, current_app
from core import angle_to_us

hardware_bp = Blueprint("hardware", __name__, url_prefix="/hardware", template_folder="templates")

def controller():
    return current_app.duck_controller

@hardware_bp.route("/")
def index():
    return render_template("hardware/index.html")

@hardware_bp.route("/api/status")
def status():
    return jsonify(controller().get_state())

@hardware_bp.route("/api/reset", methods=["POST"])
def reset():
    controller().reset()
    return jsonify({"ok": True})

@hardware_bp.route("/api/motor", methods=["POST"])
def motor():
    data = request.get_json(force=True)
    fast = max(0, min(255, int(data.get("fast", 0))))
    slow = max(0, min(255, int(data.get("slow", 0))))
    controller().send_motor(fast, slow)
    current_app.sorting_session.note_current_pwm(fast, slow)
    return jsonify({"ok": True})

@hardware_bp.route("/api/servo_angle", methods=["POST"])
def servo_angle():
    data = request.get_json(force=True)
    angle = float(data.get("angle"))
    cfg = controller().get_state().get("cfg") or {}
    us_left = cfg.get("posrestl", 990)
    us_right = cfg.get("posrestr", 2030)
    us = angle_to_us(angle, us_left, us_right)
    controller().send_servo_us(us)
    return jsonify({"ok": True, "us": us})

@hardware_bp.route("/api/simtag", methods=["POST"])
def simtag():
    data = request.get_json(force=True)
    ok = controller().send_simtag(data.get("uid", "AABBCC"))
    return jsonify({"ok": ok})

ALLOWED_CONFIG_KEYS = {
    "POSRESTL", "POSKICKL", "POSKICKR", "POSRESTR",
    "KDELAY", "KHOLD", "RHOLD", "RHOLDSW", "INVERT",
    "USTHRESHMM", "USCONFIRM", "USINTERVAL",
    "NFCTIMEOUT", "NFCRETRIES"
}

@hardware_bp.route("/api/config", methods=["POST"])
def set_config():
    data = request.get_json(force=True)
    key = str(data.get("key", "")).upper()
    value = data.get("value")
    if key not in ALLOWED_CONFIG_KEYS or value is None or value == "":
        return jsonify({"ok": False, "error": "invalid key/value"}), 400
    controller().send_config(key, value)
    return jsonify({"ok": True})

@hardware_bp.route("/api/config_refresh", methods=["POST"])
def config_refresh():
    controller().request_cfg()
    return jsonify({"ok": True})

@hardware_bp.route("/api/batch_size", methods=["POST"])
def set_batch_size():
    data = request.get_json(force=True)
    size = int(data.get("size", 100))
    if size < 1 or size > 100000:
        return jsonify({"ok": False, "error": "invalid size"}), 400
    current_app.sorting_session.BATCH_SIZE = size
    return jsonify({"ok": True})

@hardware_bp.route("/api/dev_mode", methods=["POST"])
def set_dev_mode():
    data = request.get_json(force=True)
    controller().set_dev_mode(bool(data.get("enabled", False)))
    return jsonify({"ok": True})

@hardware_bp.route("/api/test_kick", methods=["POST"])
def test_kick():
    data = request.get_json(force=True)
    side = str(data.get("side", "")).upper()
    if side not in ("L", "R"):
        return jsonify({"ok": False, "error": "side muss L oder R sein"}), 400
    controller().send_arm(side)
    controller()._send(f"KICK token=0 side={side}")
    return jsonify({"ok": True})

@hardware_bp.route("/api/nfcmode", methods=["POST"])
def set_nfcmode():
    data = request.get_json(force=True)
    mode = str(data.get("mode", "")).upper()
    if mode not in ("CONTINUOUS", "DUCKONLY"):
        return jsonify({"ok": False, "error": "invalid mode"}), 400
    controller().send_nfcmode(mode)
    return jsonify({"ok": True})
