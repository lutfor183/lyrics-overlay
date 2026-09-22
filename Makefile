.PHONY: all build test install clean
all: build
build:
	$(MAKE) -C c
test:
	$(MAKE) -C c test
install:
	$(MAKE) -C c install
clean:
	$(MAKE) -C c clean
