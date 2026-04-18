# TODO-01 -- Developer Tooling Stack

> **Goal:** Consolidate the repository's host-side developer workflow into one explicit contract: setup, build, run, debug, test, hooks, and GitHub automation all use the same paths, flags, logs, and operator expectations. Today the scripts work, but the rules live across `README.md`, `CLAUDE.md`, ad hoc shell wrappers, and workflow YAML. This TODO turns that into one maintained roadmap so local developer flow, CI, and release-adjacent tooling stop drifting.

> [!IMPORTANT]
> **Current state:** The repo already has a substantial tooling surface: `scripts/setup.sh`, `scripts/build.sh`, `scripts/test.sh`, `scripts/run-qemu.sh`, `scripts/debug.sh`, machine launchers under `scripts/machines/`, repo-tracked hooks in `.githooks/`, a pre-push hook in `scripts/hooks/pre-push`, and GitHub Actions workflows in `.github/workflows/`. The root and infrastructure indexes now point at this TODO, but the actual contract is still split across docs, scripts, workflow YAML, and adjacent consumer TODOs. Code-truth drift is visible today: `scripts/run-qemu.sh`, `docs/infrastructure/development-tooling.md`, and `CONTRIBUTING.md` still advertise old ISO or `make test` style flows even though the canonical wrappers are now `bash scripts/build.sh` / `bash scripts/test.sh`. No section in the roadmap yet owns the supported-host matrix, wrapper drift cleanup, hook-path boundary, runner/artifact policy, or a host-side regression pack for these surfaces.

---

## Inputs

- [`scripts/setup.sh`](../../scripts/setup.sh) -- host bootstrap entry point
- [`scripts/setup-deps.sh`](../../scripts/setup-deps.sh) -- dependency install helper
- [`scripts/build.sh`](../../scripts/build.sh) -- canonical build wrapper
- [`scripts/test.sh`](../../scripts/test.sh) -- canonical test wrapper
- [`scripts/lint.sh`](../../scripts/lint.sh) -- repo lint entry point
- [`scripts/run-qemu.sh`](../../scripts/run-qemu.sh) -- default QEMU launcher
- [`scripts/debug.sh`](../../scripts/debug.sh) -- developer debug entry point
- [`scripts/install-hooks.sh`](../../scripts/install-hooks.sh) -- one-shot bootstrap that configures `core.hooksPath=.githooks` and toggles the opt-in pre-push gate via a sentinel (per §5)
- [`scripts/hooks/pre-push`](../../scripts/hooks/pre-push) -- opt-in pre-push build + test gate body, delegated from `.githooks/pre-push` when the sentinel exists
- [`.githooks/`](../../.githooks/) -- canonical repo-tracked hook directory (pre-commit lint + post-commit COUNT.md always-on; sentinel-gated pre-push delegator)
- [`scripts/machines/`](../../scripts/machines/) -- per-hypervisor / per-scenario runners
- [`scripts/test-smoke.sh`](../../scripts/test-smoke.sh) -- end-to-end boot smoke test (KVM preferred, TCG fallback) wired into `/implement-todo-section`, `/debug-session`, and the Claude Code harness post-commit hook in `.claude/settings.json` (separate from git hooks)
- [`.github/workflows/build.yml`](../../.github/workflows/build.yml) -- build artifact and sentinel policy
- [`.github/workflows/release.yml`](../../.github/workflows/release.yml) -- release automation boundary
- [`.github/workflows/pages.yml`](../../.github/workflows/pages.yml) -- docs/deployment workflow policy touched by §6
- [`.github/workflows/labeler.yml`](../../.github/workflows/labeler.yml) -- maintenance automation policy touched by §6
- [`.github/workflows/stale.yml`](../../.github/workflows/stale.yml) -- maintenance automation policy touched by §6
- [`.github/CODEOWNERS`](../../.github/CODEOWNERS) -- ownership and review routing
- [`README.md`](../../README.md) -- public operator-facing setup/build/run docs
- [`CONTRIBUTING.md`](../../CONTRIBUTING.md) -- contributor-facing setup, hook, and test commands that must stop drifting from the canonical wrapper contract
- [`CLAUDE.md`](../../CLAUDE.md) -- canonical local workflow rules
- [`docs/infrastructure/development-tooling.md`](../../docs/infrastructure/development-tooling.md) -- current canonical tooling doc that §1-§3 must consolidate instead of replacing ad hoc
- -> XREF: `T04 §3, §4, §11` -- user-mode test launcher, CI-friendly output, and final test wiring extend the same tooling contract
- -> XREF: `T05 §7` -- visual-regression CI consumes the same runner and artifact policy
- -> XREF: `T03 §4` -- repo-wide deferred-test sweep depends on stable local test orchestration
- -> XREF: `D14 T01 §1-§3` -- host SDK/build-system work is complementary, but this TODO owns repo-local developer wrappers
- -> XREF: `D15 T01 §2, §5` -- release packaging and signing consume the tooling contract but remain release-domain owned
- -> XREF: `D15 T05 §3, §5` -- contributor and README/community docs consume the canonical setup/build/test/hook contract and must not restate stale commands

## Outcome

- One canonical contract for setup/build/test/run/debug wrappers and their logs, flags, and failure sentinels.
- Supported host profiles and an optional reproducible environment path make onboarding deterministic instead of distro folklore.
- Hook installation, local automation, and CI workflow behavior documented and owned in one place.
- GitHub Actions workflows aligned with the local script contract instead of duplicating behavior ad hoc.
- Legacy docs and runner surfaces stop advertising obsolete `make test`, `run-tests.sh`, or ISO-only flows once the canonical contract is enforced.
- Host-side tooling regression checks catch broken wrapper behavior before a developer or reviewer trips over it.
- An operator-facing "tooling doctor" flow gives Impossible OS a cleaner bootstrap and diagnostics story than typical OS hobby repos.

## Implementation Order

| ⭐  | Order | Deliverable                                           | Depends On     | Status |
| --- | :---: | ----------------------------------------------------- | --------------- | :----: |
| 💎  |   1   | Host bootstrap and dependency contract                | --             |  [x]   |
| 💎  |   2   | Supported host profiles and reproducible environments | §1             |  [x]   |
| 💎  |   3   | Build, test, lint, and run wrapper contract           | §1, §2         |  [x]   |
| 💎  |   4   | Machine launcher and debug profile matrix             | §2, §3         |  [x]   |
| 💎  |   5   | Git hooks and local automation lifecycle              | §1, §3         |  [x]   |
| 💎  |   6   | GitHub Actions and artifact policy alignment          | §2, §3, §4, §5 |  [x]   |
| ⭐  |   7   | Tooling doctor and regression pack                    | §1-§6          |  [x]   |
| 💎  |   8   | Unify specialist-launcher `-ExtraArgs` surface        | §4             |  [x]   |
| 💎  |   9   | Legacy-XREF sweep (lint Check 5 -> 0 errors; CI gate) | §3, §6         |  [x]   |
| 💎  |  10   | Smoke-test POST16 assertions (boot-phase manifest)    | §3             |  [ ]   |

> 💎 = parity work: Windows and Linux projects both rely on stable setup/build/test/CI contracts.
> ⭐ = exclusive work: Impossible OS can provide a single operator-facing developer workflow with self-diagnosis instead of scattered scripts and tribal knowledge.

---

## 1. Host Bootstrap and Dependency Contract

Make setup reproducible and explicit so a new machine converges on the same toolchain and package set without guesswork.

- [x] Audit `scripts/setup.sh` and `scripts/setup-deps.sh` into one documented contract: supported distros, required packages, optional packages, and no-op/idempotent reruns
- [x] Define exact outputs and sentinel checks: which tools must exist after setup (`clang-19`, `ld.lld-19`, `nasm`, `qemu-system-x86_64`, `ovmf`, `mtools`)
- [x] Consolidate the existing `docs/infrastructure/development-tooling.md` page into the canonical host-bootstrap reference, with `README.md` and `CLAUDE.md` linking to the same instructions
- [x] Add explicit boundary notes for what stays in `D14 T01 §1-§3` versus this repo-local setup flow
- [x] Commit: `"docs/tooling: define host bootstrap and dependency contract"` (ad4d3fa4)

**Test checkpoint:** On a clean supported Linux machine, `bash scripts/setup.sh` completes and the documented required tools are present on `PATH`. A second run is idempotent and reports no destructive drift.

> **Notes:**
> - `scripts/setup.sh` supports `--help` and `--verify` modes. `--verify` runs the read-only sentinel set (14 entries: clang-19, ld.lld-19, llvm-objcopy-19, llvm-ar-19, llvm-nm-19, nasm, gcc, python3, qemu-system-x86_64, mcopy, mmd, mkfs.fat, OVMF_CODE_4M.fd, OVMF_VARS_4M.fd) without touching the system.
> - The full run executes: deps install -> sentinel verify -> `bash scripts/build.sh clean`. Idempotence is inherited from `setup-deps.sh`'s `command -v` pre-check.
> - `setup-deps.sh` now installs `build-essential` (Debian) / `gcc` (Fedora/Arch) so `HOST_CC := gcc` is guaranteed on every supported distro.
> - Canonical doc: [`docs/infrastructure/development-tooling.md#host-bootstrap-contract`](../../docs/infrastructure/development-tooling.md#host-bootstrap-contract). `README.md`, `CONTRIBUTING.md`, and `CLAUDE.md` link here instead of restating distro/package lists.
> - Scope boundary recorded in the doc: repo-local dev bootstrap stays here; SDK cross-tool build system remains owned by `todo/14-host-tools/TODO-01-sdk-build-system.md` sections 1-3 (`D14 T01 §1-§3`). Supported-host profile matrix (Ubuntu/Debian vs. Fedora/Arch LLVM-19 shim work) is owned by §2 below.

> **Verified:** 2026-04-17 -- all 5 checklist items map to diff in commit `ad4d3fa4` (scripts/setup.sh sentinel array; docs/infrastructure/development-tooling.md Host Bootstrap Contract section; README.md, CONTRIBUTING.md, CLAUDE.md link-throughs; TODO boundary table). Build clean: `=== BUILD OK ===`. `bash scripts/setup.sh --verify` reports 14/14 sentinels OK on this host.
> **Quality reviewed:** 2026-04-17 -- two Codex dispatches (adversarial + quality/dead-code/consistency/performance). Round 1 found 2 High (missing llvm-ar-19/llvm-nm-19 sentinels; Arch/Fedora LLVM naming drift) + 2 Medium (OVMF fallback paths not matching Makefile hardcode; idempotence claim false re: include/build_info.h). Round 2 found 2 Medium (python3 + gcc missing from sentinel/deps; mmd not verified) + 1 Low (direct mmd in Makefile). All fixed in this review: sentinel grew to 14 entries matching Makefile exactly; setup-deps.sh now installs gcc; doc idempotence section rewritten; OVMF path narrowed to exact 4M paths. No domain code-quality skill applies (docs + bash only; no src/kernel, src/boot, src/desktop, or user/ touched).
> **Test runner:** scripts/test-tooling.sh (owned by §7). §1 has no dedicated test surface today; `bash scripts/setup.sh --help` exits 0 and `--verify` returns non-zero on missing tools (manual verification). Full tooling regression pack lands with §7.

---

## 2. Supported Host Profiles and Reproducible Environments

Setup is not complete until contributors know which hosts are supported, which are best-effort, and how to reproduce the toolchain without cargo-cult package installs.

- [x] Define the supported host profile matrix: native Ubuntu/Debian, native Fedora, native Arch, WSL2 + Ubuntu, and GitHub Actions `ubuntu-latest`; mark which profiles are fully supported, best-effort, and unsupported
- [x] Publish minimum required versions and verification commands for `clang-19`, `ld.lld-19`, `nasm`, `qemu-system-x86_64`, `OVMF`, `mtools`, `python3`, and `bear` so setup output is more precise than "binary exists"
- [x] Add an optional committed reproducible environment definition (`.devcontainer/` or equivalent container profile) that runs the same setup/build/lint/docs entry points as a fresh supported host
- [x] Document the boundary between containerized and non-containerized validation: host bootstrap and static checks can be reproduced in a container, while KVM/WHPX/VirtualBox and bare-metal validation remain owned by the named machine profiles in §4
- [x] State unsupported-host policy explicitly in developer docs: Windows native shell flow and macOS are unsupported until a tracked owner lands them; WSL2 and supported Linux hosts are the intended paths today
- [x] Add reciprocal XREFs so `README.md`, `CONTRIBUTING.md`, and community docs pull setup guidance from this section instead of freezing parallel command lists
- [x] Commit: `"docs/tooling: define supported host profiles and reproducible environment policy"`

**Test checkpoint:** On a fresh supported host or the committed reproducible environment, the documented bootstrap path reaches the required tool versions and wrapper entry points without undocumented manual steps. Unsupported hosts fail early with a documented redirect to the supported path.

> **Notes:**
> - Profile matrix lives in [`docs/infrastructure/development-tooling.md#supported-host-profiles-and-reproducible-environments`](../../docs/infrastructure/development-tooling.md#supported-host-profiles-and-reproducible-environments). Five fully-supported profiles (Ubuntu/Debian 24.04+, WSL2+Ubuntu, GHA `ubuntu-latest`, committed `.devcontainer`, Ubuntu container base), two best-effort (native Fedora, native Arch; both require the LLVM-19 + OVMF shim), two unsupported (native Windows, macOS).
> - `scripts/setup.sh --versions` reports each installed tool's version alongside its documented minimum floor. **Always exits 0** (pure advisory); `--verify` remains the hard pass/fail contract.
> - `.devcontainer/devcontainer.json` defines a reproducible Ubuntu 24.04 environment. `postCreateCommand` runs `bash scripts/setup.sh`; ships the clangd extension for C intelligence; no privileged runArgs, no sshd feature (bootstrap profile only).
> - Container/non-container boundary documented in the doc table: host bootstrap, static checks, and TCG tests reproduce inside a container; KVM/WHPX/VBox/bare-metal remain owned by §4.
> - Unsupported-host policy stated explicitly. Native Windows contributors use WSL2; macOS has no tracked owner for a toolchain port.
> - Fedora/Arch shim procedure is published in the same doc so best-effort profiles have a deterministic path to 14/14 sentinel pass.
> - `README.md`, `CONTRIBUTING.md`, and `CLAUDE.md` all link into the new §2 doc anchor in addition to the existing §1 link.

> **Verified:** 2026-04-17 -- 6 of 7 items [x] with diff in `ce3adf6d`: profile matrix (docs/infrastructure/development-tooling.md new section), minimum-versions table + `--versions` mode (scripts/setup.sh VERSION_SPECS + print_versions), `.devcontainer/devcontainer.json`, container-boundary table, unsupported-host policy paragraph, README/CONTRIBUTING/CLAUDE link-throughs. Commit item [x] as of the review commit. Build clean: `=== BUILD OK ===`. `bash scripts/setup.sh --verify` reports 14/14 OK and exit 0; `--versions` reports 13 tool rows + 2 firmware rows and always exits 0 (advisory); `--verify` under `PATH=/tmp` exits 1 as expected.
> **Quality reviewed:** 2026-04-17 -- one Codex adversarial dispatch + one Codex quality dispatch (dead code + consistency + performance). Adversarial round found 2 Medium (`--versions` coverage gap for mmd/mkfs.fat/OVMF; devcontainer over-privileged with SYS_PTRACE + seccomp=unconfined). Quality round found 1 Medium + 2 Low (`--versions` exited non-zero on optional/advisory warnings contradicting docs claim; OVMF minimum-version floor claimed without enforcement; devcontainer sshd feature with no documented workflow). All fixed this session: `--versions` now returns 0 unconditionally (advisory contract); OVMF row in the minimum-versions table reclassified as "presence" only; sshd feature removed; privileged runArgs removed. No domain code-quality skill applies (bash + docs + JSON only). Test checkpoint: `bash scripts/setup.sh --verify` passes 14/14 on a fully-supported Ubuntu profile; devcontainer runtime not exercised in this review (requires Docker/Docker-Desktop environment); devcontainer validation is deferred to first contributor trial and is non-blocking because config was validated via `python3 -c json.load`.

---

## 3. Build, Test, Lint, and Run Wrapper Contract

The wrappers are the real interface developers use. Their arguments, outputs, sentinels, and log paths must be explicit and stable.

- [x] Document the canonical entry points and arguments for `scripts/build.sh`, `scripts/test.sh`, `scripts/lint.sh`, `scripts/run-qemu.sh`, and `scripts/debug.sh`
- [x] Standardize success/failure signals: `build/build.log` sentinel, serial log paths, exit codes, and quiet/summary modes where applicable
- [x] Define which commands are authoritative in docs and hooks (`bash scripts/build.sh`, `bash scripts/test.sh`, never raw `make` for builds)
- [x] Add a single tooling reference page listing outputs and artifacts: disk image path, build log path, serial log path, screenshots/diffs if present
- [x] Audit repo-facing wrapper surfaces and fix or retire stale entry points that still reference `build/os-build.iso`, `make all`, `run-tests.sh`, or `make test` as the primary flow (`scripts/run-qemu.sh`, `scripts/test-smoke.sh`, `docs/infrastructure/development-tooling.md`, `CONTRIBUTING.md`)
- [x] Ensure wrapper docs mention the existing machine-specific scripts instead of encouraging direct ad hoc QEMU invocations
- [x] Commit: `"docs/tooling: codify build, test, lint, and run wrapper contract"` (0af2a330)

**Test checkpoint:** The documented wrapper commands match reality: `bash scripts/build.sh clean` ends with `=== BUILD OK ===`, `bash scripts/test.sh QUIET=1` produces a summary-only run, and wrapper exit codes are consistent with success/failure.

> **Notes:**
> - Added `--help` to `scripts/build.sh`, `scripts/test.sh`, and `scripts/lint.sh`; `scripts/run-qemu.sh` and `scripts/debug.sh` already had one. Every canonical wrapper now self-documents.
> - Rewrote `scripts/run-qemu.sh` and `scripts/test-smoke.sh` to boot `build/system-disk.img` via AHCI + UEFI. The legacy `build/os-build.iso` + `-cdrom` + `make all` path is retired from both scripts (the Makefile `iso:` target remains as legacy grub-mkrescue infrastructure but is no longer referenced by wrappers).
> - New [Wrapper Contract](../../docs/infrastructure/development-tooling.md#wrapper-contract) section in the canonical doc lists all five wrappers with their canonical commands, primary outputs, success sentinels, and exit codes. Input/output path table + sentinels + machine-launcher map are all in one place.
> - Canonical doc audit touched Build System, Emulator Testing Scripts, Test Framework, Scripts Directory Structure, Local CI Hooks, USB Write, Key Files, and OS Comparison. Stale counts (9 files, 38 suites, 96 assertions) replaced with current (28 files, 11 categories, ~1,400 TEST_ASSERTs). `scripts/vm/` rewritten to `scripts/machines/`. Hyper-V broken link removed. Partition table corrected (EFI + BlackBox + IXFS). Lint-check count corrected (6 -> 7).
> - `CONTRIBUTING.md` PR checklist now uses `bash scripts/test.sh` (not `make test`); hook guidance matches the currently-active pre-commit + post-commit pair; opt-in pre-push is documented honestly.
> - `make test*` shorthand remains supported as a thin passthrough, but all authoritative doc references point at the bash wrappers so the "never raw make for builds" rule is cleanly stated.
> - `scripts/test.sh` and `scripts/test-smoke.sh` both now install EXIT/INT/TERM cleanup traps so a Ctrl-C no longer leaves `boot.conf` patched or a headless QEMU running.
> - `scripts/build.sh` run modes now propagate `make run*` failures into the wrapper exit code (`RUN_STATUS` accumulator + final `exit`), while still writing the `=== BUILD OK ===` sentinel when the build portion succeeded. Sentinel remains the contract for `tail -1 build/build.log`; run-mode failures show up in the exit code only.

> **Verified:** 2026-04-17 -- all 7 checklist items map to commit `0af2a330`: wrapper --help flags (build/test/lint/run-qemu/debug); rewritten run-qemu.sh + test-smoke.sh off the ISO/make-all path; new Wrapper Contract + I/O + sentinel + machine-launcher tables in canonical doc; audited CONTRIBUTING.md. Build clean: `=== BUILD OK ===` on incremental; `tail -1 build/build.log` == `=== BUILD OK ===` in both default and `run` modes. `bash scripts/test.sh --help` lists 11 categories.
> **Quality reviewed:** 2026-04-17 -- two Codex dispatches (adversarial + quality). Adversarial round found 1 High + 3 Medium (test.sh SIGINT trap gap; test-smoke.sh QEMU leak; pre-push hook status vs install-hooks mismatch; build.sh run modes violate `tail -1` sentinel because run-mode banners were tee'd). Quality round found 1 High + 1 Medium (build.sh run modes ignore `make` return codes -> false-green `bash scripts/build.sh run`; `scripts/test.sh --help` / doc claim 10 categories but 11 ship with `TEST_CAT_X86`). All fixed this session: traps added, pre-push documented honestly as opt-in, run-mode tee removed, run-mode exit status now propagates via `RUN_STATUS`, x86 category added to `--help` + doc. No domain code-quality skill applies (bash + markdown only).
> **Test runner:** no kernel unit-test surface in §3 (docs + wrappers). Validation: `bash scripts/build.sh --help | head`, `bash scripts/test.sh --help | head`, `bash scripts/lint.sh --help | head`, `bash scripts/run-qemu.sh --help | head`, `bash scripts/debug.sh --help | head`; all exit 0. Full tooling-regression pack owned by the Tooling Doctor section (`scripts/test-tooling.sh`).

---

## 4. Machine Launcher and Debug Profile Matrix

Local and CI runs need named machine profiles instead of script archaeology across `scripts/machines/`.

- [x] Inventory `scripts/machines/` launchers and classify them by platform and purpose: WHPX, TCG, KVM, VirtualBox, storage, filesystem, and secure-boot scenarios
- [x] Create one canonical matrix document mapping each launcher to intended use, expected accelerator, artifacts, and known limitations
- [x] Define the boundary between `scripts/run-qemu.sh`, `scripts/debug.sh`, and machine-specific runners so the default path and specialist paths are obvious
- [x] Add explicit debugger entry points and serial-log expectations for each supported profile
- [x] Add a bare-metal bring-up row covering disk-image handoff, serial capture path, and expected log/artifact locations so the matrix does not stop at VM-only guidance
- [x] Mark release-validation VM flows as XREFs to `D15 T04 §2, §4` and installer/provisioning-specific VM flows as XREFs to `D15 T02 §7` instead of duplicating them here
- [x] Commit: `"docs/tooling: machine launcher and debug profile matrix"`

**Test checkpoint:** A developer can choose the right launcher or bring-up path for WHPX, TCG, VirtualBox, secure-boot, and bare-metal scenarios from one table. Each documented path produces the expected serial/artifact location without ambiguity.

> **Notes:**
> - Canonical matrix: [`docs/infrastructure/machine-matrix.md`](../../docs/infrastructure/machine-matrix.md). Six tables (default path; QEMU accelerator/topology; Secure-Boot; VirtualBox; storage; filesystem), a dedicated bare-metal bring-up section with serial capture path + `X:\` BlackBox log locations, a **Known Drift** block surfacing `run-qemu-kvm.bat` forcing WHPX and the secureboot launcher living under `scripts/debug/`, a **Scope Boundary** table pointing release/installer/hypervisor certification at their correct owners, and an update protocol for future launchers.
> - Linked from [`docs/infrastructure/development-tooling.md`](../../docs/infrastructure/development-tooling.md): Scope Boundary row, Run/Debug section intro, Specialist Launchers summary (quick-reference only; deep matrix in `machine-matrix.md`).
> - Scope boundaries cross-linked to [`D15 T04 §2, §4`](../../todo/15-installer-release/TODO-04-release-qa.md) (QEMU + VirtualBox release validation), [`D15 T02 §7`](../../todo/15-installer-release/TODO-02-unattended-install.md) (provisioning templates), and [`D15 T04 §3, §5, §6`](../../todo/15-installer-release/TODO-04-release-qa.md) (Hyper-V certification, real-hardware checklist, performance benchmarks) so this doc does not re-document release-domain VM sweeps.
> - Known drift noted but not fixed here (keeps the section doc-only per the Commit line): `run-qemu-kvm.bat` name vs. WHPX behavior, `run-secureboot.bat` directory placement, Linux-side gaps in the fs/storage harnesses. Consolidation is deliberately not scoped to §4.

> **Test runner:** validation for §4 is documentation-only (no kernel test surface). Users can walk the Default Path block in [`machine-matrix.md`](../../docs/infrastructure/machine-matrix.md#default-path) and reach the correct launcher for each scenario (WHPX, TCG, KVM, VirtualBox, secure-boot, bare-metal) without reading any script header. No structural checker ships today; drift protection lands with the §7 Tooling Doctor / `scripts/test-tooling.sh` regression pack (not yet implemented).

> **Verified:** 2026-04-17 -- 7/7 checklist items [x] with diff in commit `e2c3f06b`: every launcher in `scripts/machines/` + `scripts/debug/run-secureboot.bat` + `scripts/run-qemu.sh` + `scripts/debug.sh` + `scripts/test-smoke.sh` + `scripts/tools/read-blackbox.sh` + `scripts/deploy/write-usb.*` + `scripts/deploy/read-usb-log.sh` appears in [`docs/infrastructure/machine-matrix.md`](../../docs/infrastructure/machine-matrix.md) with accelerator, use, artifacts, debugger entry point, and known limitations. Bare-Metal Bring-Up section covers disk-image handoff (deploy helpers + dd/Rufus fallback), 115200 8N1 serial capture (matches `src/kernel/drivers/serial.c`), BlackBox paths (`X:\Logs\Serial\`, `X:\Boot\`, `X:\Crash\`, `X:\Perf\`, `X:\Diag\` per `todo/01-boot-platform/TODO-24-blackbox-service-partition.md`), offline extraction via `read-blackbox.sh` + `read-usb-log.sh`. Scope Boundary table defers release/installer/Hyper-V to [D15 T04 §2/§3/§4/§5/§6](../../todo/15-installer-release/TODO-04-release-qa.md) and [D15 T02 §7](../../todo/15-installer-release/TODO-02-unattended-install.md) with reciprocal XREFs written into those sections. Build clean: `=== BUILD OK ===`. No Unicode dashes in new content; all relative links resolve (verified programmatically).
> **Quality reviewed:** 2026-04-17 -- Codex adversarial dispatch on the first draft flagged 5 findings (all valid, all fixed before commit): OVMF_VARS lifecycle wording corrected to distinguish default wrappers (preserve across runs) from forced KVM/TCG launchers (recopy from `build/OVMF_VARS_4M.fd` each run); storage launcher default accelerator corrected from `auto` to `tcg` (NVMe doorbell/xHCI don't virtualize well under WHPX); `scripts/debug.sh` re-described as one-shot launch-plus-GDB (it launches its own QEMU) instead of attach-to-existing; BlackBox partition paths corrected to `X:\Logs\Serial\` etc. under `todo/01-boot-platform/TODO-24-blackbox-service-partition.md` (not the nonexistent `todo/02-kernel-core/TODO-17-blackbox-partition.md`); `scripts/test-tooling.sh` reference softened ("not yet implemented" -- owned by §7). Post-commit consistency audit found 2 additional gaps closed in place: `scripts/deploy/write-usb.{sh,ps1,bat}` and `scripts/deploy/read-usb-log.sh` were omitted from the bare-metal block; both added. `-ExtraArgs` parameter gap on scenario launchers filed as a concrete `[ ]` item in §7. No domain code-quality skill applies (docs + shell names only; no src/kernel, src/boot, src/desktop, or user/ touched).

---

## 5. Git Hooks and Local Automation Lifecycle

Hooks are part of the tooling contract, not a hidden convenience script.

- [x] Document the local hook lifecycle owned by `scripts/install-hooks.sh` and `scripts/hooks/pre-push`: install, remove, update, and expected blocking behavior
- [x] Reconcile repo-tracked `.githooks/` with `scripts/install-hooks.sh`: document whether both remain, which hook families use `core.hooksPath`, and which path is canonical for contributors
- [x] Decide whether the repo stays on shell-managed hooks or adopts a declarative hook manager layer; if adopting, file the exact migration under this section instead of leaving it implied
- [x] Define which checks are mandatory pre-push versus optional local quality-of-life checks
- [x] Add a repo-visible hook reference explaining why push is blocked on failed build/test and where to re-run the same checks manually
- [x] Wire hook docs into `README.md` / `CONTRIBUTING.md` / `CLAUDE.md` so all three describe the same install path
- [x] Commit: `"tooling: define git hook lifecycle and local automation policy"`

**Test checkpoint:** `bash scripts/install-hooks.sh` installs the hook idempotently, `--remove` cleans it up, and a failing local build or test blocks `git push` with the documented error path.

> **Notes:**
> - Canonical doc: [`docs/infrastructure/development-tooling.md#local-ci-hooks`](../../docs/infrastructure/development-tooling.md#local-ci-hooks) -- rewritten to reflect the new install path, lifecycle table, always-on vs opt-in split, manual re-run commands, and explicit Claude Code harness separation.
> - `scripts/install-hooks.sh` redesigned from "symlink into `.git/hooks/`" (which was a silent no-op when `core.hooksPath=.githooks` was set -- the README/CONTRIBUTING path) to a canonical bootstrap: idempotently sets `core.hooksPath=.githooks`, toggles the opt-in pre-push gate via a `.git/.impossible-os-prepush` sentinel file, prunes legacy symlinks on `--remove`, and exposes `--status` / `--help` / `--enable-pre-push` / `--disable-pre-push` / `--with-pre-push` subcommands.
> - New [`.githooks/pre-push`](../../.githooks/pre-push) delegator: checks for the sentinel and `exec`s `scripts/hooks/pre-push` when opted in; exits 0 silently otherwise. Keeps `.githooks/` as the single canonical hook directory without forcing the heavy build/test gate on every contributor.
> - **Decision (codified):** the repo stays on shell-managed hooks under `.githooks/` routed via `core.hooksPath`; no declarative hook manager (lefthook/husky/pre-commit) adopted. Rationale and re-visit trigger documented in the canonical doc.
> - **Mandatory hooks:** pre-commit lint + post-commit `COUNT.md` refresh (always active once `core.hooksPath` is set). **Opt-in:** pre-push build + test gate (activated per-clone via sentinel). CI (`build.yml`) remains the mandatory PR-time gate regardless of local pre-push state.
> - README.md added a new "Git Hooks" subsection under Testing linking to the canonical doc. CONTRIBUTING.md "Enable Git Hooks" rewritten around `bash scripts/install-hooks.sh` as the single canonical command. CLAUDE.md added a "Git Hooks" section after Doc Sync matching the same command.

> **Test runner:** docs-only section, no kernel test surface. Shell-level validation is built into the script itself: `bash scripts/install-hooks.sh --status` reports the current state, idempotence is proven by running the default command twice and verifying identical output, `--remove` leaves `core.hooksPath` unset + sentinel absent. Full smoke of the sentinel-gated delegator: `echo "" | bash .githooks/pre-push` exits 0 when sentinel absent; with sentinel touched it exec's `scripts/hooks/pre-push` (the full build+test gate). Structural drift protection lands with the §7 Tooling Doctor / `scripts/test-tooling.sh` regression pack (not yet implemented).

> **Verified:** 2026-04-17 -- 7/7 checklist items [x] with diff in commit `610b3500`: `scripts/install-hooks.sh` rewritten as one-shot bootstrap (configures `core.hooksPath=.githooks`, toggles opt-in pre-push via `.git/.impossible-os-prepush` sentinel, prunes legacy `.git/hooks/` symlinks on `--remove`, worktree-safe via `git rev-parse --git-path`); [`.githooks/pre-push`](../../.githooks/pre-push) delegator exec's `scripts/hooks/pre-push` only when sentinel exists; [`.githooks/post-commit`](../../.githooks/post-commit) guard defended with `${SKIP_COUNT:-}`; canonical doc rewritten at [`docs/infrastructure/development-tooling.md#local-ci-hooks`](../../docs/infrastructure/development-tooling.md#local-ci-hooks); README / CONTRIBUTING / CLAUDE.md all link to it and cite `bash scripts/install-hooks.sh` as the one install command. Hook-manager decision codified ("stay on shell-managed; re-visit if hook count > ~5 or cross-language gates land"). OS Comparison row and Implementation Order row 5 marked [x]. Build clean: `=== BUILD OK ===`. Manual subcommand walk: default / `--status` / `--remove` / `--enable-pre-push` (now bootstraps `core.hooksPath` before setting sentinel) / `--disable-pre-push` all behave as documented. `--status` label refined: `Always-on hooks` excludes sentinel-gated pre-push; `Opt-in hooks` line added.
> **Quality reviewed:** 2026-04-17 -- two Codex dispatches (adversarial at step 13 + consistency/dead-code/perf at step 20). Adversarial found 3 issues, all fixed before commit: `${SKIP_COUNT:-}` guard in post-commit; `--enable-pre-push` now calls `set_hooks_path` first to prevent inert-state class; sentinel + legacy-hooks paths resolved via `git rev-parse --git-path` for linked-worktree safety. Step-20 consistency round found 3 more (Unit Tests + Verification blocks carried stale `.git/hooks/pre-push` wording; `--status` label falsely implied `.githooks/pre-push` was always-on) -- all fixed this review: Unit Tests bullet updated to describe the new lifecycle, Verification bullet rewritten around `core.hooksPath` + sentinel, `status()` output split into always-on + opt-in lines. Perf/dead-code N/A (shell + docs only; no src/kernel, src/boot, src/desktop, or user/ touched). No domain code-quality skill applies.

---

## 6. GitHub Actions and Artifact Policy Alignment

CI must mirror local tooling, not fork it.

- [x] Audit `.github/workflows/build.yml`, `release.yml`, `pages.yml`, `labeler.yml`, and `stale.yml` against the local script contract and remove drift in commands, sentinels, and artifact paths
- [x] Define which workflows are required on PRs, which are release-only, and which are maintenance automation
- [x] Add explicit artifact/report policy: which logs/images are uploaded, retention periods, and failure-time diagnostics
- [x] Document when GitHub-hosted runners are sufficient and when a self-hosted runner or manual hardware validation is required
- [x] Add operator notes for manual dispatches or future matrix expansion without baking undocumented behavior into YAML only
- [x] Commit: `"ci/tooling: align GitHub Actions with local developer tooling contract"`

**Test checkpoint:** CI workflows invoke the same wrapper scripts as local docs. A failed build uploads the expected logs/artifacts, and a reviewer can map CI behavior directly back to the documented local commands.

> **Notes:**
> - Canonical doc: [`docs/infrastructure/development-tooling.md#github-actions-workflows`](../../docs/infrastructure/development-tooling.md#github-actions-workflows). Covers workflow classification (required / release-only / maintenance), wrapper alignment per workflow, artifact + retention policy, runner tier decision, manual dispatch + matrix expansion rules, and a red-build troubleshooting flow.
> - YAML drift fixed:
>   - `build.yml`: added `bash scripts/setup.sh --verify` after inline apt installs (catches drift between the workflow dependency list and the 14-sentinel contract from §1), added `bash scripts/test.sh QUIET=1` (previously the workflow header said "Build & Smoke Test" but no tests ran -- real gap, now closed), bumped artifact retention from 1 day to 7 days, and expanded the always-upload build-log artifact to include `build/test.log` so a failing test run leaves diagnosable serial output in CI.
>   - `release.yml`: same `--verify` + `test.sh QUIET=1` gates before the tag ships; prevents a regression landing on main between the last green CI and the tag push from auto-publishing to GitHub Releases.
>   - LLVM package install upgraded from `clang-19 lld-19` to `clang-19 lld-19 llvm-19` in both workflows, and the actions/cache paths expanded to cover `llvm-ar-19` + `llvm-nm-19` (owned by the `llvm-19` package). Prevents cache-hit runs from missing the binaries `setup.sh --verify` expects. Cache key bumped to `v2`.
>   - `scripts/lint.sh` is deliberately NOT a CI gate yet: the tree carries legacy numeric-TODO shorthand outside `todo/` that makes lint fail with 35 errors today. That cleanup is owned by the §7 "Legacy-XREF sweep" item; lint becomes a required gate once that sweep closes.
>   - `pages.yml`, `labeler.yml`, `stale.yml`: no build/test, no drift to fix -- documented as maintenance automation.
> - Runner tier decision codified: GHA `ubuntu-latest` is sufficient for TCG-based build + test; a self-hosted runner for KVM/WHPX/bare-metal work is not tracked today. Expanding CI to cover the developer machine matrix is explicitly *not* this section's scope -- the developer matrix [TODO-01 §4 / `machine-matrix.md`] is the source for local launchers, and release validation of those paths is owned by [D15 T04 §2, §3, §4, §5](../../todo/15-installer-release/TODO-04-release-qa.md).
> - Reciprocal XREFs already present: `docs/infrastructure/development-tooling.md` Scope Boundary row now points at the new anchor, supported-host table row for GHA updated to cite the canonical doc.

> **Test runner:** docs + YAML only, no kernel test surface. CI itself is the regression test: the first PR after this commit will exercise `setup.sh --verify`, `build.sh clean`, and `test.sh QUIET=1` on GHA (lint is held back until the legacy-XREF sweep closes; see §7). Manual local walk: each wrapper in the build/release workflow is copy-paste runnable (see the "Troubleshooting a red build" section of the canonical doc). Structural drift protection for the YAML against the wrapper contract lands with the §7 Tooling Doctor / `scripts/test-tooling.sh` regression pack (not yet implemented).

> **Verified:** 2026-04-17 -- 6/6 checklist items [x] with diff in commit `96696205`: [`.github/workflows/build.yml`](../../.github/workflows/build.yml) + [`.github/workflows/release.yml`](../../.github/workflows/release.yml) now run `bash scripts/setup.sh --verify` + `bash scripts/test.sh QUIET=1` around the build; LLVM install upgraded to include `llvm-19` + `llvm-ar-19` + `llvm-nm-19` (cache key v2); artifact retention 1d -> 7d; `build/test.log` uploaded with `if: always()` alongside `build.log`. [`docs/infrastructure/development-tooling.md#github-actions-workflows`](../../docs/infrastructure/development-tooling.md#github-actions-workflows) new canonical section covers workflow classification, wrapper-alignment, artifact + retention policy, runner tier decision, manual dispatch rules, and red-build troubleshooting. Scope Boundary + GHA host-profile rows updated to link the new anchor. Implementation Order row 6 [x]. OS Comparison "Workflow/artifact policy" row updated. Inputs block for `install-hooks.sh` / `scripts/hooks/pre-push` / `.githooks/` refreshed to match the §5 bootstrap + sentinel reality. Build clean: `=== BUILD OK ===`. YAML parse clean (python3 yaml.safe_load). `scripts/lint.sh` error count 35 -> 32 across the §4+§5+§6 arc; new doc content introduced no non-legacy shorthand.
> **Quality reviewed:** 2026-04-17 -- two Codex dispatches (adversarial at step 13 + consistency/dead-link at step 20). Step 13 caught 3 findings, all fixed before commit: `scripts/lint.sh` in CI would have red-lighted every PR against a tree with 33 legacy errors (reverted; deferred to §7 sweep with concrete owner item); LLVM install only covered `clang-19 lld-19` but `setup.sh --verify` requires `llvm-objcopy-19 llvm-ar-19 llvm-nm-19` too (fixed by installing the `llvm-19` package + expanding cache paths; key bumped v1 -> v2); test failures did not upload `build/test.log` (fixed by expanding the always-upload artifact pattern). Step 20 caught 2 further consistency issues, both fixed this review: the GHA host-profile row still listed `lint.sh` among CI wrappers after the step-13 revert, and the Scope Boundary row introduced a new `TODO-01 §6` visible-text shorthand outside `todo/` (lint rule violation the sweep is meant to prevent); both rewritten to non-shorthand link text + the troubleshooting reproduction command no longer includes `lint.sh`. No domain code-quality skill applies (YAML + docs + shell; no src/kernel, src/boot, src/desktop, or user/ touched). Perf/dead-code reviews N/A for this scope.

---

## 7. Tooling Doctor and Regression Pack

This is the refinement step: make the tooling self-diagnosing instead of forcing contributors to reverse-engineer failures.

> [!TIP]
> Windows and Linux projects usually document setup and CI, but they rarely ship one repo-local "doctor" path that validates toolchain, hook install, runner prerequisites, and wrapper availability in one pass.

- [x] Add a `scripts/tooling-doctor.sh` entry point that checks required host tools, key script executability, hook install status, and expected workflow files without mutating the repo
- [x] Add a host-side regression pack (`scripts/test-tooling.sh` or equivalent) covering argument parsing, sentinel paths, hook install/remove idempotence, and CI config sanity checks
- [x] Extend doctor coverage to runtime prerequisites named in §2 and §4: OVMF presence, `/dev/kvm` availability, `qemu-img`, `VBoxManage`/PowerShell where relevant, and whether the current host matches a supported profile or only a best-effort one
- [x] Add a machine-readable doctor mode (`--json` or equivalent report file) so lightweight CI and bug reports can consume the same diagnostics without screen-scraping prose
- [x] Make the doctor output actionable: print missing package/tool names, broken script paths, and the exact fix entry point
- [x] Wire the regression pack into a lightweight CI path so wrapper drift is caught before release or contributor onboarding breaks
- [x] Link doctor and regression-pack usage from `README.md` and developer docs
- [x] Commit: `"tooling: add doctor command and regression pack for developer workflow"`

**Test checkpoint:** On a healthy setup, `bash scripts/tooling-doctor.sh` reports PASS across toolchain, hooks, and workflow files. On a missing dependency or broken hook install, it reports the exact failed check and recovery step.

> **Notes:**
> - [`scripts/tooling-doctor.sh`](../../scripts/tooling-doctor.sh) -- read-only health check, 31 assertions across six groups (toolchain / wrappers / hooks / workflows / runtime / docs). Three modes: prose (default), `--json` (machine-readable via NUL-framed tempfile -> python3), `--quiet` (one-line summary). Delegates toolchain sentinels to `bash scripts/setup.sh --verify`, hook state to `bash scripts/install-hooks.sh --status`, workflow YAML to `python3 yaml.safe_load`, host profile to `/etc/os-release` + WSL2 detection. Every non-pass line includes an actionable `fix:` remediation.
> - [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh) -- 35-assertion regression pack covering the wrapper `--help` contract, doctor + setup invariants, hook lifecycle round-trip in a per-run mktemp'd repo (hardened to fail fast if mktemp fails rather than silently skip), workflow YAML parse, `build.yml` / `release.yml` wrapper alignment (`bash scripts/build.sh`, `bash scripts/test.sh`, `bash scripts/setup.sh --verify`), `.githooks/` executable bits, and doc-mention contract (`CLAUDE.md` / `README.md` mention `install-hooks.sh`; `development-tooling.md` mentions `tooling-doctor`).
> - Wired into [`.github/workflows/build.yml`](../../.github/workflows/build.yml) as a new `Run tooling regression pack` step, placed after `Verify toolchain sentinels` but before `Build OS (clean)` so wrapper drift surfaces on every PR before the expensive build stage runs.
> - [`scripts/test-smoke.sh`](../../scripts/test-smoke.sh) gained a `--help` block (§3 contract claimed every wrapper has `--help`; doctor caught the gap on its first pass).
> - PyYAML dependency declared: added to both workflow `apt-get install` lines + `scripts/setup-deps.sh` with a proper `python3 -c 'import yaml'` import probe per-distro (`python3-yaml` Debian, `python3-pyyaml` Fedora, `python-yaml` Arch).
> - `docs/infrastructure/development-tooling.md` new `## Tooling Doctor and Regression Pack` section enumerates both scripts' check sets + usage. `README.md` got a compact Tooling Doctor subsection under Testing.

> **Test runner:** shell + YAML + docs, no kernel test surface. Validation is exercised by the regression pack itself: `bash scripts/test-tooling.sh` passes 35/35 on a healthy host, and the pack runs in CI every PR. Build clean (`=== BUILD OK ===`) proves no unintended side effects on the kernel build. `bash scripts/tooling-doctor.sh --quiet` reports `HEALTHY (31 checks, 0 warning(s))` on this host.

> **Verified:** 2026-04-17 -- 8/8 checklist items [x] with diff in commit `cdf5fbc9`: [`scripts/tooling-doctor.sh`](../../scripts/tooling-doctor.sh) ships 31 assertions across 6 groups (toolchain / wrappers / hooks / workflows / runtime / docs) with 3 modes (prose / `--json` via NUL-framed tempfile / `--quiet`) and actionable `fix:` remediation per non-pass row. [`scripts/test-tooling.sh`](../../scripts/test-tooling.sh) ships 35 assertions covering the wrapper `--help` contract, doctor + setup invariants, hook lifecycle round-trip in a per-run `mktemp`'d repo, workflow YAML parse, `build.yml` + `release.yml` wrapper alignment, `.githooks/` executable bits, and `CLAUDE.md` / `README.md` / `development-tooling.md` doc-mention contract. Wired into CI via a new `Run tooling regression pack` step in [`.github/workflows/build.yml`](../../.github/workflows/build.yml), placed between `Verify toolchain sentinels` and `Build OS (clean)`. PyYAML dependency declared in both workflows' apt installs and in `scripts/setup-deps.sh` (per-distro probe). [`docs/infrastructure/development-tooling.md`](../../docs/infrastructure/development-tooling.md#tooling-doctor-and-regression-pack) new canonical section + [`README.md`](../../README.md) new "Tooling Doctor" subsection. Implementation Order row 7 `[x]`; §7's original items 8/9/10 moved cleanly into new §8/§9/§10 sections with full scope inherited (including `Commit:` + `Test checkpoint:`). Build clean. On this host: `tooling-doctor --quiet` -> `HEALTHY (31 checks, 0 warning(s))`; simulated CI state (unset `core.hooksPath`) -> exits 0 with 1 warning as expected.
> **Quality reviewed:** 2026-04-17 -- two Codex dispatches (adversarial at step 13 + consistency/integration at step 20). Step 13 caught 3 findings, all fixed before commit: hook lifecycle subshell silent-skip + potential real-repo mutation on `mktemp` failure (hardened: mktemp rc checked, tempdir prefix validated against `$TMPDIR_BASE/impossible-tooling-*` before setting the `rm -rf` trap, subshell under `set -e`, rc surfaced as a test failure, results file lives inside the tempdir); undeclared PyYAML dependency (added `python3-yaml` to both workflow apt installs + `scripts/setup-deps.sh` post-loop import probe with per-distro install); pipe-delimited record accumulator corrupted `--json` output for fields containing `|` (rewrote to parallel bash arrays + NUL-framed tempfile handoff to `python3`). Step 20 caught 1 more finding, fixed this review: the doctor's `core.hooksPath unset` rule was a hard failure, but a fresh `actions/checkout` worktree never has it set -- CI would red-light every PR for a workstation-only concern. Downgraded to warn with an updated detail line that clarifies "OK on CI and fresh clones". Verified by simulating the CI state (`git config --unset core.hooksPath`) and re-running `tooling-doctor --quiet`: exit 0, 1 warning. Test-tooling still passes 35/35 on a properly-configured workstation. No domain code-quality skill applies (shell + YAML + docs; no src/kernel, src/boot, src/desktop, or user/ touched). Perf/dead-code N/A for this scope.

---

## 8. Unify Specialist-Launcher Argument Surface

Scenario launchers under `scripts/machines/storage/` and `scripts/machines/fs/` do not accept `-ExtraArgs` today, so a developer cannot append `-s -S` (gdb stub) or custom `-drive` / `-netdev` arguments without editing the script. This parks a concrete drift item that the machine matrix surfaces in its "Known Drift" block.

- [x] Lift the `-ExtraArgs` parameter from [`scripts/machines/run-qemu.ps1`](../../scripts/machines/run-qemu.ps1) into each scenario launcher and thread it through to the underlying QEMU invocation: [`scripts/machines/storage/run-nvme-test.{ps1,bat}`](../../scripts/machines/storage/), [`scripts/machines/storage/run-usb-test.{ps1,bat}`](../../scripts/machines/storage/), [`scripts/machines/fs/run-fs-test.ps1`](../../scripts/machines/fs/run-fs-test.ps1), [`scripts/machines/fs/run-fat32-test.bat`](../../scripts/machines/fs/run-fat32-test.bat), [`scripts/machines/fs/run-ntfs-test.bat`](../../scripts/machines/fs/run-ntfs-test.bat), [`scripts/machines/fs/run-all-fs-tests.bat`](../../scripts/machines/fs/run-all-fs-tests.bat).
- [x] Update [`docs/infrastructure/machine-matrix.md`](../../docs/infrastructure/machine-matrix.md) to remove the `-ExtraArgs` caveats from the Storage Scenarios and Filesystem Scenarios sections.
- [x] Delete the corresponding entry from [`docs/infrastructure/machine-matrix.md`](../../docs/infrastructure/machine-matrix.md) "Known Drift" list.
- [x] Commit: `"scripts/machines: unify -ExtraArgs across scenario launchers"`

**Test checkpoint:** Each scenario launcher accepts `-ExtraArgs '-s -S'` and passes the string through to `qemu-system-x86_64.exe` verbatim. Machine matrix no longer lists `-ExtraArgs` missing as a known drift.

> **Notes:**
> - Scenario launchers (run-nvme-test.ps1, run-usb-test.ps1, run-fs-test.ps1) invoke `qemu-system-x86_64.exe` directly rather than wrapping [`scripts/machines/run-qemu.ps1`](../../scripts/machines/run-qemu.ps1); each builds its own `$QemuArgs` array for the scenario. Accordingly the `-ExtraArgs` forwarding happens INSIDE each launcher (append to `$QemuArgs` before `& $QEMU @QemuArgs`), mirroring [`run-qemu.ps1`](../../scripts/machines/run-qemu.ps1) lines 165-167. The TODO wording "thread through to run-qemu.ps1 invocation" was a misread of the actual wrapper topology; the user-visible behavior (pass `-ExtraArgs '-s -S'` from CLI, QEMU sees `-s -S` appended) is identical.
> - The three `.ps1` files gained a `[string]$ExtraArgs = ''` parameter and a tokenize-and-append block immediately before `& $QEMU @QemuArgs`. The tokenizer is a regex (`[^\s"]+|"([^"]*)"`) that splits on whitespace but preserves substrings enclosed in double quotes, so a space-bearing path in a `-drive` fragment survives: `-ExtraArgs '-drive "id=test,file=C:\path with space\disk.img,format=raw"'`. [`run-qemu.ps1`](../../scripts/machines/run-qemu.ps1) was updated in the same commit to use the same quote-aware tokenizer (previously it used `.Split(' ', RemoveEmptyEntries)` which corrupted quoted paths); the two pre-existing callers under [`scripts/debug/input/`](../../scripts/debug/input/) pass only unquoted space-separated `-device ...` tokens so the tokenizer upgrade is backward compatible.
> - The five `.bat` shims (`run-nvme-test.bat`, `run-usb-test.bat`, `run-fat32-test.bat`, `run-ntfs-test.bat`, `run-all-fs-tests.bat`) forward `%*` to powershell so `run-nvme-test.bat -ExtraArgs "-s -S"` from cmd.exe routes through. Double-click use is unchanged because `%*` is empty when no arguments are supplied. `run-all-fs-tests.bat` forwards `%*` on every per-filesystem line, so the caller can supply `-ExtraArgs` once and it applies to the entire NTFS/FAT32/ext2/ext4/IXFS sweep.

> **Note:** No kernel test surface -- this section is entirely host-side launcher surface. Validation is (a) PowerShell param-declaration grep (4/4 `.ps1` launchers -- the 3 scenario launchers plus [`run-qemu.ps1`](../../scripts/machines/run-qemu.ps1) -- have `[string]$ExtraArgs`), (b) tokenizer append-block grep (4/4 have `[regex]::Matches($ExtraArgs, '[^\s"]+|"([^"]*)"')` feeding `$QemuArgs +=`), (c) `%*` forwarding grep (5/5 `.bat` shims: `run-nvme-test.bat`, `run-usb-test.bat`, `run-fat32-test.bat`, `run-ntfs-test.bat`, `run-all-fs-tests.bat` -- the last has 5 per-filesystem invocations, each with `%*`), (d) `bash scripts/build.sh` clean, (e) `bash scripts/test-tooling.sh` 40/40. Full end-to-end `-ExtraArgs "-s -S"` pass-through to `qemu-system-x86_64.exe` is a Windows-side concern and will be validated by any developer who double-clicks the bat or invokes it with args from cmd.exe.

> **Verified:** 2026-04-18 -- 4/4 checklist items [x] with diff in commit `11fa0d2a` + this review commit. Evidence: 4/4 `.ps1` launchers (3 scenario + [`run-qemu.ps1`](../../scripts/machines/run-qemu.ps1)) expose `[string]$ExtraArgs = ''` with an identical quote-aware `[regex]::Matches(...) | ForEach-Object { if ($_.Groups[1].Success) ... }` tokenizer block. 5/5 `.bat` shims forward `%*` (the sweep runner on every per-filesystem line, 5 invocations). [`docs/infrastructure/machine-matrix.md`](../../docs/infrastructure/machine-matrix.md) no longer contains "do not accept -ExtraArgs" anywhere; Storage + Filesystem Debugger paragraphs rewritten to document the new capability with a quoted-path example; `-ExtraArgs '<string>'` added to the 3 launcher Params lists; Known Drift list reduced from 5 to 4 entries (only the `-ExtraArgs missing` item deleted). Implementation Order row 8 [x]. Build clean: `=== BUILD OK ===`. Tokenizer verified against 6 test inputs via python-equivalent regex: `-s -S`, space-bearing quoted path, existing usb-hid caller bytes, multi-whitespace collapse, empty string -- all produce expected token arrays; existing callers unchanged.
> **Quality reviewed:** 2026-04-18 -- two Codex dispatches (step 13 adversarial before commit + step 20 pipeline: adversarial + quality). Step 13 caught 2 MEDIUMs with same root cause (`.Split(' ', RemoveEmptyEntries)` corrupts quoted QEMU args like `-drive "file=C:\path with space\disk.img"`); fixed before commit with the regex tokenizer applied to all 4 launchers, preserving backward compatibility for the 2 existing callers under `scripts/debug/input/`. Step 20 adversarial: approved with no material findings (noted optional DRY improvement -- 4 copies of 3-line tokenizer block could be dot-sourced from a shared helper; deferred as polish, not a ship-blocker). Step 20 quality: caught 1 LOW (§8 Notes block cited the pre-fix `.Split` grep + "4/4 shims" when actual state is `[regex]::Matches` + "5/5 shims"); fixed in this review. No domain code-quality skill applies (host PowerShell + `.bat` + docs; no `src/kernel`, `src/boot`, `src/desktop`, or `user/` touched). Perf: regex runs once per launcher on a <200-char string; microseconds. Dead-code: zero leftover `.Split` append paths in the tree (only explanatory text in commit message + Notes historical reference).

---

## 9. Legacy-XREF Sweep

Rewrite the ~60 source / test / header / script files on the `scripts/lint.sh` `TODO_XREF_LEGACY_FILES` allowlist so they stop using numeric TODO shorthand (`TODO-NN §M`, `DNN TNN §N`, `TNN §N`) outside `todo/**`. Lint Check 5 (renumbered from Check 7 on 2026-04-17) already errors on new out-of-`todo/` drift, so this is a one-shot catch-up after which lint can become a CI gate.

- [x] For each file in the allowlist, rewrite numeric refs as either (a) a functional description of what the code does, or (b) a relative-path + heading anchor link into the canonical doc that owns the concern.
- [x] As each file is cleaned, remove it from `TODO_XREF_LEGACY_FILES` in `scripts/lint.sh`. Target: 0 entries.
- [x] Templates under `.claude/skills/**`, `.cursor/**`, `.github/PULL_REQUEST_TEMPLATE.md` intentionally teach the shorthand and stay excluded from the sweep.
- [x] Once the allowlist is empty, wire `bash scripts/lint.sh` as a required gate in [`.github/workflows/build.yml`](../../.github/workflows/build.yml) and [`.github/workflows/release.yml`](../../.github/workflows/release.yml).
- [x] Commit: `"scripts: drop legacy numeric-TODO shorthand from tree; lint becomes CI gate"`

**Test checkpoint:** `bash scripts/lint.sh` exits 0 with zero errors on a clean tree (warnings acceptable). `.github/workflows/build.yml` and `release.yml` include `bash scripts/lint.sh` as a step; a PR that introduces new `TODO-NN §N` shorthand outside `todo/` fails CI.

> **Notes:**
> - Legacy allowlist drained: [`scripts/lint.sh`](../../scripts/lint.sh) `TODO_XREF_LEGACY_FILES` is `()` (empty). 60 files on the allowlist were swept; 55 contained refs that were rewritten (~130 instances total), 5 entries were stale/no-longer-applicable and removed without edits.
> - Rewrite strategy: Rule 1 dropped the parenthetical (`/* capability (TODO-NN §M) */` -> `/* capability */`) when the surrounding prose already described the concern. Rule 2 rewrote load-bearing refs as a functional description + domain-qualified filename (`TODO-13 §1` -> `TPM measured-boot event log parser`) or, in markdown, as an anchor link (`[capability](../../todo/NN-domain/TODO-MM-*.md#N-slug)`). No regex-style mechanical replacement -- each edit was per-context because `§N` in a narrative comment is different from `§N` in a checklist cross-reference.
> - Scope guards respected: `todo/**` untouched (shorthand legitimate inside TODOs), `.claude/skills/`, `.cursor/`, `.github/PULL_REQUEST_TEMPLATE.md` untouched (templates that teach the shorthand; already excluded at [`scripts/lint.sh`](../../scripts/lint.sh) lines 307-309).
> - Third-party vendored code (`src/libs/cjson/`) added to `EXCLUDE_PATTERNS` so upstream cJSON line-length conventions don't red-light CI. Comment in the exclude block documents the policy.
> - CI wiring: `Run style lint` step added to [`.github/workflows/build.yml`](../../.github/workflows/build.yml) at line 95 (after "Verify toolchain sentinels", before "Run tooling regression pack"). Mirrored in [`.github/workflows/release.yml`](../../.github/workflows/release.yml) at line 99 (after "Verify toolchain sentinels", before "Build OS (clean)"). A PR that introduces any numeric-TODO shorthand outside `todo/**` now fails both workflows with exit 1.
> - Lint post-sweep: 0 errors, 278 warnings. All 278 remaining warnings are pre-existing Check 4 camelCase flags on NT/Rtl/Ob/Se/Ke API function names (`NtCreateEvent`, `RtlCreateAcl`, etc.) -- legitimate Win11-parity naming, out of scope per "don't change code semantics" guard. Lint exits 0 because warnings don't block.
> - Two non-XREF error fixes landed in the same sweep (both were CI-gate blockers, so gating lint without fixing them would have been incomplete): line-length wraps in [`src/kernel/test/test_boot_info.c`](../../src/kernel/test/test_boot_info.c) (my §4 `test_suite_register_cat` calls exceeded 120 chars) and [`src/kernel/test/test_security.c`](../../src/kernel/test/test_security.c) (3 similar lines); `cJSON` vendored code excluded; one long comment on [`include/kernel/nt/ntstatus.h:72`](../../include/kernel/nt/ntstatus.h) moved above the `#define` to drop below 120 chars.

> **Test runner:** N/A -- kernel-test surface unchanged. Validation is the CI runner itself (lint-as-gate).
> **Expected:** `bash scripts/lint.sh` exits 0. 278 Check 4 warnings persist; out of scope (legitimate NT-API naming).

> **Verified:** 2026-04-18 -- 5/5 checklist items [x] with diff in commits `44355379` + this review commit. Evidence: [`scripts/lint.sh`](../../scripts/lint.sh) `TODO_XREF_LEGACY_FILES=()` (empty); 130 instances of numeric-TODO shorthand rewritten across 60 source/test/header/script/doc files; [`.github/workflows/build.yml`](../../.github/workflows/build.yml) + [`.github/workflows/release.yml`](../../.github/workflows/release.yml) both run `bash scripts/lint.sh` as a required gate. Contrived regression test confirmed: inserting `TODO-99 §1` into any tracked C/H/MD/YML file causes lint to exit 1 with `error: numeric TODO shorthand outside todo/`. Build clean `=== BUILD OK ===`; smoke test `SMOKE TEST PASSED`; 345 boot tests passed; drift harness 6/6; `test-tooling.sh` 40/40. All TODO-filename references in `include/` + `src/` resolve to real files under `todo/` (`find -name` audit 0 missing after Codex-round-2 fixed 12 broken paths the agent's Rule-2 rewrite introduced).
> **Quality reviewed:** 2026-04-18 -- three Codex dispatches: step-13 adversarial on the initial sweep (1 HIGH `bootx64.so` binary delta accepted as tracked build artifact per repo convention; 1 MEDIUM 12 broken TODO filename refs from agent Rule-2 rewrite, fixed via path-audit + sed rewrites); step-20 adversarial on committed code (2 MEDIUM: lint didn't scan `.github/**` YAML for shorthand, and `docs/infrastructure/development-tooling.md` wrapper-contract table still said lint was not wired -- both fixed in this review); step-20 quality dispatch (2 MEDIUM: the release.yml wrapper list falsely inherited build.yml's test-tooling.sh + test-boot-info-abi which release.yml does NOT run, and the supported-host GHA row still documented lint as held back -- both rewritten to match actual workflow contents). `scripts/lint.sh` `TODO_XREF_ROOTS` now includes `$REPO_ROOT/.github` and the grep include list adds `*.yml`/`*.yaml`. Domain code-quality skills: N/A (pure comment/doc rewrites; no `src/kernel` logic, `src/boot` logic, `src/desktop` logic, or `user/` code touched). Dead-code: the `is_todo_xref_legacy()` helper is structurally live (returns 0 always because the allowlist is empty, preserved as the re-entry mechanism if a future mass-refactor needs to re-stage) -- kept with comment. Perf: lint scans `.github/` adds <20 files; grep throughput trivial; CI cost <5s.

---

## 10. Smoke-Test POST16 Assertions

Migrate `scripts/test-smoke.sh`'s boot-pattern match list from raw log-message strings to POST16 code assertions. Log message strings drift silently every time a contributor edits a `printf` / `serial_printf` / `klog` call, breaking the smoke test long after the fact; POST16 codes are `#define` constants in [`include/kernel/boot_init.h`](../../include/kernel/boot_init.h) and [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) emitted as `POST 0xNNNN` on serial, and they only change when the boot-phase contract changes.

- [ ] Export the POST16 code-to-phase mapping from `include/kernel/boot_init.h` + `src/boot/uefi/bootx64.c` into a small shell-consumable manifest at `build/post16-manifest.env` (or `.json`). Generated by the Makefile at build time from the `#define` values; no hand-maintained duplicate.
- [ ] Rewrite `scripts/test-smoke.sh` `BOOT_REQUIRED_PATTERNS` / `PASS_PATTERNS_ALL` to source the manifest and assert a list of required POST16 codes.
- [ ] Keep a small residual set of kernel/userland string assertions (`C:\>` prompt, `Boot complete in` timestamp) as the user-visible end-to-end signal.
- [ ] Current string-pattern list stays in place as a fallback layer until the POST16 path is proven on KVM + TCG + VirtualBox + bare metal.
- [ ] Commit: `"scripts/test-smoke: assert POST16 codes from manifest instead of raw log strings"`

**Test checkpoint:** `scripts/test-smoke.sh` passes when `build/post16-manifest.env` is generated and every POST16 code in the required list appears on serial. A deliberately renamed `printf` in a boot-path file does NOT break the smoke test (string-pattern layer may log a residual diagnostic, but the core assertion is code-based).

---

## OS Comparison

| ⭐ | Feature                       | 🪟 Win11 projects         | 🐧 Linux projects          | 🚀 Impossible OS                                                                 |
| --- | ----------------------------- | ------------------------- | -------------------------- | --------------------------------------------------------------------------------- |
| 💎 | Bootstrap script              | ⚠️ WDK/HLK heavy setup    | ⚠️ Distro docs + scripts   | ✅ §1 -- setup.sh + --verify sentinel                                            |
| 💎 | Reproducible host profiles    | ⚠️ EWDK/Dev Box or VMs    | ✅ Devcontainers common    | ✅ §2 matrix + .devcontainer + shim procedure                                    |
| 💎 | Canonical build/test wrappers | ✅ Common in mature repos | ✅ Common in mature repos  | ✅ §3 wrapper contract + --help on all 5                                         |
| 💎 | Named VM/debug profiles       | ⚠️ Often ad hoc           | ⚠️ Often ad hoc            | ✅ §4 -- machine-matrix.md + bare-metal row                                      |
| 💎 | Managed local hooks           | ⚠️ Varies by repo         | ✅ Common in many repos    | ✅ §5 -- install-hooks.sh one-command bootstrap + sentinel-gated opt-in pre-push |
| 💎 | Workflow/artifact policy      | ✅ Standard CI practice   | ✅ Standard CI practice    | ✅ §6 -- 5 workflows classified, wrapper-aligned, retention/runner policy codified |
| ⭐ | One-command tooling doctor    | ❌ Rare in OS repos       | ❌ Rare in OS repos        | ⬜ §7                                                                            |

> **After §1-§6:** Impossible OS reaches the same baseline as well-run Windows and Linux projects for setup, reproducible environments, wrappers, hooks, and CI policy.
> **After §7:** the repo gains a cleaner operator experience than either baseline by shipping a first-class doctor/regression path for its own developer tooling.

## Unit Tests

> [!NOTE]
> Host-side tooling checks live outside `test_runner_init()`. This TODO owns a shell-based regression pack for scripts, hooks, and workflow contract checks, while runtime kernel or user-mode suites remain owned by `T03`, `T04`, and `T05`.

- [ ] Create `scripts/test-tooling.sh` with concrete assertions:
  - `bash scripts/setup.sh --help` exits 0 and points at the documented bootstrap path
  - supported-host and reproducible-environment files/docs resolve to the canonical wrapper contract once §2 lands; unsupported-host guidance names the documented fallback path instead of failing silently
  - `bash scripts/build.sh clean` writes `build/build.log`, and `tail -1 build/build.log` equals `=== BUILD OK ===`
  - `bash scripts/test.sh QUIET=1` keeps the summary path while suppressing per-test PASS spam on a healthy tree
  - in a temporary git repo or worktree, `bash scripts/install-hooks.sh` sets `core.hooksPath=.githooks` (idempotent on a second run), `--with-pre-push` additionally creates the `.git/.impossible-os-prepush` sentinel, `--disable-pre-push` removes it, `--remove` unsets `core.hooksPath` + drops the sentinel + prunes any legacy `.git/hooks/` symlinks
  - canonical docs and wrapper pages reject stale primary commands/paths such as `make test`, `run-tests.sh`, and `build/os-build.iso` unless they are explicitly marked legacy
  - workflow sanity checks grep `build.yml`, `release.yml`, `pages.yml`, `labeler.yml`, and `stale.yml` for the canonical wrapper paths they are supposed to invoke
- [ ] Wire the tooling regression pack into an existing lightweight local/CI path
- [ ] Document that runtime suite coverage still comes from the owning TODOs (`T03 §1-§4`, `T04 §3-§11`, `T05 §3-§8`) so this section does not silently drop test ownership
- [ ] Commit: `"test/tooling: add developer tooling regression pack"`

**Test checkpoint:** `bash scripts/test-tooling.sh` fails with a named assertion when a wrapper path, hook lifecycle, or sentinel drifts, and passes on a healthy repo.

## Verification

- [ ] Supported-host matrix and reproducible-environment path match reality for native Linux, WSL2, CI, and any committed container profile
- [ ] `bash scripts/setup.sh` on a clean supported machine installs the documented toolchain successfully
- [ ] `bash scripts/build.sh clean` ends with `=== BUILD OK ===`
- [ ] `bash scripts/test.sh QUIET=1` produces the documented summary mode
- [ ] `bash scripts/install-hooks.sh` configures `core.hooksPath=.githooks` (`.githooks/pre-commit` + `.githooks/post-commit` activate immediately); opt-in pre-push toggles via `--enable-pre-push` / `--disable-pre-push` sentinel; `--remove` unsets `core.hooksPath`, drops the sentinel, and prunes legacy `.git/hooks/` symlinks
- [ ] `README.md`, `CONTRIBUTING.md`, and `docs/infrastructure/development-tooling.md` no longer advertise stale primary flows such as `make test`, `run-tests.sh`, or ISO-only QEMU paths unless they are explicitly marked legacy
- [ ] GitHub Actions `build.yml` invokes the documented wrapper flow and uploads the expected artifacts/logs
- [ ] `bash scripts/tooling-doctor.sh` reports PASS on a healthy environment and actionable failure output on a broken one
- [ ] `bash scripts/test-tooling.sh` passes on a healthy tree and fails when a canonical wrapper path or sentinel is intentionally broken

**Test runner:** `scripts\debug\run-all-tests.bat` (Windows-side wrapper/QEMU smoke path only; host-side tooling assertions live in `bash scripts/test-tooling.sh`)
