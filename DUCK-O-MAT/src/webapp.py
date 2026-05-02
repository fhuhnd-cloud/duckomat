from __future__ import annotations

import csv
import io
import subprocess
from pathlib import Path
from typing import Callable, Optional

from flask import Flask, Response, jsonify, redirect, render_template, request, url_for

from src.core import InventoryController
from src.mapping_store import MappingStore
from src.util import normalize_uid


def create_app(
    controller: InventoryController,
    mapping: MappingStore,
    uid_source: Optional[Callable[[], Optional[str]]] = None,
) -> Flask:
    project_root = Path(__file__).resolve().parent.parent
    templates_dir = project_root / "templates"

    # Static wieder aktivieren (für Logo etc.)
    static_dir = project_root / "static"
    app = Flask(
        __name__,
        template_folder=str(templates_dir),
        static_folder=str(static_dir),
        static_url_path="/static",
    )

    # ---- Pages ----
    @app.get("/")
    def index():
        return render_template("index.html")

    @app.get("/mapping")
    def mapping_page():
        q = (request.args.get("q", "") or "").strip()
        limit = int(request.args.get("limit", "200") or "200")
        offset = int(request.args.get("offset", "0") or "0")

        items = mapping.list_mappings(limit=limit, offset=offset, q=q)
        total = mapping.count_mappings()

        return render_template(
            "mapping.html",
            items=items,
            total=total,
            q=q,
            limit=limit,
            offset=offset,
            expected_count=controller.expected_count,
        )

    # ---- API: State ----
    @app.get("/api/state")
    def api_state():
        return jsonify(controller.snapshot())

    # ---- API: Config ----
    # UI darf nur inventory_id + expected_count setzen.
    # window_before/after bleiben ausschließlich aus config.json / Code-Konfiguration.
    @app.post("/api/config")
    def api_config():
        try:
            inventory_id = (request.form.get("inventory_id", "proto") or "proto").strip()
            expected = int(request.form.get("expected_count", "0"))

            if expected <= 0:
                return redirect(url_for("index", msg="expected_count muss > 0 sein.", ok=0))

            # Fenster NICHT aus UI übernehmen (nur behalten wie aktuell im Controller)
            wb = int(getattr(controller, "window_before_ms", 100))
            wa = int(getattr(controller, "window_after_ms", 300))

            controller.configure(inventory_id, expected, wb, wa)
            return redirect(url_for("index", msg="Konfiguration gespeichert.", ok=1))
        except Exception as e:
            return redirect(url_for("index", msg=f"Fehler: {e}", ok=0))

    # ---- API: Control ----
    @app.post("/api/start")
    def api_start():
        controller.start()
        return redirect(url_for("index", msg="Inventur gestartet.", ok=1))

    @app.post("/api/stop")
    def api_stop():
        controller.stop()
        return redirect(url_for("index", msg="Inventur gestoppt.", ok=1))

    @app.post("/api/reset")
    def api_reset():
        controller.reset_counts()
        return redirect(url_for("index", msg="Zähler zurückgesetzt.", ok=1))

    # ---- Scan (für Mapping-Buttons) ----
    @app.get("/api/scan/last")
    def api_scan_last():
        if uid_source is None:
            return jsonify({"ok": False, "uid": None, "err": "uid_source=None"})
        uid = uid_source()
        return jsonify({"ok": True, "uid": normalize_uid(uid) if uid else None})

    def _scan_uid() -> Optional[str]:
        if uid_source is None:
            return None
        uid = uid_source()
        return normalize_uid(uid) if uid else None

    # ---- Mapping Actions ----
    @app.post("/api/mapping/assign_scan")
    def api_mapping_assign_scan():
        try:
            uid = _scan_uid()
            if not uid:
                return redirect(url_for("mapping_page", msg="Kein Tag erkannt. Bitte erneut scannen.", ok=0))

            number = int(request.form.get("number", "0"))
            if number <= 0:
                return redirect(url_for("mapping_page", msg="Nummer muss > 0 sein.", ok=0))

            if mapping.get_number(uid) is not None:
                return redirect(url_for("mapping_page", msg=f"UID {uid} ist bereits gemappt.", ok=0))
            if mapping.is_number_taken(number):
                return redirect(url_for("mapping_page", msg=f"Nummer {number} ist bereits vergeben.", ok=0))

            mapping.assign(uid, number)
            return redirect(url_for("mapping_page", msg=f"Assign OK: {uid} → {number}", ok=1))
        except Exception as e:
            return redirect(url_for("mapping_page", msg=f"Assign Fehler: {e}", ok=0))

    @app.post("/api/mapping/assign_next_scan")
    def api_mapping_assign_next_scan():
        try:
            uid = _scan_uid()
            if not uid:
                return redirect(url_for("mapping_page", msg="Kein Tag erkannt. Bitte erneut scannen.", ok=0))
            if mapping.get_number(uid) is not None:
                return redirect(url_for("mapping_page", msg=f"UID {uid} ist bereits gemappt.", ok=0))

            nxt = mapping.next_free_number(controller.expected_count)
            if nxt is None:
                return redirect(url_for("mapping_page", msg="Keine freie Nummer mehr.", ok=0))

            mapping.assign(uid, nxt)
            return redirect(url_for("mapping_page", msg=f"Auto-Assign OK: {uid} → {nxt}", ok=1))
        except Exception as e:
            return redirect(url_for("mapping_page", msg=f"Auto-Assign Fehler: {e}", ok=0))

    @app.post("/api/mapping/reassign_scan")
    def api_mapping_reassign_scan():
        try:
            uid = _scan_uid()
            if not uid:
                return redirect(url_for("mapping_page", msg="Kein Tag erkannt. Bitte erneut scannen.", ok=0))

            number = int(request.form.get("number", "0"))
            if number <= 0:
                return redirect(url_for("mapping_page", msg="Nummer muss > 0 sein.", ok=0))

            mapping.reassign_number(number, uid)
            return redirect(url_for("mapping_page", msg=f"Reassign OK: {number} → {uid}", ok=1))
        except Exception as e:
            return redirect(url_for("mapping_page", msg=f"Reassign Fehler: {e}", ok=0))

    # ---- Export CSV ----
    @app.get("/api/mapping/export.csv")
    def api_mapping_export_csv():
        rows = mapping.list_all()
        s = io.StringIO()
        w = csv.writer(s)
        w.writerow(["number", "uid"])
        for r in rows:
            w.writerow([r["number"], r["uid"]])

        out = s.getvalue().encode("utf-8")
        return Response(
            out,
            headers={
                "Content-Type": "text/csv; charset=utf-8",
                "Content-Disposition": 'attachment; filename="duckomat_mapping.csv"',
            },
        )

    # ---- Mapping Clear (nur lokal) ----
    @app.post("/api/mapping/clear")
    def api_mapping_clear():
        if request.remote_addr not in ("127.0.0.1", "::1"):
            return redirect(url_for("mapping_page", msg="Clear nur lokal am Gerät erlaubt.", ok=0))
        mapping.clear_all()
        return redirect(url_for("mapping_page", msg="Mapping wurde geleert.", ok=1))

    # ---- Shutdown (nur lokal) ----
    @app.post("/api/shutdown")
    def api_shutdown():
        if request.remote_addr not in ("127.0.0.1", "::1"):
            return redirect(url_for("index", msg="Shutdown nur lokal am Gerät erlaubt.", ok=0))
        try:
            subprocess.run(["sudo", "/sbin/shutdown", "-h", "now"], check=True)
            return redirect(url_for("index", msg="Shutdown wird ausgeführt…", ok=1))
        except Exception as e:
            return redirect(url_for("index", msg=f"Shutdown fehlgeschlagen: {e}", ok=0))

    return app
