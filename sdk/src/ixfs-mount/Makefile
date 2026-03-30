CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -D_FILE_OFFSET_BITS=64
OUTDIR ?= ../../tools

CORE_SRC = ixfs-core.c ixfs-disk.c
CORE_HDR = ixfs-core.h ixfs-disk.h ixfs-structs.h
TEST_SRC = test_ixfs_core.c

all: $(OUTDIR)/test_ixfs_core

$(OUTDIR)/test_ixfs_core: $(TEST_SRC) $(CORE_SRC) $(CORE_HDR)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(CORE_SRC)

clean:
	rm -f $(OUTDIR)/test_ixfs_core
