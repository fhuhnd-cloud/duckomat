from __future__ import annotations

def normalize_uid(uid: str) -> str:
    if uid is None:
        return ""
    u = str(uid).strip().upper().replace(" ", "")
    # erlaubte Formen: HEX ohne 0x, oder schon "SIM..." – hier final nur echte, aber tolerant
    if u.startswith("0X"):
        u = u[2:]
    return u

