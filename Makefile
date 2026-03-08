# ============================================================================
# Impossible OS — Root Makefile
# Target: x86-64, UEFI-only (GRUB + Multiboot2)
# ============================================================================

# Tools
CC      := x86_64-elf-gcc
HOST_CC := gcc
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
DESKTOP_DIR:= $(SRC_DIR)/desktop

ISO_DIR    := $(BUILD_DIR)/isodir
ISO_FILE   := $(BUILD_DIR)/os-build.iso
KERNEL_BIN := $(BUILD_DIR)/kernel.exe
LINKER_SCRIPT := $(SRC_DIR)/boot/linker.ld

# --- QEMU Configuration (UEFI via OVMF) ---
QEMU        := qemu-system-x86_64
OVMF_CODE   := /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS   := /usr/share/OVMF/OVMF_VARS_4M.fd
OVMF_VARS_CP:= $(BUILD_DIR)/OVMF_VARS_4M.fd

# --- Version Information ---
# Read SemVer from VERSION file, auto-increment build number
VERSION_FILE   := VERSION
BUILD_NUM_FILE := .build_number
VERSION_RAW    := $(shell cat $(VERSION_FILE) 2>/dev/null | tr -d '[:space:]')
VERSION_MAJOR  := $(word 1,$(subst ., ,$(VERSION_RAW)))
VERSION_MINOR  := $(word 2,$(subst ., ,$(VERSION_RAW)))
VERSION_PATCH  := $(word 3,$(subst ., ,$(VERSION_RAW)))
BUILD_NUMBER   := $(shell cat $(BUILD_NUM_FILE) 2>/dev/null | tr -d '[:space:]')
GIT_HASH       := $(shell git rev-parse --short=8 HEAD 2>/dev/null || echo "unknown")

# Inject version into CFLAGS
CFLAGS += -DVERSION_MAJOR=$(VERSION_MAJOR) \
          -DVERSION_MINOR=$(VERSION_MINOR) \
          -DVERSION_PATCH=$(VERSION_PATCH) \
          -DVERSION_BUILD=$(BUILD_NUMBER) \
          -DVERSION_GIT_HASH='"$(GIT_HASH)"'

# --- Source Discovery ---
# Assembly sources (boot + kernel)
ASM_SRCS := $(shell find $(BOOT_DIR) $(KERNEL_DIR) -name '*.asm' 2>/dev/null)
ASM_OBJS := $(patsubst $(SRC_DIR)/%.asm, $(BUILD_DIR)/%.o, $(ASM_SRCS))

# C sources (kernel + libc)
C_SRCS   := $(shell find $(KERNEL_DIR) $(LIBC_DIR) $(DESKTOP_DIR) -name '*.c' 2>/dev/null)
C_OBJS   := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SRCS))

# All objects
OBJS     := $(ASM_OBJS) $(C_OBJS)

# ============================================================================
# Targets
# ============================================================================

.PHONY: all _increment_build boot kernel iso grub-efi system-disk test-disks run run-debug run-log clean

## all: Build everything (bootloader + kernel + ISO + system disk)
all: _increment_build iso grub-efi system-disk
	@echo "[VERSION] Impossible OS v$(VERSION_RAW).$(BUILD_NUMBER) ($(GIT_HASH))"

## _increment_build: Auto-increment the build number
_increment_build:
	@expr $$(cat $(BUILD_NUM_FILE) 2>/dev/null || echo 0) + 1 > $(BUILD_NUM_FILE)
	@echo "[BUILD] #$$(cat $(BUILD_NUM_FILE))"

## grub-efi: Build standalone GRUB EFI binary for direct disk boot
GRUB_EFI := $(BUILD_DIR)/tools/BOOTX64.EFI
GRUB_MODULES := part_gpt fat normal multiboot2 boot \
                all_video efi_gop gfxterm configfile echo \
                search test reboot halt font loadenv

grub-efi: $(GRUB_EFI)

$(GRUB_EFI):
	@mkdir -p $(BUILD_DIR)/tools
	grub-mkimage -O x86_64-efi -o $@ -p /boot/grub $(GRUB_MODULES)
	@echo "[EFI] $@ created ($$(wc -c < $@ | tr -d ' ') bytes)"

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
	@# Build jpg2raw converter (uses stb_image)
	@mkdir -p $(BUILD_DIR)/tools
	$(HOST_CC) -O2 -o $(BUILD_DIR)/tools/jpg2raw tools/jpg2raw.c -lm -Itools
	@# Create sysroot staging directory and test files
	@mkdir -p $(BUILD_DIR)/sysroot
	@echo -n "Hello from Impossible OS!" > $(BUILD_DIR)/sysroot/hello.txt
	@echo -n "IXFS root filesystem" > $(BUILD_DIR)/sysroot/readme.txt
	@# Convert wallpaper background image to raw BGRA
	$(BUILD_DIR)/tools/jpg2raw resources/backgrounds/background.jpg \
		$(BUILD_DIR)/sysroot/wallpaper.raw 1280 720 2>&1
	@cp $(BUILD_DIR)/sysroot/wallpaper.raw $(BUILD_DIR)/sysroot/bg.raw
	@# Convert start button icon (32x32 PNG with alpha)
	$(BUILD_DIR)/tools/jpg2raw resources/start/icon_32.png \
		$(BUILD_DIR)/sysroot/start_icon.raw 32 32 2>&1
	@# Build userland libc
	@mkdir -p $(BUILD_DIR)/user/lib
	$(AS) -f elf64 -g user/lib/crt0.asm -o $(BUILD_DIR)/user/lib/crt0.o
	$(CC) -Wall -Wextra -Werror -ffreestanding -nostdlib -nostdinc \
		-fno-stack-protector -fno-pie -no-pie -mno-red-zone \
		-mno-mmx -mno-sse -mno-sse2 -std=gnu11 -O2 -g \
		-Iuser/include -c user/lib/string.c -o $(BUILD_DIR)/user/lib/string.o
	$(CC) -Wall -Wextra -Werror -ffreestanding -nostdlib -nostdinc \
		-fno-stack-protector -fno-pie -no-pie -mno-red-zone \
		-mno-mmx -mno-sse -mno-sse2 -std=gnu11 -O2 -g \
		-Iuser/include -c user/lib/stdlib.c -o $(BUILD_DIR)/user/lib/stdlib.o
	$(CC) -Wall -Wextra -Werror -ffreestanding -nostdlib -nostdinc \
		-fno-stack-protector -fno-pie -no-pie -mno-red-zone \
		-mno-mmx -mno-sse -mno-sse2 -std=gnu11 -O2 -g \
		-Iuser/include -c user/lib/stdio.c -o $(BUILD_DIR)/user/lib/stdio.o
	$(CC) -Wall -Wextra -Werror -ffreestanding -nostdlib -nostdinc \
		-fno-stack-protector -fno-pie -no-pie -mno-red-zone \
		-mno-mmx -mno-sse -mno-sse2 -std=gnu11 -O2 -g \
		-Iuser/include -c user/lib/ctype.c -o $(BUILD_DIR)/user/lib/ctype.o
	$(CC) -Wall -Wextra -Werror -ffreestanding -nostdlib -nostdinc \
		-fno-stack-protector -fno-pie -no-pie -mno-red-zone \
		-mno-mmx -mno-sse -mno-sse2 -std=gnu11 -O2 -g \
		-Iuser/include -c user/lib/math.c -o $(BUILD_DIR)/user/lib/math.o
	x86_64-elf-ar rcs $(BUILD_DIR)/user/libc.a \
		$(BUILD_DIR)/user/lib/string.o \
		$(BUILD_DIR)/user/lib/stdlib.o \
		$(BUILD_DIR)/user/lib/stdio.o \
		$(BUILD_DIR)/user/lib/ctype.o \
		$(BUILD_DIR)/user/lib/math.o
	@echo "[LIBC] $(BUILD_DIR)/user/libc.a created"
	@# Build user-mode ELF programs (linked against crt0 + libc)
	$(CC) -Wall -Wextra -Werror -ffreestanding -nostdlib -nostdinc \
		-fno-stack-protector -fno-pie -no-pie -mno-red-zone \
		-mno-mmx -mno-sse -mno-sse2 -std=gnu11 -O2 -g \
		-Iuser/include -c user/hello.c -o $(BUILD_DIR)/user/hello.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/hello.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/hello.o $(BUILD_DIR)/user/libc.a
	$(CC) -Wall -Wextra -Werror -ffreestanding -nostdlib -nostdinc \
		-fno-stack-protector -fno-pie -no-pie -mno-red-zone \
		-mno-mmx -mno-sse -mno-sse2 -std=gnu11 -O2 -g \
		-Iuser/include -c user/shell.c -o $(BUILD_DIR)/user/shell.o
	$(LD) -nostdlib -static -T user/user.ld -o $(BUILD_DIR)/user/shell.exe \
		$(BUILD_DIR)/user/lib/crt0.o $(BUILD_DIR)/user/shell.o $(BUILD_DIR)/user/libc.a
	@cp $(BUILD_DIR)/user/hello.exe $(BUILD_DIR)/sysroot/hello.exe
	@cp $(BUILD_DIR)/user/shell.exe $(BUILD_DIR)/sysroot/shell.exe
	cp $(KERNEL_BIN) $(ISO_DIR)/boot/kernel.exe
	cp $(BOOT_DIR)/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO_FILE) $(ISO_DIR) 2>/dev/null
	@echo "[ISO] $(ISO_FILE) created"

## system-disk: Create bootable GPT system disk with EFI + IXFS partitions
SYSTEM_DISK := $(BUILD_DIR)/system-disk.img
SYSTEM_DISK_SIZE := 512M
EFI_SIZE := 256M
# Partition offsets (must match make-system-disk defaults)
# EFI: LBA 2048 = byte 1048576, size 256M = 268435456 bytes
# IXFS: LBA 526336 = byte 269484032 (next 2048-aligned LBA after EFI end)
EFI_OFFSET := 1048576
IXFS_OFFSET := 269484032
IXFS_PART_SIZE := $(shell echo $$(( (512*1024*1024 - 269484032 - 34*512) )) )

system-disk: $(SYSTEM_DISK)

$(SYSTEM_DISK): $(KERNEL_BIN) $(GRUB_EFI) $(BOOT_DIR)/grub.cfg \
                tools/make-system-disk.c tools/mkfs-ixfs.c \
                $(BUILD_DIR)/sysroot/hello.txt
	@echo "[DISK] Building system disk..."
	@mkdir -p $(BUILD_DIR)/tools
	$(HOST_CC) -O2 -o $(BUILD_DIR)/tools/make-system-disk tools/make-system-disk.c
	$(HOST_CC) -O2 -o $(BUILD_DIR)/tools/mkfs-ixfs tools/mkfs-ixfs.c
	@# Step 1: Create GPT image with partition table
	$(BUILD_DIR)/tools/make-system-disk -o $@ -s $(SYSTEM_DISK_SIZE) --efi-size $(EFI_SIZE)
	@# Step 2: Format EFI partition as FAT32 and copy boot files
	mkfs.fat -F 32 --offset $$(( $(EFI_OFFSET) / 512 )) $@
	@mkdir -p $(BUILD_DIR)/efi_staging/EFI/BOOT
	@mkdir -p $(BUILD_DIR)/efi_staging/boot/grub
	@cp $(GRUB_EFI) $(BUILD_DIR)/efi_staging/EFI/BOOT/BOOTX64.EFI
	@cp $(KERNEL_BIN) $(BUILD_DIR)/efi_staging/boot/kernel.exe
	@cp $(BOOT_DIR)/grub.cfg $(BUILD_DIR)/efi_staging/boot/grub/grub.cfg
	mcopy -i $@@@$(EFI_OFFSET) -s $(BUILD_DIR)/efi_staging/* ::
	@rm -rf $(BUILD_DIR)/efi_staging
	@# Step 3: Format IXFS partition and populate with system files
	$(BUILD_DIR)/tools/mkfs-ixfs \
		-o $@ \
		-s $(IXFS_PART_SIZE) \
		-l "Impossible OS" \
		--offset $(IXFS_OFFSET) \
		--populate $(BUILD_DIR)/sysroot
	@echo "[DISK] $@ created ($(SYSTEM_DISK_SIZE) GPT: EFI + IXFS)"

## run: Launch QEMU booting from system disk (UEFI via OVMF)
run: all
	@cp $(OVMF_VARS) $(OVMF_VARS_CP)
	$(QEMU) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
		-drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
		-device ich9-ahci,id=ahci0 \
		-device ide-hd,drive=disk0,bus=ahci0.0 \
		-m 2G \
		-serial stdio \
		-vga none \
		-device VGA,xres=1280,yres=720 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device virtio-tablet-pci \
		-rtc base=localtime \
		-no-reboot

## run-debug: Launch QEMU paused, waiting for GDB on port 1234
run-debug: all
	@cp $(OVMF_VARS) $(OVMF_VARS_CP)
	$(QEMU) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
		-drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
		-device ich9-ahci,id=ahci0 \
		-device ide-hd,drive=disk0,bus=ahci0.0 \
		-m 2G \
		-serial stdio \
		-vga none \
		-device VGA,xres=1280,yres=720 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device virtio-tablet-pci \
		-rtc base=localtime \
		-no-reboot \
		-s -S -d int,cpu_reset

## run-log: Launch QEMU with serial output captured to serial.log
run-log: all
	@cp $(OVMF_VARS) $(OVMF_VARS_CP)
	$(QEMU) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_CP) \
		-drive id=disk0,file=$(SYSTEM_DISK),format=raw,if=none \
		-device ich9-ahci,id=ahci0 \
		-device ide-hd,drive=disk0,bus=ahci0.0 \
		-m 2G \
		-serial file:serial.log \
		-vga none \
		-device VGA,xres=1280,yres=720 \
		-device rtl8139,netdev=net0 \
		-netdev user,id=net0 \
		-device virtio-tablet-pci \
		-rtc base=localtime \
		-no-reboot
	@echo "[LOG] Serial output saved to serial.log"

## test-disks: Generate test disk images for filesystem driver testing
test-disks: system-disk
	@bash tools/make-test-disks.sh $(BUILD_DIR)/test-disks $(BUILD_DIR)

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
