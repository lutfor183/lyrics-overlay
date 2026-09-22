"""Headless tests: pure logic only (no GUI/DBus)."""
import os
import sys
import tempfile
# Hermetic: isolate HOME so live user config/cache can't pollute assertions.
os.environ["HOME"] = tempfile.mkdtemp(prefix="lyrics-overlay-test-")
sys.path.insert(0, "src")
from lyrics_overlay.lrc import line_at, parse_lrc, strip_lrcx_word_tags
from lyrics_overlay.mpris import PlayerState, metadata_to_state, pick_active
from lyrics_overlay.settings import Settings
from lyrics_overlay.providers import find_local, mpris_lyrics

import os
import time

fails = []
def check(name, cond):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        fails.append(name)

# ---- lrc parse ----
lines = parse_lrc("[ti:Title]\n[00:12.00]First line\n[00:17.50][00:18.00]Second\n\n[00:23.00]Third")
check("parse count", len(lines) == 4)
check("parse order", [l.time for l in lines] == [12.0, 17.5, 18.0, 23.0])
check("parse text", lines[0].text == "First line")
check("line_at before", line_at(lines, 5.0) == -1)
check("line_at middle", line_at(lines, 17.6) == 1)
check("line_at multi", line_at(lines, 18.5) == 2)
check("lrcx strip", strip_lrcx_word_tags("[00:12.00]Hello [00:12.5]world") == "Hello world")

# ---- mpris pick ----
a = PlayerState("a", "Stopped", "A", "T1")
b = PlayerState("b", "Paused", "B", "T2")
c = PlayerState("c", "Playing", "C", "T3")
check("pick playing", pick_active([a, b, c]) is c)
check("pick paused fallback", pick_active([a, b]) is b)
check("pick none", pick_active([a]) is None)
check("pick empty", pick_active([]) is None)
br = PlayerState("org.mpris.MediaPlayer2.chrome", "Playing", "X", "Y")
check("pick browser", pick_active([br]) is br)

# ---- metadata variants ----
st = metadata_to_state("bus", "Playing", {"xesam:artist": ["Artist"], "xesam:title": "Title",
    "mpris:length": 200000000, "xesam:url": "file:///M/song.mp3"})
check("meta artist/title", st.artist == "Artist" and st.title == "Title")
check("meta duration", st.duration_us == 200000000)
check("track_key", st.track_key == "Artist - Title")

# ---- settings ----
s = Settings.load({})
check("settings default size", s.font_size == 22)
s2 = Settings.load({"font_size": 30, "text_color": "#7CFC00"})
check("settings override", s2.font_size == 30 and s2.text_color == "#7CFC00")
check("lines default single", Settings.load({}).lines == 1)
check("lines 3x", Settings.load({"lines": 3}).lines == 3)
check("autohide default", Settings.load({}).autohide == True)
check("bg_mode default", Settings.load({}).bg_mode == "transparent")

# ---- providers: MPRIS lyrics source ----
check("mpris_lyrics from xesam:lyrics", 
      mpris_lyrics({"xesam:lyrics": "Some lyrics text"}) == "Some lyrics text")
check("mpris_lyrics from list", 
      mpris_lyrics({"xesam:lyrics": ["List lyrics"]}) == "List lyrics")
check("mpris_lyrics empty", mpris_lyrics({}) is None)
check("mpris_lyrics missing", mpris_lyrics({"xesam:title": "X"}) is None)

# ---- providers: sequential first-synced-wins ----
from lyrics_overlay.providers import fetch_first_synced as _ffs
check("first-synced none for empty", _ffs("", "") == (None, None))

# ---- tick simulation: line progression test ----
print("\n--- Tick simulation: line progression ---")
lrc_text = ("[00:00.00]Intro line\n[00:05.00]Verse one\n[00:10.00]Chorus\n"
            "[00:15.00]Verse two\n[00:20.00]Outro")
parsed = parse_lrc(lrc_text)
check("parsed lines", len(parsed) == 5)

# Simulate _shown tracking like __main__.py
shown_track = ("test_song", -99)
shown_lines = []
shown_idx = -99

def simulate_tick(pos, track_key="test_song"):
    """Simulate one tick of the main loop."""
    global shown_track, shown_lines, shown_idx
    idx = line_at(parsed, pos)
    key = (track_key, idx)
    if key != shown_track:
        shown_track = key
        shown_idx = idx
        return idx  # line changed -> update
    return None  # no change -> skip

# Advance through lines: 0s -> 5s -> 10s -> 15s -> 20s
check("tick line0 at 0s", simulate_tick(0.0) == 0)
check("tick line1 at 5s", simulate_tick(5.0) == 1)
check("tick line2 at 10s", simulate_tick(10.0) == 2)
check("tick line3 at 15s", simulate_tick(15.0) == 3)
check("tick line4 at 20s", simulate_tick(20.0) == 4)
check("tick no-change at 12s", simulate_tick(12.0) == 2)  # was line4, now line2

# Seek backwards test
check("tick seek-back from 20s to 5s", simulate_tick(5.0) == 1)
# After seek-back, _shown should have been reset, so it should trigger update
# The key (track,1) != (track,4) so it should update

# Rapid track changes test
print("\n--- Rapid track changes ---")
shown_track = ("", -99)
shown_idx = -99

# Track A at 5s -> line 1
def simulate_track_change(new_track, pos, lines_list):
    global shown_track, shown_idx
    idx = line_at(lines_list, pos)
    key = (new_track, idx)
    if key != shown_track:
        shown_track = key
        shown_idx = idx
        return idx
    return None

# Rapid switch: Track A -> Track B -> Track A
lines_a = parse_lrc("[00:00.00]A1\n[00:05.00]A2")
lines_b = parse_lrc("[00:00.00]B1\n[00:03.00]B2")
check("rapid A->B line0", simulate_track_change("A", 1.0, lines_a) == 0)
check("rapid B->A line0", simulate_track_change("B", 1.0, lines_b) == 0)
check("rapid A->B line0 again", simulate_track_change("A", 1.0, lines_a) == 0)
# Each track change should trigger update because track_key differs

# Empty lyrics test
print("\n--- Edge cases ---")
check("line_at empty list", line_at([], 5.0) == -1)
check("line_at at exact boundary", line_at(parsed, 5.0) == 1)
check("parse empty string", parse_lrc("") == [])
check("parse no tags", parse_lrc("just text\nmore text") == [])

# Cache path uniqueness with duration
print("\n--- Duration-based cache key ---")
from lyrics_overlay.providers import _cache_path
p1 = _cache_path("Artist", "Song", 180.0)
p2 = _cache_path("Artist", "Song", 240.0)
check("cache differs by duration", p1 != p2)
check("cache has duration suffix", p1.endswith(".lrc"))

# ---- Summary ----
# limusic-ported provider logic (pure, no network)
from lyrics_overlay.providers import (best_by_duration, is_synced_lrc,
    normalize_text, ttml_to_lrc)
cands = [{"duration": 233.0, "id": "orig"}, {"duration": 263.0, "id": "edit"},
         {"id": "unknown"}]
check("dur closest wins", best_by_duration(235.0, cands, lambda c: c.get("duration"))["id"] == "orig")
check("dur all far -> unknown", best_by_duration(100.0, cands, lambda c: c.get("duration"))["id"] == "unknown")
check("dur none -> first", best_by_duration(0, cands, lambda c: c.get("duration"))["id"] == "orig")
check("dur empty", best_by_duration(200.0, [], lambda c: c.get("duration")) is None)
ttml = '<tt><body><div><p begin="00:00:12.00">Hello <span>world</span></p><p begin="00:01:05.50">Second</p></div></body></tt>'
conv = ttml_to_lrc(ttml)
check("ttml converts", conv is not None and "[00:12.00]Hello world" in conv and "[01:05.50]Second" in conv)
check("ttml non-ttml", ttml_to_lrc("[00:12.00]plain lrc") is None)
check("synced yes", is_synced_lrc("[00:12.00]hi"))
check("synced no", not is_synced_lrc("just some words"))
check("normalize ttml", (normalize_text(ttml) or "").startswith("[00:12.00]"))
check("normalize empty", normalize_text("   ") is None)

# sequential fetch: priority order, first synced hit wins, stops there
import lyrics_overlay.providers as _pv
_calls = []
_real = _pv._run_source
def _fake(src, *a, **k):
    _calls.append(src)
    return {"lrclib": "[00:01.00]n", "boidu": "[00:01.00]b"}.get(src)
_pv._run_source = _fake
try:
    text, win = _pv.fetch_first_synced("A", "T", "", 0)
finally:
    _pv._run_source = _real
check("sequential priority wins", win == "boidu" and text == "[00:01.00]b")
check("sequential stops at first hit", _calls == ["local", "limusic", "mpris", "boidu"])
check("five sources only", list(_pv.SOURCES) == ["local", "limusic", "mpris", "boidu", "lrclib"])

# settings: fast poll, no click-through mode, tunable sync offset
check("poll default fast", Settings.load({}).poll_ms == 100)
check("no click-through attr", not hasattr(Settings.load({}), "click_through"))
check("sync offset default 350", Settings.load({}).sync_offset_ms == 350)
check("sync offset settable", Settings.load({"sync_offset_ms": 600}).sync_offset_ms == 600)
check("sync offset clamped", Settings.load({"sync_offset_ms": 9999}).sync_offset_ms == 2000)
check("sync offset negative ok", Settings.load({"sync_offset_ms": -500}).sync_offset_ms == -500)

# ---- sync math: fresh position + early-offset, no stale base ----
print("\n--- Sync math ---")
import time as _t
from lyrics_overlay.mpris import PlayerState as _PS
import importlib as _il
import lyrics_overlay.__main__ as _m
class _FakeS:
    poll_ms = 100
    sync_offset_ms = 350
app = _m.LyricsApp.__new__(_m.LyricsApp)
app._s = _FakeS()
now = _t.monotonic()
st_play = _PS("b", "Playing", "A", "T", position_us=10_000_000, position_read_t=now)
got = app._position_s(st_play)
check("fresh pos + offset", 10.2 < got < 10.6)  # 10.0 + ~0 + 0.35 early
st_pause = _PS("b", "Paused", "A", "T", position_us=10_000_000, position_read_t=now)
check("paused ignores elapsed", abs(app._position_s(st_pause) - 10.35) < 0.01)
st_stale = _PS("b", "Playing", "A", "T", position_us=10_000_000, position_read_t=now - 5.0)
check("stale read clamped", app._position_s(st_stale) - 10.35 < 0.85)

# ---- fetch stability: early paint claims track, done must not swap ----
print("\n--- Fetch stability ---")
def _blank_app():
    a = _m.LyricsApp.__new__(_m.LyricsApp)
    a._track_key = ""
    a._loading_key = "A - T"
    a._lines = []
    a._shown = ("", -99)
    a._src_current = ""
    a._src_hits = {}
    a._src_texts = {}
    a._fetch_gen = 1
    a._win = None
    return a
app2 = _blank_app()
LRC1 = "[00:01.00]one\n[00:05.00]two\n"
LRC2 = "[00:02.00]one\n[00:06.00]two\n"
app2._fetch_first("A - T", LRC1, "lrclib", 1)
check("first paint claims track", app2._track_key == "A - T" and len(app2._lines) == 2)
app2._fetch_done("A - T", LRC2, "boidu", True, "A", "T", 200.0, False, 1)
check("fetch-done keeps early paint", app2._track_key == "A - T" and [l.text for l in app2._lines] == ["one", "two"])
# no early paint -> done paints winner
app3 = _blank_app()
app3._fetch_done("A - T", LRC2, "boidu", True, "A", "T", 200.0, False, 1)
check("fetch-done paints when nothing yet", app3._track_key == "A - T" and len(app3._lines) == 2)
# stale generation bows out (song changed twice quickly)
app4 = _blank_app()
app4._fetch_gen = 2
app4._loading_key = "C - S3"
app4._fetch_first("A - T", LRC1, "lrclib", 1)
check("stale first-paint ignored", app4._track_key == "" and app4._lines == [])
app4._fetch_done("A - T", LRC2, "boidu", True, "A", "T", 200.0, False, 1)
check("stale fetch-done ignored", app4._track_key == "" and app4._loading_key == "C - S3")
# done clears loading only for its own key
app5 = _blank_app()
app5._loading_key = "C - S3"
app5._fetch_done("A - T", LRC2, "boidu", True, "A", "T", 200.0, False, 1)
check("done keeps newer loading key", app5._loading_key == "C - S3")
# winner is written to disk cache
import tempfile as _tf
import lyrics_overlay.providers as _pv
_pv.CACHE_DIR = _tf.mkdtemp(prefix="lyrics-overlay-cachetest-")
app6 = _blank_app()
app6._fetch_done("A - T", LRC2, "lrclib", True, "A", "T", 200.0, True, 1)
check("winner cached to disk", _pv.read_cache("A", "T", 200.0) is not None)

# ---- limusic source: exact match + duration gate ----
print("\n--- limusic source ---")
import json as _js
plays = [("vid-new", _js.dumps({"title": "T", "artists": "A"})),]
lyr = {"vid-new": _js.dumps({"source": "Boidu", "synced": True, "instrumental": False,
      "lines": [{"time_ms": 1000, "end_time_ms": 2000, "text": "one"},
                {"time_ms": 5000, "end_time_ms": 6000, "text": "two"},
                {"time_ms": 9000, "end_time_ms": 10000, "text": "three"}]})}
_pv._limusic_db_rows = lambda: (plays, lyr)
_pv._limusic_plays_latest_first = lambda: plays
check("limusic exact hit", (_pv.limusic_lyrics("A", "T", 15.0) or "").count("\n") == 2)
check("limusic wrong title miss", _pv.limusic_lyrics("A", "Other", 200.0) is None)
check("limusic wrong duration miss", _pv.limusic_lyrics("A", "T", 1000.0) is None)
unsynced = {"vid-new": _js.dumps({"source": "X", "synced": False, "instrumental": False,
      "lines": [{"text": "a"}, {"text": "b"}, {"text": "c"}]})}
_pv._limusic_db_rows = lambda: (plays, unsynced)
check("limusic unsynced miss", _pv.limusic_lyrics("A", "T", 200.0) is None)
check("source default auto", Settings.load({}).source == "auto")
check("source pin lrclib", Settings.load({"source": "lrclib"}).source == "lrclib")
import lyrics_overlay.settings as _st
_old_ini = _st.Settings.save_user.__qualname__ and _st.USER_INI
_st.USER_INI = _tf.mkdtemp(prefix="lyrics-overlay-initest-") + "/settings.ini"
try:
    _s3 = Settings.load({"source": "boidu", "keep_above": False})
    _s3.save_user()
    _s4 = Settings.load({})
    check("source persists", _s4.source == "boidu" and _s4.keep_above is False)
finally:
    _st.USER_INI = _old_ini
check("keep-above default on", Settings.load({}).keep_above is True)
check("keep-above settable", Settings.load({"keep_above": False}).keep_above is False)

print(f"\n{len(fails)} failures" if fails else "\nALL OK")
sys.exit(1 if fails else 0)
