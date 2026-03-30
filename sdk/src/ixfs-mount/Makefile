CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -D_FILE_OFFSET_BITS=64
OUTDIR ?= ../../tools

CORE_SRC = ixfs-core.c ixfs-disk.c
CORE_HDR = ixfs-core.h ixfs-disk.h ixfs-structs.h

# Detect Windows (mingw32-make sets OS=Windows_NT)
ifeq ($(OS),Windows_NT)
  EXE = .exe
  RM = del /q

  # WinFsp: headers in sdk/include/, lib in sdk/lib/ (both vendored)
  WINFSP_CFLAGS = -I../../include
  WINFSP_LIBS = -L../../lib -lwinfsp-x64
  HAS_WINFSP = yes

else
  EXE =
  RM = rm -f
  HAS_WINFSP = no

  # Check for libfuse3 (Linux only)
  FUSE3_CFLAGS := $(shell pkg-config --cflags fuse3 2>/dev/null)
  FUSE3_LIBS   := $(shell pkg-config --libs fuse3 2>/dev/null)
  HAS_FUSE3    := $(if $(FUSE3_LIBS),yes,no)
endif

# Build targets
ifeq ($(OS),Windows_NT)
  ifeq ($(HAS_WINFSP),yes)
    TARGETS = $(OUTDIR)/ixfs-mount.exe
  else
    TARGETS =
  endif
else
  ifeq ($(HAS_FUSE3),yes)
    TARGETS = $(OUTDIR)/ixfs-mount
  else
    TARGETS =
  endif
endif

all: $(TARGETS)
ifeq ($(OS),Windows_NT)
  ifeq ($(HAS_WINFSP),no)
	@echo NOTE: WinFsp SDK not found -- skipping ixfs-mount.exe
  endif
else
  ifeq ($(HAS_FUSE3),no)
	@echo NOTE: libfuse3-dev not found -- skipping ixfs-mount
  endif
endif

# Linux FUSE mount
$(OUTDIR)/ixfs-mount: ixfs-fuse-linux.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) $(FUSE3_CFLAGS) -o $@ ixfs-fuse-linux.c $(CORE_SRC) $(FUSE3_LIBS)

# Windows WinFsp mount (use gnu11 not c11 -- WinFsp headers use MSVC extensions)
$(OUTDIR)/ixfs-mount.exe: ixfs-fuse-win.c $(CORE_SRC) $(CORE_HDR)
	$(CC) -O2 -Wall -Wextra -std=gnu11 -D_FILE_OFFSET_BITS=64 $(WINFSP_CFLAGS) -o $@ ixfs-fuse-win.c $(CORE_SRC) $(WINFSP_LIBS)

# Dev-only test binary (builds in-place, not shipped to sdk/tools/)
test: test_ixfs_core$(EXE)

test_ixfs_core$(EXE): test_ixfs_core.c $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o $@ test_ixfs_core.c $(CORE_SRC)

clean:
	-$(RM) $(OUTDIR)/ixfs-mount$(EXE) test_ixfs_core$(EXE)
