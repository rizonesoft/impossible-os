# ============================================================================
# Makefile for UEFI bootloader
#
# Builds a freestanding PE/COFF binary (BOOTX64.EFI) without gnu-efi.
# Uses GCC's PE/COFF output directly via -Wl,--subsystem,10
#
# Usage: make -C src/boot/uefi OUTDIR=build/tools
# ============================================================================

CC      := gcc
LD      := ld
OBJCOPY := objcopy

CFLAGS := -target x86_64-unknown-windows \
          -ffreestanding -fno-stack-protector -fno-stack-check \
          -fshort-wchar -mno-red-zone -mno-mmx -mno-sse \
          -Wall -Wextra -Werror -O2 -std=gnu11 \
          -I.

LDFLAGS := -nostdlib -Wl,-entry:efi_main -Wl,-subsystem:efi_application \
           -fuse-ld=lld-link

# Alternative: use system GCC + objcopy approach
# This works with any GCC that can output ELF, then convert to PE
GCC_CFLAGS := -ffreestanding -fno-stack-protector -fno-stack-check \
              -fshort-wchar -mno-red-zone -fpic -fno-plt \
              -Wall -Wextra -Werror -O2 -std=gnu11 -I.

OUTDIR ?= ../../../build/tools

SRCS := bootx64.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean

all: $(OUTDIR)/BOOTX64.EFI

# Method: GCC → ELF shared object → objcopy to PE/COFF
# This is the standard approach used by Linux efistub and most UEFI apps
bootx64.o: bootx64.c efi.h
	$(CC) $(GCC_CFLAGS) -c $< -o $@

bootx64.so: bootx64.o
	$(LD) -shared -Bsymbolic -z noexecstack \
	    -T uefi.lds \
	    -o $@ $<

$(OUTDIR)/BOOTX64.EFI: bootx64.so
	@mkdir -p $(OUTDIR)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic \
	    -j .dynsym -j .rel -j .rela -j .rel.* -j .rela.* \
	    -j .reloc \
	    --target efi-app-x86_64 \
	    --subsystem=10 \
	    $< $@
	@echo "[EFI] $@ created"

clean:
	rm -f *.o *.so $(OUTDIR)/BOOTX64.EFI
