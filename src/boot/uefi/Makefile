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

# Pass-through hook for stale-ABI fixture builds. A caller can override
# compile-time constants without forking this Makefile via
# `make -C src/boot/uefi EXTRA_CFLAGS="-DBOOT_INFO_VERSION=8"`.
CFLAGS += $(EXTRA_CFLAGS)

# -MMD -MP so a header this loader actually includes is tracked automatically.
# The hand-maintained prerequisite lists below stay (they cover GENERATED
# headers, which cannot appear in a .d file that does not exist yet on a first
# build) but they no longer have to be COMPLETE: until 2026-08-24 a new shared
# header was invisible to incremental builds, so editing it alone left
# bootx64.o -- and the SIGNED BOOTX64.EFI -- stale while the smoke oracle
# verified a loader built from the previous header. The kernel side never had
# this problem because Makefile:28 has carried -MMD -MP all along.
CFLAGS += -MMD -MP

OUTDIR ?= ../../../build/tools

OBJS := bootx64.o reloc.o sbat.o elf_bootproto.o boot_history.o boot_entries_parser.o boot_policy.o boot_sticky.o boot_entry_kind.o

.PHONY: all clean

all: $(OUTDIR)/BOOTX64.EFI

# Compile C source to position-independent object
# boot_version_constants.h is a UEFI-safe shared header that pins
# fault_class enum + record magic byte-for-byte against the kernel
# enum so the producer here cannot drift from the consumer in
# src/kernel/main/boot_version.c. Listed as a prerequisite so an
# incremental build that touches the constants header rebuilds
# bootx64.o; otherwise the build hazard would silently re-link a
# stale BOOTX64.EFI against a kernel image that already moved.
bootx64.o: bootx64.c efi.h boot_info_mirror.h boot_proto_mirror.h \
           elf_bootproto.h elf_types.h ../../../build/boot_proto_sha.h \
           ../../../include/kernel/boot_version_constants.h \
           ../../../include/kernel/firmware_quirks_table.inc \
           ../../../include/kernel/firmware_quirks_parse.inc \
           ../../../include/boot/uki_cmdline_check.h \
           ../../../build/boot_loader_identity.h
	$(CC) $(CFLAGS) -c $< -o $@

# ELF parser for the .bootproto descriptor; pre-ExitBootServices ABI
# mismatch check.
elf_bootproto.o: elf_bootproto.c elf_bootproto.h elf_types.h \
                 boot_proto_mirror.h efi.h
	$(CC) $(CFLAGS) -c $< -o $@

# Boot-error history ring producer (NVRAM ring + cookie).  Mirrors
# struct boot_error_history_entry from include/kernel/boot_info.h via
# boot_info_mirror.h offset asserts.
boot_history.o: boot_history.c efi.h boot_info_mirror.h
	$(CC) $(CFLAGS) -c $< -o $@

# Boot policy NVRAM sticky reader (read-only consumer of the
# `ImpossibleOS-BootSticky` variable). Surfaces cross-boot trigger bits
# to boot_info v20 audit fields. Bootloader never writes; kernel acks
# consumed triggers post-publish via uefi_var_set.
boot_sticky.o: boot_sticky.c efi.h boot_info_mirror.h \
               ../../../include/boot/boot_audit_codes.h
	$(CC) $(CFLAGS) -c $< -o $@

# Per-kind entry validators. Pure C, no UEFI types -- linkable from both
# the bootloader and the kernel unit-test build. Header lives at
# include/boot/boot_entry_kind.h; spec at docs/boot/boot-entry-schema.md.
boot_entry_kind.o: boot_entry_kind.c \
                   ../../../include/boot/boot_entry_kind.h \
                   ../../../include/boot/boot_entries.h
	$(CC) $(CFLAGS) -c $< -o $@

# Boot entry store parser. Pure C, no UEFI types -- linkable from both the
# bootloader and the kernel unit-test build. Header lives at
# include/boot/boot_entries_parser.h; spec at docs/boot/boot-entry-schema.md.
boot_entries_parser.o: boot_entries_parser.c \
                       ../../../include/boot/boot_entries_parser.h \
                       ../../../include/boot/boot_entries.h
	$(CC) $(CFLAGS) -c $< -o $@

# Boot policy ladder + counter filename parser. Pure C, no UEFI types --
# linkable from both the bootloader and the kernel unit-test build. Header
# lives at include/boot/boot_policy.h; spec at docs/boot/boot-policy.md.
boot_policy.o: boot_policy.c \
               ../../../include/boot/boot_policy.h \
               ../../../include/boot/boot_entries_parser.h \
               ../../../include/boot/boot_entries.h
	$(CC) $(CFLAGS) -c $< -o $@

# Assemble the PE relocation stub
reloc.o: reloc.asm
	$(AS) -f elf64 $< -o $@

# Embed SBAT revocation metadata (sbat.csv) as a .sbat ELF section via
# incbin, carried into the PE by the ELF->PE objcopy (-j .sbat). sbat.csv
# is a prerequisite so a generation bump rebuilds the section.
sbat.o: sbat.asm sbat.csv
	$(AS) -f elf64 $< -o $@

# Link as shared object with relocations preserved
bootx64.so: $(OBJS) uefi.lds
	$(LD) -shared -Bsymbolic \
	    -T uefi.lds \
	    -o $@ $(OBJS)

# Convert ELF → PE/COFF
$(OUTDIR)/BOOTX64.EFI: bootx64.so sbat.csv
	@mkdir -p $(OUTDIR)
	$(OBJCOPY) \
	    -j .text -j .rodata -j .sdata -j .data \
	    -j .dynamic -j .dynsym -j .dynstr \
	    -j .rel -j .rela -j .rel.* -j .rela.* \
	    -j .reloc \
	    -j .sbat \
	    --target efi-app-x86_64 \
	    $< $@
	@echo "[EFI] $@ created ($$(wc -c < $@ | tr -d ' ') bytes)"

clean:
	rm -f *.o *.so $(OUTDIR)/BOOTX64.EFI

# Auto-generated header dependencies (-MMD -MP). Hyphen-prefixed so a missing
# .d on a clean tree is not an error.
-include $(OBJS:.o=.d)
