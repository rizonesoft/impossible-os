# 🖥️ Impossible OS — Master TODO

> **Goal:** Build a freestanding x86-64 operating system from scratch — bootloader through desktop — using a sandboxed WSL 2 + QEMU development loop and Hyper-V for production validation.

---

## Legend

- `[ ]` — Not started
- `[/]` — In progress
- `[x]` — Complete

---

## Phase 0 — Environment & Safety Setup

### 0.1 WSL 2 Sandbox

- [x] Verify WSL 2 is installed and running (`wsl --list --verbose`) ✅ Kernel `6.6.87.2-microsoft-standard-WSL2`
- [x] Confirm Ubuntu distro is set to WSL **2** (not 1) ✅ Ubuntu 24.04 LTS, kernel confirms WSL2
- [x] Create dedicated workspace: `mkdir -p ~/impossible-os && cd ~/impossible-os` ✅
- [x] Confirm project lives on the **Linux** filesystem (`~/`), **not** `/mnt/c/` ✅
- [x] Set a strong UNIX password for `sudo` operations ✅

### 0.2 Connect Antigravity (IDE) to WSL

- [x] Install the **WSL** extension in Antigravity / VS Code ✅
- [x] Connect Antigravity remotely to WSL (`F1 → WSL: Connect to WSL`) ✅
- [x] Open the workspace folder `/home/<user>/impossible-os` inside the remote session ✅
- [x] Verify the integrated terminal shows a Linux shell (`bash` / `zsh`) ✅ bash on /dev/pts

### 0.3 Agent Safeguards

- [x] Disable **Turbo Mode** in terminal settings (set to `Auto` or `Off`) ✅
- [x] Create `.agents/rules.md` with the OS-dev system prompt (restrict agent to workspace only) ✅ loaded via IDE MEMORY[rules.md]
- [x] Create `.agents/workflows/build.md` — defines `make`, `make iso`, `make run` steps ✅
- [x] Create `.agents/workflows/test-hyperv.md` — documents the Hyper-V ISO hand-off process ✅
- [x] Create `.agents/workflows/release.md` — documents tagging, changelog, ISO naming conventions ✅

### 0.4 Install OS Dev Toolchain

- [x] `sudo apt update && sudo apt upgrade -y` ✅ all packages up to date
- [x] Install core build tools: `build-essential bison flex texinfo nasm` ✅ already installed
- [x] Install math/compiler libs: `libgmp3-dev libmpc-dev libmpfr-dev` ✅ already installed
- [x] Install ISO tooling: `xorriso mtools grub-pc-bin grub-common` ✅
- [x] Install QEMU: `qemu-system-x86` ✅ v8.2.2 already installed
- [x] Install UEFI firmware: `sudo apt install -y ovmf` ✅ v2024.02 already installed
- [x] Verify OVMF exists: `ls /usr/share/OVMF/OVMF_CODE_4M.fd` ✅ (Ubuntu 24.04 uses `_4M` variant)
- [x] Verify each tool is on `$PATH` (`nasm --version`, `qemu-system-x86_64 --version`, etc.) ✅ all 8 tools verified
- [x] *(Optional)* Build a **GCC cross-compiler** (`x86_64-elf-gcc`) for a fully freestanding toolchain ✅
  - [x] Download GCC & Binutils source tarballs ✅ Binutils 2.42 + GCC 13.3.0
  - [x] Configure Binutils with `--target=x86_64-elf` ✅
  - [x] Build & install Binutils to `~/opt/cross/` ✅
  - [x] Configure GCC with `--target=x86_64-elf --without-headers` ✅
  - [x] Build & install GCC to `~/opt/cross/` ✅
  - [x] Add `~/opt/cross/bin` to `$PATH` in `~/.bashrc` ✅
  - [x] Verify: `x86_64-elf-gcc --version` ✅ GCC 13.3.0

---

## Phase 1 — Repository & Project Skeleton

### 1.1 Initialize Git

- [x] `git init` in the workspace root ✅
- [x] Create `.gitignore` (ignore `build/`, `iso/`, `*.o`, `*.bin`, `*.iso`, `*.img`, `cross-compiler/`) ✅
- [x] Create initial `README.md` with project name, description, and build instructions ✅
- [x] Create `LICENSE` file (choose MIT / GPL v2 / etc.) ✅ MIT License
- [x] `git add -A && git commit -m "initial: project skeleton"` ✅ commit `f5772b2`

### 1.2 GitHub Remote

- [x] Create a new GitHub repository (public or private) ✅
- [x] Add the remote: `git remote add origin git@github.com:<user>/impossible-os.git` ✅ `https://github.com/rizonesoft/impossible-os`
- [x] Push initial commit: `git push -u origin main` ✅

### 1.3 Directory Structure

- [x] Create `src/boot/` — bootloader assembly & early C ✅
- [x] Create `src/kernel/` — kernel core ✅
- [x] Create `src/kernel/drivers/` — hardware drivers ✅
- [x] Create `src/kernel/mm/` — memory management ✅
- [x] Create `src/kernel/fs/` — filesystem ✅
- [x] Create `src/kernel/ipc/` — inter-process communication ✅
- [x] Create `src/kernel/sched/` — scheduler ✅
- [x] Create `src/libc/` — minimal C standard library ✅
- [x] Create `src/shell/` — command-line shell ✅
- [x] Create `src/desktop/` — desktop environment & window manager ✅
- [x] Create `src/installer/` — OS installer ✅
- [x] Create `include/` — shared kernel headers ✅
- [x] Create `scripts/` — helper build/test scripts ✅
- [x] Create `build/` — build output (gitignored) ✅
- [x] Create `docs/` — architecture notes, design docs ✅
- [x] Create `todo/` — this TODO file ✅
- [x] Commit: `"chore: establish directory structure"` ✅ `39bec1a`

### 1.4 Build System (Makefile)

- [x] Create root `Makefile` with variables: `CC`, `AS`, `LD`, `CFLAGS`, `LDFLAGS` ✅
- [x] Add `make all` target — builds everything into `build/` ✅
- [x] Add `make boot` target — assembles the bootloader ✅
- [x] Add `make kernel` target — compiles and links the kernel ✅
- [x] Add `make iso` target — packages `build/` into `os-build.iso` ✅
- [x] Add `make run` target — launches QEMU with the ISO ✅
- [x] Add `make clean` target — removes all build artifacts ✅
- [x] Test `make clean && make all && make run` cycle end-to-end ✅ kernel boots via UEFI, serial output confirmed
- [x] Commit: `"build: root Makefile with full build pipeline"` ✅ `531b05f`

---

## Phase 2 — QEMU Quick-Test System

### 2.1 Basic QEMU Launch

- [x] Write a minimal `scripts/run-qemu.sh` wrapper script ✅ supports `--debug` and `--headless`
- [x] Configure QEMU for UEFI boot ✅ uses split pflash OVMF (CODE + VARS)
- [x] Verify WSLg passes the QEMU GUI window through to Windows desktop ✅
- [x] If no WSLg: configure QEMU with `-nographic` or `-curses` as fallback ✅ `--headless` flag
- [x] Add `-enable-kvm` flag (if KVM is available inside WSL — check `lscpu | grep Virtualization`) ✅ VT-x + `/dev/kvm` detected, auto-enabled

### 2.2 Debug-Friendly QEMU

- [x] Add `-d int,cpu_reset` to log interrupts during crashes ✅ in `make run-debug`
- [x] Add `-s -S` flags for GDB remote debugging on port 1234 ✅ in `--debug` flag
- [x] Create `scripts/debug.sh` that launches QEMU paused + connects GDB ✅
- [x] Write a `.gdbinit` file with `target remote :1234` and symbol loading ✅
- [ ] Test breakpoint at kernel entry in GDB
- [x] Commit: `"test: QEMU launch and debug scripts"` ✅ `c2fc17c`

---

## Phase 3 — Bootloader (UEFI Only)

> ⚡ **Design Decision:** This OS is **UEFI-only** — no legacy BIOS/MBR support.
> GRUB handles the UEFI handshake and drops us directly into 64-bit Long Mode.
> This aligns with Hyper-V Generation 2 VMs, which are also UEFI-only.

### 3.1 GRUB UEFI Bootloader

- [x] Write `src/boot/grub.cfg` with `menuentry "Impossible OS"` pointing to the kernel ELF ✅
- [x] Configure GRUB to use **Multiboot2** protocol (UEFI-compatible) ✅
- [x] Write `src/boot/multiboot2_header.asm` — Multiboot2 header with framebuffer tag ✅ 1024×768×32
- [x] Request a **GOP framebuffer** via Multiboot2 framebuffer tag (no VGA text mode) ✅
- [x] Test: `grub-mkrescue` produces a bootable UEFI ISO ✅ 12 MB ISO
- [x] Test: QEMU boots the ISO with OVMF → GRUB menu appears → kernel loads ✅
- [x] Commit: `"boot: GRUB UEFI bootloader with Multiboot2"` ✅ `531b05f`

### 3.2 Kernel Entry via Multiboot2

- [x] Write `src/boot/entry.asm` — Multiboot2 entry stub ✅ verifies magic, error to serial
- [x] GRUB drops us in **32-bit protected mode** — verify via Multiboot2 magic in `eax` ✅ `0x36D76289`
- [x] Parse the **Multiboot2 info structure** (memory map, framebuffer info, ACPI RSDP) ✅ 20 mmap entries
- [x] Save framebuffer address, pitch, width, height, bpp from Multiboot2 tags ✅
- [x] Commit: `"boot: Multiboot2 entry and info parsing"` ✅ `bd29d60`

### 3.3 Enter 64-bit Long Mode

- [x] Set up identity-mapped **page tables** (PML4 → PDPT → PD → PT) ✅ 4 GiB with 2 MiB pages
- [x] Enable **PAE** (bit 5 of `cr4`) ✅
- [x] Set **Long Mode Enable** bit in `IA32_EFER` MSR ✅
- [x] Enable **paging** (bit 31 of `cr0`) ✅
- [x] Load a 64-bit GDT and far-jump to 64-bit code segment ✅
- [x] Set up 64-bit stack, zero BSS, call `kernel_main()` ✅
- [x] Verify `rip` is executing in 64-bit mode ✅ CS=CS64, serial confirms Long Mode
- [x] Commit: `"boot: transition to 64-bit Long Mode"` ✅ `8d510ed`

---

## Phase 4 — Kernel Core

### 4.1 Kernel Entry & Early Framebuffer Output

- [x] Write `src/kernel/main.c` — `kernel_main()` entry point ✅ (64-bit, System V ABI)
- [x] Write `src/kernel/framebuffer.c` / `framebuffer.h` — early framebuffer console using GOP info ✅
- [x] Embed a **PSF bitmap font** for text rendering on the framebuffer ✅ 8×16 VGA, 95 glyphs
- [x] Implement `fb_putchar()`, `fb_write()`, `fb_clear()` — draw text to pixel framebuffer ✅ + scroll, colors
- [x] Implement `printk()` — kernel printf, renders to framebuffer + serial ✅ %d/%u/%x/%p/%s/%c
- [x] Print "Impossible OS Kernel Loaded" on screen ✅
- [x] Commit: `"kernel: framebuffer console and printk"` ✅ `db71581`

### 4.2 Global Descriptor Table (Kernel-Level)

- [x] Write `src/kernel/gdt.c` / `gdt.h` ✅
- [x] Define kernel code, kernel data, user code, user data segments ✅
- [x] Define a **TSS** (Task State Segment) entry ✅ at `0x10F040`
- [x] Write `gdt_flush()` in assembly to reload segment registers ✅ `gdt_asm.asm`
- [x] Call `gdt_init()` from `kernel_main()` ✅
- [x] Commit: `"kernel: GDT with TSS"` ✅ `13b84e7`

### 4.3 Interrupt Descriptor Table (IDT)

- [x] Write `src/kernel/idt.c` / `idt.h` ✅
- [x] Define 256 IDT entries ✅
- [x] Write ISR stubs (0–31) for CPU exceptions in assembly ✅ `isr_stubs.asm`
- [x] Write IRQ stubs (32–47) for hardware interrupts ✅
- [x] Implement a generic `isr_handler()` dispatcher in C ✅ with KERNEL PANIC screen
- [x] Load IDT with `lidt` ✅
- [x] Test: trigger a divide-by-zero → handler fires, prints message → ✅
- [x] Commit: `"kernel: IDT and exception handlers"` ✅ `e3b72ee`

### 4.4 Programmable Interrupt Controller (PIC)

- [x] Write `src/kernel/drivers/pic.c` / `pic.h` ✅
- [x] Remap PIC1 (master) to IRQ 32–39, PIC2 (slave) to IRQ 40–47 ✅
- [x] Implement `pic_send_eoi()` — end-of-interrupt acknowledgment ✅
- [x] Mask/unmask individual IRQ lines ✅
- [x] Commit: `"kernel: PIC remapping and IRQ management"` ✅ `90625cb`

### 4.5 Programmable Interval Timer (PIT) — System Clock

- [x] Write `src/kernel/drivers/pit.c` / `pit.h` ✅
- [x] Program PIT channel 0 to fire at ~100 Hz (or 1000 Hz) ✅ 100 Hz, divisor 11931
- [x] Register IRQ 0 handler → increments a global `tick_count` ✅
- [x] Implement `sleep_ms()` using tick count ✅ hlt-based busy wait
- [x] Implement `uptime()` returning seconds since boot ✅
- [x] Commit: `"kernel: PIT timer at 100 Hz"` ✅ `3c4913f`

### 4.6 Keyboard Driver (PS/2)

- [x] Write `src/kernel/drivers/keyboard.c` / `keyboard.h` ✅
- [x] Register IRQ 1 handler ✅ (interrupt vector 33)
- [x] Read scan codes from port `0x60` ✅
- [x] Build a scan-code → ASCII lookup table (US QWERTY) ✅
- [x] Handle Shift, Caps Lock, Ctrl, Alt modifier keys ✅
- [x] Push characters into a circular input buffer ✅ 256-byte ring buffer
- [x] Implement `keyboard_getchar()` — blocking read from buffer ✅ + non-blocking `trygetchar()`
- [x] Test: type on keyboard → characters appear on screen → ✅ echo loop with `>` prompt
- [x] Commit: `"kernel: PS/2 keyboard driver"` ✅ `73921fe`

---

## Phase 5 — Memory Management

### 5.1 Physical Memory Manager (PMM)

- [x] Write `src/kernel/mm/pmm.c` / `pmm.h` ✅
- [x] Parse the **Multiboot2 memory map** tag (UEFI-provided) ✅
- [x] Implement a **bitmap allocator** (1 bit per 4 KiB frame) ✅ 1048576 frames = 4 GiB
- [x] `pmm_alloc_frame()` — find and mark a free frame ✅
- [x] `pmm_free_frame()` — release a frame ✅
- [x] Mark kernel memory, BIOS, and MMIO regions as reserved ✅ first 1 MiB + kernel + bitmap
- [x] Commit: `"mm: physical memory manager (bitmap)"` ✅ `d906cec`

### 5.2 Virtual Memory Manager (VMM) — Paging

- [x] Write `src/kernel/mm/vmm.c` / `vmm.h` ✅
- [x] Create kernel page directory / PML4 ✅ takes over boot PML4 from CR3
- [x] Implement `vmm_map_page(virt, phys, flags)` — maps a single 4 KiB page ✅
- [x] Implement `vmm_unmap_page(virt)` — unmaps and optionally frees the frame ✅
- [x] Identity-map the first 4 GiB (boot-time 2 MiB pages) ✅ preserved from entry.asm
- [x] Higher-half kernel mapping at `0xFFFFFFFF80000000` — deferred to future phase
- [x] Handle **page faults** (ISR 14) — print faulting address from `cr2` ✅ with decoded error code
- [x] Commit: `"mm: virtual memory manager with paging"` ✅ `3c05f6e`

### 5.3 Kernel Heap (`kmalloc` / `kfree`)

- [x] Write `src/kernel/mm/heap.c` / `heap.h` ✅
- [x] Implement a simple **first-fit free-list** allocator ✅
- [x] `kmalloc(size)` — allocate kernel memory ✅ 16-byte aligned
- [x] `kfree(ptr)` — free kernel memory ✅ with double-free protection
- [x] `krealloc(ptr, size)` — resize allocation ✅ with next-block merge optimization
- [x] Add basic **coalescing** of adjacent free blocks ✅
- [x] Test: allocate, use, and free varying sizes without corruption → ✅
- [x] Commit: `"mm: kernel heap allocator"` ✅ `8c6c131`

---

## Phase 6 — Storage & Filesystem

> ⚡ **Design Decision:** Impossible OS uses **Windows-style drive letters** (`C:\`, `D:\`)
> and a custom filesystem called **IXFS** (Impossible X FileSystem) for the root partition.
> FAT32 is used only for the EFI System Partition (required by UEFI).

### 6.1 ATA/IDE Disk Driver

- [ ] Write `src/kernel/drivers/ata.c` / `ata.h`
- [ ] Detect ATA drives on the primary bus (ports `0x1F0`–`0x1F7`)
- [ ] Implement `ata_read_sectors(lba, count, buffer)` via PIO
- [ ] Implement `ata_write_sectors(lba, count, buffer)` via PIO
- [ ] Test: read sector 0 of a QEMU disk image → get known data → ✅
- [ ] Commit: `"drivers: ATA PIO disk read/write"`

### 6.2 Virtual Filesystem (VFS) Layer — Drive Letters

- [ ] Write `src/kernel/fs/vfs.c` / `vfs.h`
- [ ] Define `vfs_node` struct: name, type (file/dir), size, inode, read/write/open/close ops
- [ ] Implement **drive letter mounting**: assign `A:\`, `C:\`, `D:\` etc. to partitions
- [ ] Implement **backslash path parsing**: `C:\Users\Default\file.txt`
- [ ] Implement `vfs_open()`, `vfs_read()`, `vfs_write()`, `vfs_close()`
- [ ] Implement `vfs_readdir()`, `vfs_finddir()`
- [ ] Allow mounting different FS drivers (IXFS, FAT32, initrd) at drive letters
- [ ] Drive letter assignment:
  - `A:\` — EFI System Partition (FAT32, read-only after boot)
  - `C:\` — Root OS partition (IXFS)
  - `D:\`, `E:\`… — Additional drives / USB
- [ ] Commit: `"fs: VFS with Windows-style drive letters"`

### 6.3 Initial RAM Filesystem (initrd)

- [ ] Write `src/kernel/fs/initrd.c` / `initrd.h`
- [ ] Define a simple archive format (header + flat file entries)
- [ ] Write a userspace tool `scripts/make-initrd.py` to pack files into the initrd image
- [ ] Mount initrd at `C:\` during early boot (before disk drivers are ready)
- [ ] Test: read a text file from initrd via VFS → ✅
- [ ] Commit: `"fs: initrd RAM filesystem"`

### 6.4 FAT32 Filesystem Driver (EFI Partition Only)

- [ ] Write `src/kernel/fs/fat32.c` / `fat32.h`
- [ ] Parse the **BPB** (BIOS Parameter Block) from the boot sector
- [ ] Read the **FAT** (File Allocation Table)
- [ ] Implement read-only directory listing (read cluster chains)
- [ ] Implement read-only file read (follow cluster chains → copy to buffer)
- [ ] Mount the EFI System Partition at `A:\`
- [ ] Test: list files on `A:\` from the OS → ✅
- [ ] Commit: `"fs: FAT32 read-only driver for EFI partition"`

### 6.5 IXFS — Impossible X FileSystem (Custom)

> IXFS is the native filesystem for Impossible OS, designed for the root partition (`C:\`).

- [ ] Design IXFS on-disk layout:
  - [ ] **Superblock** — magic number (`0x49584653` = "IXFS"), version, block size, total blocks, free count
  - [ ] **Block bitmap** — 1 bit per block (4 KiB blocks)
  - [ ] **Inode table** — fixed-size entries (permissions, size, timestamps, block pointers)
  - [ ] **Data blocks** — file/directory content
- [ ] Write `src/kernel/fs/ixfs.c` / `ixfs.h`
- [ ] Implement `ixfs_format()` — write superblock, zero bitmaps, create root directory
- [ ] Implement `ixfs_mount()` — read superblock, verify magic, load bitmaps
- [ ] Implement inode operations:
  - [ ] `ixfs_read_inode()` / `ixfs_write_inode()`
  - [ ] `ixfs_alloc_block()` / `ixfs_free_block()`
- [ ] Implement file operations:
  - [ ] `ixfs_create()` — create a new file
  - [ ] `ixfs_read()` — read file data
  - [ ] `ixfs_write()` — write file data
  - [ ] `ixfs_delete()` — unlink file, free blocks
- [ ] Implement directory operations:
  - [ ] `ixfs_mkdir()` — create directory
  - [ ] `ixfs_readdir()` — list directory entries
  - [ ] `ixfs_finddir()` — lookup by name
  - [ ] `ixfs_rmdir()` — remove empty directory
- [ ] Support **long filenames** (up to 255 chars)
- [ ] Support **file permissions** (owner, read/write/execute)
- [ ] Support **timestamps** (created, modified, accessed)
- [ ] Write `scripts/mkfs-ixfs.py` — host-side tool to format a disk image as IXFS
- [ ] Mount IXFS partition at `C:\`
- [ ] Test: format a QEMU disk as IXFS, create/read/write/delete files → ✅
- [ ] Commit: `"fs: IXFS custom filesystem"`

---

## Phase 7 — Multitasking & Processes

### 7.1 Kernel Threads (Cooperative)

- [ ] Write `src/kernel/sched/task.c` / `task.h`
- [ ] Define a **TCB** (Task Control Block): stack pointer, registers, state, PID
- [ ] Implement `task_create(entry_function)` — allocate stack and TCB
- [ ] Implement cooperative `yield()` — saves context, picks next task
- [ ] Write context-switch assembly (`switch_context.asm`)
- [ ] Test: two kernel threads alternately printing → ✅
- [ ] Commit: `"sched: cooperative kernel threads"`

### 7.2 Preemptive Scheduler

- [ ] Hook the PIT timer IRQ to call `schedule()` automatically
- [ ] Implement a **round-robin** scheduler with fixed time quantum
- [ ] Handle idle task (runs when no other tasks are ready)
- [ ] Test: tasks preempt each other without explicit yields → ✅
- [ ] Commit: `"sched: preemptive round-robin scheduler"`

### 7.3 User Mode

- [ ] Set up user-mode code/data segments in the GDT
- [ ] Implement `switch_to_user_mode()` using `iretq`
- [ ] Set up a **TSS** so the CPU knows where the kernel stack is on ring transitions
- [ ] Handle **system calls** via `int 0x80` or `syscall` instruction
- [ ] Implement basic syscalls: `sys_write`, `sys_read`, `sys_exit`, `sys_yield`
- [ ] Test: a user-mode program calls `sys_write` → text appears on screen → ✅
- [ ] Commit: `"kernel: user-mode transition and syscall interface"`

### 7.4 Process Management

- [ ] Implement `fork()` — duplicate address space (copy-on-write optional)
- [ ] Implement `exec()` — load an ELF binary into a process's address space
- [ ] Write a minimal **ELF loader** (`src/kernel/elf.c`)
- [ ] Implement `waitpid()` and `exit()` syscalls
- [ ] Implement process cleanup (free pages, close files, remove from scheduler)
- [ ] Commit: `"kernel: fork, exec, and ELF loader"`

---

## Phase 8 — Minimal C Library (`libc`)

- [ ] Write `src/libc/string.c` — `memcpy`, `memset`, `strlen`, `strcmp`, `strcpy`, `strncpy`
- [ ] Write `src/libc/stdlib.c` — `atoi`, `itoa`, `malloc` (wraps `sys_brk`/`sys_mmap`)
- [ ] Write `src/libc/stdio.c` — `printf`, `puts`, `getchar` (wraps syscalls)
- [ ] Write `src/libc/ctype.c` — `isalpha`, `isdigit`, `toupper`, `tolower`
- [ ] Write `src/libc/math.c` — basic integer math helpers
- [ ] Create `src/libc/crt0.asm` — C runtime entry (`_start` calls `main`, then `sys_exit`)
- [ ] Link userland programs against `libc.a`
- [ ] Commit: `"libc: minimal C standard library"`

---

## Phase 9 — Command-Line Shell

- [ ] Write `src/shell/shell.c` — main REPL loop
- [ ] Implement line editing (backspace, cursor left/right)
- [ ] Implement command parsing (split input by spaces)
- [ ] Built-in commands:
  - [ ] `help` — list available commands
  - [ ] `clear` — clear VGA screen
  - [ ] `echo <text>` — print text
  - [ ] `ls [path]` — list directory via VFS
  - [ ] `cat <file>` — print file contents
  - [ ] `mkdir <dir>` — create directory
  - [ ] `touch <file>` — create empty file
  - [ ] `rm <file>` — delete file
  - [ ] `ps` — list running processes
  - [ ] `kill <pid>` — terminate a process
  - [ ] `uptime` — show system uptime
  - [ ] `reboot` — reboot the machine (outb to `0x64`)
  - [ ] `shutdown` — ACPI power-off
- [ ] Implement command history (up/down arrow keys)
- [ ] Commit: `"shell: interactive command-line shell"`

---

## Phase 10 — Networking (Basic)

### 10.1 NIC Driver

- [ ] Write `src/kernel/drivers/rtl8139.c` — RTL8139 NIC driver (simple, well-documented)
- [ ] Detect the NIC via PCI enumeration
- [ ] Initialize the NIC (reset, enable Rx/Tx, set MAC address)
- [ ] Implement `nic_send(packet, length)` and receive via IRQ handler
- [ ] Commit: `"drivers: RTL8139 NIC driver"`

### 10.2 Network Stack

- [ ] Write `src/kernel/net/ethernet.c` — parse/build Ethernet frames
- [ ] Write `src/kernel/net/arp.c` — ARP request/reply
- [ ] Write `src/kernel/net/ip.c` — IPv4 send/receive
- [ ] Write `src/kernel/net/icmp.c` — ICMP echo (ping)
- [ ] Write `src/kernel/net/udp.c` — UDP send/receive
- [ ] Write `src/kernel/net/tcp.c` — basic TCP (SYN/ACK, data, FIN) *(stretch goal)*
- [ ] Write `src/kernel/net/dhcp.c` — obtain IP via DHCP
- [ ] Test: ping from QEMU guest → host responds → ✅
- [ ] Commit: `"net: basic IP/UDP/ICMP network stack"`

---

## Phase 11 — Graphics & Desktop Environment

### 11.1 Framebuffer Graphics

- [ ] Use the **GOP framebuffer** already provided by GRUB/Multiboot2 (already in graphics mode from boot)
- [ ] Write `src/kernel/drivers/framebuffer.c` — direct pixel access
- [ ] Implement `fb_put_pixel(x, y, color)`
- [ ] Implement `fb_fill_rect()`, `fb_draw_line()`, `fb_draw_circle()`
- [ ] Implement `fb_blit()` — fast block copy for double buffering
- [ ] Implement **double buffering** to eliminate flicker
- [ ] Commit: `"drivers: VESA framebuffer graphics"`

### 11.2 Font Rendering

- [ ] Embed a bitmap font (e.g., PSF font or custom 8×16 glyph table)
- [ ] Write `src/desktop/font.c` — `font_draw_char()`, `font_draw_string()`
- [ ] Support foreground + background colors
- [ ] *(Stretch)* Load TrueType fonts via a minimal renderer
- [ ] Commit: `"desktop: bitmap font rendering"`

### 11.3 Mouse Driver (PS/2)

- [ ] Write `src/kernel/drivers/mouse.c` / `mouse.h`
- [ ] Initialize the PS/2 mouse (enable auxiliary device, set sample rate)
- [ ] Register IRQ 12 handler
- [ ] Parse 3-byte mouse packets (dx, dy, buttons)
- [ ] Track global cursor position (clamp to screen bounds)
- [ ] Draw a mouse cursor sprite on the framebuffer
- [ ] Commit: `"drivers: PS/2 mouse driver with cursor"`

### 11.4 Window Manager (Compositor)

- [ ] Write `src/desktop/wm.c` / `wm.h`
- [ ] Define a **Window** struct: position, size, z-order, framebuffer, title, flags
- [ ] Implement `wm_create_window()`, `wm_destroy_window()`
- [ ] Implement `wm_move_window()`, `wm_resize_window()`
- [ ] Implement **window dragging** (click title bar + mouse move)
- [ ] Implement **window stacking** (z-order, bring-to-front on click)
- [ ] Implement **window decorations** — title bar, close/minimize/maximize buttons, border
- [ ] Composite all windows onto the screen each frame (painter's algorithm)
- [ ] Implement **dirty rectangle** tracking to avoid full-screen redraws
- [ ] Commit: `"desktop: basic stacking window manager"`

### 11.5 Desktop Shell

- [ ] Write `src/desktop/desktop.c` — the main desktop process
- [ ] Draw a **wallpaper** (solid color gradient or embedded BMP)
- [ ] Draw a **taskbar** at the bottom with a clock and window list
- [ ] Draw a **start menu** or app launcher button
- [ ] Implement launching apps (shell, file manager, etc.) from the desktop
- [ ] Commit: `"desktop: shell with taskbar and launcher"`

### 11.6 GUI Toolkit (Widgets)

- [ ] Write `src/desktop/widgets.c` / `widgets.h`
- [ ] Implement **Button** widget — draw, hover highlight, click callback
- [ ] Implement **Label** widget — static text
- [ ] Implement **TextBox** widget — editable single-line input
- [ ] Implement **ScrollBar** widget
- [ ] Implement basic **event routing** — mouse/keyboard events to focused widget
- [ ] Commit: `"desktop: basic GUI widget toolkit"`

---

## Phase 12 — System Services & Polish

### 12.1 Logging

- [ ] Write `src/kernel/log.c` — `log_info()`, `log_warn()`, `log_error()`
- [ ] Output logs to serial port (`0x3F8`) for host-side capture
- [ ] Add QEMU `-serial file:serial.log` to capture logs to a file
- [ ] Commit: `"kernel: serial logging subsystem"`

### 12.2 Real-Time Clock (RTC)

- [ ] Write `src/kernel/drivers/rtc.c` — read CMOS date/time
- [ ] Display clock in the taskbar
- [ ] Commit: `"drivers: CMOS RTC date/time"`

### 12.3 ACPI (Power Management)

- [ ] Parse **RSDP** → **RSDT** → **FADT**
- [ ] Implement `acpi_shutdown()` — write to PM1a_CNT register
- [ ] Implement `acpi_reboot()`
- [ ] Commit: `"kernel: ACPI power management"`

### 12.4 PCI Bus Enumeration

- [ ] Write `src/kernel/drivers/pci.c` / `pci.h`
- [ ] Enumerate all PCI devices (bus, device, function scan)
- [ ] Read vendor/device IDs, class codes, BARs
- [ ] Implement PCI config space read/write
- [ ] Commit: `"drivers: PCI bus enumeration"`

---

## Phase 13 — Versioning & Release Management

### 13.1 Semantic Versioning

- [ ] Create `VERSION` file in repo root (start at `0.1.0`)
- [ ] Embed version string into the kernel (print at boot & in `uname` command)
- [ ] Update `VERSION` at each milestone (see below)

### 13.2 Git Workflow

- [ ] Adopt branch naming: `feature/<name>`, `bugfix/<name>`, `release/<version>`
- [ ] Tag each milestone: `git tag -a v0.1.0 -m "Bootloader boots into protected mode"`
- [ ] Write `CHANGELOG.md` — document changes per version
- [ ] Push tags: `git push --tags`

### 13.3 Milestone Version Map

| Version | Milestone |
|---------|-----------|
| `0.1.0` | UEFI GRUB boots kernel, framebuffer console output |
| `0.2.0` | Kernel boots in Long Mode, interrupts working |
| `0.3.0` | Memory management (PMM + VMM + heap) |
| `0.4.0` | Keyboard input, PIT timer, basic shell |
| `0.5.0` | Filesystem (initrd + FAT32 read) |
| `0.6.0` | Multitasking (preemptive scheduler, user mode) |
| `0.7.0` | Framebuffer graphics + mouse driver |
| `0.8.0` | Window manager + desktop shell |
| `0.9.0` | Installer ISO boots and installs to disk |
| `1.0.0` | Full boot-to-desktop experience, stable in Hyper-V |

---

## Phase 14 — OS Installer

### 14.1 Installer Program

- [ ] Write `src/installer/installer.c` — runs as a special init process from the ISO
- [ ] Display a **welcome screen** (GUI or text-mode)
- [ ] **Disk selection** — list available drives (ATA enumeration)
- [ ] **Partitioning** — create a **GPT** partition table on the target disk (UEFI requires GPT)
  - [ ] Create an **EFI System Partition** (FAT32, ~512 MiB, type `EF00`) → `A:\`
  - [ ] Create a root partition (remainder of disk, IXFS) → `C:\`
- [ ] **Format** partitions:
  - [ ] Write FAT32 BPB + empty FAT for ESP
  - [ ] Run `ixfs_format()` on the root partition
- [ ] **Copy files** — copy kernel ELF, initrd, and OS files to `C:\`
- [ ] **Install GRUB for UEFI** — `grub-install --target=x86_64-efi` to the ESP
- [ ] **Finalize** — display "Installation Complete, Reboot" message
- [ ] Test in QEMU: boot ISO → install to a virtual disk → reboot from disk → OS loads → ✅
- [ ] Commit: `"installer: full OS installer"`

### 14.2 Bootable ISO Creation

- [ ] Write `scripts/make-iso.sh` — automates ISO creation
- [ ] Use `grub-mkrescue` or `xorriso` to produce `os-build.iso`
- [ ] Verify ISO boots in QEMU
- [ ] Sign the ISO with a checksum (`sha256sum os-build.iso > os-build.iso.sha256`)
- [ ] Commit: `"release: ISO build script"`

---

## Phase 15 — Hyper-V Production Testing

### 15.1 Hyper-V VM Setup

- [ ] Enable Hyper-V on Windows host (if not already)
- [ ] Open Hyper-V Manager → **New → Virtual Machine**
- [ ] Choose **Generation 2** VM
- [ ] Allocate ≥ 2 GB RAM, ≥ 1 vCPU
- [ ] Create a virtual hard disk (≥ 20 GB, VHDX)
- [ ] **Disable Secure Boot** (VM Settings → Security → uncheck)
- [ ] Attach ISO: `\\wsl.localhost\Ubuntu\home\<user>\impossible-os\build\os-build.iso`

### 15.2 Hyper-V Test Runs

- [ ] **Test 1:** ISO boots to installer without errors
- [ ] **Test 2:** Installer partitions and formats the virtual disk
- [ ] **Test 3:** Installer copies OS files and installs bootloader
- [ ] **Test 4:** VM reboots from disk → kernel loads → shell or desktop appears
- [ ] **Test 5:** Keyboard and mouse work inside Hyper-V
- [ ] **Test 6:** Filesystem operations work (create, read, delete files)
- [ ] **Test 7:** Window manager renders correctly at Hyper-V's resolution
- [ ] **Test 8:** Graceful shutdown/reboot via ACPI
- [ ] Document any Hyper-V-specific issues and fixes

### 15.3 Performance & Stability

- [ ] Run the OS for 30+ minutes without crash
- [ ] Stress-test memory allocator (allocate/free in a loop)
- [ ] Stress-test process creation (fork-bomb protection)
- [ ] Verify no memory leaks via serial log inspection
- [ ] Commit: `"test: Hyper-V validation pass"`

---

## Phase 16 — Agent Skills, Rules & Workflows

### 16.1 Agent Rules

- [ ] Create `.agents/rules.md` with OS-dev constraints:
  ```
  You are an expert low-level OS developer. You are operating strictly
  inside a sandboxed Ubuntu WSL environment. You have permission to
  create files, write C/C++ and Assembly, and use the terminal to
  compile the OS. Your final output must be an ISO file named
  os-build.iso. Do not attempt to modify host Windows configurations
  or access directories outside of this workspace.
  ```
- [ ] Add rule: always compile with `-Wall -Wextra -ffreestanding -nostdlib`
- [ ] Add rule: never run `dd` targeting `/dev/sda` or any real disk device
- [ ] Add rule: test all changes via `make run` (QEMU) before committing

### 16.2 Agent Workflows

- [ ] Create `.agents/workflows/build.md`:
  ```
  1. Run `make clean`
  2. Run `make all`
  // turbo
  3. Run `make iso`
  // turbo
  4. Run `make run` to test in QEMU
  ```
- [ ] Create `.agents/workflows/debug.md`:
  ```
  1. Run `make clean && make all CFLAGS+="-g"`
  // turbo
  2. Run `scripts/debug.sh` (QEMU + GDB)
  3. Set breakpoints and inspect state
  ```
- [ ] Create `.agents/workflows/release.md`:
  ```
  1. Update VERSION file
  2. Update CHANGELOG.md
  3. Run full build: `make clean && make all && make iso`
  4. Verify ISO in QEMU
  5. `git add -A && git commit -m "release: vX.Y.Z"`
  6. `git tag -a vX.Y.Z -m "Release vX.Y.Z"`
  7. `git push && git push --tags`
  8. Copy ISO to Hyper-V path for production test
  ```
- [ ] Create `.agents/workflows/test-hyperv.md`:
  ```
  1. Build ISO: `make clean && make iso`
  2. Verify QEMU: `make run`
  3. Instruct user to attach ISO in Hyper-V Manager:
     Path: \\wsl.localhost\Ubuntu\home\<user>\impossible-os\build\os-build.iso
  4. Document test results
  ```

### 16.3 Agent Skills

- [ ] Create `.agents/skills/cross-compiler/SKILL.md` — instructions for building the GCC cross-compiler
- [ ] Create `.agents/skills/iso-creation/SKILL.md` — step-by-step ISO packaging guide
- [ ] Create `.agents/skills/qemu-testing/SKILL.md` — QEMU flags, debugging tips, common errors
- [ ] Create `.agents/skills/uefi-boot/SKILL.md` — UEFI vs BIOS boot, OVMF setup
- [ ] Create `.agents/skills/hyperv-testing/SKILL.md` — Hyper-V Gen2 VM creation and ISO attachment

---

## Phase 17 — Documentation & Final Release

- [ ] Write comprehensive `README.md` — project overview, features, screenshots, build instructions
- [ ] Write `docs/ARCHITECTURE.md` — high-level system design, memory map, boot flow
- [ ] Write `docs/BUILDING.md` — step-by-step build guide for contributors
- [ ] Write `docs/CONTRIBUTING.md` — code style, branch workflow, PR guidelines
- [ ] Finalize `CHANGELOG.md` for v1.0.0
- [ ] Create a GitHub Release with the ISO binary attached
- [ ] *(Optional)* Set up GitHub Actions CI to auto-build the ISO on each push
- [ ] *(Optional)* Write a project blog post / announcement

---

## Phase 18 — ARM64 Port *(Optional)*

> ⚡ **Prerequisite:** Complete at least v0.5.0 (x86-64 with filesystem) before starting.
> Refactor arch-specific code into `src/arch/` **first**, then build the ARM64 equivalents.

### 18.1 Architecture Refactor (x86-64 prep)

- [ ] Move x86-64-specific code into `src/arch/x86_64/`:
  - [ ] `src/arch/x86_64/boot/` — Multiboot2 header, entry.asm, Long Mode setup
  - [ ] `src/arch/x86_64/gdt.c` / `idt.c` — GDT, IDT, TSS
  - [ ] `src/arch/x86_64/paging.c` — x86-64 page table management
  - [ ] `src/arch/x86_64/context_switch.asm` — register save/restore
  - [ ] `src/arch/x86_64/drivers/pic.c` / `pit.c` — PIC, PIT (x86-only hardware)
- [ ] Create `include/arch/` with arch-agnostic interfaces:
  - [ ] `arch_init()`, `arch_enable_interrupts()`, `arch_disable_interrupts()`
  - [ ] `arch_map_page()`, `arch_unmap_page()`
  - [ ] `arch_context_switch()`
  - [ ] `arch_timer_init()`, `arch_get_ticks()`
- [ ] Verify x86-64 still boots after refactor → ✅
- [ ] Commit: `"refactor: isolate x86-64 arch code into src/arch/x86_64/"`

### 18.2 ARM64 Toolchain

- [ ] Install ARM64 cross-compiler: `sudo apt install -y gcc-aarch64-linux-gnu`
- [ ] Install ARM64 QEMU: `sudo apt install -y qemu-system-aarch64`
- [ ] Install ARM UEFI firmware: `sudo apt install -y qemu-efi-aarch64`
- [ ] Verify: `aarch64-linux-gnu-gcc --version`
- [ ] Verify: `ls /usr/share/AAVMF/AAVMF_CODE.fd`
- [ ] Commit: `"build: ARM64 cross-compilation toolchain"`

### 18.3 ARM64 Boot (UEFI + GRUB)

- [ ] Write `src/arch/aarch64/boot/entry.S` — ARM64 kernel entry point
- [ ] Configure GRUB with `--target=arm64-efi`
- [ ] Create ARM64-specific `grub.cfg`
- [ ] Build kernel with `aarch64-linux-gnu-gcc -ffreestanding -nostdlib`
- [ ] Test: QEMU `qemu-system-aarch64 -M virt -bios AAVMF_CODE.fd -cdrom os-build-arm64.iso`
- [ ] Verify GRUB menu appears → kernel entry reached → ✅
- [ ] Commit: `"boot: ARM64 UEFI boot via GRUB"`

### 18.4 ARM64 Interrupts & Timers

- [ ] Write `src/arch/aarch64/exceptions.S` — exception vector table (EL1)
- [ ] Write `src/arch/aarch64/gic.c` — **GIC** (Generic Interrupt Controller) driver
- [ ] Implement IRQ routing through GIC
- [ ] Write `src/arch/aarch64/timer.c` — ARM **generic timer** (`CNTPCT_EL0`, `CNTP_TVAL_EL0`)
- [ ] Implement `arch_timer_init()` and `arch_get_ticks()` for ARM
- [ ] Test: timer interrupt fires at expected frequency → ✅
- [ ] Commit: `"arch/aarch64: exception vectors, GIC, and generic timer"`

### 18.5 ARM64 Paging & Memory

- [ ] Write `src/arch/aarch64/mmu.c` — ARM64 page table setup (4 KiB granule, 4-level)
- [ ] Configure **TCR_EL1** (Translation Control Register)
- [ ] Configure **MAIR_EL1** (Memory Attribute Indirection Register)
- [ ] Implement `arch_map_page()` / `arch_unmap_page()` for ARM64
- [ ] Enable MMU via **SCTLR_EL1** bit 0
- [ ] Test: paging works, kernel accesses mapped memory → ✅
- [ ] Commit: `"arch/aarch64: MMU and page table management"`

### 18.6 ARM64 Context Switch

- [ ] Write `src/arch/aarch64/context_switch.S` — save/restore `x0`–`x30`, `sp`, `elr_el1`, `spsr_el1`
- [ ] Integrate with the shared scheduler
- [ ] Test: two kernel threads preempt correctly on ARM64 → ✅
- [ ] Commit: `"arch/aarch64: context switch"`

### 18.7 ARM64 Build System

- [ ] Update `Makefile` with `ARCH=x86_64` / `ARCH=aarch64` variable
- [ ] `make ARCH=aarch64 all` — builds ARM64 kernel + ISO
- [ ] `make ARCH=aarch64 run` — launches `qemu-system-aarch64` with UEFI
- [ ] Verify shared code (VFS, shell, desktop, networking) compiles for both architectures
- [ ] Commit: `"build: multi-arch Makefile (x86_64 + aarch64)"`

### 18.8 ARM64 Testing

- [ ] QEMU `qemu-system-aarch64 -M virt` — full boot to shell → ✅
- [ ] Keyboard input works (virtio)
- [ ] Framebuffer output works (virtio-gpu)
- [ ] Filesystem operations work
- [ ] *(Stretch)* Test on Raspberry Pi 4 / 5 hardware
- [ ] Commit: `"test: ARM64 QEMU validation pass"`

---

## 🏁 Done!

When all boxes are checked:
- The OS boots from ISO in both QEMU and Hyper-V
- The installer partitions, formats, and installs to a virtual disk
- The OS reboots from the installed disk into a graphical desktop
- Windows, mouse, and keyboard all function
- The project is version-tagged, documented, and published on GitHub
- *(Optional)* ARM64 build boots in QEMU aarch64

**Impossible OS is complete.** 🎉
