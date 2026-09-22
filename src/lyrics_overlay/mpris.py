"""MPRIS player discovery + selection (pure selection logic separated for tests)."""
from __future__ import annotations
from dataclasses import dataclass, field

@dataclass
class PlayerState:
    bus_name: str
    status: str = "Stopped"          # Playing | Paused | Stopped
    artist: str = ""
    title: str = ""
    album: str = ""
    url: str = ""                    # xesam:url (file path for local search)
    duration_us: int = 0
    position_us: int = 0
    position_read_t: float = 0.0  # monotonic() time when position_us was read
    raw_meta: dict = field(default_factory=dict)

    @property
    def track_key(self) -> str:
        return f"{self.artist} - {self.title}".strip(" -")

def pick_active(players: list[PlayerState]) -> PlayerState | None:
    """Pick what to display no matter where it plays: Playing > Paused > else None.

    Browsers included (unlike some daemons): if it reports Playing via MPRIS, show it.
    Ties broken by order of appearance.
    """
    if not players:
        return None
    for p in players:
        if p.status == "Playing" and (p.artist or p.title):
            return p
    for p in players:
        if p.status == "Paused" and (p.artist or p.title):
            return p
    return None

def metadata_to_state(bus_name: str, status: str, metadata: dict) -> PlayerState:
    """Convert raw MPRIS metadata dict to PlayerState (tolerates variants)."""
    def _s(v, default=""):
        if isinstance(v, (list, tuple)):
            return str(v[0]) if v else default
        return str(v) if v is not None else default
    get = lambda *keys: next((metadata[k] for k in keys if k in metadata), None)
    artist = _s(get("xesam:artist", "artist"))
    title = _s(get("xesam:title", "title"))
    album = _s(get("xesam:album", "album"))
    url = _s(get("xesam:url", "url"))
    try:
        duration = int(get("mpris:length", "length") or 0)
    except (ValueError, TypeError):
        duration = 0
    return PlayerState(bus_name=bus_name, status=status or "Stopped",
                       artist=artist, title=title, album=album,
                       url=url, duration_us=duration)
