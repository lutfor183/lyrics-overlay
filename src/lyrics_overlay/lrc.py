"""LRC parsing + line lookup (pure logic, no GUI/DBus)."""
from __future__ import annotations
import re
from dataclasses import dataclass

TAG = re.compile(r"\[(\d{1,3}):(\d{2})(?:[.:](\d{1,3}))?\]")

@dataclass
class LyricLine:
    time: float  # seconds
    text: str

def parse_lrc(text: str) -> list[LyricLine]:
    """Parse standard LRC. Supports multiple tags per line, skips metadata tags."""
    lines: list[LyricLine] = []
    for raw in text.splitlines():
        tags = list(TAG.finditer(raw))
        if not tags:
            continue
        content = TAG.sub("", raw).strip()
        if not content:
            continue
        # skip header tags like [ar:...] [ti:...] (non-numeric already excluded by regex,
        # but keep guard for e.g. [00:00.00] with empty text handled above)
        for m in tags:
            try:
                mm = int(m.group(1))
                ss = int(m.group(2))
                frac = m.group(3) or "0"
                # 2 digits = centiseconds, 3 digits = milliseconds
                ms = int((frac + "000")[:3])
                t = mm * 60 + ss + ms / 1000.0
                lines.append(LyricLine(time=t, text=content))
            except ValueError:
                continue
    lines.sort(key=lambda l: l.time)
    return lines

def line_at(lines: list[LyricLine], pos: float) -> int:
    """Index of the active line at pos seconds (-1 if none yet)."""
    idx = -1
    for i, ln in enumerate(lines):
        if ln.time <= pos:
            idx = i
        else:
            break
    return idx

def strip_lrcx_word_tags(text: str) -> str:
    """Degrade LRCX word-level tags to plain lines (lightweight client)."""
    # LRCX: [00:12.00][00:12.20]First [00:12.50]word ... -> keep line text
    return TAG.sub("", text).strip()
