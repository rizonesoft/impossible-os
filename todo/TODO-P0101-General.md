# P0101 — Kernel Core Enhancements

> **Goal:** Harden and extend the kernel with threading, IPC, better error handling,
> a hardware abstraction layer, and essential runtime services. These features
> form the foundation for everything in Phases 02–13.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.


---

## 1. Threading & Synchronization

### 1.1 Kernel Threads

**Prompt:** This section is marked complete. Verify the implementation is correct and consistent: review `src/kernel/sched.c` and `include/task.h` to confirm `thread_t` struct has id, stack_ptr, stack_base, stack_size, state, parent_task fields, that `thread_create`, `thread_exit`, `thread_join`, `thread_yield` all exist and work, and that the scheduler iterates threads. Run `bash scripts/build.sh clean` and test two threads sharing a global variable. Fix any inconsistencies found in the TODO items below. Confirm the commit `"sched: kernel threads"` exists in git history. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to threading. Create or update documentation in `docs/` covering the threading model, thread API, and scheduler architecture.


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

**Prompt:** This section is marked complete. Verify the implementation is correct: review that `mutex_t` struct, `mutex_lock`, `mutex_unlock`, `mutex_trylock` exist and function correctly. Confirm the lock uses compare-and-swap with a wait queue (not busy spinning). Run `bash scripts/build.sh clean` and test mutexes protecting shared state between threads. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to synchronization primitives. Create or update documentation in `docs/` covering the mutex API and implementation details.


- [x] Define `mutex_t` struct (locked flag, owner thread)
- [x] Implement `mutex_lock(m)` — block if already locked (spinlock → sleep)
- [x] Implement `mutex_unlock(m)` — release, wake blocked thread
- [x] Implement `mutex_trylock(m)` — non-blocking attempt
- [x] Add deadlock detection (optional: lock ordering check)
- [x] Commit: `"sched: mutex synchronization"`

### 1.3 Semaphores

**Prompt:** This section is marked complete. Verify the implementation is correct: review that `semaphore_t` struct with count and wait queue exists, that `sem_wait`, `sem_signal`, `sem_init` all function correctly. Run `bash scripts/build.sh clean`. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to semaphores. Create or update documentation in `docs/` covering the semaphore API and usage patterns.


- [x] Define `semaphore_t` struct (count, wait queue)
- [x] Implement `sem_wait(s)` — decrement, block if count < 0
- [x] Implement `sem_signal(s)` — increment, wake one waiter
- [x] Implement `sem_init(s, initial_count)`
- [x] Commit: `"sched: semaphore synchronization"`

---

## 2. Inter-Process Communication (IPC)

### 2.1 Pipes

**Prompt:** This section is marked complete. Verify the implementation is correct: review `pipe_t` struct (4 KiB ring buffer, read/write positions, mutex, semaphores), confirm `pipe_create`, `pipe_write`, `pipe_read`, `pipe_close` all work, and that `SYS_PIPE` syscall (number 33) is registered. Check that SIGPIPE is sent when writing to a closed pipe. Run `bash scripts/build.sh clean` and test shell piping (e.g., `ls | grep`). Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to IPC or pipes. Create or update documentation in `docs/` covering the pipe implementation, syscalls, and SIGPIPE behavior.


- [x] Define `pipe_t` struct (4 KiB ring buffer, read/write positions, mutex, semaphores)
- [x] Implement `pipe_create(fds[2])` — allocate pipe, return read/write file descriptors
- [x] Implement `pipe_write(pipe, data, len)` — write bytes, block if full
- [x] Implement `pipe_read(pipe, buf, len)` — read bytes, block if empty
- [x] Implement `pipe_close(pipe, end)` — close one end, SIGPIPE if writing to closed pipe
- [x] Add `SYS_PIPE` syscall (number 33)
- [x] Test: shell commands piping output (e.g., `ls | grep`)
- [x] Commit: `"ipc: pipe implementation"`

### 2.2 Signals

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm signal constants (SIGKILL=9, SIGTERM=15, SIGINT=2, SIGCHLD=17), `signal_send`, `signal_handler` functions, and `SYS_SIGNAL` syscall (34) all exist. Verify SIGINT is wired from Ctrl+C in the terminal driver, SIGCHLD fires on child exit, and SIGKILL always terminates. Run `bash scripts/build.sh clean` and test Ctrl+C. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to signal handling. Create or update documentation in `docs/` covering signal constants, delivery semantics, and handler registration.


- [x] Define signal constants: `SIGKILL(9)`, `SIGTERM(15)`, `SIGINT(2)`, `SIGCHLD(17)`
- [x] Implement `signal_send(pid, sig)` — deliver signal to process
- [x] Implement `signal_handler(sig, handler)` — register user-mode handler
- [x] Handle `SIGINT` from Ctrl+C in terminal
- [x] Handle `SIGCHLD` when child process exits
- [x] Implement default handlers (SIGKILL always kills, SIGTERM clean exit)
- [x] Add `SYS_SIGNAL` syscall (number 34)
- [x] Commit: `"ipc: signal delivery"`

### 2.3 Shared Memory

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `shmem_create`, `shmem_map`, `shmem_unmap` exist with reference counting, and that `SYS_SHMEM_CREATE` (35) and `SYS_SHMEM_MAP` (36) syscalls are registered. Run `bash scripts/build.sh clean` and test two processes sharing a counter. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to shared memory. Create or update documentation in `docs/` covering named shared memory regions, syscalls, and reference counting.


- [x] Implement `shmem_create(name, size)` — allocate named shared memory region
- [x] Implement `shmem_map(id)` — map shared region into calling process's address space
- [x] Implement `shmem_unmap(id)` — unmap from process
- [x] Reference counting — free when last process unmaps
- [x] Add `SYS_SHMEM_CREATE` (35) and `SYS_SHMEM_MAP` (36) syscalls
- [x] Test: two processes sharing a counter via shared memory
- [x] Commit: `"ipc: named shared memory"`

---

## 3. Virtual Memory Enhancements

### 3.1 Swap / Page File

**Prompt:** This section is marked complete (disk-backed pagefile ✅). Verify the implementation is correct: confirm swap_init, swap_out, swap_in exist, the clock page replacement algorithm works, PTEs encode swap_id when Present=0, page faults trigger swap_in, and pagefile.sys is created at `C:\Impossible\System\pagefile.sys`. Verify Registry key `HKLM\SYSTEM\Memory\SwapSlots` controls size. Run `bash scripts/build.sh clean` and verify the boot log shows swap initialization. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to virtual memory or swap. Create or update documentation in `docs/` covering the swap system, pagefile.sys, clock replacement, and PTE encoding.


**Status: Disk-backed pagefile.** Swap out writes pages to `C:\Impossible\System\pagefile.sys`
via VFS. Swap in reads them back. Clock replacement, PTE encoding, and page fault → swap in
all work end-to-end with disk I/O. Swap size configurable via Registry `HKLM\SYSTEM\Memory\SwapSlots`.

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
- [x] Configurable swap size (default: 64 slots, stored in Registry `HKLM\SYSTEM\Memory\SwapSlots`)
- [x] Test: swap out → pagefile.sys → swap in → data integrity verified
- [x] Commit: `"mm: disk-backed swap via pagefile.sys"`

### 3.2 Memory-Mapped Files

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `mmap`, `munmap`, `msync` exist, MAP_PRIVATE and MAP_SHARED work, `SYS_MMAP` (37) and `SYS_MUNMAP` (38) syscalls are registered. Verify the eager-load implementation reads file contents into mapped pages correctly. Run `bash scripts/build.sh clean`. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to memory mapping. Create or update documentation in `docs/` covering mmap semantics, MAP_PRIVATE/MAP_SHARED, and COW fault handling.


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

### 4.1 Styled Panic Screen

**Prompt:** This section is marked complete. Verify the implementation is correct: review `panic_screen()` renders directly to framebuffer with exception name, stop code, faulting RIP, CR2, full register dump (RAX-R15, RSP, RFLAGS, CR2, CR3), and stack trace via RBP chain walking. Confirm auto-restart countdown uses Registry `HKLM\SYSTEM\Recovery\AutoRestart`. Check crash dump writes to `C:\Impossible\System\crashdump.log`. Verify `KPANIC(msg)` macro captures file/line. Confirm idt.c and vmm.c call `panic_screen()`. Run `bash scripts/build.sh clean`. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to error handling or panic screens. Create or update documentation in `docs/` covering the panic screen, crash dump format, auto-restart, and stack trace walking.


- [x] Design graphical panic screen (Impossible OS blue, sad face, error info)
- [x] Implement `panic_screen(error_code, rip, cr2, description)` with:
  - [x] Exception name and stop code
  - [x] Faulting address and RIP
  - [x] Source file + line (via `__FILE__`, `__LINE__`)
  - [x] Register dump (RAX–R15, RSP, RFLAGS, CR2, CR3)
- [x] Implement stack trace (walk RBP chain, print return addresses)
- [x] Auto-restart countdown (configurable via Registry `HKLM\SYSTEM\Recovery\AutoRestart`)
- [x] Dump crash info to `C:\Impossible\System\crashdump.log` (when FS is available)
- [x] Add `KPANIC(msg)` macro that captures file/line automatically
- [x] Replaced default handlers in `idt.c` and `vmm.c` with `panic_screen()` calls
- [x] Commit: `"kernel: styled panic screen with stack trace"`

---

## 5. Hardware Abstraction Layer (HAL)

> **Moved to Phase 16 §1.4** — merged with the Driver Model & PCI Match Tables.
> See [TODO-Phase-16-Drivers.md §1.4](TODO-Phase-16-Drivers.md#14-driver-model-hal--pci-match-tables).

---

## 6. System Logging Enhancements

### 6.1 Unified Logging System

**Prompt:** Verify the unified logging system implementation. The codebase has `klog.h`/`klog.c` with 5 levels (DEBUG → FATAL), subsystem tags, 1000-entry ring buffer, serial+framebuffer output with colored prefixes, and FATAL auto-halt. `klog_flush.c` writes to `C:\Impossible\System\Logs\kernel.log`. `SYS_LOG` syscall (#17) lets user-mode apps log. Per-subsystem log file splitting (network.log, boot.log) is not yet implemented — all entries go to a single kernel.log. After completing remaining items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: per-subsystem log files"`. Update `README.md` if it contains stale or incorrect references to logging. Create or update documentation in `docs/` covering the klog system, log levels, subsystem tags, ring buffer, disk persistence, and per-subsystem splitting.

**Implementation files:** `include/kernel/klog.h`, `src/kernel/klog.c`, `src/kernel/klog_flush.c`, `include/kernel/log.h` (legacy, serial-only)

- [x] Add `LOG_DEBUG` and `LOG_FATAL` levels (currently: INFO, WARN, ERROR) — `klog.h` lines 20-26
- [x] Add source/subsystem tag to all log calls (e.g., `"net"`, `"fs"`, `"mm"`) — used throughout `main.c`
- [x] Implement in-memory ring buffer (last 1000 log entries) — `klog.c` `klog_ring[KLOG_RING_SIZE]`
- [/] Implement periodic flush to disk:
  - [x] `C:\Impossible\System\Logs\kernel.log` — `klog_flush.c`, called from `main.c`
  - [ ] `C:\Impossible\System\Logs\network.log` — not yet (all logs go to kernel.log)
  - [ ] `C:\Impossible\System\Logs\boot.log` — not yet (all logs go to kernel.log)
- [x] Add `sys_log()` syscall for user-mode apps to log — `SYS_LOG=17` in `syscall.h`/`syscall.c`
- [x] Commit: `"kernel: unified logging with disk persistence"`


---

## 7. Environment Variables

### 7.1 System & User Environment

**Prompt:** Environment variables are key-value string pairs stored per-process, inherited by children, and essential for the shell's PATH-based command lookup. Store as a flat array of `"KEY=VALUE"` strings in each `struct task` (same as POSIX environ). System-wide defaults (PATH, SYSTEMROOT, TEMP, HOME, USERNAME, COMPUTERNAME) should be populated from Registry entries at boot. The `env_expand` function replaces `%VAR%` tokens in strings — this is used by the shell for command expansion and by file associations for argument templates. The shell's command lookup must search each PATH directory in order, trying the command name with and without `.exe` extension. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: environment variables"`. Update `README.md` if it contains stale or incorrect references to environment variables or PATH. Create or update documentation in `docs/` covering environment variable storage, per-process inheritance, env_expand, and PATH-based command lookup.


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

### 8.1 Clean Shutdown Sequence

**Prompt:** A clean shutdown prevents data loss by flushing everything before powering off. The sequence must be: send WM_CLOSE to all GUI apps (giving them a chance to prompt "Save work?"), wait up to 5 seconds then force-kill remaining apps, flush all open file handles, flush the Registry to disk, release the DHCP lease, unmount all filesystems, sync disk caches, and finally issue the ACPI power-off. For QEMU/Bochs the shortcut is `outw(0x604, 0x2000)` but for real hardware parse the ACPI FADT for the PM1a_CNT_BLK address and write SLP_TYP | SLP_EN. Show a "Shutting down..." screen during cleanup. The `shutdown` and `reboot` shell commands should trigger this sequence. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: clean shutdown sequence"`. Update `README.md` if it contains stale or incorrect references to shutdown or power management. Create or update documentation in `docs/` covering the shutdown sequence, ACPI power-off, and graceful app termination.


- [ ] Before ACPI shutdown:
  - [ ] Send `WM_CLOSE` to all GUI apps (save work prompt)
  - [ ] Flush all open file handles
  - [ ] Flush Registry to disk
  - [ ] Stop network services (release DHCP lease)
  - [ ] Unmount all filesystems
  - [ ] Sync disk caches
- [ ] Display "Shutting down..." screen during cleanup
- [ ] Implement shutdown timeout (force-kill apps after 5 seconds)
- [ ] Commit: `"kernel: clean shutdown sequence"`

### 8.2 Sleep / Lock Screen (Future)

**Prompt:** These are stretch goals. Sleep requires ACPI S3 suspend-to-RAM which involves saving all device state and entering the S3 state via the PM1a control register — on wake, the CPU resumes at the FACS waking vector. The lock screen is simpler: stop rendering the desktop, display the login/password UI overlay, and resume on correct password (check against Registry credentials from Phase 09). For QEMU testing, S3 can be simulated with `-global ICH9-LPC.disable_s3=0`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: sleep and lock screen"`. Update `README.md` if it contains stale or incorrect references to sleep or lock screen. Create or update documentation in `docs/` covering ACPI S3 suspend, waking vector, and lock screen UI flow.


- [ ] *(Stretch)* — ACPI S3 suspend to RAM
- [ ] *(Stretch)* — Lock screen (password prompt, Registry credential check)

---

## 9. Dynamic Linking

### 9.1 ELF Shared Libraries

**Prompt:** ELF shared libraries (.so) are loaded at runtime and shared between processes — study the ELF spec sections on PT_DYNAMIC, DT_NEEDED, .dynsym/.dynstr, and GOT/PLT. The dynamic linker parses the executable's DT_NEEDED list, searches the library path (app dir → `C:\Impossible\System\` → `C:\Impossible\System\Drivers\`), loads each .so into memory, applies R_X86_64_64 and R_X86_64_PC32 relocations, and patches the GOT. The `dlopen`/`dlsym`/`dlclose` API enables runtime plugin loading. This feature directly supports Phase 10's compatibility layer where Win32 DLL stubs are loaded similarly. Start by implementing symbol resolution for the current statically-linked kernel, then extend to user-mode binaries. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: dynamic linker (dlopen/dlsym)"`. Update `README.md` if it contains stale or incorrect references to dynamic linking. Create or update documentation in `docs/` covering the dynamic linker, ELF relocation types, GOT/PLT patching, and library search path.


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

### 10.1 Loadable Kernel Modules (.kmod)

**Prompt:** Loadable kernel modules let drivers be loaded at runtime without recompiling the kernel — define .kmod as relocatable ELF objects (.o files) with exported `module_init()` and `module_cleanup()` functions. The loader reads the ELF sections, allocates kernel memory for .text/.data/.bss, applies relocations against a kernel symbol table (exported via a `EXPORT_SYMBOL` macro), and calls `module_init`. Unloading calls `module_cleanup` and frees memory. Convert the RTL8139 driver as a proof-of-concept: compile it as a separate .o, load it via `insmod rtl8139.kmod`, verify networking still works. The PCI scan at boot should be able to match vendor:device IDs to .kmod files and auto-load them. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: loadable kernel modules"`. Update `README.md` if it contains stale or incorrect references to kernel modules. Create or update documentation in `docs/` covering the .kmod format, module loader, EXPORT_SYMBOL, and PCI auto-load.


- [ ] Define `.kmod` format (ELF relocatable objects with `init()`/`cleanup()`)
- [ ] Implement `kmod_load(path)` — load ELF, relocate, call `init()`
- [ ] Implement `kmod_unload(name)` — call `cleanup()`, free memory
- [ ] Implement `kmod_list(out, max)` — list loaded modules
- [ ] Convert one existing driver (e.g., RTL8139) to a loadable module
- [ ] Load modules based on PCI scan results at boot
- [ ] Commit: `"kernel: loadable kernel modules"`

---

## 11. General Utilities & Libraries

### 11.1 TrueType Font Rendering

**Prompt:** TrueType rendering via stb_truetype.h has already been partially integrated (see the previous conversation on TrueType integration). Check if `src/libs/stb_truetype/stb_truetype_impl.c`, `include/font_mgr.h`, and `src/desktop/gfx_text.c` already exist in the codebase. If they do, focus on completing the remaining items (bundling fonts, anti-aliasing, replacing bitmap font usage). If not, port stb_truetype into `src/libs/stb_truetype/`, create math shims in `include/kmath.h` (floor, ceil, sqrt, fabs, cos, sin, acos, fmod, pow — using SSE2), and compile the implementation file with `-msse2 -mfpmath=sse`. The font manager loads .ttf files from `C:\Impossible\Fonts\` using VFS. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: TrueType font rendering via stb_truetype"`. Update `README.md` if it contains stale or incorrect references to font rendering. Create or update documentation in `docs/` covering the stb_truetype integration, kmath shims, font manager API, and glyph caching.


- [ ] Port **stb_truetype** (single header, public domain) into `src/libs/stb_truetype/`
- [ ] Bundle **JetBrains Mono** (OFL 1.1) for terminal/code
- [ ] Bundle **Inter** (OFL 1.1) for UI text
- [ ] Create `font_ttf_render(font, codepoint, size, bitmap)` API
- [ ] Replace bitmap font in desktop/terminal with TrueType rendering
- [ ] Support font sizes: 8pt, 10pt, 12pt, 14pt, 16pt
- [ ] Add font anti-aliasing (grayscale blending)
- [ ] Commit: `"desktop: TrueType font rendering via stb_truetype"`

### 11.2 Compression (miniz)

**Prompt:** Port miniz (single-file, MIT license) into `src/libs/miniz/` — it provides zlib-compatible deflate/inflate and ZIP archive reading. Compile with `-ffreestanding -nostdlib` and redirect its `malloc`/`free` calls to `kmalloc`/`kfree` using `#define` overrides. miniz is needed by: IXFS transparent compression (Phase 06 §5.5), HTTP gzip content encoding (Phase 07), and the IPKG package format (Phase 12 §2.1). Test by compressing a known buffer, decompressing it, and comparing with the original. The ZIP API (`zip_open`, `zip_read_file`, `zip_close`) wraps miniz's `mz_zip_reader_*` functions. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"libs: miniz compression (deflate/ZIP)"`. Update `README.md` if it contains stale or incorrect references to compression. Create or update documentation in `docs/` covering miniz integration, deflate/inflate API, and ZIP archive reading.


- [ ] Port **miniz** (single file, MIT) into `src/libs/miniz/`
- [ ] Implement `deflate`/`inflate` for gzip support
- [ ] Implement ZIP archive read: `zip_open()`, `zip_read_file()`, `zip_close()`
- [ ] Test: decompress a gzip file
- [ ] Test: list and extract files from a ZIP archive
- [ ] Commit: `"libs: miniz compression (deflate/ZIP)"`

### 11.3 Cryptography (monocypher)

**Prompt:** Port monocypher (BSD-2, ~3000 lines) into `src/libs/monocypher/` — it provides ChaCha20 (stream cipher), Poly1305 (MAC), Blake2b (hash), Argon2i (password hash), X25519 (key exchange), and Ed25519 (signatures). Compile with `-ffreestanding`. This library is the cryptographic foundation for: password hashing in Phase 09 §1.2, TLS in Phase 07 §5, executable signing in Phase 09 §8, and data encryption in Phase 09 §4. The kernel CSPRNG should seed from RDRAND (via inline assembly) and use ChaCha20 to generate random bytes, exposed as a `/dev/random` equivalent. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"libs: monocypher crypto primitives"`. Update `README.md` if it contains stale or incorrect references to cryptography. Create or update documentation in `docs/` covering monocypher integration, CSPRNG seeding, and exposed crypto primitives.


- [ ] Port **monocypher** (3000 lines, BSD-2) into `src/libs/monocypher/`
- [ ] Expose: ChaCha20 (stream cipher), Blake2b (hash), Argon2 (password hash)
- [ ] Expose: X25519 (key exchange), Ed25519 (signatures)
- [ ] Implement kernel CSPRNG (`/dev/random` equivalent using RDRAND + ChaCha20)
- [ ] Commit: `"libs: monocypher crypto primitives"`

### 11.4 Math Library

**Prompt:** Port essential floating-point math functions from OpenLibm or musl libc (both MIT-compatible) into `src/libs/math/`. These functions are required by stb_truetype (§11.1), any audio synthesis (Phase 08), and the TTS engine (Phase 13 §7.1). Key functions: sin, cos, tan, asin, acos, atan, atan2, exp, log, log10, pow, sqrt, fabs, ceil, floor, fmod, round. If `kmath.h` already exists from the TrueType integration, extend it rather than duplicating. Compile with `-msse2` as these need floating-point hardware. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"libc: floating-point math functions"`. Update `README.md` if it contains stale or incorrect references to math support. Create or update documentation in `docs/` covering the math library, SSE2 requirements, and available functions.


- [ ] Port `sin`, `cos`, `tan` from **OpenLibm** or **musl** (MIT)
- [ ] Port `exp`, `log`, `log10`
- [ ] Port `pow`, `sqrt`, `fabs`, `ceil`, `floor`
- [ ] Port `fmod`, `atan2`
- [ ] Commit: `"libc: floating-point math functions"`

### 11.5 JSON Parser (cJSON)

**Prompt:** Port cJSON (MIT, ~2000 lines) into `src/libs/cjson/` — it provides a simple DOM-style JSON parser and serializer. Redirect `malloc`/`free` to `kmalloc`/`kfree` via `cJSON_InitHooks()`. cJSON is used for: the update manifest (Phase 12 §1.1), theme definitions (Phase 04 desktop theming), NTP server lists, and any future REST API interaction. Test by parsing a JSON string like `{"version": "1.0", "name": "Impossible OS"}`, extracting values with `cJSON_GetObjectItem`, and verifying correctness. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"libs: cJSON parser"`. Update `README.md` if it contains stale or incorrect references to JSON support. Create or update documentation in `docs/` covering cJSON integration, memory hook setup, and usage examples.


- [ ] Port **cJSON** (MIT, ~2000 lines) into `src/libs/cjson/`
- [ ] Test: parse a JSON config file
- [ ] Use for: settings files, asset manifests, theme definitions
- [ ] Commit: `"libs: cJSON parser"`

### 11.6 TCP (lwIP or Custom)

**Prompt:** TCP is the transport layer for HTTP, TLS, SSH, FTP, and virtually all internet protocols in Phases 07 and 11. The existing network stack in `src/kernel/net/` already has Ethernet, ARP, IPv4, ICMP, and UDP — study those files to understand the packet flow. TCP adds: a full connection state machine (LISTEN→SYN_SENT→SYN_RCVD→ESTABLISHED→FIN_WAIT→CLOSE_WAIT→etc.), 32-bit sequence/ack numbers, checksums (pseudo-header + TCP header + payload), retransmission timers, and sliding window flow control. lwIP (~30K lines, BSD-3) is battle-tested but large — evaluate whether its complexity is justified vs. a minimal custom implementation that only needs client-side TCP (no server listen initially). Test by making a raw HTTP GET request to an httpbin endpoint through QEMU's user networking. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"net: TCP implementation"`. Update `README.md` if it contains stale or incorrect references to networking or TCP. Create or update documentation in `docs/` covering the TCP state machine, checksum calculation, retransmission, and sliding window.


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

**Prompt:** File descriptors are the POSIX abstraction that unifies files, pipes, sockets, and devices behind integer handles. Each process gets an FD table (array of pointers to `struct file`). `struct file` has a type enum (FILE, PIPE, SOCKET, DEVICE), a VFS node pointer, a read/write offset, and type-specific callbacks for read/write/close. FDs 0/1/2 are stdin/stdout/stderr (wired to the terminal). `dup`/`dup2` enables I/O redirection (`cmd > file`). This is a prerequisite for pipes (§2.1) and the shell's I/O redirection. Study the existing VFS in `src/kernel/vfs.c` to understand how `vfs_open`/`vfs_read`/`vfs_write` work, then wrap them in the FD layer. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: file descriptor table"`. Update `README.md` if it contains stale or incorrect references to file I/O. Create or update documentation in `docs/` covering the FD table, struct file, type dispatch, and dup/dup2 semantics.


- [ ] Implement per-process file descriptor table (fd 0=stdin, 1=stdout, 2=stderr)
- [ ] `open(path, flags)` → returns fd
- [ ] `read(fd, buf, size)` / `write(fd, buf, size)` — dispatch to VFS, pipe, or device
- [ ] `close(fd)` — release fd slot
- [ ] `dup(fd)` / `dup2(old, new)` — duplicate file descriptors
- [ ] Required for pipes, I/O redirection, and proper Unix-style process model
- [ ] Commit: `"kernel: file descriptor table"`

### 12.2 Process Working Directory

**Prompt:** Every process needs a current working directory (CWD) stored as an absolute path string in `struct task`. All relative paths in VFS calls must be resolved against CWD by prepending it. The shell's `cd` command calls `chdir()` which validates the path exists (via VFS) then updates the task's CWD string. `getcwd()` returns the current string. Children inherit the parent's CWD on fork/exec. The shell prompt should display the CWD. This is a small but essential feature — without it, every path must be absolute. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: per-process working directory"`. Update `README.md` if it contains stale or incorrect references to working directories. Create or update documentation in `docs/` covering CWD storage, chdir/getcwd, relative path resolution, and child inheritance.


- [ ] Add `cwd` field to `struct task` (default: `C:\`)
- [ ] Implement `chdir(path)` syscall
- [ ] Implement `getcwd(buf, size)` syscall
- [ ] Support relative paths in VFS (resolve against cwd)
- [ ] Update shell `cd` command to use `chdir()`
- [ ] Commit: `"kernel: per-process working directory"`

### 12.3 Proper `brk`/`sbrk` Heap for User Programs

**Prompt:** User-mode programs need their own heap separate from the kernel heap. `brk(addr)` sets the program break (top of data segment), `sbrk(increment)` extends it by N bytes. The kernel maps new pages on demand as the break increases. User-mode `malloc` implementations (dlmalloc, musl's allocator) call sbrk internally, so once this works, any standard allocator can be ported. The program break starts at the end of the BSS section (read from the ELF loader). This is needed before any non-trivial user programs can run. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: brk/sbrk heap for user processes"`. Update `README.md` if it contains stale or incorrect references to user-mode memory. Create or update documentation in `docs/` covering brk/sbrk, program break, and user-mode malloc integration.


- [ ] Implement per-process heap via `brk`/`sbrk` syscalls
- [ ] Replace bump allocator in user libc with `sbrk`-backed `malloc`/`free`
- [ ] Implement `free()` with a proper free-list in user space
- [ ] Commit: `"kernel: brk/sbrk heap for user processes"`

### 12.4 Timer API for User Programs

**Prompt:** User programs need `sleep(seconds)` and `usleep(microseconds)` for delays, and `gettimeofday()` for wall-clock time. `sleep` adds the calling thread to a timer queue sorted by wake time, then yields — the PIT IRQ handler checks the queue each tick and wakes expired entries. `gettimeofday` combines the RTC (for calendar time) with the PIT tick counter (for sub-second precision). These syscalls are needed by the compositor's vsync timing, animation loops in GUI apps, and the NTP client (Phase 03 §4.4). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: user-mode timer syscalls"`. Update `README.md` if it contains stale or incorrect references to timing or sleep. Create or update documentation in `docs/` covering sleep/usleep implementation, timer queue, and gettimeofday precision.


- [ ] Implement `sleep(seconds)` syscall (block task, wake on timer)
- [ ] Implement `usleep(microseconds)` syscall
- [ ] Implement `gettimeofday()` syscall (RTC + PIT sub-second precision)
- [ ] Commit: `"kernel: user-mode timer syscalls"`

### 12.5 Kernel Memory Leak Detection (Debug Build)

**Prompt:** This is a debug-only feature enabled by `-DKMALLOC_DEBUG` with zero overhead in release builds. Wrap `kmalloc` and `kfree` to record each allocation: caller address (via `__builtin_return_address(0)`), size, file, and line. Store records in a simple linked list. On shutdown or via a `memleak` debug command, dump all un-freed allocations showing where they were allocated. Also add `kmalloc_stats()` returning current usage, peak usage, and total allocation count — useful for the Task Manager (Phase 05 §8.1) and debug console (Phase 13 §2.2). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"mm: kmalloc leak detector (debug build)"`. Update `README.md` if it contains stale or incorrect references to memory debugging. Create or update documentation in `docs/` covering the kmalloc debug mode, allocation tracking, and leak reporting.


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
| 🟢 P3 | ~~5. HAL~~ | Moved to Phase 16 §1.4 |
| 🟢 P3 | 9. Dynamic Linking | Shared libraries |
| 🟢 P3 | 10. Kernel Modules | Runtime driver loading |
| 🟢 P3 | 11.2–11.5 Libraries | Compression, crypto, math, JSON |
| 🔵 P4 | 12.3 brk/sbrk | Better user heap |
| 🔵 P4 | 12.4 Timer API | User-mode sleep/timing |
| 🔵 P4 | 12.5 Leak Detection | Debug tooling |
