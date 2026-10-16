from flask import Blueprint

media_bp = Blueprint(
    "media",
    __name__,
    url_prefix="/media",
    template_folder="templates"
)

from . import routes  # noqa: E402,F401