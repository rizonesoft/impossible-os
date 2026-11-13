<!-- docs: covers=todo/00-infrastructure/TODO-01-developer-tooling-stack.md sources=scripts/setup.sh,scripts/build.sh,scripts/test.sh,scripts/lint.sh,scripts/tooling-doctor.sh,scripts/install-hooks.sh,scripts/run-with-boot-conf.sh,scripts/patch-boot-conf.sh,scripts/run-qemu.sh,scripts/lib/boot-conf-lock.sh,scripts/with-boot-conf-lock.sh,scripts/test-swtpm.sh,Makefile,scripts/lint/check_make_duplicates.sh reviewed=2026-11-13T17:20 order=2 -->
# Developer Tooling Stack

## What is it?

The developer tooling stack is the set of host-side scripts that set up a machine, build the image, run it, test it and gate commits, all behind one contract. It exists so that a laptop, the devcontainer and GitHub Actions run the same commands with the same flags and write the same logs, instead of each drifting its own way. This page is the overview; the full reference is [Development Tooling](development-tooling.md), and the launcher catalogue is the [Machine Matrix](machine-matrix.md).

## How does it work?

Every task goes through a small number of wrapper scripts. Each wrapper owns its log file and its exit code, so a caller (a person, a git hook or a CI job) only has to read one line to know the outcome.

```mermaid
flowchart LR
  setup[setup.sh] --> build[build.sh]
  build --> test[test.sh]
  build --> run[run-qemu.sh / machines]
  test --> hooks[.githooks]
  hooks --> ci[GitHub Actions]
  doctor[tooling-doctor.sh] -.checks.-> setup
  doctor -.checks.-> hooks
```

- **Bootstrap.** [`scripts/setup.sh`](../../scripts/setup.sh) installs the toolchain through [`scripts/setup-deps.sh`](../../scripts/setup-deps.sh) and, with `--verify`, fails closed when a required tool is missing or older than its version floor.
- **Wrappers.** [`scripts/build.sh`](../../scripts/build.sh), [`scripts/test.sh`](../../scripts/test.sh), [`scripts/lint.sh`](../../scripts/lint.sh), [`scripts/run-qemu.sh`](../../scripts/run-qemu.sh) and [`scripts/debug.sh`](../../scripts/debug.sh) are the five canonical entry points. Each answers `--help`. Raw `make` is not the supported interface.
- **Launchers.** [`scripts/machines/`](../../scripts/machines/) holds one launcher per hypervisor and scenario (KVM, TCG, WHPX, VirtualBox, storage, filesystem, Secure Boot, bare metal), all taking the same `-ExtraArgs` surface.
- **Hooks.** [`scripts/install-hooks.sh`](../../scripts/install-hooks.sh) points git at the tracked [`.githooks/`](../../.githooks/) directory: pre-commit lint and post-commit bookkeeping are always on, and the build and test pre-push gate is opt-in.
- **Boot assertions.** [`tools/post16-manifest/generate.sh`](../../tools/post16-manifest/generate.sh) turns the `POST16_*` boot-phase codes into a manifest, so the smoke test asserts codes rather than fragile log strings.
- **Health.** [`scripts/tooling-doctor.sh`](../../scripts/tooling-doctor.sh) is a read-only health check of the whole stack, and [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh) is the regression pack that tests the scripts themselves. It starts its nested suites together, and the todo-graph build suite runs there as four shards (`scripts/todo-graph/tests/test_build*.sh`, sharing `test_build_lib.sh`), so that block costs what its longest shard does: 390 s on 2026-11-11, against 590 s when the suite was one file. Only the core shard, `test_build.sh`, touches the real `build/todo-cache.json`, and no test rewrites a committed file.

## What are its interfaces?

| Command | Purpose |
| ------- | ------- |
| `bash scripts/setup.sh [--verify\|--versions\|--check-versions]` | Install dependencies; verify presence and version floors |
| `bash scripts/build.sh [clean\|run]` | Incremental or clean build, or build and boot in QEMU |
| `bash scripts/test.sh [SUITE=<cat>] [QUIET=1]` | Kernel and user-mode tests, optionally one category |
| `bash scripts/test-smoke.sh` / `bash scripts/test-smoke-matrix.sh` | Boot to the command prompt on one QEMU configuration, or on four engine/CPU legs plus the boot-store, UKI-direct fixture and two-boot legs. The matrix builds once and restores an untouched copy of the image for each leg |
| `bash scripts/lint.sh` | Repository lint, the same checks the pre-commit hook runs |
| `bash scripts/install-hooks.sh [--status\|--with-pre-push\|--remove]` | Manage the tracked git hooks |
| `bash scripts/tooling-doctor.sh [--quiet\|--json]` | One-command health check |

The GitHub workflows in [`.github/workflows/`](../../.github/workflows/) call the same wrappers; [GitHub Setup](github-setup.md) documents each workflow.

## How do I use it?

On a fresh host:

```bash
bash scripts/setup.sh            # install the toolchain
bash scripts/setup.sh --verify   # prints "All required tools meet their minimum version floors."
bash scripts/install-hooks.sh    # enable the tracked hooks
bash scripts/build.sh            # tail -1 build/build.log shows "=== BUILD OK ==="
bash scripts/test.sh QUIET=1     # summary line only
```

To check that the stack itself is healthy:

```bash
bash scripts/tooling-doctor.sh --quiet
# tooling-doctor: HEALTHY (31 checks, 0 warning(s))
```

That output was measured on 2026-09-28. Before pushing, `CI_PARITY=1 bash scripts/test.sh` runs the suite under the same QEMU package and TCG engine that CI uses; the pre-push hook enforces it.

## Which make targets boot the image in QEMU?

| Target | What it does |
|---|---|
| `make run` | Builds, then boots the system disk with a window and serial on stdio. |
| `make run-only` | Boots the image already built, without building. |
| `make run-unit-tests` | Builds, then boots once with `test=1` in boot.conf (unit tests, then shutdown). |
| `make run-boot-tests` | Builds, then boots once with `debug=1` (unit tests and boot tests). |
| `make run-test DISK=<name>` | Boots with a secondary test disk on AHCI port 1, for filesystem driver testing. |
| `make run-debug` | Boots paused, waiting for GDB on port 1234. |

`run-unit-tests` and `run-boot-tests` patch boot.conf inside the built image through `scripts/run-with-boot-conf.sh`, boot through `run-only` so nothing rebuilds the disk over the patch, and restore boot.conf when QEMU exits, fails or is interrupted. Every boot of the image holds `.boot-conf.lock` (`scripts/lib/boot-conf-lock.sh`) until QEMU exits: each Makefile QEMU recipe runs through `scripts/with-boot-conf-lock.sh`, and every script that boots the image itself (the test runners, the smoke test and its matrix, `scripts/run-qemu.sh` and the `scripts/machines/` QEMU launchers, listed in the helper's header) takes it, a nested run sharing its parent's hold. The disk image is built as `system-disk.img.new` and renamed into place under the lock, so a running VM keeps the image it opened and is never truncated; `make clean` deletes `build/` under the lock, and both are refused while a run holds it. The lock covers any disk or firmware-vars image under `build/`, not only `system-disk.img`: the release and fixture image writers and launchers take it through `boot_conf_lock_if_under_build` when their image lies there, `tools/boot-cert/boot_reliability.py` holds it across its whole series (and resets boot.conf only for the canonical image), and `scripts/test-tooling.sh` (`boot_locked_recipes`) refuses a QEMU recipe, launcher or image writer that does not take it. A VirtualBox VM runs detached once `startvm` returns, so like the Windows launchers its lifetime is outside the lock. `scripts/build.sh` also builds one tree at a time (`.build.lock`): a second build waits for the first. The lock lives at the worktree root because `build.sh clean` deletes `build/`. `scripts/machines/run-qemu.ps1` boots the image from Windows and cannot hold it, so do not patch boot.conf from WSL while that VM runs, and every other run that would patch or reset that image's boot.conf through `scripts/patch-boot-conf.sh` is refused. Lint Check 38 refuses a Makefile target with two recipes: until 2026-11-13 `run-test` and `run-debug` were each defined twice, and make silently ran only the second.

## What is not implemented yet?

Everything in the roadmap file has shipped.

## How does it compare with Windows 11 and Linux?

Mature Windows and Linux projects commonly have build wrappers and CI policy, and Linux projects often ship devcontainers and managed hooks. Driver and kernel setups on Windows (WDK, HLK) are heavier and less scripted. What is rare in either world is a one-command tooling doctor, lint gates that stop cross-reference drift, and boot smoke tests that assert phase codes instead of matching log strings; those are the parts this stack adds.

## See also

- [Developer Tooling Stack roadmap](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md)
- [Development Tooling](development-tooling.md): the full reference, including host profiles and the local CI hooks
- [Machine Matrix](machine-matrix.md): every launcher and debug profile
- [GitHub Setup](github-setup.md): workflows and repository settings
