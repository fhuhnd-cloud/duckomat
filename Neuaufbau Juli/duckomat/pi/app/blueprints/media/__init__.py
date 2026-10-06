from flask import Blueprint

media_bp = Blueprint(
    "media",
    __name__,
    template_folder="templates",
    url_prefix="/media",
)

from . import routes  # noqa: E402,F401
