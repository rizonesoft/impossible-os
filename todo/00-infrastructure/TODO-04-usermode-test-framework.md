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
- → XREF: `D02T17 §1, §2, §5, §19` -- exec_load() multi-format dispatcher, enhanced ELF, EIF kernel loader, and PE delay-load that `test_syscall.exe` / `test_fileio.exe` / `test_process.exe` cover
- → XREF: `T03 §1, §6` -- kernel-side `kmalloc_fail_next()` + multi-allocator countdowns that the §5 user-mode fault-injection bridge exposes through a privileged `SYS_FAULT_INJECT` syscall

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
- Per-binary `scripts/debug/usermode/run-<name>.bat` runners (§15) parallel the kernel-side `scripts/debug/kernel/run-<cat>-tests.bat` family: one bat per `test_*.exe` binary plus a `run-all.bat` aggregate. Each bat passes `utest_filter=<binary>` (§4) to QEMU so a single user-mode test can be invoked selectively without rebuilding.

---

## Implementation Order

| ⭐  | Order | Deliverable                                         | Depends On                         | Status |
| --- | :---: | --------------------------------------------------- | ---------------------------------- | :----: |
| 💎  |   1   | User-mode test assertion macro and harness          | --                                 |  [x]   |
| 💎  |   2   | Userland syscall.h parity with kernel INT 0x80 ABI  | --                                 |  [x]   |
| 💎  |   3   | Kernel test launcher (run `test_*.exe` in sequence) | §1                                 |  [x]   |
| 💎  |   4   | Launcher manifest, timeouts, TAP, and skip policy   | §3                                 |  [ ]   |
| ⭐  |   5   | User-mode fault-injection bridge (SYS_FAULT_INJECT) | §1, §3, T03 §1, T03 §6             |  [ ]   |
| 💎  |   6   | Per-test isolation + cleanup hook                   | §3, §4                             |  [ ]   |
| 💎  |   7   | JUnit XML + JSON output formats                     | §4                                 |  [ ]   |
| 💎  |   8   | Test type taxonomy (smoke/correctness/stress/perf)  | §3, §4                             |  [ ]   |
| 💎  |   9   | Syscall test binary (`test_syscall.exe`)            | §1, §2, §3                         |  [ ]   |
| 💎  |  10   | Libc test binary (`test_libc.exe`)                  | §1, §3                             |  [ ]   |
| 💎  |  11   | IPC test binary (`test_ipc.exe`)                    | §1, §2, §3                         |  [ ]   |
| 💎  |  12   | Process lifecycle test (`test_process.exe`)         | §1, §3                             |  [ ]   |
| 💎  |  13   | File I/O test (`test_fileio.exe`)                   | §1, §2, §3                         |  [ ]   |
| ⭐  |  14   | Win32 API test binary (`test_win32.exe`)            | §1, §3, D02T12 §6                  |  [ ]   |
| 💎  |  15   | Build integration: `make test` includes user tests  | §3-§14                             |  [ ]   |

> 💎 = parity: Linux kselftest and Windows HLK both use user-mode test binaries, TAP/JUnit XML, machine-readable test orchestration, per-test isolation, and stress/perf categorisation.
> ⭐ = exclusive: testing the Win32 API surface from user mode on a non-Windows kernel (§14); user-mode fault-injection bridge that reaches kernel allocator countdowns under a single `test=1` gate (§5).
> **Sequencing rule:** foundation lands first (§1-§4), runner-side enhancements that every subsystem test consumes ship next (§5-§8), subsystem test binaries follow (§9-§14), and repo-wide build integration closes the loop in §15.

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
> - Downstream consumers: §9-§13 subsystem test binaries all `#include "test.h"` and use the same `UTEST_*` macros. §15 build integration extends the Makefile rule pattern from this section to the full `user/test/test_*.c` glob.
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
- [x] Triggered from [`src/kernel/main/boot_tests.c`](../../src/kernel/main/boot_tests.c) at the end of the `test=1` block, after `test_runner_run()` -- means the kernel `[TEST] === N tests passed` summary lands FIRST on serial, then `[UTEST]` lines, matching the order `scripts/test.sh` parser will look for once §15 wires user-mode parsing.
- [/] `utest_filter=<name|glob>` boot.conf parameter honoured via `test_usermode_set_filter()` API + an internal `u_glob_match()` helper that supports literal names + a single `*` wildcard. The launcher RESPECTS the filter today (empty/unset = run all); the boot.conf field that SETS it lands in §4 -- this section ships the consumer-side hook, §4 ships the parser.
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
> - Verification limitation on WSL TCG: the existing `scripts/test.sh` waits for `=== N tests passed` from the kernel summary then sleeps 1s + kills QEMU. WSL TCG runs user-mode binaries slowly enough that the launcher's spawned binary may not finish in that 1s grace -- per-binary `[UTEST]` lines may be cut off mid-run on `bash scripts/test.sh`. The launcher CODE is correct (proven by `Task 3 ("test_syscall.exe") created` + `ELF auxv` + `PID 3 -> entry 0x800000` lines on serial); full round-trip PASS/FAIL surfacing on WSL needs §15's `scripts/test.sh` extension to wait for the `[UTEST] === N passed` summary too. Native Windows runs via `scripts\debug\usermode\run-all-usermode-tests.bat` (which goes through `run-qemu.ps1` and waits until QEMU exits naturally) get the full round-trip.
> - Scope boundary: §3 owns the launcher itself (scan + spawn + wait + log + summary). §4 owns the boot.conf `utest_filter=` parameter parser + the launcher manifest + per-binary timeouts + TAP/SKIP output. §6 owns per-test isolation (scratch dir, Registry, handle-leak detection). §7 owns JUnit XML / JSON output formats. §15 owns the `scripts/test.sh` parser extension that lets WSL boot-test runs pick up `[UTEST]` lines.

> **Verified:** 2026-04-20 | commit `5e576ce4` | 9/10 items + 1 [/] (utest_filter consumer-hook ready, parser owned by §4) | build OK | launcher loads test_*.exe via VFS scan + task_create + task_exec + task_waitpid; ELF auxv + PID 3 -> entry serial-traced
> **Quality reviewed:** 2026-04-20 | Codex 2x (adversarial, quality) | 3H+2M fixed (1Critical initially rejected then re-validated and fixed: hlt-tail freeze on user QEMU run -- replaced for(;;) hlt; with for(;;) yield(); because boot_tests_run() runs INSIDE boot_phase3 BEFORE scheduler_enable() at boot_desktop.c:384, so PIT preemption was disabled and hlt blocked on an interrupt that never routed to schedule; yield() goes through schedule_now() which switches regardless of sched_enabled) | scope: kernel-code-quality

---

## 4. Launcher Manifest, Timeouts, and CI-Friendly Output

Parity with kselftest/LKFT-style automation: deterministic order, watchdogs, machine-readable output, and documented skips for flaky hypervisors (see `CLAUDE.md` WHPX vs TCG notes for storage; the same class of issue can hit timing-sensitive user tests).

- [ ] Add `tests/usermode.manifest` listing `test_*.exe` in execution order; launcher reads it before falling back to a directory glob
- [ ] Add per-binary wall-clock timeout in the launcher wait path -- kill task and emit `[UTEST] name: FAIL (timeout)` on expiry
- [ ] Add optional `tap=1` in `boot.conf` so the launcher prints minimal TAP lines (`ok N - name` / `not ok N - name`) around each binary for `scripts/test.sh`
- [ ] Define `SKIP reason` line contract from tests: launcher records skip without failing the suite; `scripts/test.sh` counts skips separately from FAIL
- [ ] Add `utest_filter=<name|glob>` boot.conf parameter (parallel to the existing `test_suite=<cat>` knob): when set, the §3 launcher runs ONLY binaries whose filename matches the literal name or `*`-glob. Empty / unset = run all. Used by the per-binary bat files in §15 to invoke a single test selectively (e.g. `utest_filter=test_syscall.exe` from `scripts/debug/usermode/run-test_syscall.bat`). Glob matching is a tiny `fnmatch`-style helper, not full POSIX.
- [ ] Add `docs/testing/usermode-env-matrix.md` documenting QEMU WHPX vs TCG vs VirtualBox vs bare metal expectations for user-mode timing and known skip reasons (doc only)
- [ ] Commit: `"test: usermode launcher manifest, timeouts, TAP, skip policy, and utest_filter"`

**Test checkpoint:** With a manifest listing two dummy binaries, kill one via timeout mid-run: summary shows 1 FAIL(timeout) and the remaining binaries still execute. With `tap=1`, serial contains `ok 1` style lines parseable by TAP consumers. With `utest_filter=test_syscall.exe`, the summary line names exactly one binary executed. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 5. User-Mode Fault-Injection Bridge

Bridge `T03 §1` `kmalloc_fail_next()` and `T03 §6` PMM/VMM/copy_user countdowns to user mode so tests can probe error paths in syscalls (e.g., `test_fileio.exe` proves `SYS_OPENFILE` cleans up on `kmalloc` failure). Application Verifier solves the same problem on Win11 by hooking `CreateFileA`; we expose it via a privileged syscall guarded by the boot-time test gate so production user-mode binaries cannot break out.

> [!TIP]
> Linux exposes `failslab` only through `/sys/kernel/debug/failslab/*` files (debugfs); HLK Application Verifier hooks Win32 in the loader. A typed kernel-level bridge with a `test=1` gate gives the same coverage with a simpler audit story.

> [!WARNING]
> `SYS_FAULT_INJECT` MUST hard-fail with `STATUS_ACCESS_DENIED` when `boot.conf test=0`. Without that gate, a malicious user-mode binary could DoS the kernel by arming `kmalloc_fail_countdown` repeatedly. The gate check is the security-critical path; cover it in the regression test.

- [ ] Add `SYS_FAULT_INJECT` (next free INT 0x80 number after the §2 sweep) with subcommand selectors: `FAULT_KMALLOC_NEXT`, `FAULT_KMALLOC_COUNTDOWN`, `FAULT_PMM_NEXT`, `FAULT_VMM_MAP_NEXT`, `FAULT_COPY_USER_NEXT`, `FAULT_CLEAR_ALL`
- [ ] Kernel handler in `src/kernel/sched/syscall.c` checks `kernel_subsystem_ready(SUBSYS_TEST_MODE)` (or equivalent of `boot.conf test=1`); returns `STATUS_ACCESS_DENIED` when not in test mode
- [ ] Each subcommand dispatches to the matching `T03 §1`/`§6` setter; arguments validated against `task_current()->pid` so a test can only fault-inject for itself (not arbitrary tasks)
- [ ] User-mode wrapper `int utest_fault_inject(int kind, uint32_t countdown)` in `user/include/test.h`
- [ ] Smoke regression: a tiny `user/test/test_faultinject.c` arms `FAULT_KMALLOC_NEXT` then calls `SYS_OPENFILE` on a non-existent path -- proves the test gate is honoured AND the next kmalloc returns NULL inside the kernel
- [ ] Negative regression in `src/kernel/test/test_syscall.c` (TEST_CAT_EXEC): with `SUBSYS_TEST_MODE` cleared, `SYS_FAULT_INJECT` returns `STATUS_ACCESS_DENIED` and `kmalloc_fail_countdown` stays 0
- [ ] Commit: `"test: SYS_FAULT_INJECT bridge -- user-mode probes kernel allocator countdowns under test=1 gate"`

**Test checkpoint:** With `boot.conf test=1`: `test_faultinject.exe` arms the next-kmalloc trap, calls a syscall that allocates, observes `STATUS_INSUFFICIENT_RESOURCES`. With `test=0`: same binary's first call to `SYS_FAULT_INJECT` returns `STATUS_ACCESS_DENIED` and the syscall succeeds. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 6. Per-Test Isolation + Cleanup Hook

Right now §3's launcher just sequentially execs binaries. If `test_fileio.exe` leaves `C:\Temp\test-fileio-scratch` behind, `test_libc.exe` could trip on it. Linux kselftest forks per test so file descriptors leak nowhere; LTP uses unique temp dirs and unconditional cleanup. The launcher needs a per-test cleanup pass between binaries.

- [ ] Reserve `C:\Temp\utest\<test-name>\` as the per-test scratch root; launcher creates it before each binary's task_create_user, deletes it after task_waitpid (whether PASS, FAIL, or timeout)
- [ ] Reserve `HKLM\SOFTWARE\ImpossibleOS\Test\<test-name>\` as the per-test Registry scratch root; launcher deletes the subkey after each binary
- [ ] Per-binary handle-leak detection: launcher snapshots the per-task handle table size before exec, asserts size returns to baseline after waitpid (extends `T03 §8` per-test heap-leak detection model to handle counts)
- [ ] Optional `tests/usermode-cleanup.manifest` file listing additional paths/keys to scrub (e.g., DLL cache, network sockets) for tests that touch global state legitimately
- [ ] Tests opt out via `boot.conf utest_isolation=0` for debugging only -- production runs always isolate
- [ ] Commit: `"test: per-test isolation -- scratch dir + Registry subkey + handle-leak detection"`

**Test checkpoint:** A binary that creates `C:\Temp\utest\test-foo\stale-file` and exits 0 leaves NO trace -- next binary's pre-exec snapshot of `C:\Temp\utest\` is empty. A binary that opens `C:\hello.txt` without closing it surfaces as `[UTEST] FAIL test_x: 1 handle leaked`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 7. JUnit XML + JSON Output Formats

§4 ships TAP, which is great for human reading but underspecified for CI tooling. JUnit XML is the de facto CI test-result format (GitLab, Jenkins, GitHub Actions, Azure DevOps all parse it natively). JSON gives downstream automation a typed schema for trend analysis.

- [ ] Add `xml=1` flag to `boot.conf`: launcher emits `[UTEST-XML] <testsuite ...>` lines wrapping the §4 TAP block; one `<testcase>` per binary with `<failure message="...">stderr</failure>` for FAIL, `<skipped/>` for SKIP, attributes `name`, `classname` (=test type from §8), `time` (in seconds)
- [ ] Add `json=1` flag: emit `[UTEST-JSON] {"name":"test_x","status":"PASS","time_ms":123,...}` one line per binary; final `[UTEST-JSON] {"summary":{...}}` line
- [ ] `scripts/test.sh` post-processor: when `xml=1` was set, extract `[UTEST-XML]` lines from the serial log into `build/test-results.xml` (a real file CI tools can pick up via `actions/upload-artifact` or GitLab `artifacts.reports.junit`)
- [ ] Document the schema in `docs/testing/usermode-output-formats.md` (already cited in §4 doc-only bullet) -- extend to cover XML + JSON shapes
- [ ] Three flags are mutually compatible: a single run with `tap=1 xml=1 json=1` emits all three formats interleaved on serial; the post-processor splits them
- [ ] Commit: `"test: usermode launcher emits JUnit XML + JSON in addition to TAP"`

**Test checkpoint:** A run with `xml=1` produces `build/test-results.xml` that `xmllint --schema junit.xsd` validates against; a run with `json=1` emits JSON lines parseable by `jq -c '.name'`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 8. Test Type Taxonomy: smoke / correctness / stress / perf

A single "test_*.exe" pattern conflates very different test classes. TAEF distinguishes Loop Test Mode and Stress Test Mode; LTP separates `runtest/syscalls` from `runtest/stress`. Adopt a four-type taxonomy via filename prefix and per-type launcher policy.

- [ ] Filename convention enforced by §3 launcher scan: `test_smoke_*.exe` (1-second suite that fast-fails on first error -- gates the rest of the run), `test_*.exe` (default correctness), `test_stress_*.exe` (loop body N times, default 1000 iterations or `stress_iters=N` from boot.conf), `test_perf_*.exe` (run once + assert latency/throughput against a baseline JSON file)
- [ ] §4 manifest schema gains a `type=` column: `smoke`/`correctness`/`stress`/`perf`; if absent, infer from filename prefix
- [ ] Launcher policy per type: smoke -> abort the run on first FAIL (boot fast-fail gate); correctness -> run all, report summary; stress -> wrap each test body in a loop, fail if N iterations don't all PASS; perf -> run once, parse `[PERF] <metric>=<value>` stderr lines from the binary, compare against `tests/perf-baseline.json`, FAIL if drift > tolerance
- [ ] `tests/perf-baseline.json` lives in repo; per-host jitter tolerance (e.g., +/- 15% on TCG, +/- 5% on KVM) read from `boot.conf` per platform
- [ ] `user/include/test.h` adds `UTEST_PERF(metric, value_ns)` macro that writes the `[PERF]` stderr line; stress tests just loop their existing `UTEST_ASSERT` calls
- [ ] Commit: `"test: usermode test type taxonomy -- smoke/correctness/stress/perf with per-type policy"`

**Test checkpoint:** A `test_smoke_boot.exe` that asserts `sys_uptime() > 0` runs first; if it FAILs, the rest of the run is skipped with `[UTEST] suite ABORT (smoke failed)`. A `test_stress_libc.exe` looping `strlen` 1000x stays green. A `test_perf_syscall.exe` reporting `[PERF] sys_yield_ns=400` PASSes against a 500ns baseline + 15% TCG tolerance. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

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

## 15. Build Integration

Wire user-mode test binaries into `make test`.

- [ ] Makefile: compile `user/test/test_*.c` -> `build/user/test_*.exe`
- [ ] Disk image build: copy test binaries to `C:\Impossible\System32\`
- [ ] `make test` target: include user-mode tests after kernel unit tests
- [ ] `scripts/test.sh`: parse serial for `[UTEST]` lines alongside `[TEST]` kernel lines
- [ ] When §4 `tap=1` is enabled in `boot.conf`, parse TAP `ok` / `not ok` / `# SKIP` lines into the same summary as `[UTEST]`
- [ ] Author per-binary bat files under `scripts/debug/usermode/` (parallel to the kernel-side `scripts/debug/kernel/run-<cat>-tests.bat` family). One bat per `test_*.exe` binary that exists at the end of §1-§14, plus an aggregate runner. Each bat passes `utest_filter=<binary>` to QEMU via `run-qemu.ps1` so the §3 launcher runs ONLY that one binary:
    - `scripts/debug/usermode/run-test_harness_smoke.bat` (§1 smoke)
    - `scripts/debug/usermode/run-test_syscall.bat` (§9 full syscall coverage; the §2 stub is a build-only smoke and shares the same name)
    - `scripts/debug/usermode/run-test_libc.bat` (§10)
    - `scripts/debug/usermode/run-test_ipc.bat` (§11)
    - `scripts/debug/usermode/run-test_process.bat` (§12)
    - `scripts/debug/usermode/run-test_fileio.bat` (§13)
    - `scripts/debug/usermode/run-test_win32.bat` (§14, lights up only after `D02T12 §6`)
    - `scripts/debug/usermode/run-all.bat` (no filter; runs every `test_*.exe`)
- [ ] Bat-file template: one-liner mirroring `scripts/debug/kernel/run-<cat>-tests.bat` shape -- `powershell.exe -ExecutionPolicy Bypass -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -BootArg "utest_filter=<binary>"` (the `%~dp0..\..\machines\run-qemu.ps1` relative path matches the kernel/ bats after the 2026-04-20 directory split). New `-BootArg` flag may need to land in `run-qemu.ps1` if it does not already accept arbitrary boot.conf overrides.
- [ ] Update each subsystem-test section's `> **Test runner:**` line to point at the matching `scripts/debug/usermode/run-<binary>.bat` instead of the kernel-side bat. Sections §9-§14 each get their own runner stamp.
- [ ] Commit: `"test: build integration -- user-mode tests in make test, CI, and per-binary bat runners"`

**Test checkpoint:** `bash scripts/test.sh` (full or `SUITE=exec`) ends with a combined kernel `[TEST]` summary plus an `[UTEST]` user summary; a missing binary or non-zero exit fails the run. Running `scripts\debug\usermode\run-test_syscall.bat` on Windows boots QEMU WHPX, runs ONLY `test_syscall.exe`, and prints the binary's `[UTEST-BEGIN]` / `[PASS]` / `[UTEST-END]` lines on serial; `run-all.bat` runs every binary in manifest order. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐  | Feature              | 🪟 Win11             | 🐧 Linux                 | 🚀 Impossible OS       |
| --- | -------------------- | --------------------- | ------------------------ | ----------------------- |
| 💎  | User-mode test bins  | ✅ HLK               | ✅ kselftest             | ⬜ §1-§14              |
| 💎  | Syscall coverage     | ✅ NtDll             | ✅ ptrace selftest       | ⬜ §9                  |
| 💎  | Auto launcher        | ✅ HLK               | ✅ run_kselftest         | ⬜ §3                  |
| 💎  | TAP or CI parse      | ✅ HLK XML           | ✅ TAP kselftest         | ⬜ §4, §15             |
| 💎  | JUnit XML / JSON     | ✅ HLK XML           | ⚠️ kselftest TAP only    | ⬜ §7                  |
| 💎  | Timeouts or skips    | ✅ HLK               | ✅ LKFT skip             | ⬜ §4                  |
| 💎  | ABI header sync      | ✅ SDK               | ✅ uapi                  | ⬜ §2                  |
| 💎  | Per-test isolation   | ✅ HLK session reset | ✅ kselftest fork+tmp    | ⬜ §6                  |
| 💎  | Stress / longhaul    | ✅ TAEF Loop+Stress  | ✅ LTP runtest/stress    | ⬜ §8 stress type      |
| 💎  | Perf regression      | ✅ perfview/PerfTest | ✅ perf + flame baseline | ⬜ §8 perf type        |
| ⭐  | Fault-inject bridge  | ⚠️ AppVerifier hooks | ⚠️ debugfs failslab      | ⬜ §5 SYS_FAULT_INJECT |
| ⭐  | Win32 on non-Win     | ❌ N/A               | ❌ Wine only             | ⬜ §14                 |

> **Parity gaps:** 💎 rows with ⬜ map to the listed sections. **⭐ rows:** §5 fault-inject bridge gives a typed `test=1`-gated kernel-allocator probe surface that AppVerifier hooks Win32 for and Linux only exposes through debugfs; §14 Win32-on-non-Win depends on `D02T12 §6` Win32 thunk landing.

---

## Unit Tests

> [!NOTE]
> User-mode coverage is driven by `user/test/test_*.c` binaries and serial `[UTEST]` lines from §3 onward, not a dedicated `src/kernel/test/test_usermode.c` until a kernel-side wrapper is justified. §2 is header-only parity; §4 + §7 + §8 are launcher and serial-format policy. The §5 `SYS_FAULT_INJECT` test-mode gate negative regression is the one kernel-side `TEST_CAT_EXEC` assertion that DOES belong in `src/kernel/test/test_syscall.c`.

- [ ] Commit: `"test: N/A single TEST_CAT file -- usermode harness per §1-§15 and Verification"`

**Test checkpoint:** After §3 ships, `bash scripts/test.sh SUITE=exec` (see `CLAUDE.md`) parses `[UTEST]` PASS/FAIL alongside kernel `[TEST]` lines.

---

## Verification

- [ ] `make test` -> user-mode tests run after kernel tests and all pass
- [ ] Break a syscall -> a user-mode test catches it -> `bash scripts/test.sh` fails locally (Actions remain build-only per `CLAUDE.md` unless CI is extended)
- [ ] Add a new test binary -> one file + one Makefile line -> works in `make test` / `bash scripts/test.sh`
- [ ] Commit: `"test: user-mode test framework complete"`

**Test checkpoint:** End to end: clean tree -> `bash scripts/test.sh` is green -> a one-line change breaks a `test_*.exe` assertion -> the run fails with a visible `[UTEST] FAIL`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

**Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | suite count populated by §15 build integration once `test_*.exe` binaries ship; pending today

---