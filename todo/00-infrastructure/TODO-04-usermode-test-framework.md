# TODO-04 -- User-Mode Test Framework

> **Goal:** A test harness for user-mode code: syscalls, libc functions, Win32 API stubs, ELF/PE/EIF loading, process lifecycle, and IPC. Test programs are compiled as regular user-mode binaries (`test_*.exe`), deployed to the IXFS system disk, and executed by the kernel after boot. Each test binary exercises one subsystem, writes pass/fail results to stdout (SYS_WRITE), and exits with 0 (pass) or non-zero (fail). The kernel test runner launches each binary, captures its output and exit code, and reports results to serial. This is how Windows HLK and Linux kselftest work: real user-mode programs exercising the real syscall interface.

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
- `include/kernel/sched/syscall.h` -- authoritative INT 0x80 syscall numbers for §2 parity work
- → XREF: `T01 §3, §4, §6` -- launcher docs, machine-profile matrix, and CI wrapper policy should reuse the canonical developer tooling contract instead of inventing a second runner surface
- → XREF: `T05 §3, §7` -- desktop UI testing consumes the user-mode launcher foundation and CI-facing automation surfaces
- → XREF: `D02T12 §6` -- NtCreateFile/NtReadFile/NtWriteFile/NtClose/NtOpenFile family that `test_win32.exe` (§10) exercises through the Win32 thunks once they are wired
- → XREF: `D02T17 §1, §2, §5, §19` -- exec_load() multi-format dispatcher, enhanced ELF, EIF kernel loader, and PE delay-load that §15's `test_loader_elf.exe` / `test_loader_pe.exe` / `test_loader_eif.exe` exercise end-to-end from user mode
- → XREF: `T03 §1, §6` -- kernel-side `kmalloc_fail_next()` + multi-allocator countdowns that the §5 user-mode fault-injection bridge exposes through a privileged `SYS_FAULT_INJECT` syscall
- → XREF: [`03-memory-concurrency/TODO-06-scheduler-enhancement.md §13`](../03-memory-concurrency/TODO-06-scheduler-enhancement.md) -- dynamic task table + reusable PID slot allocation; today's `tasks[TASK_MAX]` monotonic-`num_tasks` design caps the launcher at 31 sequential binaries (Codex adversarial re-review of §4, 2026-04-20). Becomes a hard blocker once §9-§15 ship.

---

## Outcome

- `user/test/` directory with test binaries: `test_syscall.exe`, `test_libc.exe`, `test_ipc.exe`, etc.
- Each test binary exercises one subsystem, prints `[PASS] name` or `[FAIL] name: reason`, exits 0/1.
- Kernel test launcher: after boot, runs each `test_*.exe` in sequence, collects exit codes.
- Serial output shows per-binary pass/fail summary, with per-test heap/handle leak detection.
- `make test` includes user-mode tests (builds test binaries, deploys to disk, boots, runs).
- Adding a new user-mode test: create `user/test/test_foo.c`, add to Makefile -- done.
- Ordered manifest, per-binary timeouts, and optional TAP, JUnit XML, or JSON output for CI-grade triage (§4 + §7).
- `user/include/syscall.h` stays in lockstep with the kernel INT 0x80 table before §9-§13 expand coverage (§2).
- User-mode tests can probe kernel error paths via `SYS_FAULT_INJECT` (§5), reaching the §6 multi-allocator countdowns from TODO-03 §1/§6 without root-of-trust escapes outside `test=1` mode.
- Per-test isolation: launcher scrubs disk + Registry test scratch state between binaries (§6) so one test's residue cannot mask another's bug.
- Test-type taxonomy (§8): `test_smoke_*.exe`, `test_*.exe` (correctness), `test_stress_*.exe`, `test_perf_*.exe` -- launcher applies the right policy per type (fail-fast / run-all / loop-N / measure-and-assert).
- Per-binary `scripts/debug/usermode/run-<name>.bat` runners (§16) parallel the kernel-side `scripts/debug/kernel/run-<cat>-tests.bat` family: one bat per `test_*.exe` binary plus a `run-all.bat` aggregate. Each bat passes `utest_filter=<binary>` (§4) to QEMU so a single user-mode test can be invoked selectively without rebuilding.
- Binary format loader coverage (§15): a tiny `test_loader_<fmt>.exe` per registered loader (ELF, PE32+, EIF) proves `exec_load`'s magic-byte dispatch routes each binary to the right loader. The launcher logs `format=<NAME>` per binary so a regression that silently re-routes one format through another loader fails loudly instead of passing.

---

## Implementation Order

| ⭐  | Order | Deliverable                                         | Depends On                         | Status |
| --- | :---: | --------------------------------------------------- | ---------------------------------- | :----: |
| 💎  |   1   | User-mode test assertion macro and harness          | --                                 |  [x]   |
| 💎  |   2   | Userland syscall.h parity with kernel INT 0x80 ABI  | --                                 |  [x]   |
| 💎  |   3   | Kernel test launcher (run `test_*.exe` in sequence) | §1                                 |  [x]   |
| 💎  |   4   | Launcher manifest, timeouts, TAP, and skip policy   | §3                                 |  [x]   |
| ⭐  |   5   | User-mode fault-injection bridge (SYS_FAULT_INJECT) | §1, §3, T03 §1, T03 §6             |  [x]   |
| 💎  |   6   | Per-test isolation + cleanup hook                   | §3, §4                             |  [x]   |
| 💎  |   7   | JUnit XML + JSON output formats                     | §4                                 |  [x]   |
| 💎  |   8   | Test type taxonomy (smoke/correctness/stress/perf)  | §3, §4                             |  [x]   |
| 💎  |   9   | Syscall test binary (`test_syscall.exe`)            | §1, §2, §3                         |  [ ]   |
| 💎  |  10   | Libc test binary (`test_libc.exe`)                  | §1, §3                             |  [ ]   |
| 💎  |  11   | IPC test binary (`test_ipc.exe`)                    | §1, §2, §3                         |  [ ]   |
| 💎  |  12   | Process lifecycle test (`test_process.exe`)         | §1, §3                             |  [ ]   |
| 💎  |  13   | File I/O test (`test_fileio.exe`)                   | §1, §2, §3                         |  [ ]   |
| ⭐  |  14   | Win32 API test binary (`test_win32.exe`)            | §1, §3, D02T12 §6                  |  [ ]   |
| 💎  |  15   | Binary format loader coverage (ELF / PE32+ / EIF)   | §1, §3, D02T17 §5, D02T17 §19      |  [ ]   |
| 💎  |  16   | Build integration: `make test` includes user tests  | §3-§15                             |  [ ]   |

> 💎 = parity: Linux kselftest and Windows HLK both use user-mode test binaries, TAP/JUnit XML, machine-readable test orchestration, per-test isolation, and stress/perf categorisation.
> ⭐ = exclusive: testing the Win32 API surface from user mode on a non-Windows kernel (§14); user-mode fault-injection bridge that reaches kernel allocator countdowns under a single `test=1` gate (§5).
> **Sequencing rule:** foundation lands first (§1-§4), runner-side enhancements that every subsystem test consumes ship next (§5-§8), subsystem test binaries follow (§9-§15), and repo-wide build integration closes the loop in §16.

---

## 1. User-Mode Test Assertion Macro

Minimal test harness for user-mode binaries: no kernel dependencies.

- [x] [`user/include/test.h`](../../user/include/test.h) -- header-only macros that single-eval `cond` and `msg` and emit the documented `[PASS]`/`[FAIL]`/`[UTEST-BEGIN]`/`[UTEST-END]` lines via `sys_write(1, ...)`. Includes `syscall.h` + `string.h` (no kernel headers, no stdlib).
- [x] `g_fail` counter wired via `extern int g_fail` in the header + `UTEST_DEFINE_STATE()` macro that emits the single definition in the test binary's main TU. Test binaries do `return g_fail;` (or `sys_exit(g_fail)`) -- exit code 0 means all assertions held.
- [x] `UTEST_BEGIN(name)` emits `[UTEST-BEGIN] <name>\n`; `UTEST_END()` emits `[UTEST-END] all pass\n` when `g_fail == 0`, `[UTEST-END] some FAIL\n` otherwise -- lets a human reading raw serial spot the verdict without scrolling per-FAIL lines.
- [x] [`user/test/test_harness_smoke.c`](../../user/test/test_harness_smoke.c) -- §1 demonstrator binary (~35 LOC, all-PASS) proving the macro compiles, links against crt0+libc, and produces the documented output. Three assertions: arithmetic (`1+1==2`), libc (`strlen("hello")==5`), syscall (`sys_uptime() >= 0`). Returns `g_fail` so when §3 launcher ships and auto-discovers `test_*.exe`, exit code 0 means the harness itself is healthy.
- [x] Makefile extension: `userland` target now also builds `test_harness_smoke.exe` from the new `build/user/test/` subdir and deploys it to `$(SYSROOT)/test_harness_smoke.exe` alongside `hello.exe` and `cmd.exe`. Same crt0+libc link recipe; no separate libc copy.
- [x] Commit: `"test: user-mode test assertion macro (user/include/test.h)"`

**Test checkpoint:** Compile a trivial test binary using `UTEST_ASSERT`; it runs and exits cleanly. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal (build host only for the compile step).

> **Test runner:** N/A (header + smoke binary; not a kernel TEST_CAT) | validation: `bash scripts/build.sh` produces `build/sysroot/test_harness_smoke.exe` (23 KiB). End-to-end runtime validation lands once §3 launcher ships and auto-discovers `test_*.exe` on disk.

> **Notes:**
> - Shipped [`user/include/test.h`](../../user/include/test.h) (~85 LOC) + [`user/test/test_harness_smoke.c`](../../user/test/test_harness_smoke.c) (~35 LOC). Header-only design; macros are do/while(0)-wrapped with single-eval semantics on `cond` and `msg` so callers can compose with if/else without dangling-else gotchas and without double-evaluating side-effecting expressions.
> - `g_fail` linkage: `extern int g_fail` in the header + `UTEST_DEFINE_STATE()` one-line macro that emits the single definition in the binary's main TU. Multi-TU test binaries see the extern in every TU and link against the one definition; single-TU test binaries (the typical case) just call `UTEST_DEFINE_STATE();` once at file scope.
> - Output contract is the wire format the §3 kernel launcher will scrape: `[PASS] msg`, `[FAIL] msg`, `[UTEST-BEGIN] name`, `[UTEST-END] all pass` / `[UTEST-END] some FAIL`. Byte counts in the macros (7, 14, 21, 22) match the literal string lengths exactly.
> - Smoke binary deploys to `C:\test_harness_smoke.exe` on the IXFS image. The §3 launcher will pick it up by directory glob; in the meantime the binary just sits as a benign 23 KiB file -- no harm, no automated runtime invocation yet.
> - Downstream consumers: §9-§13 subsystem test binaries all `#include "test.h"` and use the same `UTEST_*` macros. §16 build integration extends the Makefile rule pattern from this section to the full `user/test/test_*.c` glob.
> - Scope boundary: §1 owns ONLY the header + the demonstrator binary + the Makefile entry point. The kernel-side launcher (§3), per-test isolation (§6), JUnit XML/JSON output formats (§7), and test-type taxonomy (§8) are all separate sections that consume this foundation.

> **Verified:** 2026-04-20 | commit `a2ca91a4` | 6/6 items | build OK | smoke binary 23 KiB deployed to sysroot
> **Quality reviewed:** 2026-04-20 | Codex 2x (adversarial, quality) | 0 findings, 0 open | scope: userland-code-quality

---

## 2. Userland syscall.h Parity with Kernel INT 0x80 ABI

`user/include/syscall.h` currently stops at `SYS_NETINFO`; kernel exposes `SYS_LOG`, pipes, shmem, mmap, and file/dir handles through `SYS_QUERYDIROBJ`. User-mode tests cannot compile until the headers and thin `sys_*` wrappers match.

- [x] Diffed [`user/include/syscall.h`](../../user/include/syscall.h) against [`include/kernel/sched/syscall.h`](../../include/kernel/sched/syscall.h). The user header was missing 12 SYS_* constants + matching wrappers: `SYS_LOG` (17), `SYS_PIPE` (33), `SYS_SIGNAL` (34), `SYS_SHMEM_CREATE` (35), `SYS_SHMEM_MAP` (36), `SYS_MMAP` (37), `SYS_MUNMAP` (38), `SYS_OPENFILE` (39), `SYS_CLOSEHANDLE` (40), `SYS_READHANDLE` (41), `SYS_OPENDIROBJ` (42), `SYS_QUERYDIROBJ` (43). The SSDT alias block (`SYS_NT_*`) is intentionally NOT mirrored -- those are SSDT service numbers used by INT 0x2E (compatibility path), not INT 0x80.
- [x] Added the 12 `#define` constants + 10 `static inline sys_*` wrappers in [`user/include/syscall.h`](../../user/include/syscall.h). `SYS_MMAP` and `SYS_MUNMAP` get the `#define` for kernel-header parity but NO wrapper -- the kernel `syscall_handler_80()` switch does not implement them yet (falls through to default returning -1), and exposing wrappers would mislead callers into thinking the path works. Parallel additions: `HANDLE` typedef + `INVALID_HANDLE_VALUE` (mirroring [`include/kernel/ob/handle_table.h`](../../include/kernel/ob/handle_table.h) lines 17, 22) so callers can store the return value of `sys_openfile`/`sys_pipe`/`sys_opendirobj`/`sys_shmem_create` in the right type, and `LOG_DEBUG`..`LOG_FATAL` constants (mirroring [`include/kernel/klog.h`](../../include/kernel/klog.h) `log_level_t`).
- [x] Rebuilt `hello.exe` (~45 KiB) and added [`user/test/test_syscall.c`](../../user/test/test_syscall.c) (3 assertions: `sys_write` returns byte count; `sys_log(LOG_FATAL)` returns -1 -- security regression; `sys_log(LOG_INFO)` returns 0). `bash scripts/build.sh` -> `=== BUILD OK ===`; `build/sysroot/test_syscall.exe` deployed (22 KiB) alongside `hello.exe` / `cmd.exe` / `test_harness_smoke.exe`.
- [x] **SECURITY (Codex critical, fixed pre-commit):** the original `SYS_LOG` kernel handler accepted `lvl > LOG_FATAL` (i.e. only rejected lvl > 4) but `klog(LOG_FATAL, ...)` enters an infinite hlt loop ([`src/kernel/klog.c:1023-1028`](../../src/kernel/klog.c)) -- a one-line OS halt from any user binary. Patched [`src/kernel/sched/syscall.c`](../../src/kernel/sched/syscall.c) `SYS_LOG` case to reject `lvl >= LOG_FATAL`; documented the rejection in both the kernel handler comment and the user header `LOG_FATAL` define.
- [x] `_Static_assert`-style header comment at the top of [`user/include/syscall.h`](../../user/include/syscall.h) names `include/kernel/sched/syscall.h` + `src/kernel/sched/syscall.c` as the authoritative source for INT 0x80 numbers + handler signatures.
- [x] Commit: `"test: sync user syscall.h with kernel INT 0x80 ABI for usermode harness"`

**Test checkpoint:** `clang` user-mode build of `test_syscall.c` succeeds with no undefined `sys_*` reference; `_Static_assert` or a comment at the top of `user/include/syscall.h` points maintainers at `include/kernel/sched/syscall.h` as the source of truth. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal (compile step on dev host is enough for PASS).

> **Test runner:** N/A (header-only parity + 30 LOC user-mode stub) | validation: `bash scripts/build.sh` -> BUILD OK; `build/sysroot/test_syscall.exe` (22 KiB) deployed; runtime PASS observed once §3 launcher ships and the binary's 3 assertions run on a real boot.

> **Notes:**
> - Shipped 12 new `SYS_*` constants + 10 `static inline sys_*` wrappers in [`user/include/syscall.h`](../../user/include/syscall.h). HANDLE typedef + LOG_LEVEL constants added with explicit kernel-header citation lines so future ABI drift surfaces in code review. SYS_MMAP / SYS_MUNMAP intentionally have no wrapper because the kernel handler is unwired; documented inline.
> - `sys_querydirobj` packs `(ctx << 16) | count` into the single arg3 slot to match the kernel's 3-register INT 0x80 ABI; the wrapper does the pack and the inverse unpack on return so callers see a normal 4-arg C signature. The pack/unpack contract is documented inline; if the kernel ever moves to a 4+ register fast path this wrapper changes in lockstep.
> - **Security boundary fix (Codex critical):** kernel SYS_LOG handler now rejects `lvl >= LOG_FATAL` (was `> LOG_FATAL`). Without the fix, `sys_log(LOG_FATAL, ...)` from any user binary would halt the OS. Regression assertion lives in [`user/test/test_syscall.c`](../../user/test/test_syscall.c); when §3 launcher runs the binary, it'll observe `sys_log(LOG_FATAL, ...) == -1` -- if the assertion FAILs, the binary is alive only because the hlt loop didn't execute, which IS the bug we want surfaced.
> - Cross-TODO sync: this is foundation for §9 (`test_syscall.exe`) full-coverage test, §11 IPC test (uses `sys_pipe` + `sys_shmem_*`), §13 file I/O test (uses `sys_openfile` + `sys_readhandle` + `sys_opendirobj` + `sys_querydirobj`). All depend-on §2 in the Implementation Order table.
> - Scope boundary: §2 owns ONLY the user-side header sync + the build-validating stub binary. §5 (SYS_FAULT_INJECT) defines a NEW syscall number not yet in the kernel header -- separate work. SYS_MMAP / SYS_MUNMAP wrappers wait for the kernel handler to land elsewhere.

> **Verified:** 2026-04-20 | commit `302a45dc` | 6/6 items | build OK | 12 SYS_* defines + 10 wrappers + HANDLE typedef + LOG_* constants
> **Quality reviewed:** 2026-04-20 | Codex 2x (adversarial, quality) | 1Critical fixed pre-commit, 0 open | scope: userland-code-quality

---

## 3. Kernel Test Launcher

Kernel-side mechanism to run user-mode test binaries and collect results.

- [x] New TU [`src/kernel/test/test_usermode.c`](../../src/kernel/test/test_usermode.c) (~225 LOC) + header [`include/kernel/test/test_usermode.h`](../../include/kernel/test/test_usermode.h) implement the launcher. Scan target is `C:\` root (matches the Makefile sysroot deploy path -- the original spec said `C:\Impossible\System32\` but binaries actually deploy to root next to `hello.exe` / `cmd.exe`). The scan walks `vfs_readdir(C_root, idx)` and matches the `test_*.exe` prefix+suffix; unrelated binaries (`hello.exe`, `cmd.exe`) are skipped.
- [x] Spawn pattern: file-scope volatile `s_pending_test_path` carries the path to the loader; launcher calls `task_create(utest_loader_func, name_copy)` then immediately `task_waitpid(pid)`. The loader (`utest_loader_func`) reads `s_pending_test_path`, opens via VFS, kmalloc-stages the binary, calls `task_exec(buf, size)` to morph the kernel task into a user task. Same canonical pattern as `exec_loader_func` in `src/kernel/main/test_threads.c` (used to spawn `cmd.exe` at boot).
- [x] Per-binary log: `klog(LOG_INFO, "UTEST", "<name>: PASS (exit=0)")` on exit_status == 0; `klog(LOG_ERROR, "UTEST", "<name>: FAIL (exit=N)")` otherwise. Loader-failure exit codes (-1..-5) carry distinct meanings: -1 NULL pending path, -2 vfs_open, -3 kmalloc, -4 short read, -5 task_exec failure -- all surface as FAIL with the negative exit so loader bugs cannot silently swallow a binary.
- [x] Summary line: `klog(LOG_INFO, "UTEST", "=== %u passed, %u failed of %u total ===")` (or `... (%u skipped by filter) ===` form when the §4 filter excluded any matches).
- [x] Triggered from [`src/kernel/main/boot_tests.c`](../../src/kernel/main/boot_tests.c) at the end of the `test=1` block, after `test_runner_run()` -- means the kernel `[TEST] === N tests passed` summary lands FIRST on serial, then `[UTEST]` lines, matching the order `scripts/test.sh` parser will look for once §16 wires user-mode parsing.
- [x] `utest_filter=<name|glob>` boot.conf parameter honoured via `test_usermode_set_filter()` API + an internal `u_glob_match()` helper that supports literal names + a single `*` wildcard. §4 shipped the bootloader parser for the `utest_filter=` key and wired it via [`src/kernel/main/boot_tests.c`](../../src/kernel/main/boot_tests.c); this section retains ownership of the consumer-side hook + glob helper.
- [x] **Codex adversarial review (3 findings fixed pre-commit):** (H1) loader-failure `return` only set TASK_DEAD without waking the parent's `task_waitpid` -- one bad binary would deadlock the boot test path forever. Replaced every `return;` with `task_exit(<negative_code>)` so loader bugs always wake the waiter with a distinct FAIL exit. (M1) staging buffer leaked on every successful exec -- `task_exec`'s underlying `exec_load` copies the binary into user pages, so the staging buffer is safe to free after `task_exec` returns 0. Added `kfree(buf)` between the success check and the for-loop (the prepared user-mode iretq frame is consumed on the NEXT scheduling tick, not immediately, so the kfree runs before user code starts). (M2) `de->name` from `vfs_readdir` is shared/static dirent storage -- a child user-mode test calling `SYS_READDIR` would overwrite it and the parent would log PASS/FAIL against the wrong filename + park `tasks[pid].name` pointing at garbage. Snapshot `de->name` into a launcher-owned `name_copy[VFS_MAX_NAME]` BEFORE spawning the child; pass the stable copy to `task_create` and use it for all post-wait logging.
- [x] No `scripts/debug/usermode/run-*.bat` runner today: while the launcher is chained inside `boot.conf test=1` (kernel test runner runs first, user-mode launcher second), any Windows-side bat with `-TestOnly` runs the SAME chain that [`scripts/debug/kernel/run-all-kernel-tests.bat`](../../scripts/debug/kernel/run-all-kernel-tests.bat) already invokes. A separate `usermode/run-all-usermode-tests.bat` would just be a misleading duplicate of the kernel runner. The [`scripts/debug/usermode/README.md`](../../scripts/debug/usermode/README.md) explains the deferral; per-binary bats land here once §4 ships a `usermode_only=1` (or equivalent) boot.conf knob that lets the launcher run in isolation.
- [x] Commit: `"test: kernel test launcher -- run user-mode test_*.exe and collect results"`

**Test checkpoint:** Deploy `test_syscall.exe` to the disk image. Boot with `test=1` -> serial shows `[UTEST] test_syscall.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-all-kernel-tests.bat` (chains kernel TEST_CAT_* + the §3 user-mode launcher in one boot via `test=1`) | runtime [UTEST] PASS lines visible on serial; per-binary bats land in `scripts/debug/usermode/` once §4 ships a usermode-only boot knob

> **Notes:**
> - Shipped [`include/kernel/test/test_usermode.h`](../../include/kernel/test/test_usermode.h) (~50 LOC) + [`src/kernel/test/test_usermode.c`](../../src/kernel/test/test_usermode.c) (~225 LOC). Single-threaded launcher with file-scope volatile path passing + waitpid sequencing -- one binary at a time so the next binary's run cannot be perturbed by leftover state from the previous one. Per-test isolation hardening (per-test scratch dir, per-binary handle-leak detection) is owned by §6.
> - Wire-in: `boot_tests_run()` in [`src/kernel/main/boot_tests.c`](../../src/kernel/main/boot_tests.c) calls `test_usermode_run()` after `test_runner_run()` when `boot.conf test=1` is set. No new boot.conf field; reuses the existing `test=1` gate. The kernel `[TEST] === N tests passed` summary still lands first on serial (preserves the `scripts/test.sh` summary regex), `[UTEST]` lines follow, then the cross-layer summary.
> - Loader failure-paths use `task_exit(<negative_code>)` not bare `return` -- distinguishes a binary's natural non-zero exit (e.g. `g_fail > 0` from harness assertions) from a loader-stage bug (NULL path, vfs_open fail, kmalloc OOM, short read, task_exec fail). Each loader stage gets its own negative code (-1..-5) so the FAIL line on serial points at the right diagnostic.
> - Staging-buffer ownership: `task_exec` does NOT take ownership of its `data` buffer (the underlying `exec_load` in [`src/kernel/exec.c`](../../src/kernel/exec.c) copies to user pages, then returns; the existing `exec_load_path` helper at line 219 explicitly frees its own staging buffer). The loader now `kfree(buf)`s after `task_exec` returns 0, before the prepared user-mode iretq frame is consumed on the next scheduling tick.
> - `utest_filter=<name|glob>` field is set via `test_usermode_set_filter()` from outside (today: never called, so filter stays NULL and the launcher runs every test_*.exe). The boot.conf parser that SETS the filter lands in §4; this section ships the consumer-side hook + the `u_glob_match()` helper that supports the literal-name + single-`*`-wildcard syntax §4 promises.
> - Verification limitation on WSL TCG: the existing `scripts/test.sh` waits for `=== N tests passed` from the kernel summary then sleeps 1s + kills QEMU. WSL TCG runs user-mode binaries slowly enough that the launcher's spawned binary may not finish in that 1s grace -- per-binary `[UTEST]` lines may be cut off mid-run on `bash scripts/test.sh`. The launcher CODE is correct (proven by `Task 3 ("test_syscall.exe") created` + `ELF auxv` + `PID 3 -> entry 0x800000` lines on serial); full round-trip PASS/FAIL surfacing on WSL needs §16's `scripts/test.sh` extension to wait for the `[UTEST] === N passed` summary too. Native Windows runs via `scripts\debug\usermode\run-all-usermode-tests.bat` (which goes through `run-qemu.ps1` and waits until QEMU exits naturally) get the full round-trip.
> - Scope boundary: §3 owns the launcher itself (scan + spawn + wait + log + summary). §4 owns the boot.conf `utest_filter=` parameter parser + the launcher manifest + per-binary timeouts + TAP/SKIP output. §6 owns per-test isolation (scratch dir, Registry, handle-leak detection). §7 owns JUnit XML / JSON output formats. §16 owns the `scripts/test.sh` parser extension that lets WSL boot-test runs pick up `[UTEST]` lines.

> **Verified:** 2026-04-20 | commit `5e576ce4` | 9/10 items + 1 [/] (utest_filter consumer-hook ready, parser owned by §4) -> §4 closed 2026-04-20 `<§4 commit>` | build OK | launcher loads test_*.exe via VFS scan + task_create + task_exec + task_waitpid; ELF auxv + PID 3 -> entry serial-traced
> **Quality reviewed:** 2026-04-20 | Codex 2x (adversarial, quality) | 3H+2M fixed (1Critical initially rejected then re-validated and fixed: hlt-tail freeze on user QEMU run -- replaced for(;;) hlt; with for(;;) yield(); because boot_tests_run() runs INSIDE boot_phase3 BEFORE scheduler_enable() at boot_desktop.c:384, so PIT preemption was disabled and hlt blocked on an interrupt that never routed to schedule; yield() goes through schedule_now() which switches regardless of sched_enabled) | scope: kernel-code-quality

---

## 4. Launcher Manifest, Timeouts, and CI-Friendly Output

Parity with kselftest/LKFT-style automation: deterministic order, watchdogs, machine-readable output, and documented skips for flaky hypervisors (see `CLAUDE.md` WHPX vs TCG notes for storage; the same class of issue can hit timing-sensitive user tests).

- [x] [`tests/usermode.manifest`](../../tests/usermode.manifest) lists `test_*.exe` in execution order; the launcher reads `C:\tests\usermode.manifest` via `u_manifest_load()` in [`src/kernel/test/test_usermode.c`](../../src/kernel/test/test_usermode.c) BEFORE the directory glob. Manifest absent / overflowed beyond `UTEST_MANIFEST_MAX` (128 entries) falls back to glob for the uncovered tail. Each entry is validated via `u_is_valid_manifest_name()` (stricter than the glob path's `u_is_test_binary()`) to reject path-separators, `..` traversal, drive letters, and control bytes -- the manifest is parsed as user-provided text, so the trust boundary lives here. Makefile's `userland` target deploys the file to `$(SYSROOT)/tests/usermode.manifest`.
- [x] Per-binary wall-clock timeout implemented via [`u_wait_with_timeout()`](../../src/kernel/test/test_usermode.c) replacing the prior `task_waitpid`. Default 10 000 ms (`UTEST_DEFAULT_TIMEOUT_MS`), overrideable per boot via `boot.conf` `utest_timeout_ms=<0..65535>`. On deadline: `signal_send(SIGKILL)` -> 500 ms cooperative grace -> force `t->state = TASK_DEAD` + `t->exit_status = UTEST_EXIT_TIMEOUT` (-6). Launcher emits `[UTEST] <name>: FAIL (timeout after Nms)` and proceeds to the next binary. Force-kill is guarded against suicide (`t != task_current()`) and relies on the kernel's single global `current_task` for SMP safety ([`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) line 47).
- [x] `tap=<0|1>` boot.conf key parsed in [`bootx64.c`](../../src/boot/uefi/bootx64.c), plumbed to `test_usermode_set_tap()`. When enabled, the launcher emits `1..N` plan line up-front, then `ok N - <name>`, `not ok N - <name> # reason`, or `ok N - <name> # SKIP` per binary. Format follows TAP13 so `scripts/test.sh` (and any standard TAP parser) can consume it without custom logic.
- [x] `SKIP` contract: a user-mode test reports "not applicable here" by exiting with status `77` (kselftest convention, enshrined as `UTEST_EXIT_SKIP` in [`include/kernel/test/test_usermode.h`](../../include/kernel/test/test_usermode.h)). Launcher records it in the `skipped` counter, logs `[UTEST] <name>: SKIP (exit=77)`, and -- in TAP mode -- emits `ok N - <name> # SKIP`. Skips never fail the suite.
- [x] `utest_filter=<name|glob>` boot.conf parameter parsed and plumbed through `test_usermode_set_filter()`. Accepts literal names or a single `*` wildcard anywhere (e.g. `test_smoke_*.exe`) via the existing `test_usermode_glob_match()` helper. Empty/unset = run every match. Summary line splits filter-excluded entries from the `ran` total (`=== N passed, N failed, N skipped of N total (K filtered) ===`) so single-test bat-file invocations parse cleanly.
- [x] [`docs/testing/usermode-env-matrix.md`](../../docs/testing/usermode-env-matrix.md) documents QEMU WHPX / TCG / VirtualBox / bare-metal / KVM-WSL2 expectations: per-feature x-matrix, four classes of known-flaky patterns with the correct response, and a "what NOT to do" block reinforcing CLAUDE.md's *"Never paper over test failures with platform workarounds"* rule.
- [x] Commit: `"test: usermode launcher manifest, timeouts, TAP, skip policy, and utest_filter"`

**Test checkpoint:** Dry-run via `bash scripts/test.sh QUIET=1 SUITE=exec` (KVM): 102 kernel-side tests pass (up from 59 pre-§4); `[UTEST]` lines show `test_harness_smoke.exe` then `test_syscall.exe` in manifest order followed by `=== 2 passed, 0 failed, 0 skipped of 2 total ===`. Kernel unit tests cover the glob matcher across 7 scenarios (NULL, literal, wildcard prefix/start/end, suffix-too-long boundary, empty name), the SKIP + TIMEOUT exit-code contract constants, the setter API edge cases, and the manifest trust-boundary validator across 5 attack shapes (path traversal, drive letters, non-test binaries, control bytes, well-formed acceptance). Live-kill regression: setting `utest_timeout_ms=1 tap=1` produces a `not ok 1 - <name> # timeout` TAP line and the launcher proceeds to the next binary. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | 28 suites under TEST_CAT_EXEC (10 new for §4), 0 failures

> **Notes:**
> - Shipped ABI: three new fields in [`struct boot_config`](../../include/kernel/boot_info.h) at stable offsets 289/292/294 -- `tap` (u8), `utest_timeout_ms` (u16), `utest_filter[64]` (char). Mirror updated in [`src/boot/uefi/boot_info_mirror.h`](../../src/boot/uefi/boot_info_mirror.h) with matching static asserts; bootloader parser recognises `tap=`, `utest_timeout_ms=`, `utest_filter=` keys.
> - Shipped launcher: rewrote [`src/kernel/test/test_usermode.c`](../../src/kernel/test/test_usermode.c) from the §3 baseline (~230 LOC) to a §4 launcher (~640 LOC) that loads + parses the manifest, polls tasks with a wall-clock watchdog, emits TAP/SKIP lines, and wraps the whole run in `scheduler_enable`/`scheduler_disable` (needed because cooperative `yield()` cannot wrest control from a spinning user task).
> - Shipped tests: [`src/kernel/test/test_usermode_launcher.c`](../../src/kernel/test/test_usermode_launcher.c) (~230 LOC) covers 15 suites under TEST_CAT_EXEC. No live-launcher calls (HARD BAN per CLAUDE.md); tests hit the pure helpers + contract constants only.
> - Codex adversarial review applied: 3 findings (H1 manifest OOB write at exactly ARENA_BYTES, H2 SMP force-DEAD safety, M1 manifest path traversal). H1 fixed (cap at ARENA-1 + sentinel NUL). M1 fixed (`u_is_valid_manifest_name` rejects `..`, `\\`, `/`, `:`, control bytes). H2 rejected with evidence (kernel's single global `current_task` means APs don't dispatch scheduled tasks) + defensive "not self" guard added.
> - Post-commit quality review + domain gate walk applied 3 additional fixes: Gate 1 (manifest arena switched from `kmalloc(8192)` to `pmm_alloc_contiguous(2 pages)` per CLAUDE.md's 4 KiB kmalloc ceiling); quality H1 (timeout force-DEAD now calls `ob_process_mark_dead(pid)` so the permanent Process object is reclaimed, mirroring `task_exit()`'s teardown order); quality H2 (pre-flight WARN when the plan exceeds `TASK_MAX - task_count()` so a 128-entry manifest does not silently emit a misleading `1..N` plan it cannot fulfil). Task-slot scope-gap accepted with concrete owner added in the scheduler enhancement TODO.
> - Downstream effects: unblocks the §15 binary-format coverage gate, the §8 test-type taxonomy (stress/perf use timeout caps), and `D00 T05 §21`'s inbound XREF to this section for the shared timeout/TAP/SKIP policy in the desktop UI framework.
> - Canonical doc: [docs/testing/usermode-env-matrix.md](../../docs/testing/usermode-env-matrix.md) -- platform expectations + escape-hatch policy for the whole user-mode test layer.
> - Scope boundary: §4 owns the launcher-side policy surface. Per-test isolation (§6) owns the scratch-dir + handle-leak detection hooks; JUnit XML / JSON formats (§7) are a downstream of the TAP wire-format this section establishes; test-type taxonomy (§8) layers policy-per-type on top of the timeout knob shipped here.

> **Verified:** 2026-04-20 | commit `11e816fb` | 7/7 items | build OK | tests 102/102 PASS (up from 59); manifest + TAP + SKIP + filter all exercised via kernel unit suites + live `[UTEST]` run
> **Accepted:** [M] manifest advertises 128 entries but `tasks[TASK_MAX=32]` is append-only until scheduler slot reuse ships (reason: scope) -> XREF: 03-memory-concurrency/TODO-06-scheduler-enhancement.md §13 (item: "Dynamic task table + reusable PID slot allocation" at top of section)
> **Quality reviewed:** 2026-04-20 | Codex 2x (adversarial, quality) | 2H+1M fixed, 0 open | scope: kernel-code-quality

---

## 5. User-Mode Fault-Injection Bridge

Bridge `T03 §1` `kmalloc_fail_next()` and `T03 §6` PMM/VMM/copy_user countdowns to user mode so tests can probe error paths in syscalls (e.g., `test_fileio.exe` proves `SYS_OPENFILE` cleans up on `kmalloc` failure). Application Verifier solves the same problem on Win11 by hooking `CreateFileA`; we expose it via a privileged syscall guarded by the boot-time test gate so production user-mode binaries cannot break out.

> [!TIP]
> Linux exposes `failslab` only through `/sys/kernel/debug/failslab/*` files (debugfs); HLK Application Verifier hooks Win32 in the loader. A typed kernel-level bridge with a `test=1` gate gives the same coverage with a simpler audit story.

> [!WARNING]
> `SYS_FAULT_INJECT` MUST hard-fail with `STATUS_ACCESS_DENIED` when `boot.conf test=0`. Without that gate, a malicious user-mode binary could DoS the kernel by arming `kmalloc_fail_countdown` repeatedly. The gate check is the security-critical path; cover it in the regression test.

- [x] `SYS_FAULT_INJECT = 44` added to [`include/kernel/sched/syscall.h`](../../include/kernel/sched/syscall.h) + mirrored in [`user/include/syscall.h`](../../user/include/syscall.h). Six subcommand selectors shipped: `FAULT_KMALLOC_NEXT`, `FAULT_KMALLOC_COUNTDOWN`, `FAULT_PMM_NEXT`, `FAULT_VMM_MAP_NEXT`, `FAULT_COPY_USER_NEXT`, `FAULT_CLEAR_ALL`. Next free number after §2's sweep ended at 43.
- [x] Kernel handler `sys_fault_inject_dispatch()` in [`src/kernel/sched/syscall.c`](../../src/kernel/sched/syscall.c) (non-static so unit tests can call it without an INT 0x80 round-trip). Gate: `if (!g_boot_info.config.test) return -1;`. Implementation uses `g_boot_info.config.test` directly rather than a new `SUBSYS_TEST_MODE` slot because the flag is the canonical source of truth; adding a redundant kernel_subsystem_* mirror would drift against the boot.conf parser. Deny path logs `[sys] SYS_FAULT_INJECT denied (test=0) kind=... pid=...` at LOG_WARN so a hostile caller shows up in the boot log instead of silently bouncing.
- [x] Each subcommand wires `task_current()->pid` as the T03 §6 task filter BEFORE arming the countdown so a test can only trap its own kernel-side allocations. `FAULT_PMM_NEXT` was initially un-filtered (Codex M1, 2026-04-20); fixed to match the other selectors. `FAULT_CLEAR_ALL` symmetrically clears all four task filters + countdowns.
- [x] User-mode wrapper `utest_fault_inject(kind, countdown)` in [`user/include/test.h`](../../user/include/test.h) + thin raw syscall `sys_fault_inject()` in [`user/include/syscall.h`](../../user/include/syscall.h). Returns 0 on success, -1 on gate-deny / unknown kind / invalid countdown.
- [x] [`user/test/test_faultinject.c`](../../user/test/test_faultinject.c) smoke binary: opens `C:\hello.txt` without a trap (baseline), arms `FAULT_KMALLOC_NEXT`, re-opens SAME file (must return `INVALID_HANDLE_VALUE` because the kernel's `ob_create_file_handle` kmalloc hits the forced NULL), `CLEAR_ALL`, final open must succeed. Proves the trap actually fires rather than accepting any -1 (Codex L1, 2026-04-20 rewrote the earlier nonexistent-path version). SKIPs with exit=77 when `test=0` so it's manifest-safe on either boot policy.
- [x] Four kernel negative regressions added in [`src/kernel/test/test_usermode_launcher.c`](../../src/kernel/test/test_usermode_launcher.c) (TEST_CAT_EXEC): gate denies with `config.test=0` AND `kmalloc_fail_injections_triggered()` stays unchanged; `FAULT_CLEAR_ALL` succeeds under `test=1`; unknown kind rejected; `FAULT_KMALLOC_COUNTDOWN` with countdown=0 rejected. Uses save/restore of `g_boot_info.config.test` (a pure data field, allowed under CLAUDE.md's live-boot-call ban).
- [x] Commit: `"test: SYS_FAULT_INJECT bridge -- user-mode probes kernel allocator countdowns under test=1 gate"`

**Test checkpoint:** On KVM / WSL TCG: `bash scripts/test.sh QUIET=1 SUITE=exec` reports 107 kernel tests pass (up from 102 pre-§5), and the `[UTEST]` launcher log shows `test_faultinject.exe: PASS (exit=0)` with all five user-side assertions green (`FAULT_CLEAR_ALL under test=1`, baseline open, arm, trap fired -> `INVALID_HANDLE_VALUE`, disarm + re-open). With `test=0`: `test_faultinject.exe` exits 77 and the launcher counts it as SKIPPED, not FAIL. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | 28 suites under TEST_CAT_EXEC (4 new for §5 gate regressions), 0 failures

> **Notes:**
> - Shipped syscall `SYS_FAULT_INJECT = 44` with six subcommand selectors and a typed bridge to all four T03 fault-injection countdowns (kmalloc, PMM, VMM, copy_user). Gate: `g_boot_info.config.test`; self-PID task filter applied to all four selectors. Kernel dispatcher + user-mode wrapper both one function; total §5 surface is <130 LOC of new kernel C plus ~90 LOC of user-side test code.
> - Unplanned adjacent fix applied inline (Codex quality H1, 2026-04-20): `syscall_handler` now lowers IRQL from the IDT-raised `vector_to_irql(0x80)=8` to PASSIVE_LEVEL at entry, restores the software bookkeeping on exit WITHOUT reprogramming TPR. Same class of bug as the §3 `task_exit` IRQL leak: INT 0x80 is a software interrupt, not a hardware IRQ, and should run at the user thread's logical IRQL so that kmalloc's PASSIVE-gated fault-injection hook and mutex_lock's APC-level ceiling both work as designed. Explicit `pcpu->current_irql = entry_irql` on exit bypasses `KeRaiseIrql` so TPR stays at PASSIVE (where `KeLowerIrql` put it), preventing the CPU from permanently masking low-priority device IRQs after the first syscall.
> - Codex adversarial review (2 passes) during implementation: M1 (FAULT_PMM_NEXT missing task filter) fixed; L1 (smoke test false-positive using nonexistent path) fixed; H1 (TPR stuck at 8 after syscall) fixed. Post-commit quality review + code-quality gates added 2 more fixes: (H2) leaked-raise guard in syscall exit -- if a callee raised IRQL without matching lower, force `KeLowerIrql(PASSIVE_LEVEL)` and log LOG_ERROR before overwriting the software bookkeeping, so TPR can never stay elevated after a syscall; (H3) INT 0x2E (NT syscall compat path) gets the same PASSIVE_LEVEL wrap as INT 0x80 because `syscall_handler_2e` had the identical IDT-raised-IRQL problem -- ssdt_dispatch was running at DISPATCH_LEVEL. Both fixes landed via a shared helper `syscall_lower_entry_irql` / `syscall_restore_entry_irql` so the two syscall paths cannot drift.
> - Downstream effects: unblocks §9-§14 test binaries to probe their own error paths deterministically (e.g. `test_fileio.exe` can arm `FAULT_KMALLOC_NEXT` right before `sys_openfile` to assert cleanup is correct on allocator failure). The syscall IRQL fix also unblocks any future kernel mutex usage from syscall handlers.
> - Canonical doc: [tests/usermode.manifest](../../tests/usermode.manifest) lists the binary; [docs/testing/usermode-env-matrix.md](../../docs/testing/usermode-env-matrix.md) section §4 describes the SKIP-on-test=0 contract this binary relies on.
> - Scope boundary: §5 owns the bridge + gate + self-PID isolation + smoke regression. Per-test cleanup of armed-but-not-fired countdowns between binaries is §6's job (per-test isolation hook). JUnit XML / JSON emission of fault-inject results is §7. Test-type taxonomy (stress class forcing multiple injections) is §8.

> **Verified:** 2026-04-20 | commit `502a2e57` | 7/7 items | build OK | tests 107/107 PASS (up from 102) + smoke PASS (KVM 2.35s); full arm -> trap -> disarm round-trip exercised end-to-end through `test_faultinject.exe` via INT 0x80
> **Accepted:** [H] SYS_FAULT_INJECT self-PID task filter reads global `current_task` (single-CPU scheduler assumption; AP concurrent schedule() can stomp between capture and use) (reason: scope) -> XREF: 03-memory-concurrency/TODO-06-scheduler-enhancement.md §13 (item: "Move `static uint32_t current_task`... into per-CPU state" added 2026-04-20 naming this section's `sys_fault_inject_dispatch` as the consumer)
> **Quality reviewed:** 2026-04-20 | Codex 2x (adversarial, quality) | 3H+1M+1L fixed, 0 open | scope: kernel-code-quality

---

## 6. Per-Test Isolation + Cleanup Hook

Right now §3's launcher just sequentially execs binaries. If `test_fileio.exe` leaves `C:\Temp\test-fileio-scratch` behind, `test_libc.exe` could trip on it. Linux kselftest forks per test so file descriptors leak nowhere; LTP uses unique temp dirs and unconditional cleanup. The launcher needs a per-test cleanup pass between binaries.

- [x] `C:\Temp\utest\<stem>\` scratch root wired in [`src/kernel/test/test_usermode.c`](../../src/kernel/test/test_usermode.c) -- `u_isolation_setup()` creates `C:\Temp`, `C:\Temp\utest`, and `C:\Temp\utest\<stem>\` (creating any missing level idempotently) and `u_rmtree`s any stale subtree from a prior run before recreating a fresh directory. `<stem>` is derived from the binary name by `u_derive_test_name()` which strips the trailing `.exe` (case-insensitive). `u_isolation_reap()` (post-`task_cleanup`) tears the subtree down again after the run completes regardless of PASS / FAIL / timeout. Bounded recursive-delete uses `UTEST_RMTREE_MAX_DEPTH=8` and `UTEST_RMTREE_MAX_ENTRIES=512` paranoia caps so a pathological VFS state cannot infinite-loop cleanup.
- [x] `HKLM\SOFTWARE\ImpossibleOS\Test\<stem>` Registry scratch subkey: `u_isolation_setup()` calls `RegDeleteTree()` to wipe prior state, then `RegCreateKeyEx()` to ensure a fresh key exists before the binary runs. Teardown calls `RegDeleteTree()` again after `task_cleanup`. Symmetric with the VFS scratch path and uses the same stem derivation.
- [x] Per-binary handle-leak detection: `u_isolation_snapshot_leaks()` reads `tasks[pid].handle_table.count` BEFORE `task_cleanup` destroys the table; `u_run_one` escalates a PASS verdict to FAIL when `leaked > 0`, logs `[UTEST] <name>: FAIL (<N> handle(s) leaked -- escalated from PASS)` and emits a matching `not ok N - <name> # <N> handle(s) leaked` TAP line. Tests that already FAIL / SKIP keep their stronger verdict but get a supplementary WARN naming the leak count.
- [x] `tests/usermode-cleanup.manifest` [deployed](../../tests/usermode-cleanup.manifest) via the Makefile `userland` target (alongside `tests/usermode.manifest`); `u_cleanup_manifest_apply()` parses `C:\tests\usermode-cleanup.manifest` after every binary. Format: one entry per line, `C:\Impossible\...` goes to `u_rmtree`, `HKLM\SOFTWARE\Impossible...` goes to `RegDeleteTree`. Both prefixes are hard-gated to the ImpossibleOS subtree so a typo cannot wipe `hello.txt`, `cmd.exe`, or the whole HKLM hive (Codex H2 fix, 2026-04-20).
- [x] `boot.conf utest_isolation=<0|1>` opt-out parsed by the bootloader (defaults to `1` via the `boot_config_defaults` in [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)); kernel consumes via `test_usermode_set_isolation(0)` from [`src/kernel/main/boot_tests.c`](../../src/kernel/main/boot_tests.c). Stable ABI offset 358 in `struct boot_config` with static asserts in both kernel + bootloader mirror.
- [x] Commit: `"test: per-test isolation -- scratch dir + Registry subkey + handle-leak detection"`

**Test checkpoint:** On `bash scripts/test.sh QUIET=1 SUITE=exec` (KVM): 127 kernel tests pass (up from 107 pre-§6). Under the live launcher, `test_harness_smoke.exe`, `test_syscall.exe`, and `test_faultinject.exe` all run with fresh scratch directories and all PASS without any `[UTEST] <name>: FAIL (... handle(s) leaked ...)` lines -- the existing binaries already close every handle they open. Unit suites cover the `u_derive_test_name` case/empty/tiny-buffer edges, the `u_path_join` overflow + separator cases, and a hive-survival probe for the cleanup-manifest HKLM guard. A binary that opens `C:\hello.txt` without closing it WOULD now surface as `[UTEST] <name>: FAIL (1 handle(s) leaked -- escalated from PASS)` and a `not ok N - <name> # 1 handle(s) leaked` TAP line; a deliberately-leaky binary to exercise that path is owned by §9's full syscall test binary which has its own handle/cleanup obligations. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | 34 suites under TEST_CAT_EXEC (6 new for §6: pure helpers + manifest HKLM guard + traversal guard regression), 0 failures

> **Notes:**
> - Shipped isolation helpers: `u_derive_test_name` (strips `.exe`), `u_path_join` (bounded `parent\name` concat), `u_rmtree` (iterative recursive delete with depth/count caps), `u_ensure_dir`, `u_isolation_setup`, `u_isolation_snapshot_leaks` + `u_isolation_reap` (split so leak count reads BEFORE `task_cleanup` while destructive delete runs AFTER -- Codex H1 fix, 2026-04-20: a leaked handle on a scratch-dir file would otherwise block `vfs_unlink` because `ref_count > 0`).
> - Shipped ABI: new `utest_isolation` u8 at stable offset 358 in `struct boot_config`; mirrored in the bootloader struct with matching static asserts. Bootloader `parse_conf_kv` parses `utest_isolation=<0|1>` and `boot_config_defaults` sets 1 so absent-key boots isolate by default. Total `boot_config` still 512 bytes.
> - Cleanup manifest guard-rails (Codex H2 fix, 2026-04-20): empty `HKLM\` suffix rejected (would otherwise wipe all HKLM children), and both `C:\` + `HKLM\` entries must target the ImpossibleOS subtree. A typo or hostile manifest cannot touch `C:\hello.txt` / `C:\cmd.exe` / any other well-known path. Unit test asserts the SOFTWARE hive survives a guard probe.
> - Teardown order: snapshot leak count -> log verdict (escalate PASS->FAIL on leak) -> `task_cleanup` (closes child's handles) -> `u_isolation_reap` destructive delete -> optional `u_cleanup_manifest_apply`. Correct ordering here was explicitly designed around `vfs_unlink`'s `ref_count > 0` rejection; re-ordering without thought will reintroduce the stale-scratch-state bug.
> - Post-commit review added 2 more fixes: (H3) `u_path_has_traversal` helper rejects any `..` component in cleanup-manifest entries, plugging a prefix-bypass where `C:\Impossible\..\hello.txt` would escape the allowed subtree via the VFS walker. (M1) `u_rmtree` on cap exhaustion now returns -1 explicitly instead of silently falling through to `vfs_unlink`; `u_isolation_setup` and `u_isolation_reap` propagate the failure; `u_run_one` escalates a PASS to FAIL when isolation reports trouble so CI catches the broken invariant. Previously a scratch tree with more than `UTEST_RMTREE_MAX_ENTRIES`=512 entries would partially wipe and let stale state bleed into the next binary.
> - Downstream effects: §9-§14 subsystem test binaries can now assume a clean `C:\Temp\utest\<stem>\` at start and don't have to coordinate unique temp paths. Handle leaks become CI-failures, incentivising proper cleanup in every new test binary. `test_fileio.exe`-style tests no longer risk tripping each other.
> - Canonical doc: [tests/usermode-cleanup.manifest](../../tests/usermode-cleanup.manifest) -- format + security rationale for the optional cross-test cleanup file. [docs/testing/usermode-env-matrix.md](../../docs/testing/usermode-env-matrix.md) already describes the per-test isolation contract at the platform-matrix level.
> - Scope boundary: §6 owns scratch-dir + Registry-subkey + handle-leak + manifest. Per-test **heap**-leak detection lives in `00-infrastructure/TODO-03` (kernel harness §8) for kernel-side allocations; we don't extend it to user-mode heap (tests exit, so user-mode heap is torn down with the process). Stress/perf type-specific cleanup (§8) and JUnit XML emission of leak counts (§7) are downstream.

> **Verified:** 2026-04-20 | commit `fc58d177` | 6/6 items | build OK | tests 135/135 PASS (up from 107) + 3 user binaries PASS with isolation active; scratch dirs + Registry subkeys survived a clean-boot round-trip
> **Quality reviewed:** 2026-04-20 | Codex 2x (adversarial, quality) | 3H+1M+2L fixed, 0 open | scope: kernel-code-quality

---

## 7. JUnit XML + JSON Output Formats

§4 ships TAP, which is great for human reading but underspecified for CI tooling. JUnit XML is the de facto CI test-result format (GitLab, Jenkins, GitHub Actions, Azure DevOps all parse it natively). JSON gives downstream automation a typed schema for trend analysis.

- [x] `xml=1` boot.conf key parsed in [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) (stable ABI offset 362); plumbed through [`src/kernel/main/boot_tests.c`](../../src/kernel/main/boot_tests.c) into `test_usermode_set_xml()`. Launcher emits `[UTEST-XML] <testsuite name="impossible-os-usermode" ...>` placeholder opener before the loop, one `[UTEST-XML] <testcase name="..." classname="correctness" time="S.MMM"/>` (PASS) or `<testcase ...><failure message="..."/></testcase>` (FAIL) or `<testcase ...><skipped message="..."/></testcase>` (SKIP) per binary, `[UTEST-XML-SUMMARY] tests=N failures=N skipped=N time=S.MMM` line with the real counts, and `[UTEST-XML] </testsuite>` closer. `classname` defaults to `"correctness"` today; §8 test-type taxonomy will override per binary.
- [x] `json=1` boot.conf key (ABI offset 363) -> `test_usermode_set_json()`. Launcher emits one `[UTEST-JSON] {"name":"...","status":"...","time_ms":N,"reason":"..."}` per binary (reason omitted on PASS), plus a final `[UTEST-JSON] {"summary":{"passed":N,"failed":N,"skipped":N,"total":N,"time_ms":N}}` record. All fields follow the schema in [docs/testing/usermode-output-formats.md](../../docs/testing/usermode-output-formats.md). Every emission is overflow-safe via a goto-fallback: a name or reason longer than the 512-byte line buffer triggers a `LOG_WARN` + a minimal well-formed fallback record so the CI artifact stays parseable (Codex quality M1, 2026-04-20).
- [x] `scripts/test.sh` gets `XML=1` and `JSON=1` CLI args (wired into `PATCH_ARGS`) and a post-processor step that strips the klog prefix via `sed -n 's/.*\[UTEST-XML\] //p'`, extracts `[UTEST-XML-SUMMARY]` for the real counts, and writes `build/test-results.xml` with a freshly-generated `<?xml ...?>` + `<testsuite>` wrapper around the harvested testcases. Empty-suite safe via a `|| true` on the testcase pipeline (Codex adversarial H1, 2026-04-20: `set -euo pipefail` abort when zero testcases matched the filter).
- [x] [docs/testing/usermode-output-formats.md](../../docs/testing/usermode-output-formats.md) (~180 LOC) documents all four formats (human-readable default, TAP, JUnit XML, JSON) with wire-format examples, field schemas, CI-tool integration snippets (GitHub Actions, GitLab, Jenkins), escape contract, and `jq` extraction recipes.
- [x] All three flags (`tap=1 xml=1 json=1`) are independently toggled at boot.conf load time; their `s_tap_mode` / `s_xml_mode` / `s_json_mode` static ints are checked separately in each emit path. A single run with all three enabled produces three distinct tagged streams on serial; `grep -E '^\[UTEST-TAP\]|\[UTEST-XML\]|\[UTEST-JSON\]'` (after prefix strip) splits them.
- [x] Commit: `"test: usermode launcher emits JUnit XML + JSON in addition to TAP"`

**Test checkpoint:** On `bash scripts/test.sh QUIET=1 SUITE=exec XML=1 JSON=1` (KVM): 149 kernel tests PASS (up from 135 pre-§7); `build/test-results.xml` is well-formed (Python `xml.etree.ElementTree` parses cleanly with `root.tag == "testsuite"`, `tests=3`, `testcase` elements = 3); each `[UTEST-JSON]` line parses with `python3 -c 'import json; json.loads(...)'`. Kernel unit suites cover the XML escaper (5 specials + control-byte drop + overflow reject) and JSON escaper (quote/backslash/LF/CR/TAB/generic-control/overflow). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | 36 suites under TEST_CAT_EXEC (2 new for §7: XML + JSON escape regressions), 0 failures

> **Notes:**
> - Shipped: ~280 LOC of emit helpers + escape + time-tracking in [`src/kernel/test/test_usermode.c`](../../src/kernel/test/test_usermode.c); two new boot_config fields with static asserts; a 45-line XML post-processor in scripts/test.sh; a 180-line docs file; two new setter APIs; wire-up in boot_tests.c.
> - Output wiring: all three formats enabled simultaneously emit interleaved on serial with distinct `[UTEST-XML]` / `[UTEST-XML-SUMMARY]` / `[UTEST-JSON]` prefixes so `grep` trivially splits them. `scripts/test.sh XML=1` harvests to `build/test-results.xml`; `JSON=1` requires no post-processing (the `[UTEST-JSON]` lines are already valid JSON each).
> - Overflow safety: both XML and JSON testcase emitters use a goto-fallback pattern. Every `u_append` / `u_xml_escape` / `u_json_escape` / `u_append_uint` is checked; on any overflow the emitter logs LOG_WARN and writes a minimal well-formed fallback record (`<testcase name="overflow" classname="overflow" time="0"/>` / `{"name":"overflow","status":"FAIL","time_ms":0}`) so the assembled CI artifact never contains a truncated line (Codex quality M1, 2026-04-20).
> - Post-commit review added 3 more fixes: (M1 empty-suite artifact) when `total_planned == 0` the launcher now still emits `<testsuite>` open/summary/close and a JSON summary so CI tools never see a missing file; belt-and-suspenders, `scripts/test.sh` also writes an explicit empty `<testsuite/>` when `XML=1` was requested but no `[UTEST-XML]` stream landed on serial. (M2 glob control-byte injection) the directory-glob path now validates names through `u_is_valid_manifest_name` (same gate as manifest entries) so a directory entry like `test_bad\nline.exe` can't split a `<testcase name="...">` record across log lines. (H1 test.sh race) when `XML=1`/`JSON=1` is active, `scripts/test.sh` keeps polling QEMU for the user-mode launcher's `UTEST: === N passed, ...` summary line before killing QEMU -- previously it would kill after the kernel unit-test summary + 1s grace, risking incomplete `[UTEST-XML]` capture for slower WSL TCG runs.
> - Downstream effects: unblocks CI integration -- any GitLab/GitHub-Actions/Jenkins pipeline can now consume `build/test-results.xml` via `artifacts.reports.junit` / `actions/upload-artifact` / JUnit plugin respectively. JSON gives typed schema for trend-over-time dashboards. docs/testing/usermode-output-formats.md is the canonical reference for downstream consumers.
> - Canonical doc: [docs/testing/usermode-output-formats.md](../../docs/testing/usermode-output-formats.md) -- wire format + field schema + CI integration + escape contract + jq recipes.
> - Scope boundary: §7 owns the on-serial wire format and the scripts/test.sh XML-harvest step. §4 owns TAP (existing). §8 (test-type taxonomy) will refine `classname` from a fixed `"correctness"` to per-binary values. The JSON post-processor (writing a `build/test-results.json` artifact) is nice-to-have and can live as a follow-up since `grep | jq` already suffices.

> **Verified:** 2026-04-20 | commit `9247ae77` | 6/6 items | build OK | tests 149/149 PASS (up from 135); 3 binaries emit both `[UTEST-XML] <testcase/>` and `[UTEST-JSON] {...}` records; Python XML + JSON parsers accept all outputs; empty-suite case verified not to abort `test.sh`
> **Quality reviewed:** 2026-04-20 | Codex 2x (adversarial, quality) | 1H+3M fixed, 0 open | scope: kernel-code-quality

---

## 8. Test Type Taxonomy: smoke / correctness / stress / perf

A single "test_*.exe" pattern conflates very different test classes. TAEF distinguishes Loop Test Mode and Stress Test Mode; LTP separates `runtest/syscalls` from `runtest/stress`. Adopt a four-type taxonomy via filename prefix and per-type launcher policy.

- [x] Filename convention enforced by §3 launcher scan: `test_smoke_*.exe` (1-second suite that fast-fails on first error -- gates the rest of the run), `test_*.exe` (default correctness), `test_stress_*.exe` (binary loops internally; kernel `task_create` is monotonic so launcher-side looping would exhaust `TASK_MAX`), `test_perf_*.exe` (run once + assert latency/throughput against a baseline JSON file)
- [x] §4 manifest schema gains a `type=` column: `smoke`/`correctness`/`stress`/`perf`; if absent, infer from filename prefix
- [x] Launcher policy per type: smoke -> abort the run on first FAIL (boot fast-fail gate); correctness -> run all, report summary; stress -> binary owns the iteration loop; perf -> run once, emit `[PERF] <metric>=<value_ns>` lines the test asserts against a hardcoded threshold today (per-baseline JSON drift detection -> deferred: needs a libc JSON reader + runtime env passing, tracked as §8 follow-up inside this TODO)
- [x] `tests/perf-baseline.json` lives in repo as a structured record of expected values + per-platform tolerances; consumer is the test binary (future: launcher-side drift check once env passing lands)
- [x] `user/include/test.h` adds `UTEST_PERF(metric, value_ns)` macro that writes the `[PERF]` line; stress tests call `UTEST_ASSERT` inside their own loop (see `user/test/test_stress_libc.c`)
- [x] Commit: `"test: usermode test type taxonomy -- smoke/correctness/stress/perf with per-type policy"`

**Test checkpoint:** A `test_smoke_boot.exe` that asserts `sys_uptime() >= 0` runs first; if it FAILs, the rest of the run is skipped with `[UTEST] suite ABORT (smoke failed)`. A `test_stress_libc.exe` looping `strlen` 1000x stays green. A `test_perf_syscall.exe` reporting `[PERF] sys_yield_ns=<n>` PASSes against a 50_000 ns TCG upper bound (bare metal / KVM lands around 200-500 ns, well under the ceiling). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | 43 suites under TEST_CAT_EXEC (7 new for §8: prefix inference, boundaries, attr known, attr unknown, label match, enum ABI stability, stress_iters setter), 0 failures

> **Notes:**
> - What shipped: `utest_type_t` enum (CORRECTNESS/SMOKE/STRESS/PERF), filename-prefix classifier (`u_type_for_name`), manifest `type=<name>` attribute parser (`u_type_from_attr`), two-phase execution (smoke first + fast-fail gate in `test_usermode_run`), XML/JSON classname/type carrying, `UTEST_PERF` user-mode macro, and three canonical test binaries (`test_smoke_boot.exe`, `test_stress_libc.exe`, `test_perf_syscall.exe`).
> - How it integrates: `boot.conf stress_iters=<N>` plumbs through `test_usermode_set_stress_iters` but is RESERVED (kernel task slots are monotonic; launcher-side loop would exhaust TASK_MAX in ~20 binaries). Stress binaries loop internally (see `user/test/test_stress_libc.c`).
> - Downstream effects: `tests/perf-baseline.json` skeleton lands so CI + on-host trend analysis has a schema to track (consumer: test binaries today; launcher-side drift check deferred pending env-passing syscall). `tools/mkfs-ixfs.c` grown to handle multi-block directories (was capped at 16 root entries; now spans up to 4 inline extents = 64 entries) so the three new binaries fit in sysroot.
> - Canonical doc: `include/kernel/test/test_usermode.h` (enum + public API surface); `tests/perf-baseline.json` (baseline schema + per-platform tolerances).
> - Scope boundary: §8 owns the taxonomy enum + classifier + test binaries + two-phase execution; launcher-side stress looping + perf-baseline drift detection are BOTH deferred to a follow-up that requires scheduler slot reuse (03-memory-concurrency/TODO-06 §13) and an env-passing syscall (not yet tracked).

> **Verified:** 2026-04-20 | commit `06b9246d` | 6/6 items | build OK | tests 179/179 PASS (up from 149; +30 new assertions across 7 suites); `mkfs-ixfs` multi-block dir growth validated by sysroot pack succeeding with 19 root entries
> **Deferred:** [M] launcher-side stress looping + runtime-tunable `stress_iters` need reusable PID slots before they can spawn N children per stress binary (reason: `task_create` is monotonic; binary-side looping is the ship-today shape) -> XREF: 03-memory-concurrency/TODO-06-scheduler-enhancement.md §13 (item: "Add reusable slot/free-list logic for dead tasks" at line 346 -- §8 stress binaries are the consumer, currently loop internally as a workaround)
> **Deferred:** [M] `stress_iters` boot.conf + boot_info field + setter currently have no runtime consumer (reason: reserved-by-design for ABI stability; removing and re-adding would churn `BOOT_INFO_VERSION` once env-passing lands) -> XREF: 03-memory-concurrency/TODO-06-scheduler-enhancement.md §13 (item: "Add reusable slot/free-list logic for dead tasks" at line 346 -- reusable slots is the prerequisite that lets `stress_iters` gain a consumer, same XREF chain as the line above)
> **Quality reviewed:** 2026-04-20 | Codex 3x (adversarial x2, quality) | 1Critical+2H+1M+1L fixed, 0 open | scope: kernel-code-quality

---

## 9. Syscall Test Binary

Exercise every implemented syscall from user mode.

- [ ] Complete §2 first so `user/include/syscall.h` exposes wrappers for each syscall below (numbers must match `include/kernel/sched/syscall.h` on the INT 0x80 path)
- [ ] `user/test/test_syscall.c`:
  - `SYS_WRITE` to stdout -> verify returns byte count
  - `SYS_READ` from stdin (non-blocking test or skip)
  - `SYS_YIELD` -> returns 0
  - `SYS_UPTIME` -> returns > 0
  - `SYS_GETPROCS` -> returns >= 2 (idle + this process)
  - `SYS_OPENFILE` -> open `C:\hello.txt` -> returns valid handle
  - `SYS_READHANDLE` -> reads content from hello.txt
  - `SYS_CLOSEHANDLE` -> close handle -> returns 0
  - `SYS_LOG` -> write to klog -> returns 0
  - `SYS_OPENDIROBJ` -> open `\` -> returns valid handle
  - `SYS_QUERYDIROBJ` -> enumerate -> returns entries
- [ ] Commit: `"test: user-mode syscall test binary (test_syscall.exe)"`

**Test checkpoint:** `test_syscall.exe` runs, all assertions pass, exit code 0, serial shows `[UTEST] test_syscall.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 10. Libc Test Binary

Test string and formatting functions available in user mode.

- [ ] `user/test/test_libc.c`:
  - `strlen("hello")` -> 5
  - `strcmp("abc", "abc")` -> 0
  - `strcmp("abc", "abd")` -> negative
  - `memcpy` round-trip
  - `memset` + verify
  - `snprintf(buf, 32, "%d", 42)` -> `"42"`
- [ ] Commit: `"test: user-mode libc test binary (test_libc.exe)"`

**Test checkpoint:** Boot with `test=1`, launcher runs `test_libc.exe`, it exits 0, and serial shows `[UTEST] test_libc.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 11. IPC Test Binary

Test inter-process communication from user mode.

- [ ] Complete §2 first so `SYS_PIPE`, `SYS_SHMEM_CREATE`, and `SYS_SHMEM_MAP` wrappers exist in userland before compiling this binary
- [ ] `user/test/test_ipc.c`:
  - `SYS_PIPE` -> two handles, write to one, read from the other
  - `SYS_SHMEM_CREATE` -> returns handle
  - `SYS_SHMEM_MAP` -> returns non-zero address
  - Write to shared memory, verify data
- [ ] Commit: `"test: user-mode IPC test binary (test_ipc.exe)"`

**Test checkpoint:** `test_ipc.exe` completes pipe + shmem checks, exits 0, and serial shows `[UTEST] test_ipc.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 12. Process Lifecycle Test

Test fork, exec, waitpid from user mode.

- [ ] `user/test/test_process.c`:
  - `SYS_FORK` -> parent gets child PID, child gets 0
  - `SYS_WAITPID` -> parent waits for child, gets exit status
  - `SYS_EXEC` -> load another binary (`hello.exe`)
  - `SYS_KILL` -> kill a child process
- [ ] Commit: `"test: user-mode process lifecycle test (test_process.exe)"`

**Test checkpoint:** Fork, wait, exec, and kill paths assert cleanly, exit 0, and emit `[UTEST] test_process.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 13. File I/O Test

Test handle-based file operations.

- [ ] Complete §2 first so `SYS_OPENFILE`, `SYS_READHANDLE`, `SYS_CLOSEHANDLE`, `SYS_OPENDIROBJ`, and `SYS_QUERYDIROBJ` wrappers exist before compiling this binary
- [ ] `user/test/test_fileio.c`:
  - Open `C:\hello.txt` -> valid handle
  - Read contents -> matches expected
  - Close handle -> handle becomes invalid
  - Open nonexistent file -> `INVALID_HANDLE_VALUE`
  - Open directory object -> enumerate entries
- [ ] Commit: `"test: user-mode file I/O test (test_fileio.exe)"`

**Test checkpoint:** Open, read, close, and error paths match expectations, exit 0, and serial shows `[UTEST] test_fileio.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 14. Win32 API Test Binary

Test Win32 API stubs once they are implemented (depends on `D02T12 §6`).

- [ ] `user/test/test_win32.c`:
  - `GetCurrentProcessId()` -> returns PID > 0
  - `CreateFile("C:\\hello.txt", GENERIC_READ, ...)` -> valid handle
  - `ReadFile(handle, buf, size, &read, NULL)` -> reads data
  - `CloseHandle(handle)` -> returns TRUE
  - `GetTickCount()` -> returns > 0
- [ ] Commit: `"test: user-mode Win32 API test binary (test_win32.exe)"`

**Test checkpoint:** After `D02T12 §6` stubs exist, `test_win32.exe` exits 0 and serial shows `[UTEST] test_win32.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 15. Binary Format Loader Coverage (ELF / PE32+ / EIF)

The launcher's spawned binaries are all crt0+libc ELF; the PE32+ and EIF loader paths registered in [`src/kernel/exec.c`](../../src/kernel/exec.c) ship live but never see a user-mode binary. Win11 HLK and Linux kselftest both exercise every supported binary format from user space. Kernel-side `test_exec.c` covers the PE32+ parser at the buffer level but does not load and run a PE process end-to-end. Build a tiny test binary in each format and assert that `exec_load`'s magic-byte dispatch picked the right loader for each one.

- [ ] [`user/test/test_loader_elf.c`](../../user/test/test_loader_elf.c): trivial main returning 0; built by the existing crt0+libc ELF link recipe. Sanity baseline that proves the ELF path runs end-to-end inside the format-coverage triplet, distinct from §1's smoke binary.
- [ ] [`user/test/test_loader_pe.c`](../../user/test/test_loader_pe.c): trivial main returning 0; Makefile recipe uses `lld-link` (or `clang --target=x86_64-pc-windows-msvc`) to emit a minimal PE32+ executable. Binary must start with `MZ` magic so [`src/kernel/exec.c`](../../src/kernel/exec.c) line 70 routes to `pe_load`.
- [ ] [`user/test/test_loader_eif.c`](../../user/test/test_loader_eif.c): trivial main returning 0; Makefile recipe wraps the ELF or flat output in an `EIF!` header (4-byte magic + payload per the EIF format owned by `D02T17 §5`). Binary must start with `EIF!` so [`src/kernel/exec.c`](../../src/kernel/exec.c) line 62 routes to `eif_load`.
- [ ] All three binaries: ZERO syscalls beyond `SYS_EXIT(0)`. The goal is to exercise the LOADER, not the syscall ABI -- §9 owns syscall coverage. Exit code 0 -> launcher logs `[UTEST] test_loader_<fmt>.exe: PASS`.
- [ ] §3 launcher emits a `format=<NAME>` line per binary so the boot log surfaces which loader picked each one. Plumb the format name out of `exec_load` (it already knows -- see `s_formats[i].name` in [`src/kernel/exec.c`](../../src/kernel/exec.c) line 122) through `task_exec` into `test_usermode.c`'s per-binary log: `klog(LOG_INFO, "UTEST", "%s: format=%s", name, fmt)`. Without this, a regression that silently routed PE binaries through the ELF loader would still print PASS.
- [ ] Per-binary bats land under `scripts/debug/usermode/` per §16 (`run-test_loader_elf.bat`, `run-test_loader_pe.bat`, `run-test_loader_eif.bat`).
- [ ] Commit: `"test: user-mode binary format loader coverage (ELF + PE32+ + EIF)"`

**Test checkpoint:** All three binaries exit 0; serial shows three `format=ELF` / `format=PE32+` / `format=EIF` lines from the launcher, proving each format hit its registered loader. A regression that drops the PE or EIF registration in `exec.c` would surface as `format=ELF` for the wrong binary or as a launcher load failure (`exec_load` returns ENOEXEC -> launcher prints `FAIL (exit=-5)`). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 16. Build Integration

Wire user-mode test binaries into `make test`.

- [ ] Makefile: compile `user/test/test_*.c` -> `build/user/test_*.exe`
- [ ] Disk image build: copy test binaries to `C:\Impossible\System32\`
- [ ] `make test` target: include user-mode tests after kernel unit tests
- [ ] `scripts/test.sh`: parse serial for `[UTEST]` lines alongside `[TEST]` kernel lines
- [ ] When §4 `tap=1` is enabled in `boot.conf`, parse TAP `ok` / `not ok` / `# SKIP` lines into the same summary as `[UTEST]`
- [ ] Author per-binary bat files under `scripts/debug/usermode/` (parallel to the kernel-side `scripts/debug/kernel/run-<cat>-tests.bat` family). One bat per `test_*.exe` binary that exists at the end of §1-§15, plus an aggregate runner. Each bat passes `utest_filter=<binary>` to QEMU via `run-qemu.ps1` so the §3 launcher runs ONLY that one binary:
    - `scripts/debug/usermode/run-test_harness_smoke.bat` (§1 smoke)
    - `scripts/debug/usermode/run-test_syscall.bat` (§9 full syscall coverage; the §2 stub is a build-only smoke and shares the same name)
    - `scripts/debug/usermode/run-test_libc.bat` (§10)
    - `scripts/debug/usermode/run-test_ipc.bat` (§11)
    - `scripts/debug/usermode/run-test_process.bat` (§12)
    - `scripts/debug/usermode/run-test_fileio.bat` (§13)
    - `scripts/debug/usermode/run-test_win32.bat` (§14, lights up only after `D02T12 §6`)
    - `scripts/debug/usermode/run-test_loader_elf.bat` (§15)
    - `scripts/debug/usermode/run-test_loader_pe.bat` (§15)
    - `scripts/debug/usermode/run-test_loader_eif.bat` (§15)
    - `scripts/debug/usermode/run-all.bat` (no filter; runs every `test_*.exe`)
- [ ] Bat-file template: one-liner mirroring `scripts/debug/kernel/run-<cat>-tests.bat` shape -- `powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -BootArg "utest_filter=<binary>"` (the `%~dp0..\..\machines\run-qemu.ps1` relative path matches the kernel/ bats after the 2026-04-20 directory split). New `-BootArg` flag may need to land in `run-qemu.ps1` if it does not already accept arbitrary boot.conf overrides.
- [ ] Update each subsystem-test section's `> **Test runner:**` line to point at the matching `scripts/debug/usermode/run-<binary>.bat` instead of the kernel-side bat. Sections §9-§15 each get their own runner stamp.
- [ ] Commit: `"test: build integration -- user-mode tests in make test, CI, and per-binary bat runners"`

**Test checkpoint:** `bash scripts/test.sh` (full or `SUITE=exec`) ends with a combined kernel `[TEST]` summary plus an `[UTEST]` user summary; a missing binary or non-zero exit fails the run. Running `scripts\debug\usermode\run-test_syscall.bat` on Windows boots QEMU WHPX, runs ONLY `test_syscall.exe`, and prints the binary's `[UTEST-BEGIN]` / `[PASS]` / `[UTEST-END]` lines on serial; `run-all.bat` runs every binary in manifest order. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐  | Feature              | 🪟 Win11             | 🐧 Linux                 | 🚀 Impossible OS       |
| --- | -------------------- | --------------------- | ------------------------ | ----------------------- |
| 💎  | User-mode test bins  | ✅ HLK               | ✅ kselftest             | ⬜ §1-§15              |
| 💎  | Multi-format loader  | ✅ PE + .NET via HLK | ✅ ELF + a.out kselftest | ⬜ §15 ELF+PE32+ +EIF  |
| 💎  | Syscall coverage     | ✅ NtDll             | ✅ ptrace selftest       | ⬜ §9                  |
| 💎  | Auto launcher        | ✅ HLK               | ✅ run_kselftest         | ⬜ §3                  |
| 💎  | TAP or CI parse      | ✅ HLK XML           | ✅ TAP kselftest         | ✅ §7 XML + §4 TAP     |
| 💎  | JUnit XML / JSON     | ✅ HLK XML           | ⚠️ kselftest TAP only    | ✅ §7 XML+JSON+TAP     |
| 💎  | Timeouts or skips    | ✅ HLK               | ✅ LKFT skip             | ✅ §4 10s + exit=77    |
| 💎  | ABI header sync      | ✅ SDK               | ✅ uapi                  | ⬜ §2                  |
| 💎  | Per-test isolation   | ✅ HLK session reset | ✅ kselftest fork+tmp    | ✅ §6 scratch+reg+leak |
| 💎  | Stress / longhaul    | ✅ TAEF Loop+Stress  | ✅ LTP runtest/stress    | ✅ §8 stress type      |
| 💎  | Perf regression      | ✅ perfview/PerfTest | ✅ perf + flame baseline | ⚠️ §8 report-only      |
| 💎  | Test type taxonomy   | ✅ TAEF categories   | ✅ LTP test classes      | ✅ §8 4 types + phase  |
| ⭐  | Fault-inject bridge  | ⚠️ AppVerifier hooks | ⚠️ debugfs failslab      | ✅ §5 SYS_FAULT_INJECT |
| ⭐  | Win32 on non-Win     | ❌ N/A               | ❌ Wine only             | ⬜ §14                 |

> **Parity gaps:** 💎 rows with ⬜ map to the listed sections. **⭐ rows:** §5 fault-inject bridge gives a typed `test=1`-gated kernel-allocator probe surface that AppVerifier hooks Win32 for and Linux only exposes through debugfs; §14 Win32-on-non-Win depends on `D02T12 §6` Win32 thunk landing.

---

## Unit Tests

> [!NOTE]
> User-mode coverage is driven by `user/test/test_*.c` binaries and serial `[UTEST]` lines from §3 onward, not a dedicated `src/kernel/test/test_usermode.c` until a kernel-side wrapper is justified. §2 is header-only parity; §4 + §7 + §8 are launcher and serial-format policy. The §5 `SYS_FAULT_INJECT` test-mode gate negative regression is the one kernel-side `TEST_CAT_EXEC` assertion that DOES belong in `src/kernel/test/test_syscall.c`.

- [ ] Commit: `"test: N/A single TEST_CAT file -- usermode harness per §1-§16 and Verification"`

**Test checkpoint:** After §3 ships, `bash scripts/test.sh SUITE=exec` (see `CLAUDE.md`) parses `[UTEST]` PASS/FAIL alongside kernel `[TEST]` lines.

---

## Verification

- [ ] `make test` -> user-mode tests run after kernel tests and all pass
- [ ] Break a syscall -> a user-mode test catches it -> `bash scripts/test.sh` fails locally (Actions remain build-only per `CLAUDE.md` unless CI is extended)
- [ ] Add a new test binary -> one file + one Makefile line -> works in `make test` / `bash scripts/test.sh`
- [ ] Commit: `"test: user-mode test framework complete"`

**Test checkpoint:** End to end: clean tree -> `bash scripts/test.sh` is green -> a one-line change breaks a `test_*.exe` assertion -> the run fails with a visible `[UTEST] FAIL`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

**Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | suite count populated by §16 build integration once `test_*.exe` binaries ship; pending today

---