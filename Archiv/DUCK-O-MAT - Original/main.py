import json
from pathlib import Path

from src.storage import init_db
from src.mapping_store import MappingStore
from src.core import InventoryController
from src.webapp import create_app

from src.io.arduino_usb import ArduinoUSB
from src.io.pn532_i2c import PN532I2C

CONFIG_PATH = Path("config.json")


def load_config() -> dict:
    return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))


def main() -> None:
    init_db()
    cfg = load_config()

    mapping = MappingStore()
    controller = InventoryController(mapping)

    controller.configure(
        inventory_id=str(cfg.get("inventory_id", "proto_01")),
        expected_count=int(cfg.get("expected_count", 1000)),
        wb=int(cfg.get("window_before_ms", 100)),
        wa=int(cfg.get("window_after_ms", 300)),
    )

    io_mode = str(cfg.get("io_mode", "hw")).lower().strip()
    if io_mode != "hw":
        raise RuntimeError("Dieses Projekt ist final auf HW ausgelegt. Setze io_mode auf 'hw'.")

    enable_arduino = bool(cfg.get("enable_arduino", False))
    enable_pn532 = bool(cfg.get("enable_pn532", True))

    services = []
    pn_service = None

    if enable_arduino:
        services.append(
            ArduinoUSB(
                controller,
                port=str(cfg.get("arduino_port", "AUTO")),
                baud=int(cfg.get("arduino_baud", 115200)),
            )
        )

    if enable_pn532:
        pn_service = PN532I2C(
            controller,
            bus_id=int(cfg.get("pn532_i2c_bus", 1)),
            poll_s=float(cfg.get("pn532_poll_s", 0.12)),
        )
        services.append(pn_service)

    for s in services:
        s.start()

    uid_source = (lambda: pn_service.get_last_uid(30.0)) if pn_service else None
    app = create_app(controller, mapping, uid_source=uid_source)

    # Auf dem Pi immer im LAN/WLAN erreichbar
    app.run(host="0.0.0.0", port=5000, debug=False, threaded=False)


if __name__ == "__main__":
    main()

