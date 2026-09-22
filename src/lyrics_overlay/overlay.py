"""Transparent caption overlay: ONE line of lyrics, nothing else.

The window is ALWAYS clickable (never click-through):

Interactions (no visible chrome):
  left-drag anywhere .... move window (compositor-assisted, Wayland-safe)
  right-click .......... menu (lyric source, refresh, settings, quit)
"""
from __future__ import annotations
import html
import os

import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, GLib, Gtk, Pango  # noqa: E402

from .dlog import log, log_exc  # noqa: E402

CSS_TEMPLATE = """
.lyrics-win {{
    background-color: transparent;
    border: none;
    box-shadow: none;
}}
.lyrics-root {{
    background-color: {bg};
    border-radius: 10px;
}}
.lyrics-current {{
    font-family: {font};
    font-size: {size}px;
    font-weight: bold;
    color: {fg};
    {shadow}
}}
.lyrics-side {{
    font-family: {font};
    font-size: {side}px;
    color: {fg};
    opacity: 0.65;
    {shadow}
}}
"""

def _hex_with_alpha(rrggbbaa: str) -> str:
    s = (rrggbbaa or "").strip().lstrip("#")
    if len(s) == 6:
        s += "FF"
    if len(s) != 8:
        return "#00000080"
    try:
        r, g, b, a = (int(s[i:i + 2], 16) for i in (0, 2, 4, 6))
    except ValueError:
        return "#00000080"
    if a >= 255:
        return f"#{s[:6]}"
    return f"rgba({r},{g},{b},{a / 255:.2f})"


class LyricsWindow(Gtk.ApplicationWindow):
    def __init__(self, app: Gtk.Application, settings) -> None:
        super().__init__(application=app, title="Lyrics Overlay")
        self.add_css_class("lyrics-win")
        self._s = settings
        self._single_line = getattr(settings, "lines", 1) == 1
        self.set_decorated(False)
        self.set_resizable(True)
        try:
            self.set_keep_above(bool(getattr(settings, "keep_above", True)))
        except Exception:
            pass
        try:
            self.set_skip_taskbar_hint(True)
        except Exception:
            pass


        self._css = Gtk.CssProvider()
        self._apply_css()

        # ONE box, minimal padding, nothing else visible
        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        root.add_css_class("lyrics-root")
        root.set_margin_top(8)
        root.set_margin_bottom(8)
        root.set_margin_start(14)
        root.set_margin_end(14)
        self._root = root
        self.set_child(root)

        self.prev_label = self._mklabel("lyrics-side")
        self.cur_label = self._mklabel("lyrics-current", single_row=True)
        self.next_label = self._mklabel("lyrics-side")
        self.cur_label.set_text("\u266A")
        self._apply_wrap_width()
        root.append(self.prev_label)
        root.append(self.cur_label)
        root.append(self.next_label)
        self._apply_line_mode()

        # left-drag anywhere -> move
        drag = Gtk.GestureDrag.new()
        drag.set_button(1)
        drag.connect("drag-begin", self._on_drag_begin)
        self.add_controller(drag)



        # right-click -> menu
        rclick = Gtk.GestureClick.new()
        rclick.set_button(3)
        rclick.connect("pressed", self._on_right_click)
        self.add_controller(rclick)

        self.connect("map", lambda *_: self._ensure_clickable())
        self._menu = None
        self._save_id = 0
        self._autohide_id = 0
        self._playing = False
        log("window created single_line=", self._single_line)

    def _apply_wrap_width(self) -> None:
        # Dynamic hug: label wraps at ~settings.width px, window shrinks to text.
        try:
            px_per_char = max(8.0, float(self._s.font_size) * 0.55)
            chars = max(20, min(120, int(int(self._s.width) / px_per_char)))
            for lb in (self.prev_label, self.cur_label, self.next_label):
                lb.set_max_width_chars(-1)
        except Exception as e:
            log_exc(f"wrap {e}")

    def _mklabel(self, cls: str, single_row: bool = False) -> Gtk.Label:
        lb = Gtk.Label(label="")
        lb.add_css_class(cls)
        lb.set_justify(Gtk.Justification.CENTER)
        if single_row:
            lb.set_wrap(False)
            lb.set_single_line_mode(True)
            lb.set_ellipsize(Pango.EllipsizeMode.END)
        else:
            lb.set_wrap(True)
        lb.set_halign(Gtk.Align.CENTER)
        lb.set_valign(Gtk.Align.CENTER)
        return lb

    # -- appearance --
    def _apply_css(self) -> None:
        s = self._s
        side = max(10, int(s.font_size * s.secondary_scale))
        shadow = ("text-shadow: 0 1px 3px rgba(0,0,0,0.9), 0 0 12px rgba(0,0,0,0.7);"
                  if s.text_shadow else "text-shadow: none;")
        try:
            bg = s.resolved_bg() if hasattr(s, "resolved_bg") else _hex_with_alpha(s.bg_color)
        except Exception:
            bg = "rgba(0,0,0,0.0)"
        css = CSS_TEMPLATE.format(bg=_hex_with_alpha(bg), font=s.font_family,
                                  size=s.font_size, side=side, fg=s.text_color,
                                  shadow=shadow)
        try:
            self._css.load_from_string(css)
            Gtk.StyleContext.add_provider_for_display(
                Gdk.Display.get_default(), self._css,
                Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        except Exception as e:
            log_exc(f"css {e}")

    def refresh_settings(self, settings) -> None:
        self._s = settings
        self._single_line = getattr(settings, "lines", 1) == 1
        try:
            self.set_keep_above(bool(getattr(settings, "keep_above", True)))
        except Exception:
            pass
        self._apply_css()
        self._ensure_clickable()
        self._apply_line_mode()
        try:
            self._apply_wrap_width()
            self.queue_resize()
        except Exception as e:
            log_exc(f"refresh {e}")
        self._apply_autohide_visibility()
        log("settings applied lines=", settings.lines, "size=", settings.font_size)

    # -- content: single caption line only --
    @staticmethod
    def _esc(t: str) -> str:
        return html.escape(t or "", quote=False)

    def _ensure_clickable(self) -> None:
        # The window is ALWAYS clickable + draggable. (An old
        # click-through option used to empty the input region, which made
        # the caption impossible to move and hid the right-click menu, so
        # it was removed.) Here we (re)assert a full input region.
        try:
            surf = self.get_surface()
            if surf is None:
                return
            try:
                import cairo
                w, h = max(1, self.get_width()), max(1, self.get_height())
                surf.set_input_region(cairo.Region(cairo.RectangleInt(0, 0, w, h)))
            except Exception:
                pass
        except Exception as e:
            log_exc(f"clickable {e}")

    def _apply_click_through(self) -> None:  # back-compat alias
        self._ensure_clickable()

    def _apply_line_mode(self) -> None:
        single = self._single_line
        try:
            self.prev_label.set_visible(not single)
            self.next_label.set_visible(not single)
            if single:
                self.prev_label.set_text("")
                self.next_label.set_text("")
            try:
                _m = 4 if single else 8
                self._root.set_margin_top(_m)
                self._root.set_margin_bottom(_m)
            except Exception:
                pass
        except Exception as e:
            log_exc(f"linemode {e}")

    def _refit(self) -> None:
        # Dynamic width: measure the line and set it as the window size.
        # (Mutter ignores shrink-via-size-request; an explicit default size
        # is honored.) Capped at settings.width; longer lines ellipsize.
        try:
            _minw, natw, _minb, _natb = self.cur_label.measure(
                Gtk.Orientation.HORIZONTAL, -1)
            _minh, nath, _minhb, _nathb = self._root.measure(
                Gtk.Orientation.VERTICAL, natw)
            cap = max(200, int(self._s.width))
            # +36: root side margins (14+14) + slack, else the text
            # gets 28px less than measured and ellipsizes wrongly.
            w = min(natw + 36, cap)
            self.set_default_size(w, nath)
            log("refit", natw, "->", w)
        except Exception as e:
            log_exc(f"refit {e}")

    def show_lyrics(self, prev: str, cur: str, nxt: str) -> None:
        try:
            if self._single_line:
                self.show_single(cur)
                return
            self._apply_line_mode()
            self.prev_label.set_markup(f"<span>{self._esc(prev)}</span>" if prev else "")
            self.cur_label.set_markup(f"<span>{self._esc(cur) or '&#9834;'}</span>")
            self.next_label.set_markup(f"<span>{self._esc(nxt)}</span>" if nxt else "")
            self._refit()
            self._present()
        except Exception as e:
            log_exc(f"show_lyrics {e}")

    def show_single(self, cur: str) -> None:
        try:
            self._apply_line_mode()
            self.cur_label.set_markup(f"<span>{self._esc(cur) or '&#9834;'}</span>")
            self._refit()
            self._present()
        except Exception as e:
            log_exc(f"show_single {e}")

    def show_status(self, text: str) -> None:
        try:
            self._apply_line_mode()
            self.cur_label.set_text(text)
            self._present()
        except Exception as e:
            log_exc(f"show_status {e}")

    def _present(self) -> None:
        try:
            if not self.get_visible():
                self.present()
        except Exception as e:
            log_exc(f"present {e}")

    def dump_state(self) -> str:
        try:
            return (f"visible={self.get_visible()} mapped={self.get_mapped()} "
                    f"size={self.get_width()}x{self.get_height()} "
                    f"single={self._single_line} cur={self.cur_label.get_text()[:40]!r}")
        except Exception as e:
            return f"dump-exc {e}"

    # -- autohide --
    def _schedule_autohide(self) -> None:
        try:
            if not self._s.autohide:
                return
        except Exception:
            return
        self._cancel_autohide()
        try:
            self._autohide_id = GLib.timeout_add_seconds(
                max(1, int(self._s.autohide_timeout)), self._do_autohide)
        except Exception as e:
            log_exc(f"autohide-arm {e}")

    def _cancel_autohide(self) -> None:
        if self._autohide_id:
            try:
                GLib.source_remove(self._autohide_id)
            except Exception:
                pass
            self._autohide_id = 0

    def _do_autohide(self) -> bool:
        try:
            if getattr(self._s, "autohide", True) and not self._playing:
                self.hide()
                log("autohide: hidden")
        except Exception as e:
            log_exc(f"autohide {e}")
        return False

    def _apply_autohide_visibility(self) -> None:
        try:
            if not getattr(self._s, "autohide", True) and not self.get_visible():
                self.present()
        except Exception as e:
            log_exc(f"autohide-vis {e}")

    def set_playing(self, playing: bool) -> None:
        self._playing = playing
        if playing:
            self._cancel_autohide()
            if not self.get_visible():
                try:
                    self.present()
                except Exception as e:
                    log_exc(f"setplaying {e}")
        else:
            self._schedule_autohide()

    # -- move: left-drag anywhere --
    # NOTE: on Wayland the surface IS the toplevel (no get_toplevel()).
    def _on_drag_begin(self, gesture: Gtk.GestureDrag, sx: float, sy: float) -> None:
        try:
            tl = self.get_surface()
            tl.begin_move(gesture.get_device(), 1, sx, sy, 0)
            log("drag: move started")
        except Exception as e:
            log_exc(f"drag {e}")

    # -- scroll: font size --
    def _debounced_save(self) -> None:
        try:
            if self._save_id:
                GLib.source_remove(self._save_id)
        except Exception:
            pass
        try:
            self._save_id = GLib.timeout_add(1500, self._do_save)
        except Exception:
            pass

    def _do_save(self) -> bool:
        self._save_id = 0
        try:
            self._s.save_user()
        except Exception as e:
            log_exc(f"save {e}")
        return False

    # -- right-click menu: plain buttons (no action-group dependency) --
    def _on_right_click(self, gesture: Gtk.GestureClick, n: int, x: float, y: float) -> None:
        try:
            log("menu: right-click")
            try:
                if self._menu is not None:
                    self._menu.popdown()
            except Exception:
                pass
            self._menu = self._build_menu()
            self._menu.set_parent(self)
            try:
                self._menu.set_pointing_to(Gdk.Rectangle(int(x), int(y), 1, 1))
            except Exception:
                pass
            self._menu.popup()
            log("menu: popped")
        except Exception as e:
            log_exc(f"menu {e}")

    def _menu_btn(self, box: Gtk.Box, label: str, fn) -> None:
        b = Gtk.Button(label=label)
        b.set_has_frame(False)
        b.set_halign(Gtk.Align.FILL)
        b.connect("clicked", lambda *_: (self._menu.popdown(), fn()))
        box.append(b)

    def _build_menu(self) -> Gtk.Popover:
        pop = Gtk.Popover()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        box.set_margin_top(6); box.set_margin_bottom(6)
        box.set_margin_start(6); box.set_margin_end(6)
        app = self.get_application()
        self._menu_btn(box, "Refresh lyrics", lambda: app._act_refresh())
        self._menu_btn(box, "Settings...", lambda: self._show_settings())
        self._menu_btn(box, "Quit", lambda: app._act_quit())
        pop.set_child(box)
        return pop

    # -- settings dialog (kept) --
    def _on_settings_clicked(self, *a) -> None:
        self._show_settings()

    def _show_settings(self) -> None:
        from .dlog import log as _log
        _log("settings dialog opened")
        s = self._s
        dialog = Gtk.Dialog(title="Lyrics Overlay Settings", transient_for=self,
                            modal=True)
        try:
            dialog.add_button("Cancel", Gtk.ResponseType.CANCEL)
            dialog.add_button("Apply", Gtk.ResponseType.APPLY)
        except Exception as e:
            log_exc(f"dlg-btn {e}")
        dialog.set_default_size(400, 500)
        try:
            content = dialog.get_content_area()
        except Exception as e:
            log_exc(f"dlg-content {e}")
            return
        scrolled = Gtk.ScrolledWindow()
        scrolled.set_vexpand(True)
        listbox = Gtk.ListBox()
        listbox.set_selection_mode(Gtk.SelectionMode.NONE)

        def add_row(label_text, widget, tooltip=""):
            row = Gtk.ListBoxRow()
            hbox = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
            hbox.set_margin_top(6); hbox.set_margin_bottom(6)
            hbox.set_margin_start(12); hbox.set_margin_end(12)
            lbl = Gtk.Label(label=label_text, halign=Gtk.Align.START,
                           valign=Gtk.Align.CENTER)
            lbl.set_size_request(150, -1)
            widget.set_halign(Gtk.Align.END)
            widget.set_valign(Gtk.Align.CENTER)
            hbox.append(lbl)
            hbox.append(widget)
            if tooltip:
                lbl.set_tooltip_text(tooltip)
            row.set_child(hbox)
            listbox.append(row)
            return row, widget

        lines_combo = Gtk.ComboBoxText()
        lines_combo.append("1", "Single line (YT-caption style)")
        lines_combo.append("3", "3-line (prev / current / next)")
        try:
            lines_combo.set_active_id(str(s.lines))
        except Exception:
            pass
        add_row("Lines", lines_combo)

        src_combo = Gtk.ComboBoxText()
        src_combo.append("auto", "Auto (best match)")
        for _sname, _sdesc in (("local", "My .lrc files"),
                               ("limusic", "limusic match"),
                               ("mpris", "From player"),
                               ("boidu", "Boidu"),
                               ("lrclib", "LRCLIB")):
            src_combo.append(_sname, _sdesc)
        try:
            src_combo.set_active_id(getattr(s, "source", "auto") or "auto")
        except Exception:
            pass
        add_row("Lyrics source", src_combo)

        font_entry = Gtk.Entry()
        font_entry.set_text(s.font_family)
        add_row("Font family", font_entry)

        font_size_spin = Gtk.SpinButton.new_with_range(10, 72, 1)
        font_size_spin.set_value(s.font_size)
        add_row("Font size", font_size_spin)

        scale_spin = Gtk.SpinButton.new_with_range(0.3, 2.0, 0.05)
        scale_spin.set_value(s.secondary_scale)
        add_row("Secondary scale", scale_spin)

        color_btn = Gtk.ColorButton()
        try:
            _rgba = Gdk.RGBA()
            if _rgba.parse(str(s.text_color)):
                color_btn.set_rgba(_rgba)
        except Exception as e:
            log_exc(f"color-init {e}")
        add_row("Text color", color_btn)

        bg_combo = Gtk.ComboBoxText()
        for mode, desc in (("transparent", "Fully transparent"),
                           ("minimal_dark", "Minimal dark shade"),
                           ("light", "Light shade"),
                           ("custom", "Custom color")):
            bg_combo.append(mode, desc)
        try:
            bg_combo.set_active_id(s.bg_mode)
        except Exception:
            pass
        add_row("Background", bg_combo)

        width_spin = Gtk.SpinButton.new_with_range(200, 1200, 10)
        width_spin.set_value(s.width)
        add_row("Window width", width_spin)

        anchor_combo = Gtk.ComboBoxText()
        anchor_combo.append("bottom", "Bottom")
        anchor_combo.append("top", "Top")
        try:
            anchor_combo.set_active_id(s.anchor)
        except Exception:
            pass
        add_row("Anchor", anchor_combo)

        margin_spin = Gtk.SpinButton.new_with_range(0, 200, 1)
        margin_spin.set_value(s.margin)
        add_row("Margin", margin_spin)

        poll_spin = Gtk.SpinButton.new_with_range(100, 5000, 50)
        poll_spin.set_value(s.poll_ms)
        add_row("Poll interval (ms)", poll_spin)

        autohide_switch = Gtk.Switch()
        autohide_switch.set_active(bool(s.autohide))
        add_row("Autohide", autohide_switch, "Hide when nothing plays")

        above_switch = Gtk.Switch()
        above_switch.set_active(bool(getattr(s, "keep_above", True)))
        add_row("Always on top", above_switch)

        timeout_spin = Gtk.SpinButton.new_with_range(1, 30, 1)
        timeout_spin.set_value(s.autohide_timeout)
        add_row("Autohide timeout (s)", timeout_spin)

        sync_spin = Gtk.SpinButton.new_with_range(-2000, 2000, 50)
        sync_spin.set_value(getattr(s, "sync_offset_ms", 350))
        add_row("Show early (ms)", sync_spin,
                "Line appears this many ms early (+350 default). "
                "Raise if lyrics feel late, lower if early.")

        cache_switch = Gtk.Switch()
        cache_switch.set_active(bool(s.cache))
        add_row("Cache lyrics", cache_switch)

        scrolled.set_child(listbox)
        content.append(scrolled)
        dialog.show()

        def on_response(d, r):
            try:
                if r == Gtk.ResponseType.APPLY:
                    self._apply_dialog(s, lines_combo, font_entry, font_size_spin,
                                       scale_spin, color_btn, bg_combo, width_spin,
                                       anchor_combo, margin_spin, poll_spin,
                                       autohide_switch, timeout_spin,
                                       cache_switch, sync_spin,
                                       above_switch,
                                       src_combo)
            except Exception as e:
                log_exc(f"dlg-apply {e}")
            finally:
                try:
                    d.destroy()
                except Exception:
                    pass
        dialog.connect("response", on_response)

    def _apply_dialog(self, s, lines_combo, font_entry, font_size_spin,
                      scale_spin, color_btn, bg_combo, width_spin,
                      anchor_combo, margin_spin, poll_spin,
                      autohide_switch, timeout_spin,
                      cache_switch, sync_spin, above_switch, src_combo) -> None:
        s.lines = int(lines_combo.get_active_id() or "1")
        s.font_family = font_entry.get_text() or "Sans"
        s.font_size = int(font_size_spin.get_value())
        s.secondary_scale = float(scale_spin.get_value())
        try:
            s.text_color = color_btn.get_rgba().to_string()
        except Exception:
            pass
        s.bg_mode = bg_combo.get_active_id() or "transparent"
        s.width = int(width_spin.get_value())
        s.anchor = anchor_combo.get_active_id() or "bottom"
        s.margin = int(margin_spin.get_value())
        s.poll_ms = int(poll_spin.get_value())
        s.autohide = autohide_switch.get_active()
        s.autohide_timeout = int(timeout_spin.get_value())
        s.cache = cache_switch.get_active()
        s.keep_above = above_switch.get_active()
        try:
            s.source = src_combo.get_active_id() or "auto"
        except Exception:
            pass
        try:
            s.sync_offset_ms = max(-2000, min(2000, int(sync_spin.get_value())))
        except Exception:
            pass
        s.text_shadow = True  # drop shadow always on (no toggle)
        s.save_user()
        self.refresh_settings(s)

    def open_settings_folder(self) -> None:
        d = os.path.expanduser("~/.config/lyrics-overlay")
        try:
            os.makedirs(d, exist_ok=True)
            Gtk.show_uri(self, f"file://{d}", 0)
        except Exception as e:
            log_exc(f"open-folder {e}")

    def close(self) -> None:
        self._cancel_autohide()
        super().close()
