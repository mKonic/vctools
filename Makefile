# vctools — virtual control tools: run a GUI under test in an isolated nested
# compositor, then drive it with synthetic input and capture it, without ever
# touching the live session.
#
# Protocol glue is generated at build time from wayland-protocols' XML plus the
# two wlroots protocols vendored in protocols/, so nothing generated is checked
# in and a protocol bump is just a `make clean`.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -std=gnu11 -Igen -Isrc $(shell pkg-config --cflags wayland-client xkbcommon zlib)
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

PROTO_SRC := $(PROTOCOLS:%=gen/%-protocol.c)
PROTO_HDR := $(PROTOCOLS:%=gen/%-client-protocol.h)
PROTO_OBJ := $(PROTO_SRC:.c=.o)

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

.PHONY: all clean install test
all: vc

vc: $(PROTO_HDR) $(OBJ) $(PROTO_OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(PROTO_OBJ) $(LDLIBS)

# Every object depends on the generated headers existing first.
$(OBJ): $(PROTO_HDR)

gen/%-client-protocol.h:
	@mkdir -p gen
	$(SCANNER) client-header $(call xml,$*) $@

gen/%-protocol.c:
	@mkdir -p gen
	$(SCANNER) private-code $(call xml,$*) $@

install: vc
	install -Dm755 vc $(DESTDIR)$(HOME)/.local/bin/vc

test: vc
	@./tests/run.sh

clean:
	rm -rf gen vc $(OBJ)
