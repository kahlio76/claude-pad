# C core tests. The Swift side builds with `swift build` (see README).
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Wconversion -O2 -ISources/PadCore/include

test: .build/test_core
	./.build/test_core

.build/test_core: tests/test_core.c Sources/PadCore/core.c Sources/PadCore/include/pad_core.h
	@mkdir -p .build
	$(CC) $(CFLAGS) tests/test_core.c Sources/PadCore/core.c -o $@

.PHONY: test
