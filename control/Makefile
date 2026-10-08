CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11 -D_GNU_SOURCE
SRC     := src/main.c src/device.c src/proto.c src/tty.c src/usb.c
HDR     := src/keeper.h
BIN     := build/capsula-keeper
PREFIX  ?= /usr/local

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC)

build/proto_test: tests/proto_test.c src/proto.c $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/proto_test.c src/proto.c

check: build/proto_test
	./build/proto_test

# Frame-parser fuzzing under ASan/UBSan (needs a sanitizer-capable toolchain).
fuzz: build/fuzz_proto
	./build/fuzz_proto 30000 257

build/fuzz_proto: tests/fuzz_proto.c src/proto.c $(HDR)
	@mkdir -p build
	$(CC) -Isrc -O1 -g -fsanitize=address,undefined \
		-fno-sanitize-recover=all -std=c11 -D_GNU_SOURCE -o $@ $^

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/capsula-keeper

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/capsula-keeper

clean:
	rm -rf build

.PHONY: all check fuzz install uninstall clean
