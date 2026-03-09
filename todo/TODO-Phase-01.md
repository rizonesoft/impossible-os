# Phase 01 — Kernel Core Enhancements

> **Goal:** Harden and extend the kernel with threading, IPC, better error handling,
> a hardware abstraction layer, and essential runtime services. These features
> form the foundation for everything in Phases 02–13.

---

## 1. Threading & Synchronization
> *Research: [01_threading_ipc.md](research/phase_01_kernel_core/01_threading_ipc.md)*

### 1.1 Kernel Threads

**Prompt:** This section is marked complete. Verify the implementation is correct and consistent: review `src/kernel/sched.c` and `include/task.h` to confirm `thread_t` struct has id, stack_ptr, stack_base, stack_size, state, parent_task fields, that `thread_create`, `thread_exit`, `thread_join`, `thread_yield` all exist and work, and that the scheduler iterates threads. Run `make clean && make all && make run` and test two threads sharing a global variable. Check that `docs/architecture/threading.md` exists and covers the threading model and API — create or update it if missing or incomplete. Fix any inconsistencies found in the TODO items below (wrong function names, missing commits, etc). Confirm the commit `"sched: kernel threads"` exists in git history.


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

**Prompt:** This section is marked complete. Verify the implementation is correct: review that `mutex_t` struct, `mutex_lock`, `mutex_unlock`, `mutex_trylock` exist and function correctly. Confirm the lock uses compare-and-swap with a wait queue (not busy spinning). Check that `docs/architecture/threading.md` covers the mutex API — update it if not. Run `make clean && make all && make run` and test mutexes protecting shared state between threads. Fix any inconsistencies in the TODO items below.


- [x] Define `mutex_t` struct (locked flag, owner thread)
- [x] Implement `mutex_lock(m)` — block if already locked (spinlock → sleep)
- [x] Implement `mutex_unlock(m)` — release, wake blocked thread
- [x] Implement `mutex_trylock(m)` — non-blocking attempt
- [x] Add deadlock detection (optional: lock ordering check)
- [x] Commit: `"sched: mutex synchronization"`

### 1.3 Semaphores

**Prompt:** This section is marked complete. Verify the implementation is correct: review that `semaphore_t` struct with count and wait queue exists, that `sem_wait`, `sem_signal`, `sem_init` all function correctly. Confirm `docs/architecture/threading.md` covers the semaphore API — update it if not. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


- [x] Define `semaphore_t` struct (count, wait queue)
- [x] Implement `sem_wait(s)` — decrement, block if count < 0
- [x] Implement `sem_signal(s)` — increment, wake one waiter
- [x] Implement `sem_init(s, initial_count)`
- [x] Commit: `"sched: semaphore synchronization"`

---

## 2. Inter-Process Communication (IPC)
> *Research: [01_threading_ipc.md](research/phase_01_kernel_core/01_threading_ipc.md)*

### 2.1 Pipes

**Prompt:** This section is marked complete. Verify the implementation is correct: review `pipe_t` struct (4 KiB ring buffer, read/write positions, mutex, semaphores), confirm `pipe_create`, `pipe_write`, `pipe_read`, `pipe_close` all work, and that `SYS_PIPE` syscall (number 33) is registered. Check that SIGPIPE is sent when writing to a closed pipe. Verify `docs/architecture/ipc.md` exists and covers pipes — create or update if missing. Run `make clean && make all && make run` and test shell piping (e.g., `ls | grep`). Fix any inconsistencies in the TODO items below.


- [x] Define `pipe_t` struct (4 KiB ring buffer, read/write positions, mutex, semaphores)
- [x] Implement `pipe_create(fds[2])` — allocate pipe, return read/write file descriptors
- [x] Implement `pipe_write(pipe, data, len)` — write bytes, block if full
- [x] Implement `pipe_read(pipe, buf, len)` — read bytes, block if empty
- [x] Implement `pipe_close(pipe, end)` — close one end, SIGPIPE if writing to closed pipe
- [x] Add `SYS_PIPE` syscall (number 33)
- [x] Test: shell commands piping output (e.g., `ls | grep`)
- [x] Commit: `"ipc: pipe implementation"`

### 2.2 Signals

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm signal constants (SIGKILL=9, SIGTERM=15, SIGINT=2, SIGCHLD=17), `signal_send`, `signal_handler` functions, and `SYS_SIGNAL` syscall (34) all exist. Verify SIGINT is wired from Ctrl+C in the terminal driver, SIGCHLD fires on child exit, and SIGKILL always terminates. Check that `docs/architecture/ipc.md` covers signal semantics — update if not. Run `make clean && make all && make run` and test Ctrl+C. Fix any inconsistencies in the TODO items below.


- [x] Define signal constants: `SIGKILL(9)`, `SIGTERM(15)`, `SIGINT(2)`, `SIGCHLD(17)`
- [x] Implement `signal_send(pid, sig)` — deliver signal to process
- [x] Implement `signal_handler(sig, handler)` — register user-mode handler
- [x] Handle `SIGINT` from Ctrl+C in terminal
- [x] Handle `SIGCHLD` when child process exits
- [x] Implement default handlers (SIGKILL always kills, SIGTERM clean exit)
- [x] Add `SYS_SIGNAL` syscall (number 34)
- [x] Commit: `"ipc: signal delivery"`

### 2.3 Shared Memory

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `shmem_create`, `shmem_map`, `shmem_unmap` exist with reference counting, and that `SYS_SHMEM_CREATE` (35) and `SYS_SHMEM_MAP` (36) syscalls are registered. Check that `docs/architecture/ipc.md` covers the shared memory API — update if not. Run `make clean && make all && make run` and test two processes sharing a counter. Fix any inconsistencies in the TODO items below.


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

**Prompt:** This section is marked complete (disk-backed pagefile ✅). Verify the implementation is correct: confirm swap_init, swap_out, swap_in exist, the clock page replacement algorithm works, PTEs encode swap_id when Present=0, page faults trigger swap_in, and pagefile.sys is created at `C:\Impossible\System\pagefile.sys`. Verify Codex key `System\Memory\SwapSlots` controls size. Check that `docs/architecture/swap.md` exists and covers the swap system — create or update if missing. Run `make clean && make all && make run` and verify the boot log shows swap initialization. Fix any inconsistencies in the TODO items below.


**Status: Disk-backed pagefile.** Swap out writes pages to `C:\Impossible\System\pagefile.sys`
via VFS. Swap in reads them back. Clock replacement, PTE encoding, and page fault → swap in
all work end-to-end with disk I/O. Swap size configurable via Codex `System\Memory\SwapSlots`.

- [x] Implement `swap_init(num_slots)` — initialize swap (currently RAM-backed)
- [x] Implement `swap_out(virt_addr)` — copy page to swap slot, free frame
- [x] Implement `swap_in(swap_id, virt_addr)` — read page back from swap slot
- [x] Implement Clock (second-chance) page replacement algorithm
- [x] Track swap state in page table entries (Present=0, swap_id encoded)
- [x] Handle page fault → check if page is swapped → `swap_in()` → retry
- [x] Commit: `"mm: swap / page file support"` (RAM-backed PoC)

**Disk-backed pagefile** ✅
- [x] Create pagefile: `C:\Impossible\System\pagefile.sys`
- [x] Replace `kmalloc` backing store with `vfs_write()`/`vfs_read()` to pagefile
- [x] Configurable swap size (default: 64 slots, stored in Codex `System\Memory\SwapSlots`)
- [x] Test: swap out → pagefile.sys → swap in → data integrity verified
- [x] Commit: `"mm: disk-backed swap via pagefile.sys"`

### 3.2 Memory-Mapped Files

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `mmap`, `munmap`, `msync` exist, MAP_PRIVATE and MAP_SHARED work, `SYS_MMAP` (37) and `SYS_MUNMAP` (38) syscalls are registered. Verify the eager-load implementation reads file contents into mapped pages correctly. Check that `docs/architecture/swap.md` covers mmap details — update if not. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.

> *Research: [08_mmap_files.md](research/phase_01_kernel_core/08_mmap_files.md)*

- [x] Implement `mmap(addr, length, prot, flags, fd, offset)` — map file into address space
- [x] Implement `munmap(addr, length)` — unmap region
- [x] Implement `msync(addr, length)` — flush dirty pages to disk
- [x] Support `MAP_PRIVATE` (copy-on-write) and `MAP_SHARED` (shared writes)
- [x] Page fault handler handles COW for MAP_PRIVATE writes (eager-load for file data)
- [x] Add `SYS_MMAP` (37) and `SYS_MUNMAP` (38) syscalls
- [x] Test: mmap hello.txt, read first char as pointer → 'H' ✅
- [x] Commit: `"mm: memory-mapped files"`

---

## 4. Error Handling & Kernel Panic
> *Research: [03_error_handling_panic.md](research/phase_01_kernel_core/03_error_handling_panic.md)*

### 4.1 Styled Panic Screen

**Prompt:** This section is marked complete. Verify the implementation is correct: review `panic_screen()` renders directly to framebuffer with exception name, stop code, faulting RIP, CR2, full register dump (RAX-R15, RSP, RFLAGS, CR2, CR3), and stack trace via RBP chain walking. Confirm auto-restart countdown uses Codex `System\Recovery\AutoRestart`. Check crash dump writes to `C:\Impossible\System\crashdump.log`. Verify `KPANIC(msg)` macro captures file/line. Confirm idt.c and vmm.c call `panic_screen()`. Check that `docs/architecture/error-handling.md` exists — create or update if missing. Run `make clean && make all && make run`. Fix any inconsistencies in the TODO items below.


- [x] Design graphical panic screen (Impossible OS blue, sad face, error info)
- [x] Implement `panic_screen(error_code, rip, cr2, description)` with:
  - [x] Exception name and stop code
  - [x] Faulting address and RIP
  - [x] Source file + line (via `__FILE__`, `__LINE__`)
  - [x] Register dump (RAX–R15, RSP, RFLAGS, CR2, CR3)
- [x] Implement stack trace (walk RBP chain, print return addresses)
- [x] Auto-restart countdown (configurable via Codex `System\Recovery\AutoRestart`)
- [x] Dump crash info to `C:\Impossible\System\crashdump.log` (when FS is available)
- [x] Add `KPANIC(msg)` macro that captures file/line automatically
- [x] Replaced default handlers in `idt.c` and `vmm.c` with `panic_screen()` calls
- [x] Commit: `"kernel: styled panic screen with stack trace"`

---

## 5. Hardware Abstraction Layer (HAL)
> *Research: [04_hal.md](research/phase_01_kernel_core/04_hal.md)*

### 5.1 Generic Driver Interfaces

**Prompt:** The HAL allows the kernel to work with different hardware through abstract interfaces — study how Linux's `struct block_device_operations` or `struct net_device_ops` work. Define `blk_ops` (read_sectors, write_sectors, get_capacity), `net_ops` (send_packet, get_mac), and `input_ops` (poll_event). Each real driver (AHCI in `src/kernel/drivers/ahci.c`, RTL8139 in `src/kernel/drivers/rtl8139.c`, PS/2 keyboard/mouse) registers itself by filling in these function pointers. The PCI scan should match vendor:device IDs and auto-select the right driver implementation. This abstraction is critical for Phase 08 (USB, Intel HDA) and Phase 06 (multiple block device support). After completing all items, create `docs/architecture/driver-model.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: hardware abstraction layer"`.


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

**Prompt:** The current logging uses ad-hoc `serial_printf` and `kprintf` calls with inconsistent format — replace with a unified `klog(level, subsystem, fmt, ...)` that writes to serial with prefixes like `[OK]`, `[--]`, `[!!]`, `[??]`. Add LOG_DEBUG and LOG_FATAL levels to the existing INFO/WARN/ERROR. Store the last 1000 entries in a circular in-memory ring buffer that the debug console (Phase 13 §2.1) can read. Periodic flush to files under `C:\Impossible\System\Logs\` needs VFS write access — handle the case where the filesystem isn't yet mounted during early boot by buffering. The `sys_log()` syscall lets user-mode apps write to the system log. After completing all items, create `docs/architecture/logging.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: unified logging with disk persistence"`.


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

**Prompt:** Environment variables are key-value string pairs stored per-process, inherited by children, and essential for the shell's PATH-based command lookup. Store as a flat array of `"KEY=VALUE"` strings in each `struct task` (same as POSIX environ). System-wide defaults (PATH, SYSTEMROOT, TEMP, HOME, USERNAME, COMPUTERNAME) should be populated from Codex entries at boot. The `env_expand` function replaces `%VAR%` tokens in strings — this is used by the shell for command expansion and by file associations for argument templates. The shell's command lookup must search each PATH directory in order, trying the command name with and without `.exe` extension. After completing all items, create `docs/architecture/environment.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: environment variables"`.


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

**Prompt:** A clean shutdown prevents data loss by flushing everything before powering off. The sequence must be: send WM_CLOSE to all GUI apps (giving them a chance to prompt "Save work?"), wait up to 5 seconds then force-kill remaining apps, flush all open file handles, flush the Codex registry to disk, release the DHCP lease, unmount all filesystems, sync disk caches, and finally issue the ACPI power-off. For QEMU/Bochs the shortcut is `outw(0x604, 0x2000)` but for real hardware parse the ACPI FADT for the PM1a_CNT_BLK address and write SLP_TYP | SLP_EN. Show a "Shutting down..." screen during cleanup. The `shutdown` and `reboot` shell commands should trigger this sequence. After completing all items, create `docs/architecture/shutdown.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: clean shutdown sequence"`.


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

**Prompt:** These are stretch goals. Sleep requires ACPI S3 suspend-to-RAM which involves saving all device state and entering the S3 state via the PM1a control register — on wake, the CPU resumes at the FACS waking vector. The lock screen is simpler: stop rendering the desktop, display the login/password UI overlay, and resume on correct password (check against Codex credentials from Phase 09). For QEMU testing, S3 can be simulated with `-global ICH9-LPC.disable_s3=0`. After completing all items, update `docs/architecture/shutdown.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: sleep and lock screen"`.


- [ ] *(Stretch)* — ACPI S3 suspend to RAM
- [ ] *(Stretch)* — Lock screen (password prompt, Codex credential check)

---

## 9. Dynamic Linking
> *Research: [07_dynamic_linking.md](research/phase_01_kernel_core/07_dynamic_linking.md)*

### 9.1 ELF Shared Libraries

**Prompt:** ELF shared libraries (.so) are loaded at runtime and shared between processes — study the ELF spec sections on PT_DYNAMIC, DT_NEEDED, .dynsym/.dynstr, and GOT/PLT. The dynamic linker parses the executable's DT_NEEDED list, searches the library path (app dir → `C:\Impossible\System\` → `C:\Impossible\System\Drivers\`), loads each .so into memory, applies R_X86_64_64 and R_X86_64_PC32 relocations, and patches the GOT. The `dlopen`/`dlsym`/`dlclose` API enables runtime plugin loading. This feature directly supports Phase 10's compatibility layer where Win32 DLL stubs are loaded similarly. Start by implementing symbol resolution for the current statically-linked kernel, then extend to user-mode binaries. After completing all items, create `docs/architecture/shared-libraries.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: dynamic linker (dlopen/dlsym)"`.


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

**Prompt:** Loadable kernel modules let drivers be loaded at runtime without recompiling the kernel — define .kmod as relocatable ELF objects (.o files) with exported `module_init()` and `module_cleanup()` functions. The loader reads the ELF sections, allocates kernel memory for .text/.data/.bss, applies relocations against a kernel symbol table (exported via a `EXPORT_SYMBOL` macro), and calls `module_init`. Unloading calls `module_cleanup` and frees memory. Convert the RTL8139 driver as a proof-of-concept: compile it as a separate .o, load it via `insmod rtl8139.kmod`, verify networking still works. The PCI scan at boot should be able to match vendor:device IDs to .kmod files and auto-load them. After completing all items, create `docs/architecture/kernel-modules.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: loadable kernel modules"`.


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

**Prompt:** TrueType rendering via stb_truetype.h has already been partially integrated (see the previous conversation on TrueType integration). Check if `src/libs/stb_truetype/stb_truetype_impl.c`, `include/font_mgr.h`, and `src/desktop/gfx_text.c` already exist in the codebase. If they do, focus on completing the remaining items (bundling fonts, anti-aliasing, replacing bitmap font usage). If not, port stb_truetype into `src/libs/stb_truetype/`, create math shims in `include/kmath.h` (floor, ceil, sqrt, fabs, cos, sin, acos, fmod, pow — using SSE2), and compile the implementation file with `-msse2 -mfpmath=sse`. The font manager loads .ttf files from `C:\Impossible\Fonts\` using VFS. After completing all items, update `docs/architecture/font-rendering.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: TrueType font rendering via stb_truetype"`.


- [ ] Port **stb_truetype** (single header, public domain) into `src/libs/stb_truetype/`
- [ ] Bundle **JetBrains Mono** (OFL 1.1) for terminal/code
- [ ] Bundle **Inter** (OFL 1.1) for UI text
- [ ] Create `font_ttf_render(font, codepoint, size, bitmap)` API
- [ ] Replace bitmap font in desktop/terminal with TrueType rendering
- [ ] Support font sizes: 8pt, 10pt, 12pt, 14pt, 16pt
- [ ] Add font anti-aliasing (grayscale blending)
- [ ] Commit: `"desktop: TrueType font rendering via stb_truetype"`

### 11.2 Compression (miniz)

**Prompt:** Port miniz (single-file, MIT license) into `src/libs/miniz/` — it provides zlib-compatible deflate/inflate and ZIP archive reading. Compile with `-ffreestanding -nostdlib` and redirect its `malloc`/`free` calls to `kmalloc`/`kfree` using `#define` overrides. miniz is needed by: IXFS transparent compression (Phase 06 §5.5), HTTP gzip content encoding (Phase 07), and the IPKG package format (Phase 12 §2.1). Test by compressing a known buffer, decompressing it, and comparing with the original. The ZIP API (`zip_open`, `zip_read_file`, `zip_close`) wraps miniz's `mz_zip_reader_*` functions. After completing all items, create `docs/architecture/compression.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"libs: miniz compression (deflate/ZIP)"`.


- [ ] Port **miniz** (single file, MIT) into `src/libs/miniz/`
- [ ] Implement `deflate`/`inflate` for gzip support
- [ ] Implement ZIP archive read: `zip_open()`, `zip_read_file()`, `zip_close()`
- [ ] Test: decompress a gzip file
- [ ] Test: list and extract files from a ZIP archive
- [ ] Commit: `"libs: miniz compression (deflate/ZIP)"`

### 11.3 Cryptography (monocypher)

**Prompt:** Port monocypher (BSD-2, ~3000 lines) into `src/libs/monocypher/` — it provides ChaCha20 (stream cipher), Poly1305 (MAC), Blake2b (hash), Argon2i (password hash), X25519 (key exchange), and Ed25519 (signatures). Compile with `-ffreestanding`. This library is the cryptographic foundation for: password hashing in Phase 09 §1.2, TLS in Phase 07 §5, executable signing in Phase 09 §8, and data encryption in Phase 09 §4. The kernel CSPRNG should seed from RDRAND (via inline assembly) and use ChaCha20 to generate random bytes, exposed as a `/dev/random` equivalent. After completing all items, create `docs/architecture/crypto.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"libs: monocypher crypto primitives"`.


- [ ] Port **monocypher** (3000 lines, BSD-2) into `src/libs/monocypher/`
- [ ] Expose: ChaCha20 (stream cipher), Blake2b (hash), Argon2 (password hash)
- [ ] Expose: X25519 (key exchange), Ed25519 (signatures)
- [ ] Implement kernel CSPRNG (`/dev/random` equivalent using RDRAND + ChaCha20)
- [ ] Commit: `"libs: monocypher crypto primitives"`

### 11.4 Math Library

**Prompt:** Port essential floating-point math functions from OpenLibm or musl libc (both MIT-compatible) into `src/libs/math/`. These functions are required by stb_truetype (§11.1), any audio synthesis (Phase 08), and the TTS engine (Phase 13 §7.1). Key functions: sin, cos, tan, asin, acos, atan, atan2, exp, log, log10, pow, sqrt, fabs, ceil, floor, fmod, round. If `kmath.h` already exists from the TrueType integration, extend it rather than duplicating. Compile with `-msse2` as these need floating-point hardware. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"libc: floating-point math functions"`.


- [ ] Port `sin`, `cos`, `tan` from **OpenLibm** or **musl** (MIT)
- [ ] Port `exp`, `log`, `log10`
- [ ] Port `pow`, `sqrt`, `fabs`, `ceil`, `floor`
- [ ] Port `fmod`, `atan2`
- [ ] Commit: `"libc: floating-point math functions"`

### 11.5 JSON Parser (cJSON)

**Prompt:** Port cJSON (MIT, ~2000 lines) into `src/libs/cjson/` — it provides a simple DOM-style JSON parser and serializer. Redirect `malloc`/`free` to `kmalloc`/`kfree` via `cJSON_InitHooks()`. cJSON is used for: the update manifest (Phase 12 §1.1), theme definitions (Phase 04 desktop theming), NTP server lists, and any future REST API interaction. Test by parsing a JSON string like `{"version": "1.0", "name": "Impossible OS"}`, extracting values with `cJSON_GetObjectItem`, and verifying correctness. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"libs: cJSON parser"`.


- [ ] Port **cJSON** (MIT, ~2000 lines) into `src/libs/cjson/`
- [ ] Test: parse a JSON config file
- [ ] Use for: settings files, asset manifests, theme definitions
- [ ] Commit: `"libs: cJSON parser"`

### 11.6 TCP (lwIP or Custom)

**Prompt:** TCP is the transport layer for HTTP, TLS, SSH, FTP, and virtually all internet protocols in Phases 07 and 11. The existing network stack in `src/kernel/net/` already has Ethernet, ARP, IPv4, ICMP, and UDP — study those files to understand the packet flow. TCP adds: a full connection state machine (LISTEN→SYN_SENT→SYN_RCVD→ESTABLISHED→FIN_WAIT→CLOSE_WAIT→etc.), 32-bit sequence/ack numbers, checksums (pseudo-header + TCP header + payload), retransmission timers, and sliding window flow control. lwIP (~30K lines, BSD-3) is battle-tested but large — evaluate whether its complexity is justified vs. a minimal custom implementation that only needs client-side TCP (no server listen initially). Test by making a raw HTTP GET request to an httpbin endpoint through QEMU's user networking. After completing all items, create `docs/architecture/tcp.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"net: TCP implementation"`.


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

**Prompt:** File descriptors are the POSIX abstraction that unifies files, pipes, sockets, and devices behind integer handles. Each process gets an FD table (array of pointers to `struct file`). `struct file` has a type enum (FILE, PIPE, SOCKET, DEVICE), a VFS node pointer, a read/write offset, and type-specific callbacks for read/write/close. FDs 0/1/2 are stdin/stdout/stderr (wired to the terminal). `dup`/`dup2` enables I/O redirection (`cmd > file`). This is a prerequisite for pipes (§2.1) and the shell's I/O redirection. Study the existing VFS in `src/kernel/vfs.c` to understand how `vfs_open`/`vfs_read`/`vfs_write` work, then wrap them in the FD layer. After completing all items, create `docs/architecture/file-descriptors.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: file descriptor table"`.


- [ ] Implement per-process file descriptor table (fd 0=stdin, 1=stdout, 2=stderr)
- [ ] `open(path, flags)` → returns fd
- [ ] `read(fd, buf, size)` / `write(fd, buf, size)` — dispatch to VFS, pipe, or device
- [ ] `close(fd)` — release fd slot
- [ ] `dup(fd)` / `dup2(old, new)` — duplicate file descriptors
- [ ] Required for pipes, I/O redirection, and proper Unix-style process model
- [ ] Commit: `"kernel: file descriptor table"`

### 12.2 Process Working Directory

**Prompt:** Every process needs a current working directory (CWD) stored as an absolute path string in `struct task`. All relative paths in VFS calls must be resolved against CWD by prepending it. The shell's `cd` command calls `chdir()` which validates the path exists (via VFS) then updates the task's CWD string. `getcwd()` returns the current string. Children inherit the parent's CWD on fork/exec. The shell prompt should display the CWD. This is a small but essential feature — without it, every path must be absolute. After completing all items, update `docs/architecture/file-descriptors.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: per-process working directory"`.


- [ ] Add `cwd` field to `struct task` (default: `C:\`)
- [ ] Implement `chdir(path)` syscall
- [ ] Implement `getcwd(buf, size)` syscall
- [ ] Support relative paths in VFS (resolve against cwd)
- [ ] Update shell `cd` command to use `chdir()`
- [ ] Commit: `"kernel: per-process working directory"`

### 12.3 Proper `brk`/`sbrk` Heap for User Programs

**Prompt:** User-mode programs need their own heap separate from the kernel heap. `brk(addr)` sets the program break (top of data segment), `sbrk(increment)` extends it by N bytes. The kernel maps new pages on demand as the break increases. User-mode `malloc` implementations (dlmalloc, musl's allocator) call sbrk internally, so once this works, any standard allocator can be ported. The program break starts at the end of the BSS section (read from the ELF loader). This is needed before any non-trivial user programs can run. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: brk/sbrk heap for user processes"`.


- [ ] Implement per-process heap via `brk`/`sbrk` syscalls
- [ ] Replace bump allocator in user libc with `sbrk`-backed `malloc`/`free`
- [ ] Implement `free()` with a proper free-list in user space
- [ ] Commit: `"kernel: brk/sbrk heap for user processes"`

### 12.4 Timer API for User Programs

**Prompt:** User programs need `sleep(seconds)` and `usleep(microseconds)` for delays, and `gettimeofday()` for wall-clock time. `sleep` adds the calling thread to a timer queue sorted by wake time, then yields — the PIT IRQ handler checks the queue each tick and wakes expired entries. `gettimeofday` combines the RTC (for calendar time) with the PIT tick counter (for sub-second precision). These syscalls are needed by the compositor's vsync timing, animation loops in GUI apps, and the NTP client (Phase 03 §4.4). After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: user-mode timer syscalls"`.


- [ ] Implement `sleep(seconds)` syscall (block task, wake on timer)
- [ ] Implement `usleep(microseconds)` syscall
- [ ] Implement `gettimeofday()` syscall (RTC + PIT sub-second precision)
- [ ] Commit: `"kernel: user-mode timer syscalls"`

### 12.5 Kernel Memory Leak Detection (Debug Build)

**Prompt:** This is a debug-only feature enabled by `-DKMALLOC_DEBUG` with zero overhead in release builds. Wrap `kmalloc` and `kfree` to record each allocation: caller address (via `__builtin_return_address(0)`), size, file, and line. Store records in a simple linked list. On shutdown or via a `memleak` debug command, dump all un-freed allocations showing where they were allocated. Also add `kmalloc_stats()` returning current usage, peak usage, and total allocation count — useful for the Task Manager (Phase 05 §8.1) and debug console (Phase 13 §2.2). After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"mm: kmalloc leak detector (debug build)"`.


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
