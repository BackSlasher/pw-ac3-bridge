# SPDX-License-Identifier: LGPL-2.1-or-later
PREFIX ?= /usr/local
CC     ?= cc
PKG_CONFIG ?= pkg-config

WARN    = -Wall -Wextra -Wno-unused-parameter
CFLAGS ?= -O2 -g
CFLAGS += $(WARN)

PW_CFLAGS  := $(shell $(PKG_CONFIG) --cflags libpipewire-0.3)
PW_LIBS    := $(shell $(PKG_CONFIG) --libs libpipewire-0.3)
AV_CFLAGS  := $(shell $(PKG_CONFIG) --cflags libavcodec libavutil)
AV_LIBS    := $(shell $(PKG_CONFIG) --libs libavcodec libavutil)

all: pw-ac3-bridge tests/core-test tests/iec958-sink

pw-ac3-bridge: src/main.c src/ac3pack.c src/ac3pack.h
	$(CC) $(CFLAGS) $(PW_CFLAGS) $(AV_CFLAGS) -o $@ src/main.c src/ac3pack.c \
		$(PW_LIBS) $(AV_LIBS) -lm

tests/core-test: tests/core-test.c src/ac3pack.c src/ac3pack.h
	$(CC) $(CFLAGS) $(AV_CFLAGS) -o $@ tests/core-test.c src/ac3pack.c \
		$(AV_LIBS) -lm

tests/iec958-sink: tests/iec958-sink.c
	$(CC) $(CFLAGS) $(PW_CFLAGS) -o $@ tests/iec958-sink.c $(PW_LIBS)

install: pw-ac3-bridge
	install -Dm755 pw-ac3-bridge $(DESTDIR)$(PREFIX)/bin/pw-ac3-bridge

clean:
	rm -f pw-ac3-bridge tests/core-test tests/iec958-sink

.PHONY: all install clean
