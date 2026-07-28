---
schema_version: 1
id: usermode-test-framework
domain: 00-infrastructure
status: active
title: "TODO-04 -- User-Mode Test Framework"
---

# TODO-04 -- User-Mode Test Framework

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

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

| ⭐   | Order | Deliverable                                         | Depends On                    | Status |
| --- | :---: | --------------------------------------------------- | ----------------------------- | :----: |
| 💎   |   1   | User-mode test assertion macro and harness          | --                            |  [x]   |
| 💎   |   2   | Userland syscall.h parity with kernel INT 0x80 ABI  | --                            |  [x]   |
| 💎   |   3   | Kernel test launcher (run `test_*.exe` in sequence) | §1                            |  [x]   |
| 💎   |   4   | Launcher manifest, timeouts, TAP, and skip policy   | §3                            |  [x]   |
| ⭐   |   5   | User-mode fault-injection bridge (SYS_FAULT_INJECT) | §1, §3, T03 §1, T03 §6        |  [x]   |
| 💎   |   6   | Per-test isolation + cleanup hook                   | §3, §4                        |  [x]   |
| 💎   |   7   | JUnit XML + JSON output formats                     | §4                            |  [x]   |
| 💎   |   8   | Test type taxonomy (smoke/correctness/stress/perf)  | §3, §4                        |  [x]   |
| 💎   |   9   | Syscall test binary (`test_syscall.exe`)            | §1, §2, §3                    |  [x]   |
| 💎   |  10   | Libc test binary (`test_libc.exe`)                  | §1, §3                        |  [x]   |
| 💎   |  11   | IPC test binary (`test_ipc.exe`)                    | §1, §2, §3                    |  [x]   |
| 💎   |  12   | Process lifecycle test (`test_process.exe`)         | §1, §3                        |  [x]   |
| 💎   |  13   | File I/O test (`test_fileio.exe`)                   | §1, §2, §3                    |  [x]   |
| ⭐   |  14   | Win32 API test binary (`test_win32.exe`)            | §1, §3, D02T12 §6             |  [x]   |
| 💎   |  15   | Binary format loader coverage (ELF / PE32+ / EIF)   | §1, §3, D02T17 §5, D02T17 §19 |  [x]   |
| 💎   |  16   | Build integration: `make test` includes user tests  | §3-§15                        |  [x]   |
| 💎   |  17   | Fast-path transport hardening (probes + invariants) | §2, §3, §4                    |  [x]   |
| ⭐   |  18   | Fast-path observability + ABI versioning            | §17                           |  [x]   |
| ⭐   |  19   | Transition ring buffer + 3-way transport fuzz       | §17, §18                      |  [x]   |
| 💎   |  20   | Site-targeted and PMM-countdown fault selectors     | §5                            |  [ ]   |
| 💎   |  21   | Child-targeted fault arming across `fork()`         | §5, §20                       |  [ ]   |
| 💎   |  22   | Honest machine artifacts for skipped sub-tests      | §4, §7                        |  [ ]   |
| 💎   |  23   | Generated exit-status ABI for ring-3 assertions     | §2                            |  [ ]   |

> 💎 = parity: Linux kselftest and Windows HLK both use user-mode test binaries, TAP/JUnit XML, machine-readable test orchestration, per-test isolation, and stress/perf categorisation. §17 brings the fast-path transports (TEB/KUSD/syscall) up to the same "no silent drift, no silent hang" stability floor both competitors offer at their stable ABIs.
> ⭐ = exclusive: testing the Win32 API surface from user mode on a non-Windows kernel (§14); user-mode fault-injection bridge that reaches kernel allocator countdowns under a single `test=1` gate (§5); §18 versioned ABI fingerprint + self-describing KUSD + invariant-guarded ring transitions + transition ring buffer -- capabilities neither Windows 11 nor Linux 6.x exposes to user code today.
> **Sequencing rule:** foundation lands first (§1-§4), runner-side enhancements that every subsystem test consumes ship next (§5-§8), subsystem test binaries follow (§9-§15), repo-wide build integration closes the test-framework loop in §16, fast-path transport hardening + observability (§17-§18) lift the transport layer every subsystem depends on to a level above Win11/Linux, and §19 closes the remaining silent-hang class with a transition-ring replay + cross-transport fuzz (gated on per-CPU cached task info landing in kernel-core). §20-§23 then close the addressability and honesty gaps the framework's own consumers hit: §20 gives fault injection named sites instead of drifting ordinals, §21 extends arming to not-yet-forked children, §22 makes the machine artifacts report skips truthfully, and §23 puts kernel exit statuses on the generated ABI header.

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

> **Moved to §17:** the fast-path isolation probe (`test_fastpath.exe` covering `gs:0x40`, KUSER_SHARED_DATA, `syscall` instruction) was originally filed here as §2's last item. It moved to §17 so the probe binary can ship alongside the kernel-side invariant assertion, per-probe serial logging, and ABI generator that together make the fast-path system non-fragile; §2's own scope was always "header-only parity with kernel INT 0x80 ABI," which is done.

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

> **Verified:** 2026-04-20 | commit `5e576ce4` | 9/10 items + 1 [/] (utest_filter consumer-hook ready, parser owned by §4) -> §4 closed 2026-04-20 `11e816fb` | build OK | launcher loads test_*.exe via VFS scan + task_create + task_exec + task_waitpid; ELF auxv + PID 3 -> entry serial-traced
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

- [x] Complete §2 first so `user/include/syscall.h` exposes wrappers for each syscall below (numbers must match `include/kernel/sched/syscall.h` on the INT 0x80 path)
- [x] `user/test/test_syscall.c`:
  - `SYS_WRITE` to stdout -> verify returns byte count
  - `SYS_READ` from stdin (non-blocking: invalid-fd error path returns -1, avoids TTY block)
  - `SYS_YIELD` -> returns 0 (via `syscall0(SYS_YIELD)` direct; the public `sys_yield` wrapper is `void`)
  - `SYS_UPTIME` -> returns `>= 0` (the TODO spec's `> 0` races the first-second boundary; `>= 0` is the portable floor)
  - `SYS_GETPROCS` -> returns >= 2 (idle + this process)
  - `SYS_OPENFILE` -> open `C:\hello.txt` -> returns valid handle
  - `SYS_READHANDLE` -> reads content from hello.txt (> 0 bytes, first byte 'H')
  - `SYS_CLOSEHANDLE` -> close handle -> returns 0
  - `SYS_LOG` -> write to klog -> returns 0 (LOG_INFO; LOG_FATAL-reject is covered by §2 stub)
  - `SYS_OPENDIROBJ` -> open `\` -> returns valid handle
  - `SYS_QUERYDIROBJ` -> enumerate -> returns > 0 entries + first-entry name non-empty (layout spot check)
- [x] Commit: `"test: user-mode syscall test binary (test_syscall.exe)"`

**Test checkpoint:** `test_syscall.exe` runs, all assertions pass, exit code 0, serial shows `[UTEST] test_syscall.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\usermode\run-test_syscall.bat` pending owner-section wiring (per TODO-04 §4 per-binary-bat follow-up; `make userland` deploys the binary and the manifest fires it under `test=1`) | validation: build OK + sysroot deploy (24 KiB `test_syscall.exe`) + 179/179 kernel unit tests PASS (launcher glob + manifest name checks reference `test_syscall.exe`)

> **Notes:**
> - What shipped: `user/test/test_syscall.c` (~175 LOC) replaces the §2 stub body and exercises all 11 wired INT 0x80 syscalls in one binary; one UTEST_ASSERT per contract with dependent handle blocks guarded by the open's success.
> - How it runs / integrates: deployed by `make userland` to `C:\test_syscall.exe`; picked up by the §3 manifest (`tests/usermode.manifest`) and runs under the default correctness phase of the §8 two-phase launcher.
> - Downstream effects: first end-to-end user-mode ABI probe for the INT 0x80 surface -- a stale `SYS_*` number, wrong argument register, or missing handler case now surfaces here before any subsequent test binary hits it.
> - Canonical doc: [user/include/syscall.h](../../user/include/syscall.h) (wrapper signatures + SYS_* numbers); kernel handlers at [src/kernel/sched/syscall.c](../../src/kernel/sched/syscall.c) lines 379-595.
> - Scope boundary: §9 owns the syscall-surface probe; §10 owns libc string/format coverage, §11 owns IPC (pipe/shmem), §12 owns process lifecycle (fork/wait/exec/kill), §13 owns file-I/O happy/error paths.

> **Verified:** 2026-04-21 | commit `fd0b3122` | 3/3 items | build OK | 1 binary (24 KiB) deployed via `make userland`; 179/179 kernel unit tests PASS (unchanged)
> **Accepted:** [L] `GENERIC_READ` bit mapping deferred to SRM -- this test uses the low-level `VFS_O_READ` (0x01) mask directly because `src/kernel/ob/ob_file.c:144` DOES enforce that bit today on `sys_readhandle`. The §13 work (2026-04-21) revealed the original stamp's claim that "the kernel does not yet honour access masks" was only true for the Win32 `GENERIC_READ` -> VFS bit translation (still deferred); the raw VFS bits are enforced. Using 0 made `[FAIL] sys_readhandle reads > 0 bytes from C:\hello.txt` visible in every run -- now fixed by passing `U_OPEN_READ = 0x01` here and in §13. Directory opens keep the 0 mask since `NtOpenDirectoryObject` has no equivalent low-level read gate yet (reason: scope) -> XREF: 02-kernel-core/TODO-15-security-reference-monitor.md §5 (item: "`RtlMapGenericMask(access, mapping)` -- replaces `GENERIC_READ`/`WRITE`/`EXECUTE`/`ALL` bits with type-specific masks in-place" at line 314 + item: "In `ObpReferenceObjectByHandle`: after locating the handle entry, call `SeAccessCheck(...)`; return `STATUS_ACCESS_DENIED` if check fails" at line 334 -- once §5 ships, this test should flip to `GENERIC_READ`, add a negative-access assertion, and flip the directory-open mask to the equivalent directory `GENERIC_READ`)
> **Quality reviewed:** 2026-04-21 | Codex 2x (adversarial, quality) | 1H+2M fixed, 0 open | scope: userland-code-quality

---

## 10. Libc Test Binary

Test string and formatting functions available in user mode.

- [x] `user/test/test_libc.c`:
  - `strlen("hello")` -> 5
  - `strcmp("abc", "abc")` -> 0
  - `strcmp("abc", "abd")` -> negative
  - `memcpy` round-trip (16-byte pattern via `memcmp`)
  - `memset` + verify (32-byte 0x5A fill, spot-check first/middle/last)
  - `snprintf(buf, 32, "%d", 42)` -> `"42"` (return == 2 AND content "42\0")
- [x] Commit: `"test: user-mode libc test binary (test_libc.exe)"`

**Test checkpoint:** Boot with `test=1`, launcher runs `test_libc.exe`, it exits 0, and serial shows `[UTEST] test_libc.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\usermode\run-test_libc.bat` (utest_filter=test_libc.exe via new `-UtestFilter` param to run-qemu.ps1) | validation: build OK + sysroot deploy (47 KiB `test_libc.exe`) + 179/179 kernel unit tests PASS

> **Notes:**
> - What shipped: `user/test/test_libc.c` (~90 LOC) -- 7 UTEST_ASSERT contracts across strlen/strcmp/memcpy/memset/snprintf, the libc surface every other user binary (cmd.exe, hello.exe, every test_*.exe) links against.
> - How it integrates: deployed by `make userland` to `C:\test_libc.exe`; picked up by the §3 manifest (`tests/usermode.manifest`) and runs in the §8 correctness phase under the launcher.
> - Downstream effects: ships with the **first round of user-mode bat runners** (`scripts/debug/usermode/run-test_{libc,syscall,smoke_boot,stress_libc,perf_syscall}.bat`) that the §3-§9 stamps already referenced; new `-UtestFilter` param on [`scripts/machines/run-qemu.ps1`](../../scripts/machines/run-qemu.ps1) plumbs through to `boot.conf utest_filter=<glob>`. Follow-up landed same day: paired `test_kernel_skip` + `test_usermode_skip` boot.conf knobs (offsets 366/367) + `-NoKernelTests` / `-NoUsermodeTests` switches let the per-binary bats run usermode-only and let the cross-layer aggregate run each layer exactly once.
> - Canonical doc: [user/include/string.h](../../user/include/string.h) + [user/include/stdio.h](../../user/include/stdio.h) (function signatures); implementation in [user/lib/string.c](../../user/lib/string.c) + [user/lib/stdio.c](../../user/lib/stdio.c).
> - Scope boundary: §10 owns the libc surface probe; §9 owns the syscall ABI surface, §11 owns IPC, §12 owns process lifecycle, §13 owns file I/O.

> **Verified:** 2026-04-21 | commit `f8d6f9d8` | 2/2 items | build OK | 1 binary (47 KiB) deployed via `make userland`; 179/179 kernel unit tests PASS (unchanged); 4 per-binary bats from §8/§9 + new §10 bat now exist on disk
> **Quality reviewed:** 2026-04-21 | Codex 2x (adversarial, quality) | 1H+3M fixed, 0 open | scope: userland-code-quality

---

## 11. IPC Test Binary

Test inter-process communication from user mode.

- [x] Complete §2 first so `SYS_PIPE`, `SYS_SHMEM_CREATE`, and `SYS_SHMEM_MAP` wrappers exist in userland before compiling this binary
- [x] `user/test/test_ipc.c`:
  - `SYS_PIPE` -> two handles, write to one (via new `SYS_WRITEHANDLE`), read from the other (via `SYS_READHANDLE`); "PIPE-OK" round-trip byte-checked
  - `SYS_SHMEM_CREATE` -> returns handle
  - `SYS_SHMEM_MAP` -> returns non-zero address; `sys_unmapview` released before close to drop the view pin
  - Write to shared memory, verify data (2x uint32 marker + LE byte-order spot-check)
- [x] Commit: `"test: user-mode IPC test binary (test_ipc.exe)"`

**Test checkpoint:** `test_ipc.exe` completes pipe + shmem checks, exits 0, and serial shows `[UTEST] test_ipc.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\usermode\run-test_ipc.bat` (utest_filter=test_ipc.exe + -NoKernelTests) | validation: build OK, 179/179 kernel unit tests PASS, 2.43s smoke test

> **Notes:**
> - What shipped: `user/test/test_ipc.c` (~125 LOC) with 12 UTEST_ASSERT contracts covering SYS_PIPE write/read round-trip, SYS_SHMEM_CREATE/MAP/unmap, and handle cleanup on every path.
> - Adjacent work shipped under Branch A: the section required user-mode pipe writes, which the pre-existing SYS_WRITE rejected (stdout-only). Added **SYS_WRITEHANDLE (45)** as mirror of SYS_READHANDLE via `ob_file_write()`, and **SYS_UNMAPVIEW (46)** so the test can release the ObReferenceObject pin that `ObMapViewOfSectionFull` takes (task_cleanup does not walk section views). Both new syscalls get header defines + user wrappers (`sys_writehandle`, `sys_unmapview`).
> - Hardening landed in `ob_file_read` + `ob_file_write`: pipe direction is now gated (`pipe_end == PIPE_READ`/`PIPE_WRITE`) and file access mask checked (`VFS_O_READ`/`VFS_O_WRITE`). VFS write now advances `fo->offset` on positive writes -- previously sequential writes landed at byte 0 every time.
> - Downstream effects: `SYS_WRITEHANDLE` unblocks future file-write tests and any user binary needing non-stdout writes. `SYS_UNMAPVIEW` is the INT 0x80 counterpart to `NtUnmapViewOfSection` in the SSDT path.
> - Canonical doc: [user/include/syscall.h](../../user/include/syscall.h) (wrapper signatures); [include/kernel/ob/ob_file.h](../../include/kernel/ob/ob_file.h) + [include/kernel/ob/ob_section.h](../../include/kernel/ob/ob_section.h) (kernel surface).
> - Scope boundary: §11 owns user-mode IPC coverage; cross-task pipe IPC (fork + pipe share) is §12's job; unnamed section handles only -- named-section tests land in a later section.

> **Verified:** 2026-04-21 | commit `b28ead1f` | 3/3 items | build OK | tests 179/179 PASS, smoke test 2.51s; 2 new syscalls wired (SYS_WRITEHANDLE + SYS_UNMAPVIEW), 4 defensive gates added to ob_file_read/write, matching gates added to NtReadFile/NtWriteFile (SSDT path)
> **Accepted:** [M] `SYS_UNMAPVIEW` requires the section handle still be open because `ObUnmapViewOfSectionByBase` scans the handle table to find the view (reason: pre-existing OB design) -> XREF: 02-kernel-core/TODO-05-object-manager.md §3 (item: "Walk section views on task_cleanup to drop leaked view pins" -- added this commit, captures both the unreachability-after-close defect and the task-exit leak; the view-base index in the preceding bullet is its prerequisite)
> **Quality reviewed:** 2026-04-21 | Codex 2x (adversarial, quality) | 2H+3M fixed, 0 open | scope: userland-code-quality + kernel-code-quality (cross-domain: gate fixes in ob_file.c and nt_syscall.c, new INT 0x80 syscalls in syscall.c)

---

## 12. Process Lifecycle Test

Test fork, exec, waitpid from user mode.

- [x] `user/test/test_process.c`:
  - `SYS_FORK` -> parent gets child PID, child gets 0 (sub-test 1)
  - `SYS_WAITPID` -> parent waits for child, gets exit status (all sub-tests)
  - `SYS_EXEC` -> load another binary (`hello.exe`); parent waitpid returns hello's 42 (sub-test 2)
  - `SYS_KILL` -> kill an own child process; waitpid returns -1 (sub-test 3)
- [x] Commit: `"test: user-mode process lifecycle test (test_process.exe)"`

**Test checkpoint:** Fork, wait, exec, and kill paths assert cleanly, exit 0, and emit `[UTEST] test_process.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\usermode\run-test_process.bat` (utest_filter=test_process.exe + -NoKernelTests) | validation: build OK, 179/179 kernel unit tests PASS (unchanged)

> **Notes:**
> - What shipped: `user/test/test_process.c` (~130 LOC) with 10 UTEST_ASSERT contracts across three sub-tests (fork+exit, fork+exec(hello.exe), fork+kill). Each sub-test spawns exactly one child so the run consumes 3 task slots -- well within TASK_MAX=32 headroom given the §6 isolation task pool.
> - How it integrates: deployed by `make userland` to `C:\test_process.exe`; runs under the §8 correctness phase of the §3 launcher. Safety net: if the kill-child's cooperative `sys_yield` spin is broken (scheduler starvation or broken kill), the §4 launcher's 10 s per-binary watchdog force-DEADs the whole binary and reports FAIL instead of hanging the suite.
> - Downstream effects: first end-to-end user-mode probe of the fork/exec/waitpid/kill quartet. Any regression in `task_fork`, `task_exec`, `task_waitpid`, or SYS_KILL surfaces here before it propagates to cmd.exe or future multi-process apps.
> - Canonical doc: [src/kernel/sched/task.c](../../src/kernel/sched/task.c) (`task_fork`, `task_exec`, `task_waitpid`, `task_cleanup`); SYS_KILL handler at [src/kernel/sched/syscall.c](../../src/kernel/sched/syscall.c) line 444.
> - Scope boundary: §12 owns happy-path coverage on the caller's own child. Cross-process kill authorization (ring 3 killing arbitrary tasks) is a SeAccessCheck concern owned by TODO-15 §5 -- see Accepted stamp below. Binary format variants (PE32+, EIF exec paths) are §15's job.

> **Verified:** 2026-04-21 | commit `a2d55077` | 2/2 items | build OK | tests 179/179 PASS (unchanged); 3 sub-tests dispatch 3 forks each, cleaned up via parent waitpid + task_cleanup
> **Accepted:** [H] `SYS_KILL` accepts a raw PID and marks any target `TASK_DEAD` without parentage/ACL/privilege gating (reason: scope -- SRM + process OB type ownership) -> XREF: 02-kernel-core/TODO-15-security-reference-monitor.md §5 (item: "Authorize process-termination syscalls" -- added this commit; retrofits SYS_KILL + NtTerminateProcess with handle-based addressing + `PROCESS_TERMINATE` gate once `ObpReferenceObjectByHandle` lands in TODO-05 §3)
> **Accepted:** [M] `sys_exec(path, len)` wrapper advertises a length arg but the kernel SYS_EXEC handler ignores `len` and reads the filename as a NUL-terminated string (reason: input-validation gap, not a correctness bug for the §12 test) -- test comment updated to document the current behaviour; no kernel change required today, no separate XREF item created since an operator reading the test sees the reality and the wrapper is already the correct ABI shape for a future honour-len fix
> **Quality reviewed:** 2026-04-21 | Codex 2x (adversarial, quality) | 1M fixed + 1H/1M Accepted, 0 open | scope: userland-code-quality + kernel-code-quality (launcher budget check)

---

## 13. File I/O Test

Test handle-based file operations.

- [x] Complete §2 first so `SYS_OPENFILE`, `SYS_READHANDLE`, `SYS_CLOSEHANDLE`, `SYS_OPENDIROBJ`, and `SYS_QUERYDIROBJ` wrappers exist before compiling this binary -- all five wrappers live in [user/include/syscall.h](../../user/include/syscall.h) since §9
- [x] `user/test/test_fileio.c`:
  - Open `C:\hello.txt` -> valid handle
  - Read contents -> matches expected (25-byte byte-for-byte compare, not just "starts with 'H'")
  - Close handle -> handle becomes invalid (post-close `sys_readhandle` MUST return < 0)
  - Open nonexistent file -> `INVALID_HANDLE_VALUE`
  - Open directory object -> enumerate entries (OB root `\\`, >= 1 non-empty entry)
- [x] Commit: `"test: user-mode file I/O test (test_fileio.exe)"`

**Test checkpoint:** Open, read, close, and error paths match expectations, exit 0, and serial shows `[UTEST] test_fileio.exe: PASS`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\usermode\run-test_fileio.bat` (utest_filter=test_fileio.exe + -NoKernelTests) | validation: build OK, test_fileio.exe deployed to `C:\test_fileio.exe` + registered in `tests/usermode.manifest`

> **Notes:**
> - What shipped: `user/test/test_fileio.c` (~215 LOC) with 13 UTEST_ASSERT contracts across five probes: happy-path `sys_openfile`/`sys_readhandle`/byte-for-byte match on 25-byte `"Hello from Impossible OS!"` payload/`sys_closehandle`; post-close `sys_readhandle` returning `< 0` + sentinel-pattern buffer-integrity check (no kernel writes into caller memory on failure); write-only `sys_openfile("C:\hello.txt", VFS_O_WRITE)` + read-reject probe so the access-gate branch is actually exercised; `sys_openfile("C:\this_file_definitely_does_not_exist.bin")` returning `INVALID_HANDLE_VALUE`; `sys_opendirobj("\\")` + `sys_querydirobj` returning `>= 1` non-empty entry with `_Static_assert`-verified 96-byte layout.
> - How it integrates: deployed by `make userland` to `C:\test_fileio.exe`; runs under the §8 correctness phase of the §3 launcher alongside `test_syscall.exe` / `test_process.exe` / `test_ipc.exe`. Manifest entry `test_fileio.exe` (no `expects_tasks=` tag -- only one user task so the §3 launcher budget check counts it as 1). Per-binary runner: `scripts\debug\usermode\run-test_fileio.bat`.
> - Downstream effects: first focused user-mode probe of the `ob_close_handle -> read returns -1` invalidation contract (+ buffer-integrity hardening), the `ob_create_file_handle -> INVALID_HANDLE_VALUE` negative path, and the `!(fo->access & VFS_O_READ)` reject branch in `ob_file.c:144`. Catches a class of bugs §9's "starts with 'H'" smoke probe would miss (content truncation, partial read, stale handle slot reuse, caller-memory corruption on failure, access-gate bypass). Review also added kernel-side `_Static_assert` on `OBJECT_DIRECTORY_INFORMATION` layout and rewrote the `sys_openfile` wrapper docstring to match actual behavior.
> - Canonical doc: [src/kernel/ob/ob_file.c](../../src/kernel/ob/ob_file.c) (`ob_file_read`, close path); [src/kernel/sched/syscall.c](../../src/kernel/sched/syscall.c) SYS_OPENFILE/SYS_READHANDLE/SYS_CLOSEHANDLE/SYS_OPENDIROBJ/SYS_QUERYDIROBJ handlers; [include/kernel/ob/ob.h](../../include/kernel/ob/ob.h) OBJECT_DIRECTORY_INFORMATION layout asserts.
> - Scope boundary: §13 owns file-I/O happy-path + close-invalidation + nonexistent-path + write-only-read-reject + OB-root-enumeration. Full Win32 `ACCESS_MASK` / `GENERIC_READ` mapping is deferred to SRM -- see the revised `sys_openfile` doc in [user/include/syscall.h](../../user/include/syscall.h) and §9's Accepted stamp. Write-PATH coverage (`sys_writehandle` actually writing bytes to a file) is still deferred until the OB file-write path supports non-pipe writes. Handle-leak detection across process exit is §6's territory.

> **Verified:** 2026-04-21 | commit `8a5eced9` | 3/3 items | build OK | 13 UTEST_ASSERTs across 5 probes; 1800/1800 kernel unit tests PASS; `test_fileio.exe` (32 KiB) deployed; `[PASS] sys_readhandle reads > 0 bytes from C:\hello.txt` now visible (previously FAIL)
> **Accepted:** [H] Full Win32 `ACCESS_MASK` / `GENERIC_READ` mapping not implemented -- wrapper currently takes raw `VFS_O_READ`/`VFS_O_WRITE` bits; tests and `sys_openfile` docstring explicitly document this (reason: infra -- needs SRM) -> XREF: 02-kernel-core/TODO-15-security-reference-monitor.md §5 (item: "`RtlMapGenericMask(access, mapping)` -- replaces `GENERIC_READ`/`WRITE`/`EXECUTE`/`ALL` bits with type-specific masks in-place" at line 314 + item: "In `ObpReferenceObjectByHandle`: after locating the handle entry, call `SeAccessCheck(...)`; return `STATUS_ACCESS_DENIED` if check fails" at line 334)
> **Quality reviewed:** 2026-04-21 | Codex 2x (adversarial, quality) | 1H+2M fixed (buffer-integrity sentinel, OB layout kernel asserts, write-only negative probe tightened) + 1H Accepted (GENERIC_READ mapping), 0 open | scope: userland-code-quality

---

## 14. Win32 API Test Binary

First Impossible-OS probe of the Win32 API surface from ring 3 -- proves the "Win32 native" orientation works end-to-end at the source level without the PE32+ dynamic linker (§15). Depends on `D02T12 §6` (NtCreateFile / NtReadFile / NtClose handlers).

- [x] Minimal user-space Win32 shim ([`user/include/win32.h`](../../user/include/win32.h), [`user/lib/win32.c`](../../user/lib/win32.c)): types (HANDLE via syscall.h, BOOL / DWORD / LPOVERLAPPED / LPSECURITY_ATTRIBUTES), constants (GENERIC_READ/WRITE/ALL, OPEN_EXISTING / CREATE_NEW / CREATE_ALWAYS / OPEN_ALWAYS / TRUNCATE_EXISTING, FILE_SHARE_*, FILE_ATTRIBUTE_NORMAL), and 5 functions routed through the SYSCALL fast path into `ssdt_dispatch`. Linked statically into `libc.a`.
- [x] [`user/test/test_win32.c`](../../user/test/test_win32.c) with 8 UTEST_ASSERT contracts:
  - `GetCurrentProcessId()` -> returns PID > 0 (reads TEB.ClientId.UniqueProcess at `gs:0x40`)
  - `CreateFileA("C:\\hello.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)` -> valid handle (routes to `NtOpenFile` via SYSCALL; Win32 GenericMask mapped client-side to VFS_O_READ until SRM ships)
  - `ReadFile(handle, buf, 64, &read, NULL)` -> TRUE, `read == 25`, byte-for-byte match of `"Hello from Impossible OS!"`
  - `CloseHandle(handle)` -> TRUE; double-close returns FALSE (handle-invalidation probe at the Win32 layer)
  - `GetTickCount()` -> returns > 0 and is monotonically non-decreasing (reads KUSER_SHARED_DATA at `0x7FFE0000` + TickCountMultiplier math)
- [x] Commit: `"test: user-mode Win32 API test binary (test_win32.exe)"`

**Test checkpoint:** `test_win32.exe` exits 0 and serial shows `UTEST: test_win32.exe: PASS (exit=0)`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\usermode\run-test_win32.bat` (utest_filter=test_win32.exe + -NoKernelTests) | validation: build OK, test_win32.exe deployed to `C:\test_win32.exe` + registered in `tests/usermode.manifest`

> **Notes:**
> - What shipped: `user/include/win32.h` (~120 LOC), `user/lib/win32.c` (~200 LOC), `user/test/test_win32.c` (~170 LOC). 5 Win32 functions (`GetCurrentProcessId`, `GetTickCount`, `CreateFileA`, `ReadFile`, `CloseHandle`) + 12 UTEST_ASSERTs across 7 probes. Kernel-side: new `SYS_GETPID` (INT 0x80 service 18) returns `task_current()->pid`; `src/kernel/ob/handle_table.c` `ObpAllocateHandle` scan starts at slot 1 so handle value 0 is never valid (matches Windows `NULL` semantics).
> - How it integrates: `user/lib/win32.c` compiles into `build/user/libc.a`; every user binary inherits the Win32 shim. `test_win32.exe` is deployed by `make userland` to `C:\test_win32.exe` and runs under the §8 correctness phase of the §3 launcher. Per-binary runner: `scripts\debug\usermode\run-test_win32.bat`. **Transport**: every Win32 function routes through the INT 0x80 surface (SYS_GETPID, SYS_UPTIME, SYS_OPENFILE, SYS_READHANDLE, SYS_CLOSEHANDLE) that test_syscall / test_libc / test_ipc / test_process / test_fileio have already proven end-to-end. The native Windows transport (`gs:0x40` TEB read, `KUSER_SHARED_DATA` at `0x7FFE0000`, `syscall` instruction into `ssdt_dispatch`) all hung silently on WHPX in the first revision -- the new §2 item tracks the isolated probes required to unblock them.
> - Downstream effects: first Win32-idiomatic user binary in the tree. Future userland tests can include `win32.h` and call the same shim without re-implementing syscall-level thunks. Establishes the client-side GenericMask -> VFS bit translation pattern that migrates into the kernel once SRM (TODO-15 §5) lands. Adds `SYS_GETPID` to the INT 0x80 surface -- future non-Win32 callers (POSIX-style `getpid(3)`) can use it directly.
> - Canonical doc: [user/include/win32.h](../../user/include/win32.h) (API contract) + [user/lib/win32.c](../../user/lib/win32.c) (implementation). Kernel-side: [src/kernel/sched/syscall.c](../../src/kernel/sched/syscall.c) (SYS_GETPID handler), [src/kernel/ob/handle_table.c](../../src/kernel/ob/handle_table.c) (slot-0 reservation), [include/kernel/sched/syscall.h](../../include/kernel/sched/syscall.h) (SYS_GETPID=18 constant).
> - Scope boundary: §14 owns Win32-level CreateFile(OPEN_EXISTING) / ReadFile(sync) / CloseHandle / GetCurrentProcessId / GetTickCount. NOT in scope: CreateProcess, WaitForSingleObject, VirtualAlloc, registry APIs, CreateFile with CREATE_* / TRUNCATE_* dispositions (requires SYSCALL-arg extension past 4 regs), async ReadFile with OVERLAPPED, the Unicode-W variants, and full kernel32.dll via the PE32+ dynamic linker (§15). A thicker Win32 layer that supports those callers is the §15 territory once PE32+ imports land.

> **Verified:** 2026-04-22 | commit `86a4edf4` (initial) + follow-up | 3/3 items | build OK | 12 UTEST_ASSERTs across 7 probes; 1800/1800 kernel unit tests PASS; `test_win32.exe` (~24 KiB) deployed; smoke PASS (KVM 2.22 s)
> **Accepted:** [H] Win32 shim routes every call through INT 0x80 (proven path) instead of the Windows-native transport (`gs:0x40` / KUSD / `syscall` -> `ssdt_dispatch`) because all three fast paths hung silently in the first revision on WHPX -- public API unchanged, future migration is a single-file swap (reason: infra -- needs isolated fast-path probes before migration) -> XREF: 00-infrastructure/TODO-04 §2 (item: "Verify the user-mode fast paths that user/lib/win32.c routed around" at line 515)
> **Quality reviewed:** 2026-04-22 | Codex 3x (adversarial, adversarial-post-fix, quality) | 1Critical+1H+2M fixed (slot-0 reservation, CloseHandle sentinels, ReadFile EOF, OBJECT_ATTRIBUTES + UNICODE_STRING ABI layout; transport rewritten from SYSCALL to INT 0x80 after silent WHPX hang) + 1H Accepted (fast-path probes), 0 open | scope: userland-code-quality

---

## 15. Binary Format Loader Coverage (ELF / PE32+ / EIF)

The launcher's spawned binaries are all crt0+libc ELF; the PE32+ and EIF loader paths registered in [`src/kernel/exec.c`](../../src/kernel/exec.c) ship live but never see a user-mode binary. Win11 HLK and Linux kselftest both exercise every supported binary format from user space. Kernel-side `test_exec.c` covers the PE32+ parser at the buffer level but does not load and run a PE process end-to-end. Build a tiny test binary in each format and assert that `exec_load`'s magic-byte dispatch picked the right loader for each one.

- [x] [`user/test/test_loader_elf.c`](../../user/test/test_loader_elf.c): trivial main returning 0; built by the existing crt0+libc ELF link recipe. Sanity baseline that proves the ELF path runs end-to-end inside the format-coverage triplet, distinct from §1's smoke binary.
- [x] [`user/test/test_loader_pe.c`](../../user/test/test_loader_pe.c): trivial `_start` calling INT 0x80 SYS_EXIT(0); Makefile recipe uses `clang-19 --target=x86_64-pc-windows-msvc` + `lld-link` to emit a minimal PE32+ executable with `MZ` magic, routed by [`src/kernel/exec.c`](../../src/kernel/exec.c) to `pe_load`.
- [x] [`user/test/test_loader_eif.asm`](../../user/test/test_loader_eif.asm): flat NASM entry doing INT 0x80 SYS_EXIT(0); [`scripts/build-eif.py`](../../scripts/build-eif.py) wraps the raw code in a 64-byte EIF header + 32-byte segment table per `D02T17 §5`. Binary starts with `EIF!` (file-order bytes 0x45 0x49 0x46 0x21, little-endian `EIF_MAGIC = 0x21464945`), routed by [`src/kernel/exec.c`](../../src/kernel/exec.c) to `eif_load`.
- [x] All three binaries: ZERO syscalls beyond `SYS_EXIT(0)`. Exit code 0 -> launcher logs `[UTEST] test_loader_<fmt>.exe: PASS`.
- [x] §3 launcher emits a `format=<NAME>` line per binary: `exec_load_fmt` returns the matched format name pointer out-param; `task_exec` stashes it in `tasks[pid].loaded_format`; [`src/kernel/test/test_usermode.c`](../../src/kernel/test/test_usermode.c) reads it and emits `klog(LOG_INFO, "UTEST", "%s: format=%s", name, fmt)` before the verdict. A regression that silently routed PE binaries through the ELF loader would surface as `format=ELF` for `test_loader_pe.exe`.
- [x] Per-binary bats landed under `scripts/debug/usermode/` (`run-test_loader_elf.bat`, `run-test_loader_pe.bat`, `run-test_loader_eif.bat`).
- [x] Commit: `"test: user-mode binary format loader coverage (ELF + PE32+ + EIF)"`

**Test checkpoint:** All three binaries exit 0; serial shows three `format=ELF` / `format=PE32+` / `format=EIF` lines from the launcher, proving each format hit its registered loader. A regression that drops the PE or EIF registration in `exec.c` would surface as `format=ELF` for the wrong binary or as a launcher load failure (`exec_load` returns ENOEXEC -> launcher prints `FAIL (exit=-5)`). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\usermode\run-test_loader_elf.bat`, `run-test_loader_pe.bat`, `run-test_loader_eif.bat` (one per binary) | 3 binaries, 0 failures

> **Notes:**
> - Three loader-coverage binaries ship: [`user/test/test_loader_elf.c`](../../user/test/test_loader_elf.c) (crt0+libc ELF), [`user/test/test_loader_pe.c`](../../user/test/test_loader_pe.c) (freestanding PE32+ via `lld-link`), [`user/test/test_loader_eif.asm`](../../user/test/test_loader_eif.asm) (NASM flat wrapped by [`scripts/build-eif.py`](../../scripts/build-eif.py)). Each is the minimum that still proves its magic-byte dispatch path.
> - Format surfacing: `exec_load_fmt()` out-param plumbs the matched `s_formats[i].name` pointer into `tasks[pid].loaded_format`; [`src/kernel/test/test_usermode.c`](../../src/kernel/test/test_usermode.c) emits `UTEST: <name>: format=<NAME>` before the per-binary verdict so a loader-routing regression surfaces in the log even when the binary still exits 0.
> - Implementation surfaced three kernel bugs, all fixed root-cause (no workarounds): (1) `EIF_MAGIC` byte-order inverted so file-on-disk "EIF!" never matched the constant; (2) `vmm.c get_or_create_table()` did not propagate the User bit into intermediate PML4/PDPT/PD entries, so `vmm_map_page(..., VMM_FLAG_USER)` silently produced supervisor-only intermediate tables for non-ELF-range addresses; (3) `task_exec()` only set User on the image range, not the user stack -- PE's 8 KiB `SizeOfImage` left the stack kernel-only and PE's first push faulted at `_start`.
> - Canonical docs: `D02T17 §5` (EIF format spec), [`src/kernel/exec.c`](../../src/kernel/exec.c) (magic-byte dispatch registry), [`scripts/build-eif.py`](../../scripts/build-eif.py) (EIF build tool for any future flat-binary test fixture).
> - Scope boundary: §15 owns loader-selection coverage; §9 owns syscall coverage; `D02T17 §10` owns PE base relocation; `D02T17 §8` owns PE per-process PML4 so PE loads do not mutate shared `kernel_pml4` (Codex [M] finding Accepted via XREF).

> **Verified:** 2026-04-22 | commit `f0279dc0` | 7/7 items | build OK | smoke PASS (KVM 2.1s) | 3 loader binaries PASS
> **Accepted:** [H] PE loader still maps images into shared `kernel_pml4` and `get_or_create_table()` leaves persistent User-bit upgrades on upper tables after rollback (reason: needs per-process PML4 infra, architectural scope) -> XREF: 02-kernel-core/TODO-17 §8 (item: "Map PE images into a per-process PML4 instead of the shared `kernel_pml4`" at line 249)
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, quality) | 2H+1M fixed, 1H open | scope: userland-code-quality

---

## 16. Build Integration

Wire user-mode test binaries into `make test`.

- [x] Makefile: compile `user/test/test_*.c` -> `build/sysroot/test_*.exe` -- existing `userland` target rule already builds all 14 deployed binaries (test_smoke_boot, test_harness_smoke, test_syscall, test_libc, test_ipc, test_process, test_fileio, test_win32, test_loader_{elf,pe,eif}, test_faultinject, test_stress_libc, test_perf_syscall).
- [x] Disk image build: copy test binaries to `C:\Impossible\System32\` -- `SYSROOT := build/sysroot` is the staging tree mounted at `C:\` by the boot ISO, so anything landing in `$(SYSROOT)/test_*.exe` automatically deploys to `C:\test_*.exe`.
- [x] `make test` target: includes user-mode tests after kernel unit tests -- existing `make test` -> `scripts/test.sh` boots QEMU once and the kernel TEST sweep is followed by the §3 launcher walking `tests/usermode.manifest` in the same boot.
- [x] `scripts/test.sh`: parses serial for `[UTEST]` lines alongside `[TEST]` kernel lines -- new Step 5b block in [`scripts/test.sh`](../../scripts/test.sh) prints per-binary verdict lines (PASS/FAIL/SKIP/TIMEOUT/ISOLATION/LEAK + the loader-coverage `format=` plumb-through), pulls the launcher's `UTEST: === N passed, N failed, N skipped of N total ===` summary, and folds `UTEST_FAIL` into the final exit-code gate so a user-mode regression cannot hide behind a green kernel summary.
- [x] When `tap=1` is enabled in `boot.conf`, parse TAP `ok` / `not ok` / `# SKIP` lines into the same summary as `[UTEST]` -- the §4 launcher emits TAP producer lines (`UTEST: 1..N`, `UTEST: ok N - name`, `UTEST: not ok N - name # reason`) when `tap=1` is set; `scripts/test.sh` Step 5b detects them via grep and emits a separate `TAP producer stream:` block under the per-binary verdicts so external TAP consumers (CI, tap-junit) can lift them straight from the host log.
- [x] Per-binary bat files under `scripts/debug/usermode/` (parallel to `scripts/debug/kernel/run-<cat>-tests.bat`). All 13 bats now exist + 1 aggregate; each passes `utest_filter=<binary>` to QEMU via `run-qemu.ps1`'s `-UtestFilter` switch so the §3 launcher runs ONLY that one binary:
    - [`scripts/debug/usermode/run-test_harness_smoke.bat`](../../scripts/debug/usermode/run-test_harness_smoke.bat) (§1 framework self-check) -- new in §16
    - [`scripts/debug/usermode/run-test_smoke_boot.bat`](../../scripts/debug/usermode/run-test_smoke_boot.bat) (§1 fast-fail boot smoke)
    - [`scripts/debug/usermode/run-test_syscall.bat`](../../scripts/debug/usermode/run-test_syscall.bat) (§9 full syscall coverage)
    - [`scripts/debug/usermode/run-test_libc.bat`](../../scripts/debug/usermode/run-test_libc.bat) (§10)
    - [`scripts/debug/usermode/run-test_ipc.bat`](../../scripts/debug/usermode/run-test_ipc.bat) (§11)
    - [`scripts/debug/usermode/run-test_process.bat`](../../scripts/debug/usermode/run-test_process.bat) (§12)
    - [`scripts/debug/usermode/run-test_fileio.bat`](../../scripts/debug/usermode/run-test_fileio.bat) (§13)
    - [`scripts/debug/usermode/run-test_win32.bat`](../../scripts/debug/usermode/run-test_win32.bat) (§14)
    - [`scripts/debug/usermode/run-test_loader_elf.bat`](../../scripts/debug/usermode/run-test_loader_elf.bat) / [`run-test_loader_pe.bat`](../../scripts/debug/usermode/run-test_loader_pe.bat) / [`run-test_loader_eif.bat`](../../scripts/debug/usermode/run-test_loader_eif.bat) (§15)
    - [`scripts/debug/usermode/run-test_faultinject.bat`](../../scripts/debug/usermode/run-test_faultinject.bat) (§5 fault-injection probe) -- new in §16
    - [`scripts/debug/usermode/run-test_stress_libc.bat`](../../scripts/debug/usermode/run-test_stress_libc.bat) / [`run-test_perf_syscall.bat`](../../scripts/debug/usermode/run-test_perf_syscall.bat) (§8 stress + perf phase)
    - [`scripts/debug/usermode/run-all-usermode-tests.bat`](../../scripts/debug/usermode/run-all-usermode-tests.bat) (no filter; runs every `test_*.exe` in manifest order)
- [x] Bat-file template uses the `-UtestFilter <binary>` knob already supported by [`scripts/machines/run-qemu.ps1`](../../scripts/machines/run-qemu.ps1) (validated `^[A-Za-z0-9_.*-]{1,63}$`, joined into boot.conf as `utest_filter=`) -- no `-BootArg` shim needed. Each bat is one-liner: `powershell.exe ... -File "%~dp0..\..\machines\run-qemu.ps1" -Accel whpx -TestOnly -NoKernelTests -UtestFilter "<binary>"`.
- [x] Each subsystem-test section's `> **Test runner:**` line already points at the matching `scripts\debug\usermode\run-<binary>.bat` (§9 line 338, §10 line 368, §11 line 396, §12 line 425, §13 line 456, §14 line 486, §15 line 515) -- §16 adds the missing §1 smoke and §5 faultinject bats so all sections that ship a binary now have a runner.
- [x] Commit: `"test: build integration -- user-mode tests in make test, CI, and per-binary bat runners"`

**Test checkpoint:** `bash scripts/test.sh` ends with a combined kernel `[TEST]` summary plus a `[UTEST]` user-mode summary; a missing binary or non-zero exit fails the run via the merged `TOTAL_FAIL = FAILED + UTEST_FAIL` gate. Running `scripts\debug\usermode\run-test_syscall.bat` on Windows boots QEMU WHPX, runs ONLY `test_syscall.exe`, and prints the binary's `UTEST: ...: PASS` line on serial; `run-all-usermode-tests.bat` runs every binary in manifest order. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `bash scripts/test.sh` (full kernel + user-mode in one boot) | per-binary bats under `scripts/debug/usermode/run-test_*.bat` | 14 user-mode binaries, 0 failures (KVM 1800 kernel + 14 user-mode PASS)

> **Notes:**
> - Build integration was already mostly wired by §1-§15 (Makefile builds 14 binaries into `$(SYSROOT)/test_*.exe`, manifest at `tests/usermode.manifest` deploys to `C:\`, `scripts/test.sh` boots once for both tiers). §16 closed the two remaining gaps: (a) `scripts/test.sh` Step 5b parses `UTEST:` per-binary verdicts + the launcher's summary line + TAP producer stream when `tap=1`; (b) `UTEST_FAIL` is folded into the final exit-code gate so user-mode regressions cannot hide behind a green kernel summary.
> - Two missing per-binary bats added: [`run-test_harness_smoke.bat`](../../scripts/debug/usermode/run-test_harness_smoke.bat) (§1 framework self-check) and [`run-test_faultinject.bat`](../../scripts/debug/usermode/run-test_faultinject.bat) (§5 probe). Every deployed `test_*.exe` now has a one-binary bat runner; `run-all-usermode-tests.bat` runs them in manifest order.
> - TAP wiring: launcher already emits `UTEST: ok N - name` / `not ok N - name # reason` / `1..N` lines when `tap=1`. `scripts/test.sh` Step 5b detects the stream via grep and prints a separate `TAP producer stream:` block under the per-binary verdicts so external TAP consumers (CI, tap-junit) can lift them straight from the host log without re-parsing the launcher wrapper formatting.
> - Canonical doc: [`scripts/test.sh`](../../scripts/test.sh) Step 5b (host-side parsing); [`scripts/machines/run-qemu.ps1`](../../scripts/machines/run-qemu.ps1) `-UtestFilter` (per-binary boot knob); [`scripts/debug/usermode/`](../../scripts/debug/usermode/) (Windows runners).
> - Scope boundary: §16 owns `make test` integration + per-binary bats + UTEST/TAP host parsing; §7 owns `[UTEST-XML]` JUnit XML stream (already wired in test.sh Step 6); §4 owns the kernel-side `tap=` config + TAP emission; §15 ships the loader-coverage triplet (UTEST `format=` plumb-through is consumed by §16's parser to surface routing regressions).

> **Verified:** 2026-04-22 | 10/10 items | build OK | tests 1800 kernel + 14 user-mode PASS (KVM) | smoke PASS (2.3s)
> **Quality reviewed:** 2026-04-22 | Codex 1x (adversarial) | 1H+1M fixed, 0 open | scope: userland-code-quality

---

## 17. Fast-Path Transport Hardening (probes + ABI drift + silent-hang elimination)

The ring-0↔3 fast paths (`gs:`-relative TEB reads, `KUSER_SHARED_DATA` at 0x7FFE0000, `syscall`/`sysret`) are load-bearing for Win11 parity but historically fragile: the first §14 revision tried all three mechanisms, hung silently on WHPX with no log or task-exit, and was routed around via INT 0x80. Every piece of infrastructure they depend on is now in place (TEB allocated with `ClientId.UniqueProcess` at offset 0x40 in [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c):1377, `MSR_IA32_KERNEL_GS_BASE` context-switched in the scheduler lines 750-760, KUSD page mapped user-RO at 0x7FFE0000 in [`src/kernel/time/kusd_time.c`](../../src/kernel/time/kusd_time.c), STAR/LSTAR/SFMASK + EFER.SCE programmed in [`src/kernel/sched/syscall_fast.c`](../../src/kernel/sched/syscall_fast.c):51-107) -- the work here is isolation probes + the five hardening pieces that make silent hangs impossible to reproduce ever again.

> [!WARNING]
> The §14 revision hung on WHPX at `movq %gs:0x40, %rax` with no panic, no task-exit log, no serial output. Every item below is chosen to convert that failure mode into a visible, named diagnostic; if ANY probe still hangs after this section ships, that is a kernel or hypervisor bug worth filing separately, not a reason to route around the fast path again.

- [x] Built [`user/test/test_fastpath.c`](../../user/test/test_fastpath.c) -- five isolated probes each emitting `UTEST: fastpath: probe N: ENTER` + `RESULT=<value>` + `PASS/FAIL` via `sys_log` so a hung probe leaves a serial trail naming exactly which one. Probes: (1) `gs:0x30` NT_TIB self-pointer, (2) `gs:0x60` ProcessEnvironmentBlock, (3) `gs:0x40` ClientId.UniqueProcess compared against `sys_getpid()`, (4) `*(volatile uint32_t *)0x7FFE0000` KUSD TickCountLowDeprecated, (5) `syscall` instruction with `rax = SSDT_NtClose, r10 = 0xFFFFFFFF` expecting `STATUS_INVALID_HANDLE = 0xC0000008`. ALL FIVE PROBES PASS (KVM 2026-04-22). Probe 5's original post-sysret #PF at CR2=0 was root-caused to an under-specified inline-asm clobber list: the kernel's `syscall_entry.asm` stub TRANSLATES Win64-ABI args into SysV-ABI slots before calling the C dispatcher (`rdi=rax`, `rsi=r10`, `rcx=r8`, `r8=r9`), leaving `rdi/rsi/rdx/r8/r9/r10` trashed on `sysret` return. Clang kept treating those registers as live, so the next read of `rdi` in the caller loaded kernel scratch garbage (pointing at 0x0) and user wrote to linear 0x0. Fixed by expanding the clobber list to include all kernel-trashed registers AND modeling r10 as `"+r"(r10)` inout so the compiler treats it as dead after the asm (Codex [H] fix on inline asm contract). Exit code = OR of FAIL bits so a drifted probe names itself in the launcher's `UTEST: test_fastpath.exe: FAIL (exit=<bitmap>)` verdict. Summary reports `fastpath: 5/5 probes PASS`.
- [x] Kernel invariant in [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) ring-3 dispatch path: after every `msr_write(MSR_IA32_KERNEL_GS_BASE, new_gs)` on the ring-3-bound path (preemptive `schedule()` and cooperative `schedule_now()`), read back and `klog(LOG_FATAL, ...)` on mismatch. Turns a WHPX-swallowed MSR write into a visible crash at the write site instead of silent TEB corruption that propagates into `gs:0x40` land. PLUS: fail-closed panic when `new_gs == 0` but the next task has a TEB on either `tasks[].teb` or `threads[].teb` (Codex [H] fix -- original version only logged LOG_ERROR and continued, which silently sent user mode back to ring 3 with GS_BASE=0).
- [x] Exec-pending save-gate: `task_exec()` writes `threads[0].kernel_gs_base = TEB` but the first scheduler switch-IN has not yet programmed the MSR, so the save-before-write in the scheduler used to overwrite the primed TEB with the stale kernel MSR value. Root-caused + fixed: the save branch now skips when `__atomic_load_n(&tasks[prev_task].exec_pending, __ATOMIC_ACQUIRE)` is set; `exec_pending` writes at `task_exec()` entry + at the switch-IN clear are now `__atomic_store_n(..., __ATOMIC_RELEASE)` paired so a future cross-CPU scheduler redesign inherits correct memory-order semantics (Codex [Critical] belt-and-suspenders; current Impossible OS scheduler is single-CPU, so no race exists today, but the atomics cost nothing and pre-empt the SMP bug).
- [x] Kernel-side `_Static_assert(__builtin_offsetof(TEB, ClientId) == 0x40, ...)` already present in [`include/kernel/ob/teb.h`](../../include/kernel/ob/teb.h):87. Added mirror in new [`user/include/teb.h`](../../user/include/teb.h) -- minimal user-visible TEB subset (only the fields inline asm reads) with asserts for `NtTib.Self` (0x30), `ClientId.UniqueProcess` (0x40), `ClientId.UniqueThread` (0x48), `ProcessEnvironmentBlock` (0x60), `LastErrorValue` (0x68). Similarly [`user/include/kusd.h`](../../user/include/kusd.h) mirrors [`include/kernel/nt/kusd.h`](../../include/kernel/nt/kusd.h) offsets for `TickCountLowDeprecated` (0x000), `TickCountMultiplier` (0x004), `InterruptTime` (0x008), `SystemTime` (0x014), `TickCount` (0x320), `Cookie` (0x330). Kernel-side has even more asserts already; user side covers exactly what inline asm dereferences.
- [x] [`scripts/gen-user-abi.py`](../../scripts/gen-user-abi.py): reads `include/kernel/sched/syscall.h` (`SYS_*` + `FAULT_*`), `include/kernel/nt/service_numbers.h` (`SSDT_*` allowlist: `NtClose`, `NtTerminateProcess`, `NtYieldExecution`, `NtQuerySystemInformation`), `include/kernel/nt/ntstatus.h` (`STATUS_*` allowlist). Emits [`user/include/abi_numbers.h`](../../user/include/abi_numbers.h) (generated, committed: 50 syscalls + 4 SSDT + 4 NTSTATUS). `--check` mode diffs committed vs regenerated and exits 1 on drift. [`Makefile`](../../Makefile) new `check-abi` target runs it in --check mode; wired into `all:` so every `make test` / `bash scripts/build.sh` / CI invocation fails fast on drift (Codex [H] fix -- previously the target was standalone and not chained into the default build). [`user/include/syscall.h`](../../user/include/syscall.h) collapsed the hand-copied `#define SYS_*` block to `#include "abi_numbers.h"` so ALL user binaries (not just the fastpath probe) get drift-protected numbers (Codex [H] fix -- original wired only test_fastpath.c to the generator).
- [x] `user/lib/win32.c` GetCurrentProcessId migrated off INT 0x80 to the native `movq %gs:0x40, %rax` fast path. Zero syscall, zero ring transition. The INT 0x80 fallback was deleted per the checklist gate; regression now surfaces immediately as a user-mode #PF on the probe binary AND on any Win32 caller. Inline `_Static_assert(__builtin_offsetof(USER_TEB, ClientId.UniqueProcess) == 0x40, ...)` inside the function body pins the offset at compile time. Validated: `test_win32.exe`'s `GetCurrentProcessId() returns PID > 0` assertion still PASSes after migration (KVM 2026-04-22).
- [x] Platform coverage: all 5/5 probes PASS on KVM (validated 2026-04-22). Prior WHPX run (4/4 active + 1 SKIP) captured the kernel_gs_base fix; re-validation on WHPX with the probe-5 live is the user's next check on their test rig. TCG + VirtualBox + bare metal remain user-owned validation; the probe binary auto-runs in manifest order ahead of every other user-mode binary so a regression on any of those platforms surfaces as FAIL on this binary alone. Serial-log summary line: `fastpath: 5/5 probes PASS`.
- [x] Commit: `"test+sched: fast-path transport hardening (probes, MSR readback, ABI generator, static asserts)"`

**Test checkpoint:** `scripts\debug\usermode\run-test_fastpath.bat` boots and prints five `fastpath: probe N: ENTER` + `RESULT=<value>` + `PASS/SKIP/FAIL` lines on serial followed by `fastpath: 4/4 active probes PASS, 1 SKIP (probe 5 syscall)`; a forced regression (change TEB offset in one header and rebuild) fails the `_Static_assert` at compile time OR prints `fastpath: probe 3: FAIL gs:0x40 != sys_getpid` at runtime, naming exactly which offset drifted. Kernel invariant panic fires if `MSR_IA32_KERNEL_GS_BASE` readback disagrees with the written value OR if a task with a TEB reaches the ring-3 switch with `kernel_gs_base=0`. `make check-abi` re-runs `gen-user-abi.py` and exits non-zero if `user/include/abi_numbers.h` drifted from kernel source -- wired into `all:` so every build (including `make test` and CI) gates on it. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\usermode\run-test_fastpath.bat` (utest_filter=test_fastpath.exe via `-UtestFilter` + `-NoKernelTests`) | 5/5 probes PASS, exit=0 (validated KVM 2026-04-22)

> **Notes:**
> - Five-probe binary ships at [`user/test/test_fastpath.c`](../../user/test/test_fastpath.c). All five PASS end-to-end on KVM: TEB self-pointer (gs:0x30), PEB (gs:0x60), ClientId.UniqueProcess matching `sys_getpid()` (gs:0x40), KUSD readback at 0x7FFE0000, and SYSCALL into SSDT_NtClose returning STATUS_INVALID_HANDLE (0xC0000008). Probe 5's original post-sysret #PF at CR2=0 was a user-side inline-asm clobber-list bug, not a kernel sysret path bug -- kernel stub is correct.
> - Root-cause fix for the original -14 "silent gs:0x40 hang on WHPX": the scheduler's save-before-write sequence was overwriting `threads[0].kernel_gs_base` with the stale MSR value before the first ring-3 switch. Added `exec_pending` save-gate (atomic load) so `task_exec`'s TEB pointer survives until the scheduler programs it into the MSR. Paired fail-closed `klog(LOG_FATAL, ...)` for the `TEB-set-but-kernel_gs_base=0` invariant so any future priming bug panics at the scheduler rather than corrupting user GS.
> - MSR readback invariant: both `schedule()` and `schedule_now()` now RDMSR after every `msr_write(MSR_IA32_KERNEL_GS_BASE, ...)` and call `klog(LOG_FATAL, ...)` on mismatch. Costs ~30 cycles per context switch; converts a hypervisor that silently swallows the MSR write into a named crash at the write site.
> - ABI generator [`scripts/gen-user-abi.py`](../../scripts/gen-user-abi.py) reads kernel `SYS_*` + `SSDT_*` + `STATUS_*` tables, emits generated+committed [`user/include/abi_numbers.h`](../../user/include/abi_numbers.h). `make check-abi` runs `--check` mode; wired into `all:` so every build gates on drift. [`user/include/syscall.h`](../../user/include/syscall.h) collapsed its hand-copied `SYS_*` block to `#include "abi_numbers.h"` so ALL user binaries get the protection, not just the probe.
> - User-side layout asserts: new [`user/include/teb.h`](../../user/include/teb.h) + [`user/include/kusd.h`](../../user/include/kusd.h) mirror kernel offsets via `_Static_assert(__builtin_offsetof(...) == literal)` for every field the inline asm dereferences. TEB/KUSD layout drift on either side of the ring-0/3 boundary fails compilation with a named message.
> - Canonical docs: [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) schedule paths (MSR readback + exec_pending gate); [`include/kernel/ob/teb.h`](../../include/kernel/ob/teb.h) (kernel TEB + asserts); [`include/kernel/nt/kusd.h`](../../include/kernel/nt/kusd.h) (kernel KUSD + asserts); [`scripts/gen-user-abi.py`](../../scripts/gen-user-abi.py) (drift gate).
> - Scope boundary: §17 owns probe + per-probe logging + MSR readback + ABI generator + static asserts -- the stability floor. §18 extends beyond parity with versioned ABI fingerprint, self-describing KUSD, all-ring-transition invariants, transition ring-buffer dumping. Probe 5 sysret investigation AND the win32.c PID migration both live in future debug sessions; concrete trace: user-mode #PF at CR2=0x0 in format_hex64 after sysret suggests either user RSP corruption or wrong RIP restore (look in [`src/kernel/sched/syscall_entry.asm`](../../src/kernel/sched/syscall_entry.asm) pop-rsp path).

> **Verified:** 2026-04-22 | 8/8 items | build OK | check-abi PASS | fastpath 5/5 probes PASS (KVM 2026-04-22) | test_win32 GetCurrentProcessId PASS on native gs:0x40
> **Quality reviewed:** 2026-04-22 | Codex 2x (adversarial, follow-up) | 1Critical+4H+1M fixed, 0 open | scope: kernel-code-quality + userland-code-quality

> [!TIP]
> **Scope boundary:** §17 owns the probe + per-probe logging + MSR readback + ABI generator + static asserts -- the stability floor that makes the fast path as reliable as Win11/Linux parity today. §18 builds on top with features NEITHER Win11 NOR Linux ships: versioned ABI fingerprint, self-describing KUSD, all-ring-transition invariants, and transition ring-buffer dumping.

---

## 18. Fast-Path Observability + ABI Versioning (beyond Win11 + Linux)

§17 stops silent hangs and kills drift; §18 makes the fast-path system stronger than both competitors. Win11 has KUSER_SHARED_DATA at a fixed VA but no layout fingerprint, so a silent ABI bump between Win10 and Win11 24H2 shifts later fields and user code discovers it at runtime. Linux's vDSO is signed but carries no layout version either, and kselftest has no concept of "dump the last 64 ring transitions on crash" -- a user-mode hang requires out-of-band `perf`/`ftrace` setup. Impossible OS can ship always-on observability + versioned ABI + invariant-guarded transitions in the baseline kernel, not as opt-in profiler add-ons.

> [!TIP]
> Every item in this section is `⭐` (exclusive). Neither Windows 11 nor Linux 6.x exposes this capability today; together they turn silent fast-path failures into named, replayable, compile-time-verifiable diagnostics.

- [x] Versioned ABI fingerprint. [`scripts/gen-user-abi.py`](../../scripts/gen-user-abi.py) computes an FNV-1a 64-bit hash over the sorted tuple of `(SYS_*, SSDT_*, TEB offsets, KUSD offsets)` and emits the same `IMPOSSIBLE_OS_ABI_HASH` constant into [`user/include/abi_numbers.h`](../../user/include/abi_numbers.h) AND [`include/kernel/abi_hash.h`](../../include/kernel/abi_hash.h). New `SYS_ABI_HANDSHAKE = 47` in [`include/kernel/sched/syscall.h`](../../include/kernel/sched/syscall.h) returns the kernel's compiled-in hash; [`user/lib/crt_init.c`](../../user/lib/crt_init.c) calls it from the new crt0-invoked `crt_init()` helper and `exit(EX_ABI_MISMATCH = 0x42)` on disagreement BEFORE any other user syscall with potentially corrupted semantics. `make check-abi` (already wired into `all:` by -17) catches drift at build time; the handshake is the runtime gate for kernel/libc version skew. Neither Win11 nor Linux does this -- on both you discover ABI drift by crashing.
- [ ] Fold syscall arg counts into the ABI fingerprint in `gen-user-abi.py` (today only SYS_* numbers) so a signature change (SYS_EXEC path,len->path,argv,envp, TODO-22 §4) bumps IMPOSSIBLE_OS_ABI_HASH and stale binaries fail the handshake
- [x] Self-describing KUSER_SHARED_DATA header. Placed at offset 0x340 (start of the previously-reserved padding, AFTER the Windows-compatible portion so a hypothetical Win11 KUSD layout drop-in would not clobber it): 32-byte typed block with `AbiMagic = 'KUSD' (0x4453554B)`, `AbiVersion`, `AbiStructSize = 0x340`, `AbiLayoutHash = IMPOSSIBLE_OS_ABI_HASH`, `AbiBuildTimestamp` (future), and two reserved slots. Kernel writes in [`src/kernel/time/kusd_time.c`](../../src/kernel/time/kusd_time.c) `kusd_init()` with magic written LAST so readers see a complete header atomically. User-side mirror in [`user/include/kusd.h`](../../user/include/kusd.h) with matching `_Static_assert` on every field offset; [`user/lib/crt_init.c`](../../user/lib/crt_init.c) validates magic + hash after the SYS_ABI_HANDSHAKE check so a wedge-state KUSD (magic zero, wrong hash) triggers the same `EX_ABI_MISMATCH=0x42` abort a kernel/libc mismatch does.
- [x] Invariant-guarded ring-3 transitions -- TWO of five asserts shipped today: (a) `CS & 3 == 3` before iretq return to ring 3, (b) `SS & 3 == 3` paired (CS DPL=3 with SS DPL!=3 would deliver a #GP on iretq anyway; this surfaces the corrupt frame at the scheduler instead of the CPU). Implemented in [`src/kernel/idt.c`](../../src/kernel/idt.c) `isr_handler` tail after IRQL restore, `klog(LOG_FATAL, ...)` with selector dump on violation. Validated always-on across all 1812 kernel tests + 15 user-mode binaries: zero false positives. The remaining three asserts (MSR_KERNEL_GS_BASE matches expected TEB, CR3 matches expected page table, RSP within user stack range) need per-CPU cached task info that does not exist in the kernel today -- calling `task_current()` mid-iretq is unsafe per the design review. Filed as owner item: [02-kernel-core/TODO-06-executive-support-runtime.md § per-CPU cached task info] so those three invariants can land when the cache does.
- [x] Kernel-side pair: [`src/kernel/test/test_fastpath_hardening.c`](../../src/kernel/test/test_fastpath_hardening.c) (new, 5 test functions, 12 assertions under `TEST_CAT_EXEC`): (a) `IMPOSSIBLE_OS_ABI_HASH` non-zero + not all-ones + upper 32 bits non-zero (catches generator truncation), (b) `g_kusd` populated (kusd_init ran), AbiMagic == `KUSD_ABI_MAGIC`, AbiVersion matches, AbiStructSize == 0x340, (c) AbiLayoutHash == IMPOSSIBLE_OS_ABI_HASH (same-kernel-binary consistency), (d) offset asserts pin the layout at test time even though `_Static_assert` also catches drift at compile time. Registered via new `test_register_fastpath_hardening()` after `test_register_usermode_launcher()` in [`src/kernel/test/test_runner.c`](../../src/kernel/test/test_runner.c). All 12 assertions PASS (kernel test total 1812, up from 1800).
- [x] Commit: `"sched+mm: fast-path hardening beyond Win11/Linux (ABI hash, self-describing KUSD, iretq invariants partial)"`

**Test checkpoint:** `make test` prints `TEST: === 1812 tests passed ...` (+12 from §18 fastpath_hardening suite). All 15 user-mode binaries including `test_fastpath.exe` 5/5 probes PASS -- every binary runs crt_init's ABI handshake silently on the happy path; a forced libc rebuild with the hash constant flipped would cause every binary to exit 0x42 BEFORE its first real syscall. Kernel-side `g_kusd->AbiMagic` is literal 'KUSD' in memory at 0x340. iretq invariants fire silently; a forced CS.DPL=0 frame would LOG_FATAL with selector dump. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-exec-tests.bat` (SUITE=exec) | 1812 kernel tests PASS (12 new for §18); `scripts\debug\usermode\run-all-usermode-tests.bat` | 15/15 user-mode binaries PASS (KVM 2026-04-22)

> **Notes:**
> - Three items shipped fully (versioned ABI fingerprint, self-describing KUSD, kernel unit test); one item partially shipped (iretq invariants: 2/5 asserts); two items deferred together as a coherent follow-up block (transition ring + fuzz binary, which share per-CPU-cache infrastructure with the remaining 3 iretq asserts).
> - ABI fingerprint hash today: `0x9DE28D9C5784C062`. Changes whenever any of SYS_* numbers, SSDT_* numbers, TEB field offsets, or KUSD field offsets change. `make check-abi` gates `all:` and the runtime handshake in `crt_init` gates every user process start -- two layers, same `IMPOSSIBLE_OS_ABI_HASH` constant, one source of truth in the generator.
> - KUSD header at 0x340 is AFTER Windows's documented KUSD layout; a hypothetical Win11 drop-in of Microsoft's struct would not clobber the Impossible-OS ABI block. `_Static_assert` pairs on kernel + user sides catch layout drift at compile time on BOTH boundaries.
> - iretq partial: 2 cheap asserts (CS.DPL, SS.DPL) land in `isr_handler`'s tail. The remaining 3 (MSR_KERNEL_GS_BASE, CR3, RSP-in-stack) need a per-CPU cached `struct task_info` because calling `task_current()` mid-return is unsafe (pointer deref requires kernel page tables that could be mid-swap). That cache is a standalone piece of infrastructure that other subsystems will also consume, so it belongs in the kernel-core TODO rather than inlined here.
> - Canonical docs: [`scripts/gen-user-abi.py`](../../scripts/gen-user-abi.py) (single source of truth for the hash), [`include/kernel/abi_hash.h`](../../include/kernel/abi_hash.h) (generated kernel copy), [`user/lib/crt_init.c`](../../user/lib/crt_init.c) (runtime handshake site).
> - Scope boundary: §18 owns the ABI fingerprint + self-describing KUSD + iretq DPL invariants + their unit tests. Transition ring buffer + 3-way fuzz + remaining 3 iretq invariants live as a §18 follow-up pending per-CPU cached task info.

> **Verified:** 2026-04-22 | 5/5 items | build OK | check-abi PASS | kernel 1812 PASS (+12 §18) | user-mode 15/15 PASS incl fastpath 5/5
> **Accepted:** [M] 3/5 iretq invariants (MSR_KERNEL_GS_BASE, CR3, RSP) need per-CPU cached task info (reason: task_current() unsafe mid-iretq) -> XREF: 02-kernel-core/TODO-06-executive-support-runtime.md (kernel-core executive support; re-open once per-CPU cache lands)
> **Quality reviewed:** 2026-04-22 | Codex 1x (adversarial) | 1H+1M fixed, 0 open | scope: kernel-code-quality + userland-code-quality

---

## 19. Fast-Path Transition Ring Buffer + Cross-Transport Fuzz

§18 establishes the ABI handshake + static invariants that stop a drifted fast path from corrupting user processes silently. §19 ships the OBSERVABILITY layer on top: an always-on per-CPU ring buffer that records every ring-0↔3 transition, dumped on any panic, plus a fuzz binary that exercises all three native transports (INT 0x80 / SYSCALL / INT 0x2E) comparing their returns for parity. Together they turn the remaining "silent hang" class of bugs into debuggable diagnostics: a user-mode task that deadlocks with no output still has its last 64 ring-3 transitions visible at NMI panic time, and any transport-level regression fails the fuzz binary with a replayable seed rather than a silent divergence weeks later.

> [!TIP]
> This section was split out of §18 so the tightly-coupled transition-ring + fuzz pair can be tracked + closed as a unit without gating the §18 ABI handshake work. Earlier drafts claimed §19 depended on per-CPU cached task info landing in `02-kernel-core/TODO-06` -- that was incorrect. The ring only records CURRENT register state (tsc, cr3, gs_base, kernel_gs_base, rip, rsp via direct reads or frame pointer); it never needs the `task_current()->...` comparisons that block the remaining §18 iretq invariants. §19 is implementable independently. Only the 3 remaining §18 iretq invariants (MSR/CR3/RSP matching cached expected values) still wait on the kernel-core per-CPU cache.

- [x] Per-CPU transition ring shipped at [`src/kernel/sched/transition_ring.c`](../../src/kernel/sched/transition_ring.c) + [`include/kernel/sched/transition_ring.h`](../../include/kernel/sched/transition_ring.h). 64-entry lock-free circular buffer per CPU, entry layout `{tsc, thread_id, direction, cr3, rip, rsp, gs_base, kernel_gs_base}`. Head index + init marker live in `struct per_cpu_data` at the end of the struct (no fixed offset contract -- C-only access, no ASM dereference). Init function `transition_ring_init_this_cpu()` is idempotent via a `TRANSITION_INIT_MARKER = 0xC0CAF00D` that's written LAST after all entries are zeroed, so a concurrent reader cannot see a partially-initialized ring.
- [x] Hooks at all three transport sites: (a) ISR entry in [`src/kernel/idt.c`](../../src/kernel/idt.c) `isr_handler` when `(frame->cs & 3) == 3` -- covers INT 0x80, INT 0x2E, and hardware IRQs preempting user code, (b) ISR exit in the same function's tail alongside the existing CS.DPL invariant, recording the ring-3-bound `(rip, rsp)` from the returning iret frame, (c) SYSCALL entry + exit in [`src/kernel/sched/syscall_entry.asm`](../../src/kernel/sched/syscall_entry.asm) with `transition_ring_record` called before/after `syscall_dispatch_fast`, preserving `rax`/`r10`/`rdx` (entry) and `rax` (exit) across the C call. All hooks are no-ops before the per-CPU init marker is set so pre-init transitions don't fault.
- [x] Panic dump in [`src/kernel/panic.c`](../../src/kernel/panic.c): `transition_ring_dump_to_serial(smp_this_cpu())` runs on the panic-owner CPU immediately after `panic_build_context` and before framebuffer/BSOD rendering. Emits `RING[slot] tsc=0x... U/K pid=... rip=0x... rsp=0x... cr3=0x... gs=0x... kgs=0x...` oldest-first. Guarded by the existing `s_panic_owner` CAS so only one CPU emits the dump; secondary panic CPUs park. Uninitialized ring (marker mismatch) emits a single `RING: not-initialized` line instead of faulting.
- [x] Fuzz binary [`user/test/test_fastpath_fuzz.c`](../../user/test/test_fastpath_fuzz.c): 1000 iterations per transport (3000 total syscalls, ~100ms on KVM). INT 0x80 stability check via `sys_getpid()` against a baseline; SYSCALL + INT 0x2E run `NtClose(INVALID ^ prng_byte)` asserting STATUS_INVALID_HANDLE on every call; cross-transport parity check calls both gates once with the same arg and asserts byte-identical NTSTATUS. `do_syscall_nt` + `do_int2e_nt` use matching clobber lists covering every register the kernel's SysV-ABI dispatcher may trash. Fuzz binary caught a REAL kernel bug on first run: `syscall_handler_2e` sign-extended NTSTATUS when writing `frame->rax` so user-mode saw `0xFFFFFFFFC0000008` via INT 0x2E vs `0xC0000008` via SYSCALL. Fixed in [`src/kernel/sched/syscall.c`](../../src/kernel/sched/syscall.c) via `(uint64_t)(uint32_t)result`; §19 is shipping a passing fuzz now.
- [x] Kernel-side assertion: new test `RING: -19 transition ring initialized` in [`src/kernel/test/test_fastpath_hardening.c`](../../src/kernel/test/test_fastpath_hardening.c) -- pure oracle reads of `smp_this_cpu()->transition_init_marker` and `TRANSITION_RING_SIZE` (no live init call, no boot-infrastructure call). Asserts the marker is set on the BSP and the ring is sized to 64 entries.
- [x] Manifest + bat runner: [`tests/usermode.manifest`](../../tests/usermode.manifest) gets `test_fastpath_fuzz.exe` in the correctness phase; [`scripts/debug/usermode/run-test_fastpath_fuzz.bat`](../../scripts/debug/usermode/run-test_fastpath_fuzz.bat) mirrors the other per-binary bat runners with `utest_filter=test_fastpath_fuzz.exe + -NoKernelTests`.
- [x] Commit: `"sched+test: fast-path transition ring buffer + 3-way transport fuzz"`

**Test checkpoint:** `test_fastpath_fuzz.exe` PASSes 4/4 assertions (1000 iters each of INT 0x80 sys_getpid, SYSCALL NtClose(invalid), INT 0x2E NtClose(invalid), + cross-transport parity). Kernel test `RING: -19 transition ring initialized` adds 3 assertions to `TEST_CAT_EXEC` (1815 -> 1818 total). Forced panic dumps the ring as `RING[0..63]` entries before the regular panic layout; entries are visibly monotonically non-decreasing by TSC. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\usermode\run-test_fastpath_fuzz.bat` | 4/4 assertions PASS (3000 transport syscalls + 1 parity check, exit=0, validated KVM 2026-04-22); kernel-side ring-init assertion adds 3 tests to TEST_CAT_EXEC.

> **Notes:**
> - Ring subsystem lives at [`src/kernel/sched/transition_ring.c`](../../src/kernel/sched/transition_ring.c) (~140 LOC) + header at [`include/kernel/sched/transition_ring.h`](../../include/kernel/sched/transition_ring.h). Per-CPU storage piggybacks on `struct per_cpu_data` tail -- no fixed gs: offset contract, no ASM dereference. C-only API makes future changes (additional fields, different dump format) a one-file edit.
> - ASM hooks added only on the SYSCALL path; INT 0x80 + INT 0x2E + hardware IRQs flow through `isr_handler` where the C-level check `(frame->cs & 3) == 3` gates the record. Combined cost: ~30 cycles per ring-3 transition plus one RDMSR (`MSR_IA32_KERNEL_GS_BASE`) per record. Measurable but not meaningful for syscall-heavy workloads.
> - Fuzz found a real bug: `syscall_handler_2e` used `(uint64_t)result` which sign-extends an `int32_t` NTSTATUS. User-mode INT 0x2E callers saw `0xFFFFFFFFC0000008` (sign-extended) where SYSCALL callers saw `0xC0000008` (zero-extended via x86-64 ABI's `mov eax, ...` implicit top-32-bit clearing). Every NT error code in the `0xC0000000+` range would have produced a different 64-bit value depending on which transport was used -- any user-mode caller comparing NTSTATUS as uint64 across transports (which sign-extended is `!= STATUS_SUCCESS` when a raw-read test expects exact equality to the uint32 value) would have been buggy forever. Not caught by existing tests because no prior test exercised INT 0x2E from user mode.
> - Canonical docs: [`include/kernel/sched/transition_ring.h`](../../include/kernel/sched/transition_ring.h) (API contract); [`src/kernel/sched/transition_ring.c`](../../src/kernel/sched/transition_ring.c) (implementation); [`user/test/test_fastpath_fuzz.c`](../../user/test/test_fastpath_fuzz.c) (consumer example).
> - Scope boundary: §19 owns the per-CPU ring + 3 transport hooks + panic dump + fuzz binary + ring-init unit test + the NTSTATUS sign-extension fix in INT 0x2E. Does NOT own: the 3 remaining §18 iretq invariants (MSR/CR3/RSP vs cached expected values -- those still wait on `02-kernel-core/TODO-06` per-CPU cached task info). §19 unblocked because the ring only records CURRENT register state, not comparisons against expected state.

> **Verified:** 2026-04-22 | 7/7 items | build OK | check-abi PASS | kernel 1818 PASS (+3 for ring-init) | user-mode 16/16 PASS incl test_fastpath_fuzz.exe 4/4
> **Quality reviewed:** 2026-04-22 | Codex pending | scope: kernel-code-quality + userland-code-quality

> [!NOTE]
> Earlier drafts warned this section waited on `02-kernel-core/TODO-06` per-CPU cached task info. That was wrong: the ring only records CURRENT register state, so no cached-expected-value comparison is needed. Only the 3 remaining §18 iretq invariants (MSR/CR3/RSP vs expected) still wait on the kernel-core cache.

---

## 20. Site-Targeted and PMM-Countdown Fault Selectors

§5 shipped the ring-3 fault-injection bridge, and it is enough to prove that SOME allocation failure on a path is handled. It cannot prove that a NAMED allocation site is handled, because the only addressing it offers is an ordinal: `FAULT_KMALLOC_COUNTDOWN` fails the Nth kmalloc after arming, and which branch that lands on shifts the moment any code on the path adds or removes an allocation. Two consequences surfaced while implementing `02-kernel-core/TODO-21` §19 and neither is fixable in the consuming test: a countdown cannot pin `task_exec`'s individual pre-commit OOM branches (they are reachable, but only by an ordinal that silently drifts), and `FAULT_PMM_NEXT` is single-shot with no countdown form at all, so the Nth PMM frame allocation cannot be failed from ring 3 even by ordinal.

> [!TIP]
> Linux `fail_page_alloc` / `failslab` address this with per-callsite debugfs knobs plus `probability`/`interval`/`times`; a `should_fail()` call at the site consults its own state. The equivalent here is a small site ID passed to the existing `*_should_fire()` gate, which keeps the arming surface a single syscall rather than a filesystem.

> [!NOTE]
> Split 2026-07-28. This section originally carried three unrelated mechanisms behind one heading and the complexity oracle flagged it SPLIT-RECOMMENDED (6 subsystems, ABI impact, 7 open items). §20 now owns ONLY the self-PID fault-selector addressing work (PMM countdown + named site IDs). Child-targeted arming across `fork()` moved to §21 because it is a different mechanism -- the filter must name a PID that does not exist yet, which is an inheritance question, not an addressing one. The launcher skip-reporting protocol and the generated exit-status ABI moved to §22 because neither touches fault injection at all; they are harness/ABI truthfulness work.

- [ ] `FAULT_PMM_COUNTDOWN` selector mirroring `FAULT_KMALLOC_COUNTDOWN`: bridge `pmm_alloc_fail_countdown_set(N)` (already implemented in
      `src/kernel/mm/pmm.c`, currently unreachable from ring 3) through `sys_fault_inject_dispatch` with the same self-PID task filter the other selectors apply. Without it no ring-3 test can fail the Nth frame allocation, which is what the post-load PEB/TEB allocations in `task_exec` need. -> XREF: `02-kernel-core/TODO-21-process-model-extensions.md` §19 (item: "Site-targeted fault injection for `task_exec`'s OWN pre-commit OOM branches")
- [ ] Site-targeted arming: a stable site ID (enum in a shared header, not a line number) that a test names to fail one
      SPECIFIC allocation regardless of how many allocations precede it. Minimum viable set is the sites existing tests cannot otherwise pin: the `task_exec` argv address table and private-frame table (reachable only by kmalloc ORDINAL, which drifts) and the `peb_alloc_for_task` frames (need the PMM countdown above). The replacement guarded kernel stack is NOT in this set -- `FAULT_PMM_NEXT` already lands on it directly and it is covered by a ring-3 test today
- [ ] Extend `user/test/test_faultinject.c` with a regression per new selector, and assert the negative case: arming a
      site ID that the exercised path never reaches must NOT fire, so a mis-targeted test fails loudly instead of passing on an unrelated allocation
- [ ] Commit: `"test: site-targeted and PMM-countdown fault selectors"`

**Test checkpoint:** `bash scripts/test.sh SUITE=exec` -- a test arming a named `task_exec` site observes that exact branch's refusal (identified by its klog) and no other; arming `FAULT_PMM_COUNTDOWN` with N greater than the path's frame count leaves the path succeeding; the gate still denies every new selector under `boot.conf test=0`. Test on: QEMU TCG, QEMU KVM; bare metal.

---

## 21. Child-Targeted Fault Arming Across `fork()`

Every selector §5 and §20 ship binds its task filter to `task_current()->pid` at ARM time. That is the correct default -- it is what stops a sibling kthread or the launcher task from consuming a test's pending trap -- but it makes one class of failure permanently untestable from ring 3: a fault that must fire inside a child that does not exist yet. The concrete casualty is fork's `vmm_create_user_pml4` allocation. Its failure path leaves a fork child running on the parent's CR3 with no isolated address space; the kernel-side refusal is shipped and gate-verified (`task.c` `forked_shares_parent_image`), but nothing in ring 3 can drive it, because by the time the child exists the arming window has closed. Arming from inside the child is not a substitute either: the allocation happens during `fork()` itself, before any child code runs.

> [!TIP]
> Linux solves the same problem with `fail-nth` on the task_struct plus fault-injection attributes that a child inherits across `clone()`. The narrower equivalent here is an arm-for-descendants flag on the existing filter: the filter matches the arming PID OR any task forked from it after the arm, consumed once, so the parent's own subsequent allocations do not eat the trap.

- [ ] Filter semantics: extend the allocator task filters so an arm can name descendants of the arming task rather than the
      arming task itself, and define the consumption rule explicitly (which task consumes the trap when parent and child both allocate, and whether the arm survives a second fork). Kernel-side only -- no new syscall number; the selector kind or a flag bit carries the intent through the existing `sys_fault_inject_dispatch` signature. The self-PID default must be unchanged for every existing selector: §5's isolation contract is what keeps the other ring-3 tests deterministic
- [ ] Ring-3 regression in `user/test/test_faultinject.c` driving fork's `vmm_create_user_pml4` OOM: the parent arms for its
      next child, forks, and asserts fork refuses rather than producing a child sharing the parent's CR3. Assert the negative case too -- a descendant-targeted arm must NOT be consumed by the parent's own allocations between arm and fork. -> XREF: `02-kernel-core/TODO-21-process-model-extensions.md` §19 (item: "Fork-child-with-no-isolated-CR3")
- [ ] Commit: `"sched+test: descendant-targeted fault arming across fork"`

**Test checkpoint:** `bash scripts/test.sh SUITE=exec` plus the `test_faultinject.exe` ring-3 binary -- a descendant-targeted arm fires exactly once inside the forked child and fork refuses with its named klog; the parent's allocations between arm and fork do not consume it; every existing self-PID selector still fires on the arming task only; `boot.conf test=0` still denies the new arming form. Test on: QEMU TCG, QEMU KVM; bare metal.

---

## 22. Honest Machine Artifacts for Skipped Sub-Tests

`UTEST_SKIP` (added 2026-07-28 in `user/include/test.h`) prints a `[SKIP]` line and records no state, so a binary whose preconditions were unavailable still ends `[UTEST-END] all pass`, returns `g_fail == 0`, and is classified PASS with zero skips in TAP / JUnit XML / JSON -- only a whole-binary exit 77 increments the launcher's skip counter. Raw serial is honest; every automated consumer is not, which is exactly the false-coverage signal the macro was introduced to remove. Filed as an Accepted finding by `02-kernel-core/TODO-21` §19 rather than fixed there because it is harness-side, not kernel-side.

> [!WARNING]
> Do NOT "fix" the skip reporting by returning exit 77 whenever any sub-test skipped. That discards the assertions that DID pass and reports a partly-verified binary as entirely unverified -- the opposite error, equally dishonest. The launcher protocol must be able to carry passed AND skipped together for the same binary.

- [ ] Track per-sub-test skip counts in harness state (`user/include/test.h`) and extend the launcher protocol so a binary
      reports passed + failed + skipped together, rather than the current pass/fail-plus-whole-binary-77 encoding. The exit-code contract stays backward compatible: 0 still means no failures and 77 still means the whole binary was skippable
- [ ] Propagate the skip counts into every machine artifact the launcher emits -- TAP (`# SKIP` directives on the skipped
      points), JUnit XML (`<skipped/>` elements plus the `skipped` attribute on the suite), and JSON -- so a consumer sees the same three-way outcome the serial log shows. §4 and §7 own these emitters
- [ ] Commit: `"test: honest skip reporting in machine artifacts"`

**Test checkpoint:** `bash scripts/test.sh` -- a binary that skips one sub-test and passes the rest is classified PASS with a NON-ZERO skip count in TAP, JUnit XML, and JSON simultaneously, and its passing assertions are still counted; a whole-binary exit 77 still reports as fully skipped. -> XREF: `02-kernel-core/TODO-21-process-model-extensions.md` §19 (item: "Fault-injected exec lifecycle coverage"). Test on: QEMU TCG, QEMU KVM.

---

## 23. Generated Exit-Status ABI for Ring-3 Assertions

Kernel exit-status constants that ring-3 tests assert on are hand-copied literals, so `make check-abi` cannot see them drift. `TASK_EXIT_EXEC_IMAGE_DESTROYED` (`include/kernel/sched/task.h`) is currently restated as a bare `-1001` in `user/test/test_process.c`: a kernel-side change to the value is invisible to the ABI check and surfaces only if that one runtime test happens to run, on a platform where it is not skipped. Every other cross-ring constant in the repo already rides the generated header; this class does not, for a mechanical reason rather than a design one.

> [!TIP]
> `parse_defines` in `scripts/gen-user-abi.py` extracts UNSIGNED integer literals by prefix, while the authoritative value here is a negative expression (`TASK_EXIT_REASON_BASE - 1`). The generator needs signed/expression resolution before it can carry any exit-status constant, so this is generator work first and a one-line consumer change second.

- [ ] Teach `scripts/gen-user-abi.py` to resolve signed and expression-valued constants (at minimum `BASE - N` forms over
      an already-extracted symbol), with an allowlisted `include/kernel/sched/task.h` exit-status source so the generator does not start hoovering unrelated `task.h` defines. A value the generator cannot resolve must fail the build loudly rather than silently omitting the name
- [ ] Emit `TASK_EXIT_EXEC_IMAGE_DESTROYED` into `user/include/abi_numbers.h` and consume it in `user/test/test_process.c`
      in place of the bare `-1001`, so the assertion tracks the kernel definition through `make check-abi`. -> XREF: `02-kernel-core/TODO-21-process-model-extensions.md` §19 (item: "`TASK_EXIT_EXEC_IMAGE_DESTROYED` added")
- [ ] Commit: `"abi: generate signed exit-status constants for ring-3 assertions"`

**Test checkpoint:** `make check-abi` PASSes with `TASK_EXIT_EXEC_IMAGE_DESTROYED` generated rather than hand-copied, and flipping the kernel-side value fails the check instead of passing silently; an unresolvable expression in the allowlisted source fails the build rather than omitting the constant; `bash scripts/test.sh` stays green with `test_process.exe` asserting on the generated name. Test on: QEMU TCG, QEMU KVM.

---

## OS Comparison

| ⭐   | Feature              | 🪟 Win11               | 🐧 Linux                 | 🚀 Impossible OS          |
| --- | -------------------- | --------------------- | ----------------------- | ------------------------ |
| 💎   | User-mode test bins  | ✅ HLK                 | ✅ kselftest             | ✅ §1-§15 15 binaries     |
| 💎   | Multi-format loader  | ✅ PE + .NET via HLK   | ✅ ELF + a.out kselftest | ✅ §15 ELF+PE32+ +EIF     |
| 💎   | Syscall coverage     | ✅ NtDll               | ✅ ptrace selftest       | ✅ §9 11 syscalls         |
| 💎   | Auto launcher        | ✅ HLK                 | ✅ run_kselftest         | ✅ §3 manifest-driven     |
| 💎   | TAP or CI parse      | ✅ HLK XML             | ✅ TAP kselftest         | ✅ §7 XML + §4 TAP        |
| 💎   | JUnit XML / JSON     | ✅ HLK XML             | ⚠️ kselftest TAP only   | ✅ §7 XML+JSON+TAP        |
| 💎   | Timeouts or skips    | ✅ HLK                 | ✅ LKFT skip             | ✅ §4 10s + exit=77       |
| 💎   | ABI header sync      | ✅ SDK                 | ✅ uapi                  | ✅ §2+§17 gen+static asr  |
| 💎   | Per-test isolation   | ✅ HLK session reset   | ✅ kselftest fork+tmp    | ✅ §6 scratch+reg+leak    |
| 💎   | Libc surface probe   | ✅ HLK CRT tests       | ✅ kselftest libc        | ✅ §10 7 contracts        |
| 💎   | IPC surface probe    | ✅ HLK pipe+shmem      | ✅ kselftest pipe+shm    | ✅ §11 pipe + shmem RT    |
| 💎   | Process lifecycle    | ✅ HLK fork+exec       | ✅ kselftest fork+exec   | ✅ §12 fork+exec+kill     |
| 💎   | File I/O surface     | ✅ HLK filesys tests   | ✅ kselftest openat etc. | ✅ §13 open+read+enum     |
| 💎   | Stress / longhaul    | ✅ TAEF Loop+Stress    | ✅ LTP runtest/stress    | ✅ §8 stress type         |
| 💎   | Perf regression      | ✅ perfview/PerfTest   | ✅ perf + flame baseline | ⚠️ §8 threshold-only     |
| 💎   | Test type taxonomy   | ✅ TAEF categories     | ✅ LTP test classes      | ✅ §8 4 types + phase     |
| ⭐   | Fault-inject bridge  | ⚠️ AppVerifier hooks  | ⚠️ debugfs failslab     | ✅ §5 SYS_FAULT_INJECT    |
| ⭐   | Win32 on non-Win     | ❌ N/A                 | ❌ Wine only             | ✅ §14 statically linked  |
| 💎   | Fast-path isolation  | ⚠️ HLK TEB probes     | ⚠️ kselftest vdso_test  | ✅ §17 5/5 probes PASS    |
| 💎   | ABI drift guard      | ⚠️ SDK hdr versioning | ✅ syscall.tbl generator | ✅ §17 gen-user-abi.py    |
| ⭐   | ABI fingerprint hash | ❌ silent Win10/11     | ❌ vDSO unsigned layout  | ✅ §18 FNV-1a handshake   |
| ⭐   | Self-describing KUSD | ❌ KUSD raw struct     | ❌ vDSO no layout ver    | ✅ §18 magic+ver+hash     |
| ⭐   | Ring-3 invariants    | ⚠️ debug-only checks  | ⚠️ CONFIG_DEBUG_ENTRY   | 🔄 §18 2/5 always-on      |
| ⭐   | Transition ring dump | ⚠️ opt-in perf/xperf  | ⚠️ opt-in perf/ftrace   | ✅ §19 always-on panic    |
| ⭐   | Transport fuzz in CI | ❌ external TAEF       | ❌ external syzkaller    | ✅ §19 3-way fuzz at boot |

> **Parity state (§1-§17 shipped):** every 💎 parity row is ✅ except `Perf regression`, which is ⚠️ because §8 asserts against hardcoded thresholds rather than tracking a historical baseline over time -- threshold regressions fail the run, but silent drift below the threshold would not. Closing that to full ✅ needs launcher-side `tests/perf-baseline.json` drift detection, which is deferred to §8 follow-up pending an env-passing syscall (tracked inline in §8's Deferred stamp). **⭐ exclusives:** §5 fault-inject bridge gives a typed `test=1`-gated kernel-allocator probe surface that AppVerifier hooks Win32 for and Linux only exposes through debugfs; §14 Win32-on-non-Win depends on `D02T12 §6` Win32 thunk landing; §17 closes the fast-path stability floor (probes + ABI generator + invariant panic) so TEB/KUSD/syscall transports are as robust as Win11 TEB reads and Linux vDSO calls; §18 will put Impossible OS past both competitors by baking versioned ABI handshake, self-describing KUSD, always-on ring-3 invariants, always-on transition ring-buffer dumps, and in-boot transport fuzzing into the baseline kernel -- capabilities that on Win11 and Linux require opt-in profilers, external fuzzers, or debug-only builds.

---

## Unit Tests

> [!NOTE]
> User-mode coverage is driven by `user/test/test_*.c` binaries and serial `[UTEST]` lines from §3 onward, not a dedicated `src/kernel/test/test_usermode.c` until a kernel-side wrapper is justified. §2 is header-only parity; §4 + §7 + §8 are launcher and serial-format policy. The §5 `SYS_FAULT_INJECT` test-mode gate negative regression is the one kernel-side `TEST_CAT_EXEC` assertion that DOES belong in `src/kernel/test/test_syscall.c`.

- [x] Commit: `"test: N/A single TEST_CAT file -- usermode harness per §1-§16 and Verification"` (no new kernel test file; user-mode coverage IS the test surface per the NOTE above, plus the single §5 SYS_FAULT_INJECT regression in test_syscall.c which shipped under its section)

**Test checkpoint:** After §3 ships, `bash scripts/test.sh SUITE=exec` (see `CLAUDE.md`) parses `[UTEST]` PASS/FAIL alongside kernel `[TEST]` lines.

---

## Verification

- [x] `make test` -> user-mode tests run after kernel tests and all pass (KVM 2026-04-23: `bash scripts/test.sh QUIET=1` exit=0, summary `PASS: 2184 kernel + 16 user-mode tests passed`; TCG 2026-04-23 via `FORCE_TCG=1`: 2166 kernel + 16 user-mode PASS, 0 leaked)
- [x] Break a syscall -> a user-mode test catches it -> `bash scripts/test.sh` fails locally. Demonstrated 2026-04-23: injected `ret = -1;` after the legitimate `ret = t->pid` in `src/kernel/sched/syscall.c` `case SYS_GETPID`, rebuilt + ran `bash scripts/test.sh QUIET=1`. Result: `test_fastpath.exe FAIL (exit=4)` (probe 3: `gs:0x40 ClientId.UniqueProcess` did not match `sys_getpid`) + `test_fastpath_fuzz.exe FAIL (exit=1)` (`sys_getpid baseline is positive` assertion fired) + wrapper exit=1 + `FAIL: 0 kernel + 2 user-mode of 2200 failed`. Reverted in the same session; subsequent run returned `exit=0` + `PASS: 2184 kernel + 16 user-mode tests passed`.
- [x] Add a new test binary -> one file + one Makefile line -> works in `make test` / `bash scripts/test.sh`. Demonstrated by the existing 16 binaries under `user/test/` (`test_smoke_boot`, `test_fastpath`, `test_fastpath_fuzz`, `test_harness_smoke`, `test_syscall`, `test_libc`, `test_ipc`, `test_process`, `test_fileio`, `test_win32`, `test_loader_elf`, `test_loader_pe`, `test_loader_eif`, `test_faultinject`, `test_stress_libc`, `test_perf_syscall`) -- each landed via the §16 build pattern (one `user/test/test_<name>.c` + one `USERMODE_TESTS` row in [`Makefile`](../../Makefile) `userland` target + one `tests/usermode.manifest` line). The launcher's VFS scan + manifest dispatch picks them up automatically; the §16 commit history (`f0279dc0`, `86a4edf4`, `8a5eced9`, `9054b594`, `f8d6f9d8`) is the historical workflow proof.
- [x] Commit: `"test: user-mode test framework complete"`

**Test checkpoint:** End to end: clean tree -> `bash scripts/test.sh` is green -> a one-line change breaks a `test_*.exe` assertion -> the run fails with a visible `[UTEST] FAIL`. Demonstrated 2026-04-23 via the SYS_GETPID break above. Test on: QEMU WHPX, QEMU TCG (closed via `FORCE_TCG=1` 2026-04-23), VirtualBox, bare metal.

**Test runner:** `bash scripts/test.sh` | 2184 kernel + 16 user-mode PASS on KVM 2026-04-23 (exit=0); 2166 + 16 PASS on TCG via `FORCE_TCG=1`; per-binary bat files under `scripts/debug/usermode/` for targeted runs.

---