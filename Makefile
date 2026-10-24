# C tests. The Swift side builds with `swift build` (see README).
#   make test       both suites
#   make core-test  the keypad logic (Sources/PadCore)
#   make fw-test    the Pico-side logic (firmware/src)
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Wconversion -O2 -ISources/PadCore/include

test: core-test fw-test

core-test: .build/test_core
	./.build/test_core

fw-test: .build/test_fw
	./.build/test_fw

.build/test_core: tests/test_core.c Sources/PadCore/core.c Sources/PadCore/include/pad_core.h
	@mkdir -p .build
	$(CC) $(CFLAGS) tests/test_core.c Sources/PadCore/core.c -o $@

.build/test_fw: firmware/tests/test_fw.c firmware/src/pad_fw.c firmware/src/pad_fw.h Sources/PadCore/core.c Sources/PadCore/include/pad_core.h
	@mkdir -p .build
	$(CC) $(CFLAGS) -Ifirmware/src firmware/tests/test_fw.c firmware/src/pad_fw.c Sources/PadCore/core.c -o $@

.PHONY: test core-test fw-test
