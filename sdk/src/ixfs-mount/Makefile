CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -D_FILE_OFFSET_BITS=64
OUTDIR ?= ../../tools

CORE_SRC = ixfs-core.c ixfs-disk.c
CORE_HDR = ixfs-core.h ixfs-disk.h ixfs-structs.h

# Check for libfuse3 (Linux only)
FUSE3_CFLAGS := $(shell pkg-config --cflags fuse3 2>/dev/null)
FUSE3_LIBS   := $(shell pkg-config --libs fuse3 2>/dev/null)
HAS_FUSE3    := $(if $(FUSE3_LIBS),yes,no)

ifeq ($(HAS_FUSE3),yes)
TARGETS = $(OUTDIR)/ixfs-mount
else
TARGETS =
endif

all: $(TARGETS)
ifeq ($(HAS_FUSE3),no)
	@echo NOTE: libfuse3-dev not found -- skipping ixfs-mount
endif

$(OUTDIR)/ixfs-mount: ixfs-fuse-linux.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) $(FUSE3_CFLAGS) -o $@ ixfs-fuse-linux.c $(CORE_SRC) $(FUSE3_LIBS)

# Dev-only test binary (builds in-place, not shipped to sdk/tools/)
test: test_ixfs_core

test_ixfs_core: test_ixfs_core.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o $@ test_ixfs_core.c $(CORE_SRC)

clean:
	rm -f $(OUTDIR)/ixfs-mount test_ixfs_core
