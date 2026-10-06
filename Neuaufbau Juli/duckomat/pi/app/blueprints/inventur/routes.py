from flask import Blueprint, render_template, jsonify, request, current_app

inventur_bp = Blueprint("inventur", __name__, url_prefix="/inventur", template_folder="templates")

def session():
    return current_app.sorting_session

def controller():
    return current_app.duck_controller

@inventur_bp.route("/")
def index():
    return render_template("inventur/index.html")

@inventur_bp.route("/api/status")
def status():
    st = session().get_status()
    st["dev_mode"] = controller().get_state().get("dev_mode", False)
    return jsonify(st)

@inventur_bp.route("/api/set_start_direction", methods=["POST"])
def set_start_direction():
    data = request.get_json(force=True)
    ok = session().set_start_direction(data.get("direction", "LEFT"))
    if not ok:
        return jsonify({"ok": False, "error": "direction muss LEFT oder RIGHT sein"}), 400
    return jsonify({"ok": True})

@inventur_bp.route("/api/start", methods=["POST"])
def start():
    session().start()
    return jsonify({"ok": True})

@inventur_bp.route("/api/stop", methods=["POST"])
def stop():
    session().stop()
    return jsonify({"ok": True})

@inventur_bp.route("/api/reset", methods=["POST"])
def reset():
    session().reset_session()
    return jsonify({"ok": True})

@inventur_bp.route("/api/set_stop_on_rejects", methods=["POST"])
def set_stop_on_rejects():
    data = request.get_json(force=True)
    session().set_stop_on_rejects(bool(data.get("enabled", False)))
    return jsonify({"ok": True})

@inventur_bp.route("/api/set_reject_stop_size", methods=["POST"])
def set_reject_stop_size():
    data = request.get_json(force=True)
    session().set_reject_stop_size(int(data.get("size", 100)))
    return jsonify({"ok": True})

@inventur_bp.route("/api/resume_after_rejects", methods=["POST"])
def resume_after_rejects():
    session().resume_after_rejects()
    return jsonify({"ok": True})

@inventur_bp.route("/api/motor", methods=["POST"])
def motor():
    data = request.get_json(force=True)
    fast = max(0, min(255, int(data.get("fast", 0))))
    slow = max(0, min(255, int(data.get("slow", 0))))
    controller().send_motor(fast, slow)
    session().note_current_pwm(fast, slow)
    return jsonify({"ok": True})
