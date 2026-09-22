# lyrics-overlay

Synced lyrics overlay for any MPRIS music player (limusic, Spotify, VLC, mpv, browsers).

Transparent always-on-top caption window. Left-drag to move, right-click for menu.

## Install

```bash
make -C c
sudo make -C c install
```

Needs GTK4, libsoup3, json-glib and sqlite3 headers to build.

## Run

```bash
lyrics-overlay
```

## How it works

- Reads position and metadata from the playing MPRIS player.
- Lyrics sources, first synced hit wins: disk cache, local `.lrc` files,
  limusic's own matched lyrics, player metadata, Boidu, LRCLIB.
- Each lyric line gets its own timer at its exact timestamp; position is
  interpolated locally and re-synced every few seconds. No polling loop,
  no redraws unless the line changes.

## Settings

Right-click the window, Settings. Stored in `~/.config/lyrics-overlay/settings.ini`.
