# app.py
from flask import Flask
import config
from core import DuckController
from blueprints.hardware import hardware_bp
from blueprints.mapping import mapping_bp
from blueprints.mapping.logic import AutoAssigner
from blueprints.inventur import inventur_bp
from blueprints.inventur.logic import SortingSession

app = Flask(__name__)

app.duck_controller = DuckController(config.SERIAL_PORT, config.BAUD_RATE,
                                      stale_after_s=config.CONNECTION_STALE_S)
app.sorting_session = SortingSession(app.duck_controller)
app.auto_assigner = AutoAssigner(app.duck_controller)

app.register_blueprint(hardware_bp)
app.register_blueprint(mapping_bp)
app.register_blueprint(inventur_bp)

@app.route("/")
def root():
    from flask import redirect
    return redirect("/hardware/")

if __name__ == "__main__":
    app.run(host=config.HOST, port=config.PORT, debug=config.DEBUG)
