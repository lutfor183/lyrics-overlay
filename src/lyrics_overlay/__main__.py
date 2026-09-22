"""lyrics-overlay entry point: any-player MPRIS -> lrclib -> transparent overlay."""
from __future__ import annotations
import argparse
import threading
import time

import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib, Gtk  # noqa: E402

from . import __version__
from .lrc import line_at, parse_lrc
from .mpris import PlayerState, metadata_to_state, pick_active
from .dlog import log, log_exc
from .overlay import LyricsWindow
from .providers import fetch_first_synced, read_cache, write_cache
from .settings import Settings

MPRIS_PREFIX = "org.mpris.MediaPlayer2."

class Tracker:
    """DBus MPRIS tracker: sees every player, no matter where music plays."""

    def __init__(self) -> None:
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.players: dict[str, PlayerState] = {}
        self._proxies: dict[str, Gio.DBusProxy] = {}
        self._sub_ids: list[int] = []
        self.refresh_players()
        try:
            self._sub_ids.append(self.bus.signal_subscribe(
                "org.freedesktop.DBus", "org.freedesktop.DBus",
                "NameOwnerChanged", "/org/freedesktop/DBus", None,
                Gio.DBusSignalFlags.NONE, self._on_name_owner, None))
        except Exception:
            pass

    def close(self) -> None:
        for sid in self._sub_ids:
            try:
                self.bus.signal_unsubscribe(sid)
            except Exception:
                pass
        self._sub_ids.clear()

    # -- discovery --
    def _list_names(self) -> list[str]:
        try:
            res = self.bus.call_sync(
                "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "ListNames", None, None,
                Gio.DBusCallFlags.NONE, 2000, None)
            return [str(n) for n in res[0] if str(n).startswith(MPRIS_PREFIX)]
        except Exception:
            return []

    def refresh_players(self) -> None:
        names = set(self._list_names())
        for dead in [n for n in self._proxies if n not in names]:
            # Closed players must go: a stale "Playing" ghost would freeze
            # or yank the lyrics (wrong position source).
            self._proxies.pop(dead, None)
            self.players.pop(dead, None)
        for name in names:
            if name not in self._proxies:
                self._attach(name)
        for name in list(self._proxies):
            self._read_player(name)

    def _attach(self, bus_name: str) -> None:
        try:
            proxy = Gio.DBusProxy.new_sync(
                self.bus, Gio.DBusProxyFlags.NONE, None,
                bus_name, "/org/mpris/MediaPlayer2",
                "org.mpris.MediaPlayer2.Player", None)
            self._proxies[bus_name] = proxy
            self.bus.signal_subscribe(
                bus_name, "org.freedesktop.DBus.Properties",
                "PropertiesChanged", "/org/mpris/MediaPlayer2", None,
                Gio.DBusSignalFlags.NONE, self._on_props, None)
        except Exception:
            pass

    # -- signals --
    def _on_name_owner(self, *args) -> None:
        GLib.idle_add(lambda: (self.refresh_players(), False)[1])

    def _on_props(self, conn, sender, path, iface, sig, params, data) -> None:
        bus_name = str(sender or "")
        if bus_name.startswith(MPRIS_PREFIX):
            GLib.idle_add(lambda: (self._read_player(bus_name), False)[1])

    # -- state --
    @staticmethod
    def _unpack(v):
        try:
            if v is None:
                return None
            if hasattr(v, "unpack"):
                return v.unpack()
            if hasattr(v, "unwrap"):
                return v.unwrap()
            return v
        except Exception:
            return None

    def _prop(self, proxy: Gio.DBusProxy, name: str):
        try:
            return self._unpack(proxy.get_cached_property(name))
        except Exception:
            return None

    def _live_position(self, bus_name: str) -> int | None:
        # Cached Position goes stale: most players never emit PropertiesChanged
        # for it. A direct Get returns the true current value. Timeout is
        # short on purpose: a slow player must never stall the lyric loop
        # (that stall is what used to make lines appear ~1s late).
        try:
            from gi.repository import GLib as _GLib
            res = self.bus.call_sync(
                bus_name, "/org/mpris/MediaPlayer2",
                "org.freedesktop.DBus.Properties", "Get",
                _GLib.Variant("(ss)", ("org.mpris.MediaPlayer2.Player", "Position")),
                _GLib.VariantType("(v)"),
                Gio.DBusCallFlags.NONE, 350, None)
            return int(res.unpack()[0])
        except Exception:
            return None

    def _read_player(self, bus_name: str) -> None:
        proxy = self._proxies.get(bus_name)
        if proxy is None:
            return
        try:
            status = self._prop(proxy, "PlaybackStatus") or "Stopped"
            raw_meta = self._prop(proxy, "Metadata") or {}
            meta = {}
            if isinstance(raw_meta, dict):
                for k, v in raw_meta.items():
                    u = self._unpack(v)
                    meta[k] = u if u is not None else v
            st = metadata_to_state(bus_name, str(status), meta)
            st.raw_meta = meta  # keep full metadata (lyrics source uses it)
            try:
                pos = self._prop(proxy, "Position")
                st.position_us = int(pos or 0)
                st.position_read_t = time.monotonic()
            except Exception:
                pass
            self.players[bus_name] = st
        except Exception:
            pass

    def snapshot(self) -> list[PlayerState]:
        self._read_positions()
        return list(self.players.values())

    def _read_positions(self) -> None:
        for name, proxy in list(self._proxies.items()):
            st = self.players.get(name)
            if st is None:
                continue
            try:
                if st.status == "Playing":
                    # One D-Bus round-trip per tick (~1.5ms measured).
                    live = self._live_position(name)
                    if live is not None:
                        st.position_us = int(live)
                        st.position_read_t = time.monotonic()
                        continue
                pos = self._prop(proxy, "Position")
                if pos is not None:
                    st.position_us = int(pos or 0)
                    st.position_read_t = time.monotonic()
            except Exception:
                pass


class LyricsApp(Gtk.Application):
    def __init__(self, settings: Settings) -> None:
        super().__init__(application_id="io.github.lyrics-overlay",
                         flags=Gio.ApplicationFlags.FLAGS_NONE)
        self._s = settings
        self._win: LyricsWindow | None = None
        self._tracker: Tracker | None = None
        self._lines: list = []
        self._track_key = ""
        self._loading_key = ""
        self._shown: tuple = ("", -99)   # (track_key, line_idx) last painted
        self._timer_id = 0
        self._playing = False
        self._fetch_gen = 0  # bumps on every track change; stale fetches bow out
        self._add_actions()

    def _add_actions(self) -> None:
        for name, fn in (("refresh", self._act_refresh),
                         ("settings", self._act_settings),
                         ("open-settings", self._act_open_settings),
                         ("quit", self._act_quit)):
            act = Gio.SimpleAction.new(name, None)
            act.connect("activate", fn)
            self.add_action(act)

    def _act_refresh(self, *a) -> None:
        self._track_key = ""
        self._shown = ("", -99)
        self._tick()

    def _act_settings(self, *a) -> None:
        if self._win:
            self._win._show_settings()

    def _act_open_settings(self, *a) -> None:
        if self._win:
            self._win.open_settings_folder()

    def _act_quit(self, *a) -> None:
        self.quit()

    # -- lifecycle --
    def do_activate(self) -> None:
        if self._win is None:
            self._win = LyricsWindow(self, self._s)
            try:
                self._tracker = Tracker()
            except Exception:
                self._tracker = None
            self._win.show_status("Waiting for music \u2026")
            self._arm(max(100, self._s.poll_ms))
        try:
            self._win.present()
        except Exception:
            pass

    def _arm(self, ms: int) -> None:
        try:
            if self._timer_id:
                GLib.source_remove(self._timer_id)
        except Exception:
            pass
        self._timer_id = GLib.timeout_add(ms, self._tick_once)

    def _tick_once(self):
        self._timer_id = 0
        try:
            self._maybe_reload_settings()
            self._tick()
        except Exception:
            log_exc("tick")
        self._ticks = getattr(self, "_ticks", 0) + 1
        if self._ticks % 120 == 0 and self._win is not None:
            try:
                log("state", self._win.dump_state(), "track=", self._track_key[:50])
            except Exception:
                pass
        # adaptive: full rate only while music plays, idle otherwise (less CPU)
        try:
            interval = max(100, self._s.poll_ms) if self._playing else 2000
        except Exception:
            interval = 2000
        self._arm(interval)
        return False

    def _maybe_reload_settings(self) -> None:
        # CLI (--settings) writes the ini; the service adopts it live.
        try:
            import os as _os
            from .settings import USER_INI as _INI
            mt = _os.path.getmtime(_INI)
            if mt != getattr(self, "_ini_mtime", 0):
                self._ini_mtime = mt
                if getattr(self, "_ini_mtime", 0) and self._win is not None:
                    self._s = Settings.load({})
                    self._win.refresh_settings(self._s)
                    log("settings reloaded from ini")
        except Exception:
            pass

    def do_shutdown(self) -> None:
        try:
            if self._tracker:
                self._tracker.close()
        except Exception:
            pass
        Gtk.Application.do_shutdown(self)

    # -- main loop --
    def _tick(self):
        try:
            players = self._tracker.snapshot() if self._tracker else []
        except Exception:
            players = []
        active = pick_active(players)
        self._playing = bool(active and active.status == "Playing")
        if self._win:
            self._win.set_playing(self._playing)
        if active is None or not active.track_key:
            self._lines, self._track_key = [], ""
            self._shown = ("", -99)
            if self._win:
                self._win.show_status("Waiting for music \u2026")
            return True
        if active.track_key != self._track_key and active.track_key != self._loading_key:
            # Track change: reset shown to force re-render
            self._loading_key = active.track_key
            self._fetch_gen += 1
            _gen = self._fetch_gen
            self._shown = ("", -99)  # FIX: reset on track change so line always updates
            log("track-change", active.track_key[:60], "status=", active.status)
            if self._win:
                label = active.track_key.strip()
                self._win.show_status(f"\u266A {label} \u2026" if label else "Searching lyrics\u2026")
            dur_s = (active.duration_us or 0) / 1e6
            fast = None
            if self._s.cache:
                try:
                    fast = read_cache(active.artist, active.title, dur_s)
                except Exception:
                    fast = None
            if fast:
                # Instant paint from cache.
                try:
                    self._lines = parse_lrc(fast)
                except Exception:
                    self._lines = []
                self._track_key = active.track_key
                self._loading_key = ""
                self._shown = ("", -99)
                log("cache-hit", active.track_key[:40], "lines=", len(self._lines))
            th = threading.Thread(target=self._fetch_bg, args=(
                active.artist, active.title, active.url, dur_s,
                self._s.cache, active.track_key, active.bus_name,
                active.album, fast is None, _gen), daemon=True)
            th.start()
        elif self._loading_key == active.track_key:
            pass  # fetch in flight; keep track-name status
        elif self._lines:
            pos = self._position_s(active)
            idx = line_at(self._lines, pos)
            # FIX: handle seek backwards - reset shown if idx regresses significantly
            if idx < self._shown[1] - 1:
                self._shown = ("", -99)  # force re-render on seek-back
            if self._win and (active.track_key, idx) != self._shown:
                log("line", active.track_key[:40], "idx=", idx,
                    (self._lines[idx].text[:50] if 0 <= idx < len(self._lines) else "-"))
                self._shown = (active.track_key, idx)
                if idx >= 0:
                    prev = self._lines[idx - 1].text if idx > 0 else ""
                    cur = self._lines[idx].text
                    nxt = self._lines[idx + 1].text if idx + 1 < len(self._lines) else ""
                    self._win.show_lyrics(prev, cur, nxt)
                else:
                    self._win.show_single("\u266A")  # intro before first line
        return True

    def _position_s(self, active: PlayerState) -> float:
        # Fresh position every tick: snapshot() just read it via D-Bus, so
        # use it directly plus only the tiny elapsed time since that read.
        # The old code interpolated from the PREVIOUS tick's position, which
        # drifted late on every tick and skipped short lines playing catch-up.
        try:
            base_us = max(0, int(active.position_us or 0))
            if active.status == "Playing":
                try:
                    read_t = float(active.position_read_t or 0.0)
                except (TypeError, ValueError):
                    read_t = 0.0
                if read_t > 0:
                    elapsed = max(0.0, time.monotonic() - read_t)
                    # clamp: never extrapolate more than one poll window plus
                    # slack; larger gaps mean the player stalled, not that the
                    # song jumped forward.
                    try:
                        cap = max(0.25, float(self._s.poll_ms) / 1000.0 + 0.15)
                    except Exception:
                        cap = 0.4
                    base_us += int(min(elapsed, cap) * 1e6)
            pos = base_us / 1e6
            try:
                pos += float(getattr(self._s, "sync_offset_ms", 0) or 0) / 1000.0
            except (TypeError, ValueError):
                pass
            return max(0.0, pos)
        except Exception:
            return 0.0

    def _fetch_bg(self, artist: str, title: str, url: str,
                   dur_s: float, use_cache: bool, key: str,
                   bus_name: str = "", album: str = "",
                   display: bool = True, gen: int = 0) -> None:
        # One background thread, sources in priority order, first synced
        # hit wins. No race, no swapping: timestamps always come from the
        # highest-priority source (same-length preference).
        try:
            meta = {}
            if self._tracker:
                st = self._tracker.players.get(bus_name)
                if st is not None:
                    meta = dict(getattr(st, "raw_meta", {}) or {})
            album = album or meta.get("xesam:album", "")
            from .providers import SOURCES as _SS, _run_source as _rs
            pinned = (getattr(self._s, "source", "auto") or "auto").lower()
            if pinned in _SS:
                try:
                    one = _rs(pinned, artist, title, url, dur_s,
                              album, meta)
                except Exception:
                    one = None
                text, where = (one, pinned) if one else (None, None)
                log("sources", key[:40], "pinned=", pinned,
                    "hit=", bool(one))
            else:
                text, where = fetch_first_synced(
                    artist, title, url, dur_s, album, meta)
            log("sources", key[:40], "win=", where,
                "lines=", len(text.splitlines()) if text else 0)
            GLib.idle_add(self._fetch_done, key, text, where, display,
                          artist, title, dur_s, use_cache, gen)
        except Exception:
            log_exc("fetch")
            GLib.idle_add(self._fetch_done, key, None, None, display,
                          artist, title, dur_s, use_cache, gen)

    def _fetch_first(self, key: str, text: str, src: str,
                     gen: int = 0) -> bool:
        # Paint now so the line starts moving instead of freezing until
        # the (single, sequential) lookup finishes.
        try:
            if gen != self._fetch_gen:
                return False  # a newer song already took over; bow out
            if key != self._loading_key:
                return False
            if self._track_key == key and self._lines:
                return False
            self._lines = parse_lrc(text)
            self._track_key = key
            self._shown = ("", -99)
            log("first-paint", key[:40], "src=", src,
                "lines=", len(self._lines))
        except Exception:
            log_exc("first-paint")
        return False

    def _fetch_done(self, key: str, text: str | None,
                    source: str | None = None, display: bool = True,
                    artist: str = "", title: str = "", dur_s: float = 0,
                    use_cache: bool = True, gen: int = 0) -> bool:
        if gen != self._fetch_gen:
            return False  # stale song's fetch; must not touch the new one
        if self._loading_key == key:
            self._loading_key = ""
        if not display:
            return False
        if use_cache and text:
            try:
                write_cache(artist, title, text, dur_s)
            except Exception:
                pass
        if self._track_key == key and self._lines:
            return False  # already painting this track; keep timestamps
        self._shown = ("", -99)
        self._lines = parse_lrc(text) if text else []
        log("fetch-done", key[:50], "src=", source,
            "lines=", len(self._lines))
        self._track_key = key  # avoid refetch loop; show status until change
        if not self._lines and self._win:
            self._win.show_status("No lyrics found")
        return False


def build_settings(argv: argparse.Namespace) -> Settings:
    return Settings.load({
        "font_family": argv.font_family,
        "font_size": argv.font_size,
        "text_color": argv.text_color,
        "bg_color": argv.bg_color,
        "width": argv.width,
        "lines": argv.lines,
        "anchor": argv.anchor,
        "margin": argv.margin,
    })

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="lyrics-overlay",
                                 description="Lightweight DE-agnostic synced-lyrics overlay (any MPRIS player).")
    ap.add_argument("--version", action="version", version="%(prog)s 0.2.4")
    ap.add_argument("--font-family", dest="font_family", default=None)
    ap.add_argument("--font-size", dest="font_size", type=int, default=None)
    ap.add_argument("--text-color", dest="text_color", default=None,
                    help="Hex, e.g. #ffffff")
    ap.add_argument("--bg-color", dest="bg_color", default=None,
                    help="RRGGBBAA, e.g. #00000080 (transparent)")
    ap.add_argument("--width", type=int, default=None)
    ap.add_argument("--lines", type=int, choices=[1, 3], default=None,
                    help="1 = single YT-caption line (default), 3 = prev/current/next")
    ap.add_argument("--anchor", choices=["top", "bottom"], default=None)
    ap.add_argument("--margin", type=int, default=None)
    ap.add_argument("--no-cache", action="store_true")
    ap.add_argument("--write-defaults", action="store_true",
                    help="Write default ~/.config/lyrics-overlay/settings.ini and exit")
    ap.add_argument("--settings", action="store_true",
                    help="Open the settings dialog (service adopts on save)")
    args = ap.parse_args(argv)
    settings = build_settings(args)
    if args.no_cache:
        settings.cache = False
    if args.write_defaults:
        Settings().save_user()
        print("wrote ~/.config/lyrics-overlay/settings.ini")
        return 0
    if args.settings:
        from .overlay import LyricsWindow
        w = LyricsWindow(None, settings)
        w._show_settings()
        loop = GLib.MainLoop()
        GLib.timeout_add_seconds(300, lambda: (loop.quit(), False)[1])
        loop.run()
        return 0
    app = LyricsApp(settings)
    return app.run([])

if __name__ == "__main__":
    raise SystemExit(main())
