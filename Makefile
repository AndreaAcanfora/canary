CC ?= cc
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror
SANITIZE ?= -fsanitize=address,undefined -fno-omit-frame-pointer
CORE_SRC := $(wildcard core/src/*.c)
CORE_HDR := $(wildcard core/include/canary/*.h)
LINUX_SRC := $(wildcard linux/*.c)
VERSION := $(shell cat VERSION)

.PHONY: all test linux clean

all: test linux

build/test_core: tests/test_core.c $(CORE_SRC) $(CORE_HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -Icore/include -o $@ tests/test_core.c $(CORE_SRC) -lm

test: build/test_core
	./build/test_core

build/canary: $(LINUX_SRC) $(CORE_SRC) $(CORE_HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -DCANARY_VERSION='"$(VERSION)"' -Icore/include -o $@ $(LINUX_SRC) $(CORE_SRC) -lm -ldl

linux: build/canary

clean:
	rm -rf build
