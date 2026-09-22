APP := lyrics-overlay
PKG := lyrics_overlay
VER := 0.2.4
SRC := src/lyrics_overlay
DEBROOT := dist/debroot
DEB := dist/$(APP)_$(VER)_amd64.deb

.PHONY: all run test lint deb install clean

all: test

run:
	PYTHONPATH=src python3 -m lyrics_overlay

test:
	PYTHONPATH=src python3 -m pytest tests/ -q 2>/dev/null || PYTHONPATH=src python3 tests/run_tests.py

lint:
	python3 -m py_compile $(SRC)/*.py && echo "compile OK"

deb: lint test
	rm -rf $(DEBROOT) dist/*.deb
	mkdir -p $(DEBROOT)/DEBIAN $(DEBROOT)/usr/lib/$(APP)/$(PKG) $(DEBROOT)/usr/bin $(DEBROOT)/usr/share/applications $(DEBROOT)/usr/share/doc/$(APP) $(DEBROOT)/usr/lib/systemd/user $(DEBROOT)/etc/$(APP)
	cp $(SRC)/*.py $(DEBROOT)/usr/lib/$(APP)/$(PKG)/
	printf '#!/bin/sh\nPYTHONPATH=/usr/lib/$(APP)$${PYTHONPATH:+:$$PYTHONPATH}\nexport PYTHONPATH\nexec /usr/bin/python3 -m $(PKG) "$$@"\n' > $(DEBROOT)/usr/bin/$(APP)
	chmod 755 $(DEBROOT)/usr/bin/$(APP)
	cp packaging/$(APP).desktop $(DEBROOT)/usr/share/applications/
	cp packaging/$(APP).service $(DEBROOT)/usr/lib/systemd/user/
	cp packaging/settings.ini.example $(DEBROOT)/etc/$(APP)/settings.ini
	cp README.md $(DEBROOT)/usr/share/doc/$(APP)/
	sed 's/@VER@/$(VER)/' packaging/control > $(DEBROOT)/DEBIAN/control
	dpkg-deb --build $(DEBROOT) $(DEB)
	@echo "built $(DEB)"

install: deb
	sudo dpkg -i $(DEB) || dpkg -i $(DEB)

clean:
	rm -rf dist build __pycache__ $(SRC)/__pycache__ tests/__pycache__
