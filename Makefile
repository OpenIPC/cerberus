# The agent lives in src/ — its source, its Makefile and its init script — and that directory is
# what a firmware build points at (`make -C src`). This one only forwards to it, so `make`,
# `make STATIC=1`, `make install DESTDIR=...` work from the root as they always did. Variables given
# on the command line reach src/Makefile on their own; the two that are paths are made absolute
# first, since src/Makefile runs one directory down.
#
# Each goal is a make of its own one level down, so `make -j all install` would build and strip
# src/cerberus twice at once while install copies it: the goals here run one after another (the
# work inside src/ is still parallel).
.NOTPARALLEL:

FORWARD = $(if $(DESTDIR),DESTDIR=$(abspath $(DESTDIR))) $(if $(MBEDTLS),MBEDTLS=$(abspath $(MBEDTLS)))

all install size:
	$(MAKE) -C src $@ $(FORWARD)

# ./cerberus is where builds landed before the move to src/; a stale one must not pass for current.
clean:
	$(MAKE) -C src $@
	rm -f cerberus

test:
	tests/run.sh

.PHONY: all install size clean test
