from flask import Blueprint, render_template, jsonify, request, current_app, Response
from . import db
import csv
import io

mapping_bp = Blueprint("mapping", __name__, url_prefix="/mapping", template_folder="templates")

def controller():
    return current_app.duck_controller

def assigner():
    return current_app.auto_assigner

@mapping_bp.route("/")
def index():
    return render_template("mapping/index.html")

@mapping_bp.route("/api/list")
def list_mappings():
    return jsonify(db.list_all())

@mapping_bp.route("/api/status")
def status():
    st = controller().get_state()
    auto_status = assigner().get_status()
    return jsonify({
        "last_live_uid": st.get("last_uid"),
        "mapped_count": db.count(),
        "dev_mode": st.get("dev_mode", False),
        **auto_status,
    })

@mapping_bp.route("/api/add", methods=["POST"])
def add():
    data = request.get_json(force=True)
    uid = data.get("uid")
    nummer = data.get("nummer")
    if not uid or nummer is None or nummer == "":
        return jsonify({"ok": False, "error": "uid/nummer fehlt"}), 400
    db.add_mapping(uid, nummer)
    return jsonify({"ok": True})

@mapping_bp.route("/api/delete", methods=["POST"])
def delete():
    data = request.get_json(force=True)
    db.delete_mapping(data.get("uid"))
    return jsonify({"ok": True})

@mapping_bp.route("/api/next_number")
def next_number():
    return jsonify({"next": db.next_free_number()})

@mapping_bp.route("/api/auto_start", methods=["POST"])
def auto_start():
    data = request.get_json(force=True) if request.data else {}
    assigner().start(reset=bool(data.get("reset", False)))
    return jsonify({"ok": True})

@mapping_bp.route("/api/auto_stop", methods=["POST"])
def auto_stop():
    assigner().stop()
    return jsonify({"ok": True})

@mapping_bp.route("/api/reset_all", methods=["POST"])
def reset_all():
    db.reset_all()
    return jsonify({"ok": True})

@mapping_bp.route("/api/export_csv")
def export_csv():
    rows = db.list_all()
    buf = io.StringIO()
    writer = csv.DictWriter(buf, fieldnames=["uid", "nummer", "created_at"])
    writer.writeheader()
    writer.writerows(rows)
    return Response(buf.getvalue(), mimetype="text/csv",
                     headers={"Content-Disposition": "attachment; filename=mapping.csv"})

@mapping_bp.route("/api/import_csv", methods=["POST"])
def import_csv():
    file = request.files.get("file")
    if not file:
        return jsonify({"ok": False, "error": "keine Datei"}), 400
    text = file.read().decode("utf-8")
    reader = csv.DictReader(io.StringIO(text))
    rows = list(reader)
    imported = db.import_rows(rows)
    return jsonify({"ok": True, "imported": imported})

@mapping_bp.route("/api/motor", methods=["POST"])
def motor():
    data = request.get_json(force=True)
    fast = max(0, min(255, int(data.get("fast", 0))))
    slow = max(0, min(255, int(data.get("slow", 0))))
    controller().send_motor(fast, slow)
    return jsonify({"ok": True})
