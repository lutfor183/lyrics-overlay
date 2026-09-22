"""Lyrics lookup: your files -> limusic match -> player -> Boidu -> LRCLIB.

Mirrors limusic (SimoHypers/limusic src-tauri/src/lyrics.rs) provider chain,
minus YouTube-InnerTube (needs API keys). Rules per limusic:
  - synced lines only (plain untimed text never wins over a synced hit)
  - duration match within +-5s, CLOSEST wins (remixes/live cuts rank nearby)
  - unknown lengths rank last, never dropped
"""
from __future__ import annotations
import hashlib
import json
import os
import re
import urllib.parse
import urllib.request

CACHE_DIR = os.path.expanduser("~/.cache/lyrics-overlay")
UA = "lyrics-overlay/0.2"
TOLERANCE_S = 5.0
TIMEOUT = 5.0
PROBE_CAP_S = 25.0

SOURCES = ("local", "limusic", "mpris", "boidu", "lrclib")


# ---------- tiny http ----------

def _get_json(url: str, headers: dict | None = None) -> object | None:
    try:
        req = urllib.request.Request(url, headers={"User-Agent": UA, **(headers or {})})
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
            return json.loads(resp.read().decode("utf-8", "replace"))
    except Exception:
        return None
# ---------- cache + per-track source override ----------

def _key(artist: str, title: str, duration_s: float = 0) -> str:
    dur = str(int(duration_s)) if duration_s and duration_s > 0 else "x"
    return hashlib.sha1(f"{artist}\x00{title}\x00{dur}".lower().encode()).hexdigest()

def _cache_path(artist: str, title: str, duration_s: float = 0) -> str:
    return os.path.join(CACHE_DIR, _key(artist, title, duration_s) + ".lrc")

def read_cache(artist: str, title: str, duration_s: float = 0) -> str | None:
    try:
        p = _cache_path(artist, title, duration_s)
        if os.path.exists(p):
            with open(p, encoding="utf-8", errors="replace") as fh:
                return fh.read() or None
    except Exception:
        pass
    return None

def write_cache(artist: str, title: str, text: str, duration_s: float = 0) -> None:
    try:
        os.makedirs(CACHE_DIR, exist_ok=True)
        with open(_cache_path(artist, title, duration_s), "w", encoding="utf-8") as fh:
            fh.write(text)
    except Exception:
        pass
# ---------- duration matching (limusic best_by_duration) ----------

def best_by_duration(ours: float, cands: list, secs) -> object | None:
    """Closest length within +-5s wins; unknown lengths rank last, not dropped."""
    if not cands:
        return None
    if not ours or ours <= 0:
        return cands[0]
    ranked = []
    for c in cands:
        try:
            d = secs(c)
            dist = abs(float(d) - ours) if d is not None else float("inf")
        except (TypeError, ValueError):
            dist = float("inf")
        ranked.append((dist, c))
    ranked.sort(key=lambda t: t[0])
    best_dist, best = ranked[0]
    if best_dist != float("inf") and best_dist > TOLERANCE_S:
        # every KNOWN length is out of tolerance: fall back to unknown-length
        # candidates only; if none, reject all.
        unknowns = [c for dist, c in ranked if dist == float("inf")]
        return unknowns[0] if unknowns else None
    return best

# ---------- TTML -> LRC (boidu answers ttml first) ----------

_TTML_P = re.compile(
    r'<p\s+[^>]*begin="(?:(\d+):)?(\d+):([\d.]+)"[^>]*>(.*?)</p>',
    re.DOTALL | re.IGNORECASE)
_TAG = re.compile(r"<[^>]+>")

def ttml_to_lrc(text: str) -> str | None:
    if "<p" not in text or "begin=" not in text:
        return None
    out = []
    for m in _TTML_P.finditer(text):
        h = int(m.group(1) or 0)
        mm = int(m.group(2))
        ss = float(m.group(3))
        total = h * 3600 + mm * 60 + ss
        line = _TAG.sub("", m.group(4)).strip().replace("\n", " ")
        if line:
            out.append(f"[{int(total // 60):02d}:{total % 60:05.2f}]{line}")
    return "\n".join(out) if out else None

def normalize_text(text: str) -> str | None:
    """Provider payload -> plain LRC (None if empty/unsynced-garbage)."""
    if not text or not text.strip():
        return None
    t = text.strip()
    if t.startswith("<") and ("<tt" in t[:200] or "<p" in t):
        t = ttml_to_lrc(t) or ""
    if not t.strip():
        return None
    return t

def is_synced_lrc(text: str) -> bool:
    import re as _re
    return bool(_re.search(r"\[\d{1,3}:\d{2}[.:]\d+\]", text or ""))

# ---------- local + mpris ----------

def _candidate_dirs(music_url: str) -> list[str]:
    home = os.path.expanduser("~")
    music = os.environ.get("XDG_MUSIC_DIR", os.path.join(home, "Music"))
    dirs = [os.path.join(home, ".lyrics"), music, home]
    if music_url.startswith("file://"):
        try:
            from urllib.parse import unquote, urlparse
            d = os.path.dirname(unquote(urlparse(music_url).path))
            if d:
                dirs.insert(0, d)
        except Exception:
            pass
    seen, out = set(), []
    for d in dirs:
        if d and d not in seen and os.path.isdir(d):
            seen.add(d)
            out.append(d)
    return out

def find_local(artist: str, title: str, music_url: str = "") -> str | None:
    if music_url.startswith("file://"):
        try:
            from urllib.parse import unquote, urlparse
            path = unquote(urlparse(music_url).path)
            root, _ = os.path.splitext(path)
            for ext in (".lrcx", ".lrc"):
                cand = root + ext
                if os.path.exists(cand):
                    with open(cand, encoding="utf-8", errors="replace") as fh:
                        return fh.read() or None
        except Exception:
            pass
    names = []
    if artist and title:
        names.append(f"{artist} - {title}")
    if title:
        names.append(title)
    for d in _candidate_dirs(music_url):
        for b in names:
            for ext in (".lrcx", ".lrc"):
                p = os.path.join(d, b + ext)
                if os.path.exists(p):
                    try:
                        with open(p, encoding="utf-8", errors="replace") as fh:
                            return fh.read() or None
                    except Exception:
                        continue
    return None

def mpris_lyrics(metadata: dict) -> str | None:
    for key in ("xesam:lyrics", "mpris:lyrics", "xesam:comment"):
        v = (metadata or {}).get(key)
        if isinstance(v, (list, tuple)):
            v = v[0] if v else ""
        if isinstance(v, str) and v.strip():
            return normalize_text(v)
    return None

_get_mpris_lyrics = mpris_lyrics  # back-compat alias

# ---------- limusic (same lyrics the player itself shows) ----------

def _limusic_paths():
    home = os.path.expanduser("~")
    base = os.path.join(home, ".local/share/com.limusic.desktop")
    return (os.path.join(base, "limusic.sqlite"),
            os.path.join(base, "limusic.log"))

def _limusic_plays_latest_first():
    """[(video_id, song_json)] most-recently-played first, or []."""
    db, _ = _limusic_paths()
    if not os.path.exists(db):
        return []
    try:
        import sqlite3 as _sq
        con = _sq.connect(f"file:{db}?mode=ro", uri=True, timeout=2.0)
        try:
            return list(con.execute(
                "SELECT video_id, song_json FROM plays "
                "ORDER BY played_at DESC"))
        finally:
            con.close()
    except Exception:
        return []

def _limusic_db_rows():
    """(plays[(video_id, song_json)], lyrics{video_id: json}) or (None, None)."""
    db, _ = _limusic_paths()
    if not os.path.exists(db):
        return None, None
    try:
        import sqlite3 as _sq
        con = _sq.connect(f"file:{db}?mode=ro", uri=True, timeout=2.0)
        try:
            plays = con.execute(
                "SELECT video_id, song_json FROM plays").fetchall()
            lyrics = dict(con.execute(
                "SELECT video_id, lyrics FROM lyrics_cache").fetchall())
        finally:
            con.close()
        return plays, lyrics
    except Exception:
        pass
    # fallback: copy db+wal and read the copy (never locks the player)
    try:
        import sqlite3 as _sq
        import tempfile as _tf
        tmp = _tf.mkdtemp(prefix="lyrics-overlay-limusic-")
        for ext in ("", "-shm", "-wal"):
            s = db + ext
            if os.path.exists(s):
                with open(s, "rb") as fh:
                    data = fh.read()
                with open(os.path.join(tmp, "l.sqlite" + ext), "wb") as fh:
                    fh.write(data)
        con = _sq.connect(os.path.join(tmp, "l.sqlite"), timeout=2.0)
        try:
            plays = con.execute(
                "SELECT video_id, song_json FROM plays").fetchall()
            lyrics = dict(con.execute(
                "SELECT video_id, lyrics FROM lyrics_cache").fetchall())
        finally:
            con.close()
        return plays, lyrics
    except Exception:
        return None, None

def _norm_title(s: str) -> str:
    return re.sub(r"\s+", " ", (s or "").strip().lower())

def limusic_lyrics(artist: str, title: str,
                   duration_s: float = 0) -> str | None:
    """Lyrics limusic itself already matched for this video.

    Candidates: exact title/artist rows in limusic's play history first,
    then its most recently resolved YouTube videos. Each candidate's cached
    JSON is converted to LRC only if it is synced AND its length plausibly
    matches the playing track (same-length preference, enforced).
    """
    if not title and not artist:
        return None
    _, lyrics_map = _limusic_db_rows()
    if not lyrics_map:
        return None
    plays = _limusic_plays_latest_first()
    # Candidates: limusic's own play history, exact title+artist match,
    # most recent first. (The live log is prefetch noise — it resolves
    # upcoming queue items in advance — so it must NOT be used: it once
    # matched a completely different song.)
    cands: list = []
    try:
        nt, na = _norm_title(title), _norm_title(artist)
        for vid, song in plays or []:
            try:
                d = json.loads(song) if isinstance(song, str) else {}
            except Exception:
                continue
            if not isinstance(d, dict):
                continue
            if (_norm_title(d.get("title")) == nt and na
                    and na in _norm_title(d.get("artists", ""))):
                if vid not in cands:
                    cands.append(vid)
    except Exception:
        pass
    for vid in cands:
        js = lyrics_map.get(vid)
        if not js:
            continue
        try:
            d = json.loads(js) if isinstance(js, str) else js
        except Exception:
            continue
        if not isinstance(d, dict):
            continue
        if d.get("instrumental"):
            continue
        if not d.get("synced"):
            continue
        raw = d.get("lines") or []
        timed = []
        for ln in raw:
            if not isinstance(ln, dict):
                continue
            t = ln.get("time_ms")
            tx = (ln.get("text") or "").strip()
            if isinstance(t, (int, float)) and tx:
                timed.append((float(t), tx))
        if len(timed) < 3:
            continue
        timed.sort(key=lambda t: t[0])
        if duration_s and duration_s > 0:
            try:
                last_end = max(float((ln.get("end_time_ms")
                                               if isinstance(ln, dict) else None)
                                      or (ln.get("time_ms") if isinstance(ln, dict)
                                          else 0) or 0)
                               for ln in raw if isinstance(ln, dict)) / 1000.0
            except Exception:
                last_end = 0.0
            if last_end > duration_s + 12:
                continue  # longer than the playing track: wrong version
            if last_end < min(45.0, duration_s * 0.5):
                continue  # far shorter than the playing track: wrong version
        out = []
        for tms, tx in timed:
            t = tms / 1000.0
            out.append(f"[{int(t // 60):02d}:{t % 60:05.2f}]{tx}")
        return "\n".join(out)
    return None


# ---------- boidu ----------

def fetch_boidu(artist: str, title: str, album: str = "",
                duration_s: float = 0) -> str | None:
    if not (artist or "").strip() and not (title or "").strip():
        return None
    qs = {"s": title, "a": artist}
    if album:
        qs["al"] = album
    if duration_s and duration_s > 0:
        qs["d"] = str(int(round(duration_s)))
    url = "https://lyrics-api.boidu.dev/getLyrics?" + urllib.parse.urlencode(qs)
    data = _get_json(url)
    if not isinstance(data, dict):
        return None
    for field in ("ttml", "syncedLyrics", "lyrics", "lrc"):
        v = data.get(field)
        if isinstance(v, str) and v.strip():
            hit = normalize_text(v)
            if hit and is_synced_lrc(hit):
                return hit
    return None

# ---------- lrclib ----------

def fetch_lrclib(artist: str, title: str, duration_s: float = 0,
                 album: str = "") -> str | None:
    if not artist or not title:
        return None
    qs = {"artist_name": artist, "track_name": title}
    if album:
        qs["album_name"] = album
    if duration_s and duration_s > 0:
        qs["duration"] = int(duration_s)
    url = "https://lrclib.net/api/get?" + urllib.parse.urlencode(qs)
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "lyrics-overlay/0.2"})
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
            data = json.loads(resp.read().decode("utf-8", "replace"))
        return data.get("syncedLyrics") or None
    except Exception:
        return None

def fetch_lrclib_search(artist: str, title: str,
                        duration_s: float = 0) -> str | None:
    """Fuzzy fallback: synced candidate with CLOSEST duration (+-5s)."""
    if not artist or not title:
        return None
    url = ("https://lrclib.net/api/search?" + urllib.parse.urlencode(
        {"artist_name": artist, "track_name": title}))
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "lyrics-overlay/0.2"})
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
            items = json.loads(resp.read().decode("utf-8", "replace"))
    except Exception:
        return None
    if not isinstance(items, list):
        return None
    synced = [t for t in items
              if isinstance(t, dict) and (t.get("syncedLyrics") or "").strip()]
    if not synced:
        return None
    best = best_by_duration(duration_s, synced,
                            lambda t: t.get("duration"))
    if best is None:
        return None
    return best.get("syncedLyrics") or None

# ---------- chain ----------

def _run_source(src: str, artist: str, title: str, music_url: str,
                duration_s: float, album: str, metadata: dict) -> str | None:
    try:
        if src == "local":
            return find_local(artist, title, music_url)
        if src == "limusic":
            return limusic_lyrics(artist, title, duration_s)
        if src == "mpris":
            return mpris_lyrics(metadata or {})
        if src == "boidu":
            return fetch_boidu(artist, title, album, duration_s)
        if src == "lrclib":
            return (fetch_lrclib(artist, title, duration_s, album)
                    or fetch_lrclib_search(artist, title, duration_s))
    except Exception:
        pass
    return None

def fetch_first_synced(artist: str, title: str, music_url: str = "",
                       duration_s: float = 0, album: str = "",
                       metadata: dict | None = None) -> tuple:
    """Try each source in priority order; return (text, source) of the first
    synced hit, or (None, None). Slow and simple on purpose: it runs in a
    background thread, and first-hit-wins means timestamps always come from
    the highest-priority source (same-length preference)."""
    args = (artist, title, music_url, duration_s, album, metadata or {})
    for src_name in SOURCES:
        try:
            hit = _run_source(src_name, *args)
        except Exception:
            hit = None
        if hit and is_synced_lrc(hit):
            return hit, src_name
    return None, None
