from flask import render_template
from . import media_bp

@media_bp.route("")
@media_bp.route("/")
@media_bp.route("/index")
@media_bp.route("/index.html")
def index():
    return render_template("media/index.html")