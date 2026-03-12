# ============================================================================
# Makefile for UEFI bootloader
#
# Builds a freestanding PE/COFF binary (BOOTX64.EFI) without gnu-efi.
# Uses GCC → ELF shared object → objcopy to PE/COFF.
#
# Usage: make -C src/boot/uefi OUTDIR=build/tools
# ============================================================================

CC      := gcc
AS      := nasm
LD      := ld
OBJCOPY := objcopy

GCC_CFLAGS := -ffreestanding -fno-stack-protector -fno-stack-check \
              -fshort-wchar -mno-red-zone -fpic -fno-plt \
              -Wall -Wextra -Werror -O2 -std=gnu11 -I.

OUTDIR ?= ../../../build/tools

OBJS := bootx64.o reloc.o

.PHONY: all clean

all: $(OUTDIR)/BOOTX64.EFI

# Compile C source to position-independent object
bootx64.o: bootx64.c efi.h
	$(CC) $(GCC_CFLAGS) -c $< -o $@

# Assemble the PE relocation stub
reloc.o: reloc.asm
	$(AS) -f elf64 $< -o $@

# Link as shared object with relocations preserved
bootx64.so: $(OBJS)
	$(LD) -shared -Bsymbolic -z noexecstack \
	    -T uefi.lds \
	    -o $@ $(OBJS)

# Convert ELF → PE/COFF
$(OUTDIR)/BOOTX64.EFI: bootx64.so
	@mkdir -p $(OUTDIR)
	$(OBJCOPY) \
	    -j .text -j .rodata -j .sdata -j .data \
	    -j .dynamic -j .dynsym -j .dynstr \
	    -j .rel -j .rela -j .rel.* -j .rela.* \
	    -j .reloc \
	    --target efi-app-x86_64 \
	    $< $@
	@echo "[EFI] $@ created ($$(wc -c < $@ | tr -d ' ') bytes)"

clean:
	rm -f *.o *.so $(OUTDIR)/BOOTX64.EFI
