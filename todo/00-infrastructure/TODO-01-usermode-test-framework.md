# TODO-01 -- User-Mode Test Framework

> **Goal:** A test harness for user-mode code: syscalls, libc functions, Win32 API stubs, ELF/PE/EIF loading, process lifecycle, and IPC. Test programs are compiled as regular user-mode binaries (`test_*.exe`), deployed to the IXFS system disk, and executed by the kernel after boot. Each test binary exercises one subsystem, writes pass/fail results to stdout (SYS_WRITE), and exits with 0 (pass) or non-zero (fail). The kernel test runner launches each binary, captures its output and exit code, and reports results to serial. This is how Windows HLK and Linux kselftest work -- real user-mode programs exercising the real syscall interface.

> [!IMPORTANT]
> **Current state:** One user-mode binary exists: `hello.exe` (prints a message, exercises SYS_WRITE + SYS_EXIT). No test binaries. No mechanism to launch multiple user-mode programs in sequence and collect results. The shell can run `exec <name>` but there's no automated test launcher. User-mode libc (`src/libc/`) has basic string functions but no tests.

---

## Inputs

- `user/` -- user-mode source directory (hello.c, user.ld, include/)
- `user/include/syscall.h` -- user-mode syscall wrappers
- `src/libc/` -- kernel libc (string, printf -- shared with user mode)
- `src/kernel/sched/task.c` -- task_create_user, task_exec
- `src/kernel/sched/syscall.c` -- syscall dispatcher
- `CLAUDE.md` -- `bash scripts/test.sh` and SUITE=exec for local kernel + user-mode coverage (no CI QEMU in Actions)
- `include/kernel/sched/syscall.h` -- authoritative INT 0x80 syscall numbers for §10 parity work
- → XREF: `00-infrastructure/TODO-02-desktop-ui-test-framework.md` -- desktop UI test TODO consumes §2 launcher and §11 env matrix (see its Inputs XREFs to this file)
- → XREF: `02-kernel-core/TODO-05-native-api-ssdt.md` -- NtXxx syscalls tested here
- → XREF: `02-kernel-core/TODO-08-binary-system.md` -- ELF/PE/EIF loader tested here

---

## Outcome

- `user/test/` directory with test binaries: `test_syscall.exe`, `test_libc.exe`, `test_ipc.exe`, etc.
- Each test binary: exercises one subsystem, prints `[PASS] name` or `[FAIL] name: reason`, exits 0/1.
- Kernel test launcher: after boot, runs each `test_*.exe` in sequence, collects exit codes.
- Serial output shows per-binary pass/fail summary.
- `make test` includes user-mode tests (builds test binaries, deploys to disk, boots, runs).
- Adding a new user-mode test: create `user/test/test_foo.c`, add to Makefile -- done.
- Ordered manifest, per-binary timeouts, and optional TAP or skip lines for CI-grade triage (§11).
- `user/include/syscall.h` stays in lockstep with the kernel INT 0x80 table before §3-§7 expand coverage (§10).

---

## Implementation Order

| ⭐  | Order | Deliverable                                     | Depends On     | Status |
| --- | :---: | ----------------------------------------------- | -------------- | :----: |
| 💎  |   1   | User-mode test assertion macro and harness       | --              |  [ ]   |
| 💎  |   2   | Kernel test launcher (run test_*.exe in sequence) | §1            |  [ ]   |
| 💎  |  10   | Userland syscall.h parity with kernel INT 0x80 ABI | --          |  [ ]   |
| 💎  |   3   | Syscall test binary (test_syscall.exe)           | §1, §2, §10    |  [ ]   |
| 💎  |   4   | Libc test binary (test_libc.exe)                 | §1, §2         |  [ ]   |
| 💎  |   5   | IPC test binary (test_ipc.exe)                   | §1, §2, §10    |  [ ]   |
| 💎  |   6   | Process lifecycle test (test_process.exe)         | §1, §2         |  [ ]   |
| 💎  |   7   | File I/O test (test_fileio.exe)                  | §1, §2, §10    |  [ ]   |
| ⭐  |   8   | Win32 API test binary (test_win32.exe)            | §1, §2, TODO-05-native-api-ssdt.md §5 |  [ ]   |
| 💎  |  11   | Launcher manifest, timeouts, TAP or skip hooks | §2            |  [ ]   |
| 💎  |   9   | Build integration -- `make test` includes user tests | §2, §3, §4, §5, §6, §7, §8, §11 |  [ ]   |

> 💎 = parity -- Linux kselftest and Windows HLK both use user-mode test binaries.
> ⭐ = exclusive -- testing Win32 API surface from user-mode on a non-Windows kernel.
> **Order column** matches `## N.` section numbers. Row **10** appears before row **3** in the table because §10 is an ABI gate for §3, §5, and §7.

---

## 1. User-Mode Test Assertion Macro

Minimal test harness for user-mode binaries -- no kernel dependencies.

- [ ] Create `user/include/test.h`:
  ```c
  #define UTEST_ASSERT(cond, msg) do { \
      if (cond) { sys_write(1, "[PASS] ", 7); sys_write(1, msg, strlen(msg)); sys_write(1, "\n", 1); } \
      else { sys_write(1, "[FAIL] ", 7); sys_write(1, msg, strlen(msg)); sys_write(1, "\n", 1); g_fail++; } \
  } while(0)
  ```
- [ ] `g_fail` counter -- exit with `sys_exit(g_fail)` at end of main
- [ ] `UTEST_BEGIN(name)` / `UTEST_END()` -- print suite header/footer
- [ ] Commit: `"test: user-mode test assertion macro (user/include/test.h)"`

**Test checkpoint:** Compile a trivial test binary using UTEST_ASSERT -- runs and exits cleanly. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal (build host only for compile step).

---

## 2. Kernel Test Launcher

Kernel-side mechanism to run user-mode test binaries and collect results.

- [ ] In `boot_tests.c` or a new `test_usermode.c`: scan `C:\Impossible\System32\test_*.exe`
- [ ] For each: `task_create_user(entry, name)` → `task_waitpid(pid)` → collect exit status
- [ ] Log: `"[UTEST] test_syscall.exe: %s (exit=%d)"` with PASS/FAIL based on exit code
- [ ] Summary: `"[UTEST] %u passed, %u failed of %u user-mode tests"`
- [ ] Triggered by `test=1` in boot.conf (same gate as kernel unit tests)
- [ ] Commit: `"test: kernel test launcher -- run user-mode test_*.exe and collect results"`

**Test checkpoint:** Deploy `test_syscall.exe` to disk image. Boot with `test=1` → serial shows `[UTEST] test_syscall.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 3. Syscall Test Binary

Exercise every implemented syscall from user mode.

- [ ] Complete §10 first so `user/include/syscall.h` exposes wrappers for each syscall below (numbers must match `include/kernel/sched/syscall.h` INT 0x80 path).
- [ ] `user/test/test_syscall.c`:
  - `SYS_WRITE` to stdout → verify returns byte count
  - `SYS_READ` from stdin (non-blocking test or skip)
  - `SYS_YIELD` → returns 0
  - `SYS_UPTIME` → returns > 0
  - `SYS_GETPROCS` → returns ≥ 2 (idle + this process)
  - `SYS_OPENFILE` → open `C:\hello.txt` → returns valid handle
  - `SYS_READHANDLE` → reads content from hello.txt
  - `SYS_CLOSEHANDLE` → close handle → returns 0
  - `SYS_LOG` → write to klog → returns 0
  - `SYS_OPENDIROBJ` → open `\` → returns valid handle
  - `SYS_QUERYDIROBJ` → enumerate → returns entries
- [ ] Commit: `"test: user-mode syscall test binary (test_syscall.exe)"`

**Test checkpoint:** `test_syscall.exe` runs, all assertions pass, exit code 0, serial `[UTEST] test_syscall.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 4. Libc Test Binary

Test string and formatting functions available in user mode.

- [ ] `user/test/test_libc.c`:
  - `strlen("hello")` → 5
  - `strcmp("abc", "abc")` → 0
  - `strcmp("abc", "abd")` → negative
  - `memcpy` round-trip
  - `memset` + verify
  - `snprintf(buf, 32, "%d", 42)` → `"42"`
- [ ] Commit: `"test: user-mode libc test binary (test_libc.exe)"`

**Test checkpoint:** Boot with `test=1`, launcher runs `test_libc.exe` then exit 0 and serial shows `[UTEST] test_libc.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 5. IPC Test Binary

Test inter-process communication from user mode.

- [ ] Complete §10 so `SYS_PIPE`, `SYS_SHMEM_CREATE`, and `SYS_SHMEM_MAP` wrappers exist in userland before compiling this binary.
- [ ] `user/test/test_ipc.c`:
  - `SYS_PIPE` → two handles, write to one, read from other
  - `SYS_SHMEM_CREATE` → returns handle
  - `SYS_SHMEM_MAP` → returns non-zero address
  - Write to shared memory, verify data
- [ ] Commit: `"test: user-mode IPC test binary (test_ipc.exe)"`

**Test checkpoint:** `test_ipc.exe` completes pipe + shmem checks, exit 0, serial `[UTEST] test_ipc.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 6. Process Lifecycle Test

Test fork, exec, waitpid from user mode.

- [ ] `user/test/test_process.c`:
  - `SYS_FORK` → parent gets child PID, child gets 0
  - `SYS_WAITPID` → parent waits for child, gets exit status
  - `SYS_EXEC` → load another binary (hello.exe)
  - `SYS_KILL` → kill a child process
- [ ] Commit: `"test: user-mode process lifecycle test (test_process.exe)"`

**Test checkpoint:** Fork, wait, exec, and kill paths assert clean, exit 0, `[UTEST] test_process.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 7. File I/O Test

Test handle-based file operations.

- [ ] Complete §10 so `SYS_OPENFILE`, `SYS_READHANDLE`, `SYS_CLOSEHANDLE`, `SYS_OPENDIROBJ`, and `SYS_QUERYDIROBJ` wrappers exist before compiling this binary.
- [ ] `user/test/test_fileio.c`:
  - Open `C:\hello.txt` → valid handle
  - Read contents → matches expected
  - Close handle → handle becomes invalid
  - Open nonexistent file → INVALID_HANDLE_VALUE
  - Open directory object → enumerate entries
- [ ] Commit: `"test: user-mode file I/O test (test_fileio.exe)"`

**Test checkpoint:** Open, read, close, and error paths match expectations, exit 0, `[UTEST] test_fileio.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 8. Win32 API Test Binary

Test Win32 API stubs once they're implemented (depends on TODO-05 Native API Layer).

- [ ] `user/test/test_win32.c`:
  - `GetCurrentProcessId()` → returns PID > 0
  - `CreateFile("C:\\hello.txt", GENERIC_READ, ...)` → valid handle
  - `ReadFile(handle, buf, size, &read, NULL)` → reads data
  - `CloseHandle(handle)` → returns TRUE
  - `GetTickCount()` → returns > 0
- [ ] Commit: `"test: user-mode Win32 API test (test_win32.exe)"`

**Test checkpoint:** After TODO-05 §5 stubs exist: `test_win32.exe` exit 0 and `[UTEST] test_win32.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 9. Build Integration

Wire user-mode test binaries into `make test`.

- [ ] Makefile: compile `user/test/test_*.c` → `build/user/test_*.exe`
- [ ] Disk image build: copy test binaries to `C:\Impossible\System32\`
- [ ] `make test` target: include user-mode tests after kernel unit tests
- [ ] `scripts/test.sh`: parse serial for `[UTEST]` lines alongside `[TEST]` kernel lines
- [ ] When §11 `tap=1` is enabled in boot.conf, parse TAP `ok` / `not ok` / `# SKIP` lines into the same summary as `[UTEST]`
- [ ] Commit: `"test: build integration -- user-mode tests in make test and CI"`

**Test checkpoint:** `bash scripts/test.sh` (full or SUITE=exec) ends with combined kernel `[TEST]` summary plus `[UTEST]` user summary; missing binary or non-zero exit fails the run. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 10. Userland syscall.h parity with kernel INT 0x80 ABI

`user/include/syscall.h` currently stops at `SYS_NETINFO`; kernel exposes `SYS_LOG`, pipes, shmem, mmap, and file/dir handles through `SYS_QUERYDIROBJ`. User-mode tests cannot compile until the headers and thin `sys_*` wrappers match.

- [ ] Diff `user/include/syscall.h` against `include/kernel/sched/syscall.h` for every `#define SYS_` used on the INT 0x80 path (ignore SSDT alias block at bottom of kernel header for this pass).
- [ ] Add missing `#define` constants and `static inline` syscall wrappers for `SYS_LOG` through `SYS_QUERYDIROBJ`, plus `SYS_PIPE`, `SYS_SHMEM_CREATE`, `SYS_SHMEM_MAP`, `SYS_MMAP`, `SYS_MUNMAP` if §5-§7 need them.
- [ ] Rebuild `hello.exe` and a stub `user/test/test_syscall.c` that only calls `SYS_WRITE` to prove the header still links.
- [ ] Commit: `"test: sync user syscall.h with kernel INT 0x80 ABI for usermode harness"`

**Test checkpoint:** `clang` user-mode build of `test_syscall.c` succeeds with no undefined `sys_*` reference; `_Static_assert` or comment at top of `user/include/syscall.h` points maintainers at `include/kernel/sched/syscall.h` as source of truth. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal (compile step on dev host is enough for PASS).

---

## 11. Launcher manifest, timeouts, and CI-friendly output

Parity with kselftest/LKFT-style automation: deterministic order, watchdogs, machine-readable output, and documented skips for flaky hypervisors (see CLAUDE.md WHPX vs TCG notes for storage; same class of issue can hit timing-sensitive user tests).

- [ ] Add `tests/usermode.manifest` listing `test_*.exe` in execution order; launcher reads it before falling back to directory glob.
- [ ] Add per-binary wall-clock timeout in the launcher wait path -- kill task and emit `[UTEST] name: FAIL (timeout)` on expiry.
- [ ] Add optional `tap=1` in `boot.conf` so the launcher prints minimal TAP lines (`ok N - name` / `not ok N - name`) around each binary for `scripts/test.sh` (pairs with §9 parser bullet).
- [ ] Define `SKIP reason` line contract from tests: launcher records skip without failing the suite; `scripts/test.sh` counts skips separately from FAIL.
- [ ] Add `docs/testing/usermode-env-matrix.md` documenting QEMU WHPX vs TCG vs VirtualBox vs bare metal expectations for user-mode timing and known skip reasons (no kernel code, doc only).
- [ ] Commit: `"test: usermode launcher manifest, timeouts, TAP, and skip policy"`

**Test checkpoint:** With manifest listing two dummy binaries, kill one via timeout mid-run -- summary shows 1 FAIL(timeout) and remaining binaries still execute. With `tap=1`, serial contains `ok 1` style lines parseable by TAP consumers. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐ | Feature | 🪟 Win11 | 🐧 Linux | 🚀 Impossible OS |
| --- | --- | --- | --- | --- |
| 💎 | User-mode test bins | ✅ HLK | ✅ kselftest | ⬜ §1-§8 |
| 💎 | Syscall coverage | ✅ NtDll | ✅ ptrace selftest | ⬜ §3 |
| 💎 | Auto launcher | ✅ HLK | ✅ run_kselftest | ⬜ §2 |
| 💎 | TAP or CI parse | ✅ HLK XML | ✅ TAP kselftest | ⬜ §9, §11 |
| 💎 | Timeouts or skips | ✅ HLK | ✅ LKFT skip | ⬜ §11 |
| 💎 | ABI header sync | ✅ SDK | ✅ uapi | ⬜ §10 |
| ⭐ | Win32 on non-Win | ❌ N/A | ❌ Wine only | ⬜ §8 🚀 |

> **Parity gaps:** 💎 rows with ⬜ map to listed sections. **⭐ row** needs TODO-05 §5 Win32 stubs.

---

## Unit Tests

> [!NOTE]
> User-mode coverage is driven by `user/test/test_*.c` binaries and serial `[UTEST]` lines from §2 onward, not a dedicated `src/kernel/test/test_usermode.c` until a kernel-side wrapper is justified. §10 is header-only parity; §11 is launcher and serial format policy.

- [ ] Commit: `"test: N/A single TEST_CAT file -- usermode harness per §1-§11 and Verification"`

**Test checkpoint:** After §2 ships: `bash scripts/test.sh SUITE=exec` (see CLAUDE.md) parses `[UTEST]` PASS/FAIL alongside kernel `[TEST]` lines.

---

## Verification

- [ ] `make test` → user-mode tests run after kernel tests, all pass.
- [ ] Break a syscall → user-mode test catches it → `bash scripts/test.sh` fails locally (Actions build-only per CLAUDE.md unless CI is extended).
- [ ] Add new test binary → one file + one Makefile line → works in `make test` / `bash scripts/test.sh`.
- [ ] Commit: `"test: user-mode test framework complete"`

**Test checkpoint:** End-to-end: clean tree → `bash scripts/test.sh` green → one-line change breaks a `test_*.exe` assertion → run fails with visible `[UTEST]` FAIL. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

**Test runner:** `scripts\debug\run-exec-tests.bat` (SUITE=exec)

---

## History

| Date | Action | Summary |
| --- | --- | --- |
| 2026-04-10 | validate | validate-todo-file: Inputs CLAUDE.md + test.sh (removed dead kernel-test-framework path); Impl row 8 Depends On TODO-05 §5; §3-§9 + Unit Tests + Verification checkpoints with platforms; OS Comparison compact + parity note; History; run-exec-tests.bat; back-XREF added on TODO-05 Inputs. |
| 2026-04-10 | gap-analysis | Added §10 syscall.h parity, §11 manifest/TAP/skip/doc; Impl Order + deps; OS rows (TAP, timeouts, ABI); Sources comment; §3/§5/§7/§9 checklist gaps; Inputs kernel syscall.h + TODO-02 XREF; TODO-02 Inputs patched (CLAUDE + §11). |
| 2026-04-10 | validate | validate-todo-file: fixed Inputs XREF (TODO-02 §2 was wrong section); Impl row 8 Depends On full TODO-05 filename + §5; Unit Tests Commit §1-§11; Verification bullets match CLAUDE Actions build-only; History row. |
