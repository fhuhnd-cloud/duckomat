from __future__ import annotations

from dataclasses import dataclass, field

@dataclass
class LiveState:
    running: bool = False

    ls_total: int = 0
    ok_reads: int = 0
    timeouts: int = 0
    unknown_uids: int = 0

    last_seq: int | None = None
    last_uid: str | None = None
    last_number: int | None = None
    last_result: str | None = None  # "A" oder "B"
    last_reason: str | None = None

    seen_numbers: set[int] = field(default_factory=set)

