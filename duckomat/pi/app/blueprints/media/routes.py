from flask import render_template

from . import media_bp

# Statische Liste der Medien. Spätere Erweiterung (automatisches Scannen
# eines Ordners, Upload-Funktion) folgt in einem separaten Schritt.
MEDIA_ITEMS = [
    {
        "type": "video",
        "src": "/static/assets/media/videos/GSD_V1.mp4",
    },
]


@media_bp.route("/")
def index():
    return render_template("media/index.html", items=MEDIA_ITEMS)
