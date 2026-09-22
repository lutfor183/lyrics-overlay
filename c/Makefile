R := /tmp/cdeps/root
CC := gcc
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations
INC := -I$(R)/usr/include -I$(R)/usr/include/gtk-4.0 -I$(R)/usr/include/pango-1.0 \
 -I$(R)/usr/include/harfbuzz -I$(R)/usr/include/fribidi -I$(R)/usr/include/gdk-pixbuf-2.0 \
 -I/usr/include/cairo -I$(R)/usr/include/graphene-1.0 -I$(R)/usr/lib/x86_64-linux-gnu/graphene-1.0/include \
 -I$(R)/usr/include/libsoup-3.0 -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include \
 -I/usr/include/json-glib-1.0
LIBS := -l:libgtk-4.so.1 -l:libsoup-3.0.so.0 -l:libsqlite3.so.0 \
 $(shell pkg-config --libs glib-2.0 gobject-2.0 gio-2.0 json-glib-1.0 2>/dev/null) -lm \
 -l:libcairo.so.2 -l:libpango-1.0.so.0 -l:libpangocairo-1.0.so.0 -l:libgdk_pixbuf-2.0.so.0 -l:libepoxy.so.0 -l:libharfbuzz.so.0
SRC := log.c lrc.c settings.c lyrics.c mpris.c overlay.c main.c
OBJ := $(SRC:.c=.o)
BIN := lyrics-overlay-c

all: $(BIN) test-lo
%.o: %.c common.h
	$(CC) $(WARN) -g -O2 $(INC) -c $< -o $@
$(BIN): $(OBJ)
	$(CC) $(WARN) -g -O2 -o $@ $(OBJ) $(LIBS)
test-lo: test.c log.c lrc.c settings.c lyrics.c
	$(CC) $(WARN) -g -O2 $(INC) -o $@ test.c log.c lrc.c settings.c lyrics.c $(LIBS)
test: test-lo
	./test-lo
clean:
	rm -f $(OBJ) $(BIN) test-lo
PREFIX := $(HOME)/.local
install: $(BIN)
	install -m 755 $(BIN) $(PREFIX)/bin/lyrics-overlay
asan: $(SRC) common.h
	$(CC) $(WARN) -g -O1 -fsanitize=address -fno-omit-frame-pointer $(INC) -o /tmp/lo-asan $(SRC) $(LIBS)
