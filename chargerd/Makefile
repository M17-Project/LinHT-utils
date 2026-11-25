# linht-chargerd: BQ25792 charger daemon for LinHT
#
#   make            the daemon (needs libsystemd)
#   make test       unit tests on the host with a fake chip (no libsystemd)
#   make asan       the same tests with AddressSanitizer and UBSan
#   make replay     tests/replay, logged battery data through the telemetry
#   make install    DESTDIR and PREFIX as usual

CC ?= gcc
CFLAGS ?= -O2
WARN = -std=gnu11 -Wall -Wextra
PREFIX ?= /usr
SYSCONFDIR ?= /etc
UNITDIR ?= /lib/systemd/system
DBUSDIR ?= $(PREFIX)/share/dbus-1/system.d

LIB = core.c bq25792.c policy.c telemetry.c store.c config.c
SRC = main.c dbus.c $(LIB)
HDR = $(wildcard *.h)
TESTS = tests/run.c tests/fake_chip.c tests/test_policy.c tests/test_telemetry.c tests/test_core.c
TEST_HDR = $(wildcard tests/*.h)

all: linht-chargerd

linht-chargerd: $(SRC) $(HDR)
	$(CC) $(WARN) $(CFLAGS) $(LDFLAGS) $(SRC) -o $@ -lsystemd -lm

tests/run-tests: $(TESTS) $(TEST_HDR) $(LIB) $(HDR)
	$(CC) $(WARN) $(CFLAGS) -g -I. $(TESTS) $(LIB) -o $@ -lm

tests/run-tests-asan: $(TESTS) $(TEST_HDR) $(LIB) $(HDR)
	$(CC) $(WARN) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I. \
		$(TESTS) $(LIB) -o $@ -lm

tests/replay: tests/replay.c telemetry.c config.c $(HDR)
	$(CC) $(WARN) $(CFLAGS) -I. tests/replay.c telemetry.c config.c -o $@ -lm

replay: tests/replay

test: tests/run-tests
	./tests/run-tests

asan: tests/run-tests-asan
	./tests/run-tests-asan

install: linht-chargerd
	install -D -m 0755 linht-chargerd $(DESTDIR)$(PREFIX)/bin/linht-chargerd
	install -D -m 0644 linht-chargerd.service $(DESTDIR)$(UNITDIR)/linht-chargerd.service
	install -D -m 0644 org.m17project.linht.Charger1.conf \
		$(DESTDIR)$(DBUSDIR)/org.m17project.linht.Charger1.conf
	install -D -m 0644 chargerd.conf $(DESTDIR)$(SYSCONFDIR)/linht/chargerd.conf

clean:
	rm -f linht-chargerd tests/run-tests tests/run-tests-asan tests/replay

.PHONY: all test asan replay install clean
