from __future__ import annotations

def missing_numbers(expected_count: int, seen: set[int]) -> list[int]:
    expected_count = int(expected_count)
    out: list[int] = []
    for n in range(1, expected_count + 1):
        if n not in seen:
            out.append(n)
    return out

