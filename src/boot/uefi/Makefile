# ============================================================================
# Makefile for UEFI bootloader
#
# Builds a freestanding PE/COFF binary (BOOTX64.EFI) without gnu-efi.
# Uses Clang → ELF shared object → llvm-objcopy to PE/COFF.
#
# Usage: make -C src/boot/uefi OUTDIR=build/tools
# ============================================================================

CC      := clang-19
AS      := nasm
LD      := ld.lld-19
OBJCOPY := objcopy   # GNU objcopy — llvm-objcopy lacks efi-app-x86_64 target

CFLAGS  := --target=x86_64-elf -ffreestanding -fno-stack-protector -fno-stack-check \
           -fshort-wchar -mno-red-zone -fpic -fno-plt \
           -Wall -Wextra -Werror -O2 -std=gnu11 -I. \
           -I../../../build

OUTDIR ?= ../../../build/tools

OBJS := bootx64.o reloc.o elf_bootproto.o

.PHONY: all clean

all: $(OUTDIR)/BOOTX64.EFI

# Compile C source to position-independent object
bootx64.o: bootx64.c efi.h boot_info_mirror.h boot_proto_mirror.h \
           elf_bootproto.h elf_types.h ../../../build/boot_proto_sha.h
	$(CC) $(CFLAGS) -c $< -o $@

# ELF parser for the .bootproto descriptor; pre-ExitBootServices ABI
# mismatch check.
elf_bootproto.o: elf_bootproto.c elf_bootproto.h elf_types.h \
                 boot_proto_mirror.h efi.h
	$(CC) $(CFLAGS) -c $< -o $@

# Assemble the PE relocation stub
reloc.o: reloc.asm
	$(AS) -f elf64 $< -o $@

# Link as shared object with relocations preserved
bootx64.so: $(OBJS)
	$(LD) -shared -Bsymbolic \
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
