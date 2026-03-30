CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -D_FILE_OFFSET_BITS=64
OUTDIR ?= ../../tools

CORE_SRC = ixfs-core.c ixfs-disk.c
CORE_HDR = ixfs-core.h ixfs-disk.h ixfs-structs.h

# Detect Windows (mingw32-make sets OS=Windows_NT)
ifeq ($(OS),Windows_NT)
  EXE = .exe
  RM = del /q
else
  EXE =
  RM = rm -f
endif

# Check for libfuse3 (Linux only)
FUSE3_CFLAGS := $(shell pkg-config --cflags fuse3 2>/dev/null)
FUSE3_LIBS   := $(shell pkg-config --libs fuse3 2>/dev/null)
HAS_FUSE3    := $(if $(FUSE3_LIBS),yes,no)

TARGETS = $(OUTDIR)/test_ixfs_core$(EXE)
ifeq ($(HAS_FUSE3),yes)
TARGETS += $(OUTDIR)/ixfs-mount
endif

all: $(TARGETS)
ifeq ($(HAS_FUSE3),no)
ifeq ($(OS),Windows_NT)
	@echo NOTE: FUSE not available on Windows -- only building test_ixfs_core
else
	@echo NOTE: libfuse3-dev not found -- skipping ixfs-mount FUSE binary
endif
endif

$(OUTDIR)/test_ixfs_core$(EXE): test_ixfs_core.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o $@ test_ixfs_core.c $(CORE_SRC)

$(OUTDIR)/ixfs-mount: ixfs-fuse-linux.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) $(FUSE3_CFLAGS) -o $@ ixfs-fuse-linux.c $(CORE_SRC) $(FUSE3_LIBS)

clean:
	-$(RM) $(OUTDIR)/test_ixfs_core$(EXE) $(OUTDIR)/ixfs-mount
