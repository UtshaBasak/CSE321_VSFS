# VSFS - Very Simple File System
# Builds three standalone tools from src/ into bin/.

CC       ?= cc
CSTD     := -std=c11
WARN     := -Wall -Wextra -Wshadow -Wpedantic
OPT      ?= -O2
CFLAGS   ?= $(CSTD) $(WARN) $(OPT)
# _POSIX_C_SOURCE exposes pread/pwrite, which strict -std=c11 would otherwise
# hide on glibc. _FILE_OFFSET_BITS keeps off_t 64-bit on 32-bit systems.
CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64

BIN_DIR   := bin
BUILD_DIR := build
PROGRAMS  := mkfs validator journal
TARGETS   := $(addprefix $(BIN_DIR)/,$(PROGRAMS))
OBJECTS   := $(addprefix $(BUILD_DIR)/,$(addsuffix .o,$(PROGRAMS)))
IMAGE     ?= vsfs.img

# Without this, make treats the object files as intermediates of a chained
# implicit rule and deletes them after every build.
.SECONDARY: $(OBJECTS)

.PHONY: all
all: $(TARGETS)

$(BIN_DIR)/%: $(BUILD_DIR)/%.o | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/%.o: src/%.c include/vsfs.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BIN_DIR) $(BUILD_DIR):
	mkdir -p $@

## image: format a fresh filesystem image (override with IMAGE=path)
.PHONY: image
image: $(BIN_DIR)/mkfs
	./$(BIN_DIR)/mkfs $(IMAGE)

## check: build everything and run the test suite
.PHONY: check
check: all
	./tests/run_tests.sh

## debug: rebuild unoptimised with AddressSanitizer and UBSan
.PHONY: debug
debug:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CSTD) $(WARN) -O0 -g3 -fsanitize=address,undefined" all

## clean: remove build output
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

## distclean: also remove generated filesystem images
.PHONY: distclean
distclean: clean
	rm -f $(IMAGE)

.PHONY: help
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/^## /  make /'
