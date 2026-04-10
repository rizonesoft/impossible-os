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
- → XREF: `TODO-03-kernel-test-framework.md §2` -- `make test` target (local headless QEMU, no CI QEMU)
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

---

## Implementation Order

| ⭐  | Order | Deliverable                                     | Depends On     | Status |
| --- | :---: | ----------------------------------------------- | -------------- | :----: |
| 💎  |   1   | User-mode test assertion macro and harness       | --              |  [ ]   |
| 💎  |   2   | Kernel test launcher (run test_*.exe in sequence) | §1            |  [ ]   |
| 💎  |   3   | Syscall test binary (test_syscall.exe)           | §1, §2         |  [ ]   |
| 💎  |   4   | Libc test binary (test_libc.exe)                 | §1, §2         |  [ ]   |
| 💎  |   5   | IPC test binary (test_ipc.exe)                   | §1, §2         |  [ ]   |
| 💎  |   6   | Process lifecycle test (test_process.exe)         | §1, §2         |  [ ]   |
| 💎  |   7   | File I/O test (test_fileio.exe)                  | §1, §2         |  [ ]   |
| ⭐  |   8   | Win32 API test binary (test_win32.exe)            | §1, §2, T05 §5 |  [ ]   |
| 💎  |   9   | Build integration -- `make test` includes user tests | §2          |  [ ]   |

> 💎 = parity -- Linux kselftest and Windows HLK both use user-mode test binaries.
> ⭐ = exclusive -- testing Win32 API surface from user-mode on a non-Windows kernel.

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

**Test checkpoint:** Compile a trivial test binary using UTEST_ASSERT -- runs and exits cleanly.

---

## 2. Kernel Test Launcher

Kernel-side mechanism to run user-mode test binaries and collect results.

- [ ] In `boot_tests.c` or a new `test_usermode.c`: scan `C:\Impossible\System32\test_*.exe`
- [ ] For each: `task_create_user(entry, name)` → `task_waitpid(pid)` → collect exit status
- [ ] Log: `"[UTEST] test_syscall.exe: %s (exit=%d)"` with PASS/FAIL based on exit code
- [ ] Summary: `"[UTEST] %u passed, %u failed of %u user-mode tests"`
- [ ] Triggered by `test=1` in boot.conf (same gate as kernel unit tests)
- [ ] Commit: `"test: kernel test launcher -- run user-mode test_*.exe and collect results"`

**Test checkpoint:** Deploy `test_syscall.exe` to disk image. Boot with `test=1` → serial shows `[UTEST] test_syscall.exe: PASS`.

---

## 3. Syscall Test Binary

Exercise every implemented syscall from user mode.

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

**Test checkpoint:** `test_syscall.exe` runs, all assertions pass, exit code 0.

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

---

## 5. IPC Test Binary

Test inter-process communication from user mode.

- [ ] `user/test/test_ipc.c`:
  - `SYS_PIPE` → two handles, write to one, read from other
  - `SYS_SHMEM_CREATE` → returns handle
  - `SYS_SHMEM_MAP` → returns non-zero address
  - Write to shared memory, verify data
- [ ] Commit: `"test: user-mode IPC test binary (test_ipc.exe)"`

---

## 6. Process Lifecycle Test

Test fork, exec, waitpid from user mode.

- [ ] `user/test/test_process.c`:
  - `SYS_FORK` → parent gets child PID, child gets 0
  - `SYS_WAITPID` → parent waits for child, gets exit status
  - `SYS_EXEC` → load another binary (hello.exe)
  - `SYS_KILL` → kill a child process
- [ ] Commit: `"test: user-mode process lifecycle test (test_process.exe)"`

---

## 7. File I/O Test

Test handle-based file operations.

- [ ] `user/test/test_fileio.c`:
  - Open `C:\hello.txt` → valid handle
  - Read contents → matches expected
  - Close handle → handle becomes invalid
  - Open nonexistent file → INVALID_HANDLE_VALUE
  - Open directory object → enumerate entries
- [ ] Commit: `"test: user-mode file I/O test (test_fileio.exe)"`

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

---

## 9. Build Integration

Wire user-mode test binaries into `make test`.

- [ ] Makefile: compile `user/test/test_*.c` → `build/user/test_*.exe`
- [ ] Disk image build: copy test binaries to `C:\Impossible\System32\`
- [ ] `make test` target: include user-mode tests after kernel unit tests
- [ ] `scripts/test.sh`: parse serial for `[UTEST]` lines alongside `[TEST]` kernel lines
- [ ] Commit: `"test: build integration -- user-mode tests in make test and CI"`

---

## OS Comparison

| ⭐ | Feature                  | 🪟 Win11             | 🐧 Linux              | 🚀 Impossible OS        |
|----|--------------------------|-------------------|--------------------|----------------------|
| 💎 | User-mode test binaries  | ✅ HLK test.exe   | ✅ kselftest       | ⬜ §1–§7             |
| 💎 | Syscall coverage tests   | ✅ NtDll tests    | ✅ kselftest/ptrace | ⬜ §3                |
| 💎 | Automated test launcher  | ✅ HLK runner     | ✅ run_kselftest.sh | ⬜ §2                |
| ⭐ | Win32 API on non-Windows | ❌ N/A            | ❌ Wine tests only | ⬜ §8 🚀             |

---

## Verification

- [ ] `make test` → user-mode tests run after kernel tests, all pass.
- [ ] Break a syscall → user-mode test catches it → CI fails.
- [ ] Add new test binary → one file + one Makefile line → works in CI.
- [ ] Commit: `"test: user-mode test framework complete"`
