# Build Toolchain

Impossible OS uses a **freestanding cross-compiler** targeting x86-64 with no
standard library. This avoids linking against Linux's glibc and ensures the
kernel binary is truly standalone.

## Cross-Compiler (x86_64-elf-gcc)

Built from source and installed to `~/opt/cross/bin/`.

| Component | Version | Source |
|-----------|---------|--------|
| GCC | 13.3.0 | `gcc-13.3.0.tar.xz` |
| Binutils | 2.42 | `binutils-2.42.tar.xz` |
| Target | `x86_64-elf` | Bare-metal ELF (no OS) |
| Prefix | `~/opt/cross/` | Added to `$PATH` in `~/.bashrc` |

### Build Steps (Reference)

```bash
# 1. Build Binutils
cd /tmp && tar xf binutils-2.42.tar.xz
mkdir build-binutils && cd build-binutils
../binutils-2.42/configure --target=x86_64-elf --prefix=$HOME/opt/cross \
    --with-sysroot --disable-nls --disable-werror
make -j$(nproc) && make install

# 2. Build GCC
cd /tmp && tar xf gcc-13.3.0.tar.xz
mkdir build-gcc && cd build-gcc
../gcc-13.3.0/configure --target=x86_64-elf --prefix=$HOME/opt/cross \
    --disable-nls --enable-languages=c --without-headers
make -j$(nproc) all-gcc all-target-libgcc
make install-gcc install-target-libgcc
```

### Verification

```bash
x86_64-elf-gcc --version
# x86_64-elf-gcc (GCC) 13.3.0

x86_64-elf-ld --version
# GNU ld (GNU Binutils) 2.42
```

## Compiler Flags

The Makefile enforces strict freestanding compilation:

```makefile
CFLAGS := -Wall -Wextra -Werror \
          -ffreestanding -nostdlib -nostdinc \
          -fno-stack-protector -fno-pie -no-pie \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
          -mcmodel=kernel -std=gnu11 -O2 -g
```

| Flag | Purpose |
|------|---------|
| `-Wall -Wextra -Werror` | Treat all warnings as errors — zero-tolerance |
| `-ffreestanding` | No hosted environment assumptions |
| `-nostdlib -nostdinc` | No standard library or include paths |
| `-fno-stack-protector` | No `__stack_chk_fail` (not available freestanding) |
| `-mno-red-zone` | Required for kernel code (interrupts clobber the red zone) |
| `-mno-mmx -mno-sse -mno-sse2` | Avoid SSE in kernel (interrupt handlers don't save XMM) |
| `-mcmodel=kernel` | Use kernel memory model (addresses above 2 GiB) |
| `-std=gnu11` | C11 with GNU extensions (inline asm, attributes) |

## System Tools

These are installed via `apt` (Ubuntu 24.04 packages):

| Tool | Version | Package | Purpose |
|------|---------|---------|---------|
| NASM | 2.16.01 | `nasm` | x86-64 assembler (Intel/NASM syntax) |
| GNU Make | 4.3 | `build-essential` | Build automation |
| GCC (host) | 13.3.0 | `build-essential` | Host tools (e.g., `make-initrd`) |
| xorriso | — | `xorriso` | ISO 9660 image creation |
| mtools | — | `mtools` | FAT filesystem manipulation for ISOs |
| grub-mkrescue | 2.12 | `grub-pc-bin grub-common` | GRUB bootable ISO creation |
| Git | 2.43.0 | `git` | Version control |

### Install All (One Command)

```bash
sudo apt update && sudo apt install -y \
    build-essential bison flex texinfo nasm \
    libgmp3-dev libmpc-dev libmpfr-dev \
    xorriso mtools grub-pc-bin grub-common \
    qemu-system-x86 ovmf git
```
