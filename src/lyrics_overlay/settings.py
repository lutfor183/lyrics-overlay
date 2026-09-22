"""Settings: ~/.config/lyrics-overlay/settings.ini + CLI overrides.

The window is ALWAYS clickable/draggable (left-drag moves, right-click
opens the menu). There is no click-through mode: it made the caption
impossible to move and hid the menu, which is never what you want.
"""
from __future__ import annotations
import configparser
import os
from dataclasses import dataclass

APP = "lyrics-overlay"
USER_INI = os.path.expanduser("~/.config/lyrics-overlay/settings.ini")

DEFAULTS = {
    "font_family": "Sans",
    "font_size": "22",
    "secondary_scale": "0.75",
    "text_color": "#ffffff",
    "bg_color": "#00000000",   # RRGGBBAA (fully transparent default)
    "bg_mode": "transparent",  # transparent | minimal_dark | light | custom
    "bg_custom_color": "#00000000",
    "text_shadow": "true",
    "width": "480",
    "anchor": "bottom",        # bottom | top (hint; window is free-floating)
    "margin": "48",
    "poll_ms": "100",
    "lines": "1",              # 1 = single YT-caption line, 3 = prev/current/next
    "cache": "true",
    "autohide": "true",        # auto-hide when nothing plays
    "autohide_timeout": "5",   # seconds to wait before hiding
    "keep_above": "true",    # always on top of other windows
    "source": "auto",      # lyrics source: auto or one of local/limusic/mpris/boidu/lrclib
    "sync_offset_ms": "350",   # show each line this many ms EARLY
                               # (compensates render + perception lag;
                               #  negative = show late). Range -2000..2000.
}

@dataclass
class Settings:
    font_family: str = "Sans"
    font_size: int = 22
    secondary_scale: float = 0.75
    text_color: str = "#ffffff"
    bg_color: str = "#00000000"
    bg_mode: str = "transparent"
    bg_custom_color: str = "#00000000"
    text_shadow: bool = True
    width: int = 480
    anchor: str = "bottom"
    margin: int = 48
    poll_ms: int = 100
    lines: int = 1
    cache: bool = True
    autohide: bool = True
    autohide_timeout: int = 5
    keep_above: bool = True
    source: str = "auto"
    sync_offset_ms: int = 350

    @classmethod
    def load(cls, overrides: dict | None = None) -> "Settings":
        cfg = configparser.ConfigParser()
        cfg["display"] = dict(DEFAULTS)
        for path in ("/etc/lyrics-overlay/settings.ini", USER_INI):
            try:
                if os.path.exists(path):
                    cfg.read(path)
            except Exception:
                pass
        d = dict(cfg["display"]) if "display" in cfg else dict(DEFAULTS)
        if overrides:
            for k, v in overrides.items():
                if v is not None:
                    d[k] = str(v)
        def _bool(v: str) -> bool:
            return str(v).strip().lower() in ("1", "true", "yes", "on")
        def _clamp(v, lo, hi, default):
            try:
                return max(lo, min(hi, int(v)))
            except (TypeError, ValueError):
                return default
        try:
            return cls(
                font_family=d.get("font_family", "Sans"),
                font_size=int(d.get("font_size", 22)),
                secondary_scale=float(d.get("secondary_scale", 0.75)),
                text_color=d.get("text_color", "#ffffff"),
                bg_color=d.get("bg_color", "#00000000"),
                bg_mode=d.get("bg_mode", "transparent"),
                bg_custom_color=d.get("bg_custom_color", "#00000000"),
                text_shadow=_bool(d.get("text_shadow", "true")),
                width=int(d.get("width", 480)),
                anchor=d.get("anchor", "bottom"),
                margin=int(d.get("margin", 48)),
                poll_ms=_clamp(d.get("poll_ms", 100), 50, 1000, 100),
                lines=max(1, min(3, int(d.get("lines", 1)))),
                cache=_bool(d.get("cache", "true")),
                autohide=_bool(d.get("autohide", "true")),
                autohide_timeout=max(1, int(d.get("autohide_timeout", 5))),
                keep_above=_bool(d.get("keep_above", "true")),
                source=(d.get("source", "auto") or "auto").strip().lower(),
                sync_offset_ms=_clamp(d.get("sync_offset_ms", 350),
                                      -2000, 2000, 350),
            )
        except ValueError:
            return cls()

    def save_user(self) -> None:
        try:
            os.makedirs(os.path.dirname(USER_INI), exist_ok=True)
            cfg = configparser.ConfigParser()
            cfg["display"] = {
                "font_family": self.font_family,
                "font_size": str(self.font_size),
                "secondary_scale": str(self.secondary_scale),
                "text_color": self.text_color,
                "bg_color": self.bg_color,
                "bg_mode": self.bg_mode,
                "bg_custom_color": self.bg_custom_color,
                "text_shadow": str(self.text_shadow).lower(),
                "width": str(self.width),
                "anchor": self.anchor,
                "margin": str(self.margin),
                "poll_ms": str(self.poll_ms),
                "lines": str(self.lines),
                "cache": str(self.cache).lower(),
                "autohide": str(self.autohide).lower(),
                "autohide_timeout": str(self.autohide_timeout),
                "keep_above": str(self.keep_above).lower(),
                "source": self.source,
                "sync_offset_ms": str(self.sync_offset_ms),
            }
            with open(USER_INI, "w") as fh:
                cfg.write(fh)
        except Exception:
            pass

    def resolved_bg(self) -> str:
        """Return the effective background color CSS value based on bg_mode."""
        if self.bg_mode == "transparent":
            return "#00000000"
        elif self.bg_mode == "minimal_dark":
            return "#000000B3"  # ~70% black
        elif self.bg_mode == "light":
            return "#FFFFFFCC"
        elif self.bg_mode == "custom":
            return self.bg_custom_color
        return self.bg_color
