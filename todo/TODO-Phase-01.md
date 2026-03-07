# Phase 01 — Kernel Core Enhancements

> **Goal:** Harden and extend the kernel with threading, IPC, better error handling,
> a hardware abstraction layer, and essential runtime services. These features
> form the foundation for everything in Phases 02–13.

---

## 1. Threading & Synchronization
> *Research: [01_threading_ipc.md](research/phase_01_kernel_core/01_threading_ipc.md)*

### 1.1 Kernel Threads

- [x] Define `thread_t` struct (id, stack_ptr, stack_base, stack_size, state, parent_task)
- [x] Implement `thread_create(entry, arg, stack_size)` — allocate stack, init context
- [x] Implement `thread_exit(status)` — clean up, notify joiners
- [x] Implement `thread_join(thread)` — block until target thread exits
- [x] Implement `thread_yield()` — voluntary context switch to next thread
- [x] Add per-task thread list (linked list of `thread_t` within `struct task`)
- [x] Update scheduler to schedule threads (not just tasks)
- [x] Test: two threads in one process sharing globals
- [x] Commit: `"sched: kernel threads"`

### 1.2 Mutexes

- [x] Define `mutex_t` struct (locked flag, owner thread)
- [x] Implement `mutex_lock(m)` — block if already locked (spinlock → sleep)
- [x] Implement `mutex_unlock(m)` — release, wake blocked thread
- [x] Implement `mutex_trylock(m)` — non-blocking attempt
- [x] Add deadlock detection (optional: lock ordering check)
- [x] Commit: `"sched: mutex synchronization"`

### 1.3 Semaphores

- [x] Define `semaphore_t` struct (count, wait queue)
- [x] Implement `sem_wait(s)` — decrement, block if count < 0
- [x] Implement `sem_signal(s)` — increment, wake one waiter
- [x] Implement `sem_init(s, initial_count)`
- [x] Commit: `"sched: semaphore synchronization"`

---

## 2. Inter-Process Communication (IPC)
> *Research: [01_threading_ipc.md](research/phase_01_kernel_core/01_threading_ipc.md)*

### 2.1 Pipes

- [x] Define `pipe_t` struct (4 KiB ring buffer, read/write positions, mutex, semaphores)
- [x] Implement `pipe_create(fds[2])` — allocate pipe, return read/write file descriptors
- [x] Implement `pipe_write(pipe, data, len)` — write bytes, block if full
- [x] Implement `pipe_read(pipe, buf, len)` — read bytes, block if empty
- [x] Implement `pipe_close(pipe, end)` — close one end, SIGPIPE if writing to closed pipe
- [x] Add `SYS_PIPE` syscall (number 33)
- [x] Test: shell commands piping output (e.g., `ls | grep`)
- [x] Commit: `"ipc: pipe implementation"`

### 2.2 Signals

- [x] Define signal constants: `SIGKILL(9)`, `SIGTERM(15)`, `SIGINT(2)`, `SIGCHLD(17)`
- [x] Implement `signal_send(pid, sig)` — deliver signal to process
- [x] Implement `signal_handler(sig, handler)` — register user-mode handler
- [x] Handle `SIGINT` from Ctrl+C in terminal
- [x] Handle `SIGCHLD` when child process exits
- [x] Implement default handlers (SIGKILL always kills, SIGTERM clean exit)
- [x] Add `SYS_SIGNAL` syscall (number 34)
- [x] Commit: `"ipc: signal delivery"`

### 2.3 Shared Memory

- [x] Implement `shmem_create(name, size)` — allocate named shared memory region
- [x] Implement `shmem_map(id)` — map shared region into calling process's address space
- [x] Implement `shmem_unmap(id)` — unmap from process
- [x] Reference counting — free when last process unmaps
- [x] Add `SYS_SHMEM_CREATE` (35) and `SYS_SHMEM_MAP` (36) syscalls
- [x] Test: two processes sharing a counter via shared memory
- [x] Commit: `"ipc: named shared memory"`

---

## 3. Virtual Memory Enhancements
> *Research: [02_virtual_memory_swap.md](research/phase_01_kernel_core/02_virtual_memory_swap.md)*

### 3.1 Swap / Page File

- [x] Create swap file: `C:\Impossible\System\pagefile.sys`
- [x] Implement `swap_init(path, size)` — initialize swap file
- [x] Implement `swap_out(phys_frame)` — write page to swap, free frame
- [x] Implement `swap_in(swap_id, phys_frame)` — read page back from swap
- [x] Implement LRU or Clock page replacement algorithm
- [x] Track swap state in page table entries (Present=0, swap_id encoded)
- [x] Handle page fault → check if page is swapped → `swap_in()` → retry
- [x] Configurable swap size (default: 1× RAM, stored in Codex)
- [x] Test: allocate more memory than physical RAM → swap kicks in
- [x] Commit: `"mm: swap / page file support"`

### 3.2 Memory-Mapped Files
> *Research: [08_mmap_files.md](research/phase_01_kernel_core/08_mmap_files.md)*

- [ ] Implement `mmap(addr, length, prot, flags, fd, offset)` — map file into address space
- [ ] Implement `munmap(addr, length)` — unmap region
- [ ] Implement `msync(addr, length)` — flush dirty pages to disk
- [ ] Support `MAP_PRIVATE` (copy-on-write) and `MAP_SHARED` (shared writes)
- [ ] Update page fault handler to load pages on demand from the mapped file
- [ ] Add `SYS_MMAP` and `SYS_MUNMAP` syscalls
- [ ] Test: mmap a text file, read its contents as a pointer
- [ ] Commit: `"mm: memory-mapped files"`

---

## 4. Error Handling & Kernel Panic
> *Research: [03_error_handling_panic.md](research/phase_01_kernel_core/03_error_handling_panic.md)*

### 4.1 Styled Panic Screen

- [ ] Design graphical panic screen (Impossible OS blue, sad face, error info)
- [ ] Implement `panic_screen(error_code, rip, cr2, description)` with:
  - [ ] Exception name and stop code
  - [ ] Faulting address and RIP
  - [ ] Source file + line (via `__FILE__`, `__LINE__`)
  - [ ] Register dump (RAX–R15, RSP, RFLAGS, CR2, CR3)
- [ ] Implement stack trace (walk RBP chain, print return addresses)
- [ ] Auto-restart countdown (configurable via Codex `System\Recovery\AutoRestart`)
- [ ] Dump crash info to `C:\Impossible\System\crashdump.log` (when FS is available)
- [ ] Add `KPANIC(msg)` macro that captures file/line automatically
- [ ] Commit: `"kernel: styled panic screen with stack trace"`

---

## 5. Hardware Abstraction Layer (HAL)
> *Research: [04_hal.md](research/phase_01_kernel_core/04_hal.md)*

### 5.1 Generic Driver Interfaces

- [ ] Define `blk_ops` interface: `read(dev, lba, count, buf)`, `write(...)`, `capacity(dev)`
- [ ] Define `net_ops` interface: `send(dev, data, len)`, `set_mac(dev, mac)`, `get_mac(dev)`
- [ ] Define `input_ops` interface: `poll(dev)`, `get_event(dev, event)`
- [ ] Register current ATA driver as a `blk_ops` implementation
- [ ] Register current RTL8139 driver as a `net_ops` implementation
- [ ] Register current keyboard/mouse as `input_ops` implementations
- [ ] Implement `hal_register_driver(type, ops)` and `hal_get_driver(type)`
- [ ] PCI scan → match vendor:device → auto-select driver
- [ ] Commit: `"kernel: hardware abstraction layer"`

---

## 6. System Logging Enhancements
> *Research: [06_system_logs.md](research/phase_01_kernel_core/06_system_logs.md)*

### 6.1 Unified Logging System

- [ ] Add `LOG_DEBUG` and `LOG_FATAL` levels (currently: INFO, WARN, ERROR)
- [ ] Add source/subsystem tag to all log calls (e.g., `"net"`, `"fs"`, `"mm"`)
- [ ] Implement in-memory ring buffer (last 1000 log entries)
- [ ] Implement periodic flush to disk:
  - [ ] `C:\Impossible\System\Logs\kernel.log`
  - [ ] `C:\Impossible\System\Logs\network.log`
  - [ ] `C:\Impossible\System\Logs\boot.log`
- [ ] Add `sys_log()` syscall for user-mode apps to log
- [ ] Commit: `"kernel: unified logging with disk persistence"`

---

## 7. Environment Variables
> *Research: [09_environment_variables.md](research/phase_01_kernel_core/09_environment_variables.md)*

### 7.1 System & User Environment

- [ ] Implement `env_get(name)` — look up variable by name
- [ ] Implement `env_set(name, value)` — set/create variable
- [ ] Implement `env_expand(input, output, max)` — expand `%VAR%` references
- [ ] Define default variables:
  - [ ] `PATH` = `C:\Impossible\Bin;C:\Programs\`
  - [ ] `SYSTEMROOT` = `C:\Impossible\`
  - [ ] `TEMP` = `C:\Temp\`
  - [ ] `HOME` = `C:\Users\{name}\`
  - [ ] `USERNAME` = `Default`
  - [ ] `COMPUTERNAME` = `IMPOSSIBLE-PC`
- [ ] Per-process environment (inherited on fork, replaceable on exec)
- [ ] Add `SYS_GETENV` and `SYS_SETENV` syscalls
- [ ] Update shell to use `PATH` for command lookup
- [ ] Commit: `"kernel: environment variables"`

---

## 8. Power Management Enhancements
> *Research: [11_power_management.md](research/phase_01_kernel_core/11_power_management.md)*

### 8.1 Clean Shutdown Sequence

- [ ] Before ACPI shutdown:
  - [ ] Send `WM_CLOSE` to all GUI apps (save work prompt)
  - [ ] Flush all open file handles
  - [ ] Flush Codex registry to disk
  - [ ] Stop network services (release DHCP lease)
  - [ ] Unmount all filesystems
  - [ ] Sync disk caches
- [ ] Display "Shutting down..." screen during cleanup
- [ ] Implement shutdown timeout (force-kill apps after 5 seconds)
- [ ] Commit: `"kernel: clean shutdown sequence"`

### 8.2 Sleep / Lock Screen (Future)

- [ ] *(Stretch)* — ACPI S3 suspend to RAM
- [ ] *(Stretch)* — Lock screen (password prompt, Codex credential check)

---

## 9. Dynamic Linking
> *Research: [07_dynamic_linking.md](research/phase_01_kernel_core/07_dynamic_linking.md)*

### 9.1 ELF Shared Libraries

- [ ] Implement ELF `.symtab`/`.dynsym` symbol table scanner
- [ ] Implement ELF relocations (`R_X86_64_*` types)
- [ ] Implement `dlopen(path)` — load `.so` into memory, relocate
- [ ] Implement `dlsym(handle, symbol)` — find exported function
- [ ] Implement `dlclose(handle)` — unload, free memory
- [ ] Define library search path: app dir → `C:\Impossible\System\` → `C:\Impossible\System\Drivers\`
- [ ] Test: load a shared library, call an exported function
- [ ] Commit: `"kernel: dynamic linker (dlopen/dlsym)"`

---

## 10. Kernel Modules
> *Research: [05_kernel_modules.md](research/phase_01_kernel_core/05_kernel_modules.md)*

### 10.1 Loadable Kernel Modules (.kmod)

- [ ] Define `.kmod` format (ELF relocatable objects with `init()`/`cleanup()`)
- [ ] Implement `kmod_load(path)` — load ELF, relocate, call `init()`
- [ ] Implement `kmod_unload(name)` — call `cleanup()`, free memory
- [ ] Implement `kmod_list(out, max)` — list loaded modules
- [ ] Convert one existing driver (e.g., RTL8139) to a loadable module
- [ ] Load modules based on PCI scan results at boot
- [ ] Commit: `"kernel: loadable kernel modules"`

---

## 11. General Utilities & Libraries
> *Research: [10_general_utilities.md](research/phase_01_kernel_core/10_general_utilities.md)*

### 11.1 TrueType Font Rendering

- [ ] Port **stb_truetype** (single header, public domain) into `src/libs/stb_truetype/`
- [ ] Bundle **JetBrains Mono** (OFL 1.1) for terminal/code
- [ ] Bundle **Inter** (OFL 1.1) for UI text
- [ ] Create `font_ttf_render(font, codepoint, size, bitmap)` API
- [ ] Replace bitmap font in desktop/terminal with TrueType rendering
- [ ] Support font sizes: 8pt, 10pt, 12pt, 14pt, 16pt
- [ ] Add font anti-aliasing (grayscale blending)
- [ ] Commit: `"desktop: TrueType font rendering via stb_truetype"`

### 11.2 Compression (miniz)

- [ ] Port **miniz** (single file, MIT) into `src/libs/miniz/`
- [ ] Implement `deflate`/`inflate` for gzip support
- [ ] Implement ZIP archive read: `zip_open()`, `zip_read_file()`, `zip_close()`
- [ ] Test: decompress a gzip file
- [ ] Test: list and extract files from a ZIP archive
- [ ] Commit: `"libs: miniz compression (deflate/ZIP)"`

### 11.3 Cryptography (monocypher)

- [ ] Port **monocypher** (3000 lines, BSD-2) into `src/libs/monocypher/`
- [ ] Expose: ChaCha20 (stream cipher), Blake2b (hash), Argon2 (password hash)
- [ ] Expose: X25519 (key exchange), Ed25519 (signatures)
- [ ] Implement kernel CSPRNG (`/dev/random` equivalent using RDRAND + ChaCha20)
- [ ] Commit: `"libs: monocypher crypto primitives"`

### 11.4 Math Library

- [ ] Port `sin`, `cos`, `tan` from **OpenLibm** or **musl** (MIT)
- [ ] Port `exp`, `log`, `log10`
- [ ] Port `pow`, `sqrt`, `fabs`, `ceil`, `floor`
- [ ] Port `fmod`, `atan2`
- [ ] Commit: `"libc: floating-point math functions"`

### 11.5 JSON Parser (cJSON)

- [ ] Port **cJSON** (MIT, ~2000 lines) into `src/libs/cjson/`
- [ ] Test: parse a JSON config file
- [ ] Use for: settings files, asset manifests, theme definitions
- [ ] Commit: `"libs: cJSON parser"`

### 11.6 TCP (lwIP or Custom)

- [ ] Evaluate **lwIP** (BSD-3, ~30K lines) vs. custom minimal TCP
- [ ] Implement TCP SYN/SYN-ACK/ACK handshake
- [ ] Implement TCP data send/receive with sequence numbers
- [ ] Implement TCP FIN/ACK connection teardown
- [ ] Implement TCP retransmission (basic timeout)
- [ ] Implement TCP sliding window (basic flow control)
- [ ] Test: TCP echo server, HTTP GET request
- [ ] Commit: `"net: TCP implementation"`

---

## 12. Agent-Recommended Additions

> Items not in the research files but important for a robust OS.

### 12.1 File Descriptors & I/O Abstraction

- [ ] Implement per-process file descriptor table (fd 0=stdin, 1=stdout, 2=stderr)
- [ ] `open(path, flags)` → returns fd
- [ ] `read(fd, buf, size)` / `write(fd, buf, size)` — dispatch to VFS, pipe, or device
- [ ] `close(fd)` — release fd slot
- [ ] `dup(fd)` / `dup2(old, new)` — duplicate file descriptors
- [ ] Required for pipes, I/O redirection, and proper Unix-style process model
- [ ] Commit: `"kernel: file descriptor table"`

### 12.2 Process Working Directory

- [ ] Add `cwd` field to `struct task` (default: `C:\`)
- [ ] Implement `chdir(path)` syscall
- [ ] Implement `getcwd(buf, size)` syscall
- [ ] Support relative paths in VFS (resolve against cwd)
- [ ] Update shell `cd` command to use `chdir()`
- [ ] Commit: `"kernel: per-process working directory"`

### 12.3 Proper `brk`/`sbrk` Heap for User Programs

- [ ] Implement per-process heap via `brk`/`sbrk` syscalls
- [ ] Replace bump allocator in user libc with `sbrk`-backed `malloc`/`free`
- [ ] Implement `free()` with a proper free-list in user space
- [ ] Commit: `"kernel: brk/sbrk heap for user processes"`

### 12.4 Timer API for User Programs

- [ ] Implement `sleep(seconds)` syscall (block task, wake on timer)
- [ ] Implement `usleep(microseconds)` syscall
- [ ] Implement `gettimeofday()` syscall (RTC + PIT sub-second precision)
- [ ] Commit: `"kernel: user-mode timer syscalls"`

### 12.5 Kernel Memory Leak Detection (Debug Build)

- [ ] Track all `kmalloc()` calls with caller file/line (`__FILE__`, `__LINE__`)
- [ ] On shutdown, print list of unfreed allocations
- [ ] Add `kmalloc_stats()` — current usage, peak usage, allocation count
- [ ] Only active with `-DKMALLOC_DEBUG` (zero overhead in release)
- [ ] Commit: `"mm: kmalloc leak detector (debug build)"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 1. Threading & Sync | Prerequisite for GUI apps, IPC, everything |
| 🔴 P0 | 2.1 Pipes | Shell pipes, process communication |
| 🔴 P0 | 12.1 File Descriptors | Required by pipes, I/O redirection |
| 🟠 P1 | 4. Panic Screen | User-visible crash recovery |
| 🟠 P1 | 7. Environment Variables | Shell PATH, app config |
| 🟠 P1 | 8.1 Clean Shutdown | Data integrity |
| 🟠 P1 | 12.2 Working Directory | Shell `cd`, relative paths |
| 🟡 P2 | 2.2 Signals | Ctrl+C, child notification |
| 🟡 P2 | 6. Logging Enhancements | Debugging, disk persistence |
| 🟡 P2 | 11.1 TrueType Fonts | Visual quality upgrade |
| 🟡 P2 | 11.6 TCP | HTTP, real networking |
| 🟢 P3 | 2.3 Shared Memory | Advanced IPC |
| 🟢 P3 | 3.1 Swap | Run more apps than RAM |
| 🟢 P3 | 3.2 Memory-Mapped Files | Fast I/O, shared memory |
| 🟢 P3 | 5. HAL | Multi-driver support |
| 🟢 P3 | 9. Dynamic Linking | Shared libraries |
| 🟢 P3 | 10. Kernel Modules | Runtime driver loading |
| 🟢 P3 | 11.2–11.5 Libraries | Compression, crypto, math, JSON |
| 🔵 P4 | 12.3 brk/sbrk | Better user heap |
| 🔵 P4 | 12.4 Timer API | User-mode sleep/timing |
| 🔵 P4 | 12.5 Leak Detection | Debug tooling |
