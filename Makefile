# cerberus — on-camera agent. mbedTLS is the only dependency; everything else is libc.
#
# Native build:      make
# Static binary:     make STATIC=1
# Cross (OpenIPC):   make CC=arm-openipc-linux-musleabi-gcc MBEDTLS=/path/to/staging STATIC=1
# Release version:   make VERSION=1.2.0      (otherwise the version in main.c)
# Tests:             make test               (needs Docker; see tests/run.sh)
#
# MBEDTLS points at a prefix holding include/mbedtls and lib/libmbed*. Leave it empty when the
# toolchain already sees them (a distro's mbedtls-dev, or a Buildroot sysroot).

CC      ?= cc
STRIP   ?= strip
DESTDIR ?=
PREFIX  ?= /usr
MBEDTLS ?=
VERSION ?=

CFLAGS  ?= -Os -std=c99 -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS ?= -Wl,--gc-sections
LIBS     = -lmbedtls -lmbedx509 -lmbedcrypto

# CI adds -Werror here without having to restate the defaults above.
CFLAGS  += $(EXTRA_CFLAGS)

ifneq ($(VERSION),)
CFLAGS  += -DCERBERUS_VERSION=\"$(VERSION)\"
endif

ifneq ($(MBEDTLS),)
CFLAGS  += -I$(MBEDTLS)/include
LDFLAGS += -L$(MBEDTLS)/lib
endif

ifeq ($(STATIC),1)
LDFLAGS += -static
endif

BIN = cerberus

all: $(BIN)

$(BIN): main.c
	$(CC) $(CFLAGS) -o $@ main.c $(LDFLAGS) $(LIBS)
	$(STRIP) $@
	@echo "==> $@: $$(wc -c <$@) bytes"

size: $(BIN)
	@size $(BIN) || true
	@ls -l $(BIN)

install: $(BIN)
	install -D -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/sbin/$(BIN)
	install -D -m 0755 S99cerberus $(DESTDIR)/etc/init.d/S99cerberus

test:
	tests/run.sh

clean:
	rm -f $(BIN)

.PHONY: all size install test clean
