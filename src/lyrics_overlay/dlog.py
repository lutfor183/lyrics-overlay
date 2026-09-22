"""Tiny file logger: ~/.cache/lyrics-overlay/app.log (observability)."""
from __future__ import annotations
import datetime
import os
import traceback

PATH = os.path.expanduser("~/.cache/lyrics-overlay/app.log")
MAX_BYTES = 300 * 1024

def log(*parts) -> None:
    try:
        d = os.path.dirname(PATH)
        os.makedirs(d, exist_ok=True)
        if os.path.exists(PATH) and os.path.getsize(PATH) > MAX_BYTES:
            try:
                os.replace(PATH, PATH + ".old")
            except Exception:
                pass
        ts = datetime.datetime.now().strftime("%H:%M:%S")
        with open(PATH, "a", encoding="utf-8") as fh:
            fh.write(ts + " " + " ".join(str(p) for p in parts) + "\n")
    except Exception:
        pass

def log_exc(where: str) -> None:
    try:
        log(where, "EXC:", traceback.format_exc(limit=3).replace("\n", " | "))
    except Exception:
        pass
