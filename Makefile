# ============================================================================
# Impossible OS — Root Makefile
# Target: x86-64, UEFI-only (GRUB + Multiboot2)
# ============================================================================

# --- Cross-Compiler Toolchain ---
CC      := x86_64-elf-gcc
AS      := nasm
LD      := x86_64-elf-ld
OBJCOPY := x86_64-elf-objcopy

# --- Compiler / Linker Flags ---
CFLAGS  := -Wall -Wextra -Werror \
           -ffreestanding -nostdlib -nostdinc \
           -fno-stack-protector -fno-pie -no-pie \
           -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
           -mcmodel=kernel -std=gnu11 -O2 -g
ASFLAGS := -f elf64 -g
LDFLAGS := -nostdlib -static -z max-page-size=0x1000

# --- Directories ---
SRC_DIR    := src
BUILD_DIR  := build
INCLUDE    := include
BOOT_DIR   := $(SRC_DIR)/boot
KERNEL_DIR := $(SRC_DIR)/kernel
LIBC_DIR   := $(SRC_DIR)/libc

ISO_DIR    := $(BUILD_DIR)/isodir
ISO_FILE   := $(BUILD_DIR)/os-build.iso
KERNEL_BIN := $(BUILD_DIR)/kernel.elf
LINKER_SCRIPT := $(SRC_DIR)/boot/linker.ld

# --- QEMU Configuration (UEFI via OVMF) ---
QEMU        := qemu-system-x86_64
OVMF_CODE   := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS   := /usr/share/OVMF/OVMF_VARS_4M.fd
OVMF_VARS_CP:= $(BUILD_DIR)/OVMF_VARS_4M.fd
QEMU_FLAGS  := -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
               -drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
               -cdrom $(ISO_FILE) \
               -drive file=$(BUILD_DIR)/disk.img,format=raw,if=ide \
               -m 2G \
               -serial stdio \
               -no-reboot -no-shutdown

# --- Source Discovery ---
# Assembly sources (boot + kernel)
ASM_SRCS := $(shell find $(BOOT_DIR) $(KERNEL_DIR) -name '*.asm' 2>/dev/null)
ASM_OBJS := $(patsubst $(SRC_DIR)/%.asm, $(BUILD_DIR)/%.o, $(ASM_SRCS))

# C sources (kernel + libc)
C_SRCS   := $(shell find $(KERNEL_DIR) $(LIBC_DIR) -name '*.c' 2>/dev/null)
C_OBJS   := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SRCS))

# All objects
OBJS     := $(ASM_OBJS) $(C_OBJS)

# ============================================================================
# Targets
# ============================================================================

.PHONY: all boot kernel iso run run-debug clean

## all: Build everything (bootloader + kernel + ISO)
all: iso

## boot: Assemble the bootloader
boot: $(ASM_OBJS)
	@echo "[BOOT] Bootloader objects built"

## kernel: Compile and link the kernel ELF
kernel: $(KERNEL_BIN)
	@echo "[KERNEL] $(KERNEL_BIN) built"

$(KERNEL_BIN): $(OBJS) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) -o $@ $(OBJS)
	@echo "[LD] Linked $@"

## iso: Package kernel into a bootable UEFI ISO via GRUB
iso: $(ISO_FILE)

$(ISO_FILE): $(KERNEL_BIN) $(BOOT_DIR)/grub.cfg
	@mkdir -p $(ISO_DIR)/boot/grub
	cp $(KERNEL_BIN) $(ISO_DIR)/boot/kernel.elf
	cp $(BOOT_DIR)/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO_FILE) $(ISO_DIR) 2>/dev/null
	@echo "[ISO] $(ISO_FILE) created"

## run: Launch QEMU with the ISO (UEFI boot via OVMF)
run: $(ISO_FILE)
	@cp $(OVMF_VARS) $(OVMF_VARS_CP)
	@test -f $(BUILD_DIR)/disk.img || qemu-img create -f raw $(BUILD_DIR)/disk.img 64M
	$(QEMU) $(QEMU_FLAGS)

## run-debug: Launch QEMU paused, waiting for GDB on port 1234
run-debug: $(ISO_FILE)
	@cp $(OVMF_VARS) $(OVMF_VARS_CP)
	$(QEMU) $(QEMU_FLAGS) -s -S -d int,cpu_reset

## clean: Remove all build artifacts
clean:
	rm -rf $(BUILD_DIR)
	@echo "[CLEAN] Build directory removed"

# ============================================================================
# Pattern Rules
# ============================================================================

# Compile C source files (64-bit)
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INCLUDE) -I$(KERNEL_DIR) -c $< -o $@
	@echo "[CC] $<"

# Assemble NASM source files
# entry.asm is multi-format (starts 32-bit, transitions to 64-bit)
# multiboot2_header.asm is format-agnostic (data only)
# Both assembled as elf64 since the linker expects elf64 objects
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@
	@echo "[AS] $<"
