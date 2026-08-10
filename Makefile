# vctools — virtual control tools: run a GUI under test in an isolated nested
# compositor, then drive it with synthetic input and capture it, without ever
# touching the live session.
#
# Everything generated or compiled lands in build/; nothing is written into src/
# and nothing generated is checked in, so a protocol bump is just `make clean`.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -std=gnu11 -Ibuild/gen -Isrc $(shell pkg-config --cflags wayland-client xkbcommon zlib)
LDLIBS  := $(shell pkg-config --libs wayland-client xkbcommon zlib)

WLPROTO := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
SCANNER := $(shell pkg-config --variable=wayland_scanner wayland-scanner)

PROTOCOLS := \
	ext-foreign-toplevel-list-v1 \
	ext-image-capture-source-v1 \
	ext-image-copy-capture-v1 \
	xdg-output-unstable-v1 \
	wlr-virtual-pointer-unstable-v1 \
	virtual-keyboard-unstable-v1

# Protocol XMLs live in three different layouts (staging/, unstable/, ours), so
# just look the file up by name instead of hardcoding each path.
xml = $(shell find $(WLPROTO) protocols -name "$(1).xml" 2>/dev/null | head -1)

GEN       := build/gen
PROTO_SRC := $(PROTOCOLS:%=$(GEN)/%-protocol.c)
PROTO_HDR := $(PROTOCOLS:%=$(GEN)/%-client-protocol.h)
PROTO_OBJ := $(PROTOCOLS:%=build/obj/%-protocol.o)

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/obj/%.o)

.PHONY: all clean install test
all: vc

vc: $(OBJ) $(PROTO_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/obj/%.o: src/%.c $(PROTO_HDR)
	@mkdir -p build/obj
	$(CC) $(CFLAGS) -c -o $@ $<

build/obj/%-protocol.o: $(GEN)/%-protocol.c
	@mkdir -p build/obj
	$(CC) $(CFLAGS) -c -o $@ $<

$(GEN)/%-client-protocol.h:
	@mkdir -p $(GEN)
	$(SCANNER) client-header $(call xml,$*) $@

$(GEN)/%-protocol.c:
	@mkdir -p $(GEN)
	$(SCANNER) private-code $(call xml,$*) $@

.PRECIOUS: $(GEN)/%-protocol.c $(GEN)/%-client-protocol.h

install: vc
	install -Dm755 vc $(DESTDIR)$(HOME)/.local/bin/vc

test: vc
	@./tests/run.sh

clean:
	rm -rf build vc
