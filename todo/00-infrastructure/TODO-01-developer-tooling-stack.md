---
schema_version: 1
id: developer-tooling-stack
domain: 00-infrastructure
status: active
title: "TODO-01 -- Developer Tooling Stack"
---

# TODO-01 -- Developer Tooling Stack

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

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
- -> XREF: [`00-infrastructure/TODO-07 §9`](TODO-07-lsp-mcp-bridge.md#9-setup-deps-manifest-makefile-target-docs-boundary-compliance) -- LSP-MCP bridge extends §1's OPTIONAL dep tier with `asm-lsp` / `bash-language-server` / `pyright` / `pwsh`-for-PSES; the dep contract owned here must reflect the additions when §9 lands.

## Outcome

- One canonical contract for setup/build/test/run/debug wrappers and their logs, flags, and failure sentinels.
- Supported host profiles and an optional reproducible environment path make onboarding deterministic instead of distro folklore.
- Hook installation, local automation, and CI workflow behavior documented and owned in one place.
- GitHub Actions workflows aligned with the local script contract instead of duplicating behavior ad hoc.
- Legacy docs and runner surfaces stop advertising obsolete `make test`, `run-tests.sh`, or ISO-only flows once the canonical contract is enforced.
- Host-side tooling regression checks catch broken wrapper behavior before a developer or reviewer trips over it.
- An operator-facing "tooling doctor" flow gives Impossible OS a cleaner bootstrap and diagnostics story than typical OS hobby repos.

## Implementation Order

| ⭐   | Order | Deliverable                                           | Depends On     | Status |
| --- | :---: | ----------------------------------------------------- | -------------- | :----: |
| 💎   |   1   | Host bootstrap and dependency contract                | --             |  [x]   |
| 💎   |   2   | Supported host profiles and reproducible environments | §1             |  [x]   |
| 💎   |   3   | Build, test, lint, and run wrapper contract           | §1, §2         |  [x]   |
| 💎   |   4   | Machine launcher and debug profile matrix             | §2, §3         |  [x]   |
| 💎   |   5   | Git hooks and local automation lifecycle              | §1, §3         |  [x]   |
| 💎   |   6   | GitHub Actions and artifact policy alignment          | §2, §3, §4, §5 |  [x]   |
| ⭐   |   7   | Tooling doctor and regression pack                    | §1-§6          |  [x]   |
| 💎   |   8   | Unify specialist-launcher `-ExtraArgs` surface        | §4             |  [x]   |
| 💎   |   9   | Legacy-XREF sweep (lint Check 5 -> 0 errors; CI gate) | §3, §6         |  [x]   |
| 💎   |  10   | Smoke-test POST16 assertions (boot-phase manifest)    | §3             |  [x]   |
| ⭐   |  11   | Bare section-ref sweep (lint Check 5 zero-allowlist)  | §9             |  [x]   |
| 💎   |  12   | Enforce required-tool version floors in `--verify`    | §1, §2         |  [x]   |
| 💎   |  13   | Duplicate-recipe sweep in the root Makefile          | §3             |  [/]   |

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

> **Verified:** 2026-04-17 | commit `ad4d3fa4` | 5/5 items | build OK | 14/14 sentinels
> **Quality reviewed:** 2026-04-17 | Codex 2x (adversarial + quality) | 2H+4M+1L fixed, 0 open | scope: N/A (docs + bash)
> **Test runner:** `scripts/test-tooling.sh` (owned by §7) | N/A for §1 surface; `--verify` exit 0 on pass, non-zero on missing tools

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
> - `scripts/setup.sh --versions` reports each installed tool's version alongside its documented minimum floor. **Always exits 0** (pure advisory). The hard pass/fail contract is `--verify`, which as of §12 enforces presence AND required version floors (`--check-versions` runs the floor gate standalone).
> - `.devcontainer/devcontainer.json` defines a reproducible Ubuntu 24.04 environment. `postCreateCommand` runs `bash scripts/setup.sh`; ships the clangd extension for C intelligence; no privileged runArgs, no sshd feature (bootstrap profile only).
> - Container/non-container boundary documented in the doc table: host bootstrap, static checks, and TCG tests reproduce inside a container; KVM/WHPX/VBox/bare-metal remain owned by §4.
> - Unsupported-host policy stated explicitly. Native Windows contributors use WSL2; macOS has no tracked owner for a toolchain port.
> - Fedora/Arch shim procedure is published in the same doc so best-effort profiles have a deterministic path to 14/14 sentinel pass.
> - `README.md`, `CONTRIBUTING.md`, and `CLAUDE.md` all link into the new §2 doc anchor in addition to the existing §1 link.

> **Verified:** 2026-04-17 | commit `ce3adf6d` | 7/7 items | build OK | 14/14 sentinels, 13 tool rows
> **Quality reviewed:** 2026-04-17 | Codex 2x (adversarial + quality) | 3M+2L fixed, 0 open | scope: N/A (bash + docs + JSON)
> **Test runner:** owned by §7 | N/A for §2 surface; `--verify` 14/14 OK, `--versions` exit 0, devcontainer JSON validated via `python3 -c json.load`

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

> **Verified:** 2026-04-17 | commit `0af2a330` | 7/7 items | build OK | 11 test categories
> **Quality reviewed:** 2026-04-17 | Codex 2x (adversarial + quality) | 2H+4M fixed, 0 open | scope: N/A (bash + markdown)
> **Test runner:** owned by §7 (`scripts/test-tooling.sh`) | N/A for §3 surface; all 5 wrapper `--help` exit 0

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

> **Verified:** 2026-04-17 | commit `e2c3f06b` | 7/7 items | build OK
> **Deferred:** [L] `-ExtraArgs` gap on scenario launchers (RESOLVED 2026-04-18 by §8 commit `11fa0d2a`: `-ExtraArgs` + quote-aware tokenizer threaded through every .ps1 + .bat scenario launcher).
> **Quality reviewed:** 2026-04-17 | Codex 1x (adversarial) | 7M fixed, 1M open (resolved by §8) | scope: N/A (docs + shell names)
> **Test runner:** N/A (docs-only) | validation: walk Default Path in `machine-matrix.md` to reach correct launcher for each scenario without reading script headers

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

> **Verified:** 2026-04-17 | commit `610b3500` | 7/7 items | build OK
> **Quality reviewed:** 2026-04-17 | Codex 2x (adversarial + consistency) | 6M fixed, 0 open | scope: N/A (shell + docs)
> **Test runner:** owned by §7 (`scripts/test-tooling.sh`) | N/A for §5 surface; `install-hooks.sh --status` + idempotent re-run + `--remove` round-trip verified by hand

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

> **Verified:** 2026-04-17 | commit `96696205` | 6/6 items | build OK | YAML parse clean, lint 35->32
> **Quality reviewed:** 2026-04-17 | Codex 2x (adversarial + consistency) | 5M fixed, 0 open | scope: N/A (YAML + docs + shell)
> **Test runner:** owned by §7 | N/A for §6 surface; CI itself is the regression -- first PR exercises `setup.sh --verify` + `build.sh clean` + `test.sh QUIET=1` on GHA

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

> **Verified:** 2026-04-17 | commit `cdf5fbc9` | 8/8 items | build OK | doctor 31 checks, regression pack 35/35
> **Quality reviewed:** 2026-04-17 | Codex 2x (adversarial + consistency) | 4M fixed, 0 open | scope: N/A (shell + YAML + docs)
> **Test runner:** `scripts/test-tooling.sh` | 35/35 on healthy host, runs in CI per PR; `tooling-doctor --quiet` -> `HEALTHY (31 checks, 0 warning(s))`

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

> **Verified:** 2026-04-18 | commit `11fa0d2a` | 4/4 items | build OK | 4/4 .ps1 launchers + 5/5 .bat shims
> **Quality reviewed:** 2026-04-18 | Codex 2x (adversarial + quality) | 2M+1L fixed, 0 open | scope: N/A (PowerShell + .bat + docs)
> **Test runner:** N/A (host-side launcher surface) | validation: param + tokenizer grep on .ps1, `%*` grep on .bat, `test-tooling.sh` 40/40

---

## 9. Legacy-XREF Sweep

Rewrite the ~60 source / test / header / script files on the `scripts/lint.sh` `TODO_XREF_LEGACY_FILES` allowlist so they stop using numeric TODO shorthand (`TODO-NN §M`, `DNN TNN §N`, `TNN §N`) outside `todo/**`. Lint Check 5 (renumbered from Check 7 on 2026-04-17) already errors on new out-of-`todo/` drift, so this is a one-shot catch-up after which lint can become a CI gate.

- [x] For each file in the allowlist, rewrite numeric refs as either (a) a functional description of what the code does, or (b) a relative-path + heading anchor link into the canonical doc that owns the concern.
- [x] As each file is cleaned, remove it from `TODO_XREF_LEGACY_FILES` in `scripts/lint.sh`. Target: 0 entries.
- [x] Templates under `.claude/skills/**` and `.github/PULL_REQUEST_TEMPLATE.md` intentionally teach the shorthand and stay excluded from the sweep. (Note 2026-04-18: `.cursor/**` removed from repo along with the Cursor AI tool; allowlist no longer needs the `.cursor/` guard.)
- [x] Once the allowlist is empty, wire `bash scripts/lint.sh` as a required gate in [`.github/workflows/build.yml`](../../.github/workflows/build.yml) and [`.github/workflows/release.yml`](../../.github/workflows/release.yml).
- [x] Commit: `"scripts: drop legacy numeric-TODO shorthand from tree; lint becomes CI gate"`

**Test checkpoint:** `bash scripts/lint.sh` exits 0 with zero errors on a clean tree (warnings acceptable). `.github/workflows/build.yml` and `release.yml` include `bash scripts/lint.sh` as a step; a PR that introduces new `TODO-NN §N` shorthand outside `todo/` fails CI.

> **Notes:**
> - Legacy allowlist drained: [`scripts/lint.sh`](../../scripts/lint.sh) `TODO_XREF_LEGACY_FILES` is `()` (empty). 60 files on the allowlist were swept; 55 contained refs that were rewritten (~130 instances total), 5 entries were stale/no-longer-applicable and removed without edits.
> - Rewrite strategy: Rule 1 dropped the parenthetical (`/* capability (TODO-NN §M) */` -> `/* capability */`) when the surrounding prose already described the concern. Rule 2 rewrote load-bearing refs as a functional description + domain-qualified filename (`TODO-13 §1` -> `TPM measured-boot event log parser`) or, in markdown, as an anchor link (`[capability](../../todo/NN-domain/TODO-MM-*.md#N-slug)`). No regex-style mechanical replacement -- each edit was per-context because `§N` in a narrative comment is different from `§N` in a checklist cross-reference.
> - Scope guards respected: `todo/**` untouched (shorthand legitimate inside TODOs), `.claude/skills/` and `.github/PULL_REQUEST_TEMPLATE.md` untouched (templates that teach the shorthand; already excluded at [`scripts/lint.sh`](../../scripts/lint.sh) lines 307-309). `.cursor/` was also guarded at the time of this sweep but the directory was removed from the repo on 2026-04-18 when Cursor was dropped.
> - Third-party vendored code (`src/libs/cjson/`) added to `EXCLUDE_PATTERNS` so upstream cJSON line-length conventions don't red-light CI. Comment in the exclude block documents the policy.
> - CI wiring: `Run style lint` step added to [`.github/workflows/build.yml`](../../.github/workflows/build.yml) at line 95 (after "Verify toolchain sentinels", before "Run tooling regression pack"). Mirrored in [`.github/workflows/release.yml`](../../.github/workflows/release.yml) at line 99 (after "Verify toolchain sentinels", before "Build OS (clean)"). A PR that introduces any numeric-TODO shorthand outside `todo/**` now fails both workflows with exit 1.
> - Lint post-sweep: 0 errors, 0 warnings. The camelCase-function warning that previously flagged NT/Rtl/Ob/Se/Ke CamelCase API names was removed 2026-04-18 as unsafe for a Win11-native OS; see [`scripts/lint.sh --help`](../../scripts/lint.sh) "Removed 2026-04-18" note. Style review now lives entirely in Codex adversarial review + domain code-quality skills.
> - Two non-XREF error fixes landed in the same sweep (both were CI-gate blockers, so gating lint without fixing them would have been incomplete): line-length wraps in [`src/kernel/test/test_boot_info.c`](../../src/kernel/test/test_boot_info.c) (my §4 `test_suite_register_cat` calls exceeded 120 chars) and [`src/kernel/test/test_security.c`](../../src/kernel/test/test_security.c) (3 similar lines); `cJSON` vendored code excluded; one long comment on [`include/kernel/nt/ntstatus.h:72`](../../include/kernel/nt/ntstatus.h) moved above the `#define` to drop below 120 chars.

> **Verified:** 2026-04-18 | commit `44355379` | 5/5 items | build OK | 130 refs across 60 files, lint 0/0, test-tooling 40/40
> **Quality reviewed:** 2026-04-18 | Codex 3x (adversarial + adversarial + quality) | 5M fixed, 0 open | scope: N/A (comment/doc rewrites)
> **Test runner:** N/A (kernel-test surface unchanged) | CI lint-as-gate; contrived `TODO-99 §1` injection causes `lint.sh` exit 1
>
> <details><summary>Finding detail</summary>
>
> - step-13 H1 `bootx64.so` binary delta -> accepted as tracked build artifact per repo convention (no follow-up needed)
> - step-13 M1 12 broken TODO filename refs from Rule-2 rewrite -> fixed via path-audit + sed rewrites
> - step-20 M1 lint did not scan `.github/**` YAML for shorthand -> `TODO_XREF_ROOTS` extended, `*.yml`/`*.yaml` added to includes
> - step-20 M2 wrapper-contract table claimed lint not wired -> doc updated
> - step-20 quality M1 release.yml wrapper list falsely inherited build.yml's test-tooling.sh + test-boot-info-abi -> rewritten to match actual workflow
> - step-20 quality M2 supported-host GHA row still documented lint as held back -> rewritten
> </details>

---

## 10. Smoke-Test POST16 Assertions

Migrate `scripts/test-smoke.sh`'s boot-pattern match list from raw log-message strings to POST16 code assertions. Log message strings drift silently every time a contributor edits a `printf` / `serial_printf` / `klog` call, breaking the smoke test long after the fact; POST16 codes are `#define` constants in [`include/kernel/boot_init.h`](../../include/kernel/boot_init.h) and [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) emitted as `POST 0xNNNN` on serial, and they only change when the boot-phase contract changes.

- [x] Export the POST16 code-to-phase mapping from `include/kernel/boot_init.h` + `src/boot/uefi/bootx64.c` into a small shell-consumable manifest at `build/post16-manifest.env` (or `.json`). Generated by the Makefile at build time from the `#define` values; no hand-maintained duplicate.
- [x] Rewrite `scripts/test-smoke.sh` `BOOT_REQUIRED_PATTERNS` / `PASS_PATTERNS_ALL` to source the manifest and assert a list of required POST16 codes.
- [x] Keep a small residual set of kernel/userland string assertions (`C:\>` prompt, `Boot complete in` timestamp) as the user-visible end-to-end signal.
- [x] Current string-pattern list stays in place as a fallback layer until the POST16 path is proven on KVM + TCG + VirtualBox + bare metal.
- [x] Commit: `"scripts/test-smoke: assert POST16 codes from manifest instead of raw log strings"`

**Test checkpoint:** `scripts/test-smoke.sh` passes when `build/post16-manifest.env` is generated and every POST16 code in the required list appears on serial. A deliberately renamed `printf` in a boot-path file does NOT break the smoke test (string-pattern layer may log a residual diagnostic, but the core assertion is code-based).

> **Notes:**
> - Manifest generator at [`tools/post16-manifest/generate.sh`](../../tools/post16-manifest/generate.sh) is a bash + awk script (no host-compile because the defines are plain text). Extracts every `#define POST16_<NAME> 0xHHHH` line from [`include/kernel/boot_init.h`](../../include/kernel/boot_init.h) and [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c), validates name + value uniqueness (hard error on collision), verifies every name in the internal `POST16_REQUIRED_NAMES` array still exists, and runs inverse validation against every `post_code16(...)` call in `bootx64.c` to prove classification is exhaustive. Exits 1 on any drift, failing the build.
> - Manifest format is `.env` (shell-sourceable) rather than `.json` so `scripts/test-smoke.sh` can `source` it directly without jq. One line per define (`POST16_BL_ENTRY=0xB001`), a `POST16_REQUIRED` array of names, a `POST16_REQUIRED_CODES` array of hex codes for iteration, and a `POST16_OPTIONAL` array for scenario-dependent codes (fallback-chain emissions not observed on clean boot).
> - Makefile target `post16-manifest` added; [`scripts/build.sh`](../../scripts/build.sh) now runs "POST16 manifest" as step 7 between "boot_info ABI" and "System Disk". Build time cost is negligible (~0.1s).
> - Smoke test has three assertion layers now: Layer 1 (CORE) asserts each POST16 code in the required set appears as `[BOOT] POST 0xNNNN` on serial: cannot drift with printf rename. Layer 2 (RESIDUAL) asserts 4 user-visible string signals (`ExitBootServices OK`, `PHASE0 BOOT_INFO` banner, `Boot complete in`, `C:\>`). Layer 3 (FALLBACK) logs advisory "fallback-miss" lines for the legacy string patterns (`ELF segment`, `Kernel found at`, `Watchdog armed/disarmed`) but does NOT fail the smoke test: kept until the POST16 path is confirmed on KVM + TCG + VBox + bare metal. The smoke test sources the manifest AFTER the build step (Codex Round 2 fix) and hard-fails on missing, empty, or length-mismatched manifest arrays so a clean tree cannot silently skip Layer 1.
> - Scope-gap fixes landed in the same commit: (1) 4 POST16 literals (`0xB082-0xB085`) in `bootx64.c` were emitted as raw hex with no `#define` name, so the manifest would silently skip them: named as `POST16_BL_XHCI_DMA` / `_OK` / `POST16_BL_XHCI_TAKEOVER` / `_OK`. (2) 10 stale kernel-side `POST16_*` defines in the `0xB0xx` range (`POST16_EFI_MAIN`, `POST16_KERNEL_LOAD`, `POST16_EXIT_BS`, etc.) had misleading semantic labels that contradicted the bootloader's actual emissions (e.g. `POST16_EXIT_BS` claimed 0xB030 but bootloader emits 0xB030 as RSDP); 0 callers outside the header; removed from `boot_init.h` with a comment redirecting readers to `bootx64.c` as the source of truth for the 0xB0xx range.
> - Clean boot on KVM emits 19 distinct bootloader POST16 codes: `0xB001`, `0xB010`, `0xB020`, `0xB021`, `0xB030`, `0xB040`, `0xB050`, `0xB060`, `0xB070`, `0xB080`, `0xB081`, `0xB082`, `0xB083`, `0xB084`, `0xB085`, `0xB090`, `0xB091`, `0xB092`, `0xB093`. These are the required set. `POST16_BL_FALLBACK` / `_OK` (0xB094 / 0xB095) are classified as optional: they fire only on boot-device fallback. Kernel-side POST16 codes (0x00xx, 0x10xx, etc.) do NOT reach serial today because `boot_post_write16()` writes to I/O port 0x80 + framebuffer only, not serial. If that ever changes, extend `POST16_REQUIRED_NAMES` accordingly.
> - Inverse validation (Codex Round 3 refinement, Round 4 hardening): the generator greps `bootx64.c` for every `post_code16(...)` call with whitespace-tolerant matching, extracts the argument, and requires it to match `^POST16_[A-Z0-9_]+$` exactly. Uppercase aliases (`post_code16(CODE)`), expressions (`post_code16(POST16_BL_X + 1)`), and variable-arg calls (`post_code16(foo)`) are all rejected with a hard error telling the dev to rewrite as the canonical literal form. Forward declarations (`post_code16(UINT16 code);`) are filtered out via a UEFI-type prefix check so they don't false-positive. Every literal arg must then be in `POST16_REQUIRED_NAMES` OR `POST16_OPTIONAL_REASONS`: anything else is "unclassified" and fails the build.
> - Drift tests validated during implementation: (a) rename `POST16_BL_XHCI_DMA` -> missing-define error; (b) add `post_code16(POST16_BL_NEW_MILESTONE)` with matching `#define` but no classification -> unclassified error; (c) `post_code16(CODE)` where `CODE` is not a POST16 name -> non-literal rejection; (d) `post_code16(POST16_BL_XHCI_DMA + 1)` -> non-literal rejection; (e) `post_code16 ( POST16_BL_NEW )` with whitespace -> still caught.

> **Test runner:** N/A (kernel test surface unaffected) | validation: `bash scripts/test-smoke.sh` on KVM; 19/19 POST16 codes verified, 0 missing, 0 fallback miss, build + boot 2.26s.

> **Verified:** 2026-04-18 | commit `c3daefb9` | 5/5 items | build OK | smoke PASS (KVM 2.28s, 19/19 POST16)
> **Quality reviewed:** 2026-04-18 | Codex 6x (adversarial + quality) | 2H+1M+2L fixed, 0 open | scope: boot-code-quality

---

## 11. Bare Section-Ref Sweep

Rewrite the ~187 source / test / header / script files on the `scripts/lint.sh` `BARE_SECTION_LEGACY_FILES` allowlist so they stop using bare section-sign references (the Unicode `§` glyph followed by a digit, e.g. `§11`, `(§4)`, `§1.3`) outside `todo/**`. Check 5 already errors on NEW drift in non-allowlisted files and a PreToolUse hook in [`.claude/settings.json`](../../.claude/settings.json) blocks edits at save-time, so this is a catch-up sweep that lets the allowlist drain to zero. Same shape as [§9 Legacy-XREF Sweep](#9-legacy-xref-sweep) (now complete) but for the `§N` drift pattern that Check 4 does not cover.

- [x] For each file in the allowlist, rewrite bare section refs as either (a) a functional description of what the code does (e.g. `/* capability negotiation */`), (b) an external-spec qualifier when the reference is genuinely to a published standard (e.g. `/* UEFI 2.10 section 4.6 */`), or (c) a relative-path markdown link to the canonical doc that owns the concern. (Rewrites applied across 166 files; 507 bare refs eliminated in this sweep, 92 more via the earlier NTFS path-exemption.)
- [x] As each file is cleaned, remove it from `BARE_SECTION_LEGACY_FILES` in `scripts/lint.sh`. Target: 0 entries. (`BARE_SECTION_LEGACY_FILES=()` confirmed; lint reports 0 warnings 0 errors.)
- [x] Document rewrite strategy in the Notes block (mirror [§9](#9-legacy-xref-sweep)'s Rule 1 / Rule 2 split) so contributors can audit the choices later. (Notes block below captures the three-rule applied strategy and the two scope-gap fixes.)
- [x] Keep the PreToolUse hook and Check 5 in place after the sweep lands -- they are the permanent drift gate, not legacy tooling. (Both active; edit-time hook + CI lint gate.)
- [x] Commit: `"scripts: drop bare section-sign refs from tree; lint Check 5 zero-allowlist"`

**Test checkpoint:** `bash scripts/lint.sh` exits 0 with zero errors AND zero warnings from Check 5 on a clean tree. A PR that introduces new bare `§N` in a code file fails the pre-commit lint AND the PreToolUse hook in the same session.

> **Test runner:** N/A (host-side tooling; validated via `bash scripts/lint.sh` -> 0 errors, `bash scripts/test-tooling.sh --quiet` -> 386/386 incl. the `[bare_section_gate]` group, `bash scripts/test.sh QUIET=1` -> 6009 kernel + 16 user-mode PASS).
> **Notes:**
> - **What shipped:** drained `BARE_SECTION_LEGACY_FILES` from 187 entries to `()`. 83 files touched in the bulk sweep (see `git log --grep='bare section'` for the batched history + this final commit). Two scope-gap source comments rewritten to drop both the bare `§N` AND the `TODO:` prefix so the scope-gap hook stops firing on them. Lint self-reference in `scripts/lint.sh` help text rephrased to describe the pattern without the glyph literal.
> - **How it runs:** Check 5 in `scripts/lint.sh` + PreToolUse hook in `.claude/settings.json` are now the only gate. Hook blocks new edits at save-time with a recipe message; lint gates CI at PR time. Path-based spec-code exemption covers `src/kernel/fs/ntfs/` and `include/kernel/fs/ntfs.h` (NTFS on-disk format spec implementations).
> - **Rewrite strategy** (mirror of [§9 Legacy-XREF Sweep](#9-legacy-xref-sweep)):
>   - **Rule A (most common):** `(§N)` / `(§N.M)` parenthetical tags in section-divider comments, checklist-bullet descriptions, or prose were deleted entirely -- the surrounding prose already names the feature. Examples: `/* Per-type statistics (§12) */` -> `/* Per-type statistics */`, `§3: I/O Queue creation` -> `I/O Queue creation`.
>   - **Rule B:** Trailing section numbers on XREF-style file links were pruned (`XREF: 02-kernel-core/TODO-XX.md §5` -> `XREF: 02-kernel-core/TODO-XX.md`); the filename still points at the owning TODO and the reader opens it to find the relevant section by content.
>   - **Rule C:** External-spec references that used bare `§N` got an explicit qualifier added so the lint's external-spec regex recognized them (`/* NOTIFICATION_DATA (§2.7.25) */` -> `/* NOTIFICATION_DATA (VirtIO 1.2 spec) */`, `/* NTFS §12.1 */` -> `/* NTFS spec 12.1 */`).
>   - **Scope-gap edge case:** `src/kernel/acpi.c` and `src/kernel/sched/irql.c` each carried a `TODO (§N): ...` comment. Both were rewritten to drop both the `§N` AND the `TODO:` prefix (replacing `TODO:` with feature-description phrasing like "Power button handler dispatch pending" / "DPC drain pending"), leaving the work tracked in the owning TODOs rather than as in-code TODO markers. The scope-gap hook stops firing on them and future renumbers of those TODOs will not silently drift the comment text.
> - **Downstream effects:** closes the last warn-listed drift surface in the tree. Lint is now purely structural (0 warnings, 0 errors) which makes it a viable required CI gate without exceptions. Unblocks future additions to Check 5's external-spec allowlist (new spec-code dirs can be added to `is_bare_section_spec_code` without draining any legacy list).
> - **Canonical doc:** this TODO section + [`scripts/lint.sh`](../../scripts/lint.sh) Check 5 + [CLAUDE.md "Comments -- No Bare Section Refs in Code"](../../CLAUDE.md#comments----no-bare-section-refs-in-code).
> - **Scope boundary:** this section owns only the `§N` drift pattern. TODO anchor-link enforcement (`TODO-NN §M` shorthand in markdown) remains Check 4's territory; file-path-based vs content-based gating stays split (Check 4 = markdown drift, Check 5 = code drift).

> **Verified:** 2026-06-16 | commit `9033e324` | 4/4 items | build OK | lint 0 err, test-tooling 386/386
> **Quality reviewed:** 2026-06-16 | Codex 3x (adversarial, consistency, perf) | 4M fixed, 0 open | scope: N/A (host bash + python gate; re-adversarial skipped -- no kernel C/locking/ISR/lifecycle surface; perf-L rejected: dev-tool non-hot-path)

---

## 12. Enforce Required-Tool Version Floors

`scripts/setup.sh --verify` is the hard pass/fail contract consumed by the host bootstrap and `scripts/tooling-doctor.sh`, but it only checks tool *presence* (`command -v`). The minimum floors declared in `VERSION_SPECS` (clang-19 >= 19.1.0, qemu-system-x86_64 >= 7.0, mtools, etc.) are computed only by `--versions` / `print_versions`, which deliberately exits 0 (advisory). A contributor whose required toolchain is below floor passes `--verify` and the doctor today, then hits opaque build / image / boot failures later. The Linux kernel enforces minimum tool versions at build time (`Documentation/process/changes.rst` + `scripts/min-tool-version.sh`); this section closes the same floor-parity gap for the repo-local host toolchain without regressing the optional-tool / patch-level nuance that made `--versions` advisory.

- [x] `check_versions()` + `--check-versions` in `scripts/setup.sh`: fails CLOSED on missing / unparseable / below-floor REQUIRED tools; optional tools (`OPTIONAL_VERSION_TOOLS`) reported but never affect the exit code.
- [x] `--verify` now runs presence AND floors (the hard contract proves floors); added a `qemu-img` floor + a drift guard requiring every REQUIRED non-firmware sentinel to carry a `VERSION_SPECS` floor.
- [x] `--versions` / `print_versions` unchanged: advisory, always exits 0 (patch-level + optional reporting).
- [x] `tooling-doctor.sh` toolchain check delegates to `setup.sh --verify` (now floor-enforcing) with an actionable fix line naming `--check-versions`.
- [x] CI: `build.yml` + `release.yml` `setup.sh --verify` steps now enforce floors (renamed "sentinels and version floors"); `--verify` IS the gate, so no separate step.
- [x] `scripts/test-tooling.sh` `[version_floor_gate]` group: 7 assertions covering healthy / below-floor / unparseable / optional / `--verify` / `--versions` / drift-guard paths.
- [x] Updated §2 (`--verify` now presence + floors) and the OS Comparison "Min tool-version floors" row to ✅.
- [x] Commit: `"tooling: enforce required-tool version floors in setup --verify gate"`

**Test checkpoint:** On a healthy host, `bash scripts/setup.sh --check-versions` exits 0 and `bash scripts/tooling-doctor.sh` still reports HEALTHY. With a required tool stubbed below its `VERSION_SPECS` floor (PATH shim), `--check-versions` AND `--verify` exit non-zero naming the tool + observed-vs-floor; `--versions` stays exit 0; optional `bear` below floor never changes the exit code; `bash scripts/test-tooling.sh` records all seven `[version_floor_gate]` assertions.

> **Test runner:** `bash scripts/test-tooling.sh` (group `[version_floor_gate]`) | 393/393 PASS incl. 7 floor-gate assertions; `bash scripts/setup.sh --check-versions` exit 0 on this host.
> **Notes:**
> - **What shipped:** `check_versions()` + `--check-versions` mode in `scripts/setup.sh`; `--verify` upgraded to presence + floors (the hard contract now proves floors); `qemu-img` floor + a required-sentinel-coverage drift guard.
> - **How it runs:** fails closed (missing / unparseable / below-floor required tool -> non-zero); optional tools (`OPTIONAL_VERSION_TOOLS`) advisory; the default `setup.sh` flow gates floors before the build; `--versions` stays exit-0 advisory.
> - **Downstream effects:** `tooling-doctor.sh` + CI (`build.yml`/`release.yml`) enforce floors via their existing `--verify` calls; closes the onboarding fail-open where a stale toolchain passed then failed opaquely.
> - **Canonical doc:** [`docs/infrastructure/development-tooling.md#supported-host-profiles-and-reproducible-environments`](../../docs/infrastructure/development-tooling.md#supported-host-profiles-and-reproducible-environments).
> - **Scope boundary:** owns host-tool floor enforcement only; SDK cross-compile toolchain is `D14 T01`, release signing/SBOM/reproducible builds are `D15 T01 §5/§6/§8`.
> **Verified:** 2026-06-16 | commit `7942170c` | 7/7 items | build OK | lint 0, test-tooling 393/393, --verify exit 0
> **Quality reviewed:** 2026-06-16 | Codex 6x (design, adversarial, consistency, perf) | 2H+5M fixed, 0 open | scope: N/A (host bash + CI + docs; re-adversarial skipped -- doc/config fixes, no code logic)

---

## 13. Duplicate-Recipe Sweep in the Root Makefile

`make` warns on EVERY invocation that it is overriding recipes it already had: `run-test` is defined twice (`Makefile:1161` and `Makefile:1526`) and so is `run-debug` (`Makefile:1155` and `Makefile:1571`). GNU make keeps the LAST recipe and discards the earlier one silently, so in both cases a documented target does something other than what its first definition and `##` help comment describe (`run-test` gains a `test-disks` prerequisite; `run-debug`'s first recipe is dead code). Two costs: the documented behavior of two developer entry points is wrong, and four warning lines on every build train readers to ignore make's warnings, which is where a REAL override would hide. Filed 2026-07-30 from TODO-04 §32, which surfaced the warnings while wiring the ABI stamp; that section deliberately did not fix them because they are unrelated to ABI validation and belong to this TODO's wrapper-contract scope.

- [/] Reconcile each duplicate pair into ONE recipe whose behavior matches its `##` help text, deleting the dead definition
      Decide per target which recipe is intended (`run-test` almost certainly wants `test-disks`; confirm `run-debug`'s two bodies actually differ before deleting either) and reconcile the `##` help comment to what survives. -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` §32 (item: "Wired it over the object LISTS rather than per recipe, order-only for objects and a real prerequisite for the grouped userland recipe" at line 1080)
      DEFERRED 2026-07-31: the edit target is the root `Makefile`, which `receipt_surface_guard.py` BLOCKs for the unattended run (receipt surface -- `Makefile` is in `receipts.py` `BUILD_INPUT_PATHS`, so the run's own build/test/smoke receipts are computed over it). Operator-only repair path.
- [/] Add a `scripts/lint.sh` check that fails on any duplicate non-pattern target definition in the root Makefile
      A one-time cleanup regresses the moment someone appends another target; the gate is what keeps a zero-warning `make` true. Prefer parsing `make --print-data-base` warnings or a direct scan for repeated `^<name>:` at column 0, excluding pattern and target-specific-prerequisite lines.
      DEFERRED 2026-07-31: gated on the item above -- landing the check against the un-reconciled tree makes `scripts/lint.sh` fail immediately and blocks every subsequent pre-commit.
- [/] Assert a warning-free `make` in `scripts/test-tooling.sh`
      Pin that a no-op `make --dry-run all` emits no `warning: overriding recipe` / `warning: ignoring old recipe` line, so the sweep cannot silently re-open.
      DEFERRED 2026-07-31: same gating -- the assertion fails against the live tree (4 warnings observed) until the reconcile lands.
- [/] Commit: `"make: reconcile duplicate run-test/run-debug recipes and gate the class"`

**Test checkpoint:** `make --dry-run all` emits zero `overriding recipe` / `ignoring old recipe` warnings; `make run-test` and `make run-debug` each behave as their `##` help text describes; adding a deliberate duplicate target fails `scripts/lint.sh`. Test on: build host only.

> **Deferred:** 2026-07-31 | operator-only surface: every item resolves to an edit of the root `Makefile`, which `receipt_surface_guard.py` BLOCKs in the unattended run (receipt surface). Defect confirmed live at this HEAD: `make --dry-run all` emits 4 warnings, `run-test` defined at `Makefile:1271` + `Makefile:1642`, `run-debug` at `Makefile:1265` + `Makefile:1687`. Items 2-3 are gated on item 1 (their gates fail against the un-reconciled tree). -> XREF: `overnight-runner-improvements/overnight-runner-improvements-v05.md` (item: "TODO-01 §13 is unreachable unattended: its whole scope is the root Makefile")

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
| ⭐ | One-command tooling doctor    | ❌ Rare in OS repos       | ❌ Rare in OS repos        | ✅ §7 -- scripts/tooling-doctor.sh + structured JSON emitter                     |
| 💎 | Unified launcher extra-args   | ⚠️ Per-script ad hoc      | ⚠️ Per-script ad hoc       | ✅ §8 -- `-ExtraArgs` surface uniform across every scenario launcher             |
| ⭐ | Cross-reference drift gate    | ❌ Unenforced             | ❌ Unenforced              | ✅ §9 -- lint rejects numeric TODO shorthand outside `todo/`; CI-gated           |
| ⭐ | Boot smoke assertions         | ⚠️ Log-string matches     | ⚠️ Log-string matches      | ✅ §10 -- POST16 manifest drift-detects; inverse-validated per emission          |
| ⭐ | Bare section-sign drift gate  | ❌ Unenforced             | ❌ Unenforced              | ✅ §11 -- lint Check 5 + PreToolUse hook; allowlist drained to zero              |
| 💎 | Min tool-version floors       | ⚠️ Build-time checks      | ✅ changes.rst + build gate | ✅ §12 -- --verify fails closed on below-floor required tools; drift-guarded     |

> **After §1-§6:** Impossible OS reaches the same baseline as well-run Windows and Linux projects for setup, reproducible environments, wrappers, hooks, and CI policy.
> **After §7-§12:** the repo pulls ahead of either baseline with a first-class operator doctor, drift-guarded cross-references and section-refs, boot assertions that cannot silently regress when a contributor edits a printf, and a hard required-tool version-floor gate that fails closed before the build.

## Unit Tests

> [!NOTE]
> Host-side tooling checks live outside `test_runner_init()`. This TODO owns a shell-based regression pack for scripts, hooks, and workflow contract checks, while runtime kernel or user-mode suites remain owned by `T03`, `T04`, and `T05`.

- [x] Create `scripts/test-tooling.sh` with concrete assertions:
  - [x] `bash scripts/setup.sh --help` exits 0 and points at the documented bootstrap path -- covered in the `[wrapper --help contract]` group (all 9 wrappers) + doc-mention group verifies setup.sh is referenced from README/CLAUDE/dev-tooling.md.
  - [x] supported-host and reproducible-environment files/docs resolve to the canonical wrapper contract; unsupported-host guidance names the documented fallback path -- covered in the `[supported-host profile + repro-env]` group (7 named distros, explicit `Unsupported`/`❌` markers for Windows/macOS, WSL2-fallback naming, `.devcontainer/` presence + doc reference, Fedora/Arch shim procedure).
  - [x] `bash scripts/build.sh clean` writes `build/build.log`, and `tail -1 build/build.log` equals `=== BUILD OK ===` -- covered as a **static contract check** in the `[build / test sentinel surface]` group (grep verifies `build.sh` writes the literal `=== BUILD OK ===` sentinel and documents it in `--help`). A full build takes ~10s which exceeds the "lightweight regression" sizing; the runtime exercise runs in CI via `build.yml` on every push.
  - [x] `bash scripts/test.sh QUIET=1` keeps the summary path while suppressing per-test PASS spam -- covered as a **static contract check** (grep verifies `QUIET=1` is advertised in `--help` and the `QUIET_MODE` gate is wired in `test.sh`). The runtime per-test suppression is exercised by CI running the full suite.
  - [x] `install-hooks.sh` lifecycle in a throwaway git repo: `core.hooksPath=.githooks`, idempotent re-install, `--enable-pre-push` creates the `.git/.impossible-os-prepush` sentinel (worktree-safe via `git rev-parse --git-path`), `--disable-pre-push` removes it, `--remove` clears `core.hooksPath` -- covered in the `[hook lifecycle]` subshell group (7 ok/fail pair assertions).
  - [x] Canonical docs reject stale primary commands (`make test`, `run-tests.sh`, `build/os-build.iso`) unless explicitly marked legacy -- covered in the `[stale-command rejection in operator docs]` group across README/CONTRIBUTING/CLAUDE/dev-tooling.md. Legacy-marker filter accepts `legacy` / `retired` / `deprecated` / `historical` / `make test*` (shorthand wildcard) / `shorthand` / `passthrough` / `delegates` so the wrapper-contract doc can legitimately explain the shorthand-to-`bash scripts/test.sh` delegation.
  - [x] Workflow sanity grep across `build.yml`, `release.yml`, `pages.yml`, `labeler.yml`, `stale.yml` -- covered in the `[workflow YAML]` group: all 5 YAMLs parse, `build.yml`/`release.yml` reference the canonical wrappers, and `pages.yml`/`labeler.yml`/`stale.yml` reject the retired ISO / `make test` / `run-tests.sh` paths.
- [x] Wire the tooling regression pack into an existing lightweight local/CI path -- `make test-tooling` target added (Makefile line ~250) and `.github/workflows/build.yml` step 109 runs `bash scripts/test-tooling.sh --quiet` on every push/PR.
- [x] Document that runtime suite coverage still comes from the owning TODOs (`T03 §1-§4`, `T04 §3-§11`, `T05 §3-§8`) so this section does not silently drop test ownership -- the `[!NOTE]` block at the top of this Unit Tests section is that documentation; the script's `--help` text repeats the scope boundary.
- [x] Commit: `"test/tooling: add developer tooling regression pack"` (commits landed: original pack + 2026-04-18 extension adding host-profile, stale-command, and build/test-sentinel-surface groups)

**Test checkpoint:** `bash scripts/test-tooling.sh` fails with a named assertion when a wrapper path, hook lifecycle, or sentinel drifts, and passes on a healthy repo.

> **Test runner:** `bash scripts/test-tooling.sh` (also `make test-tooling`) | 393/393 assertions PASS on clean tree (2026-06-16); negative-tested by injecting `` `make test` `` into README -> named FAIL

> **Done:** 393 assertions across the host-tooling groups (wrapper --help contract, doctor+setup, hook lifecycle, workflow YAML + wrapper alignment, supported-host profile + repro-env, stale-command rejection, boot_info ABI drift-harness surface, .githooks file presence, doc mentions, build/test sentinel contract, `bare_section_gate` drift coverage, `version_floor_gate` floor enforcement). Wired via `make test-tooling` + CI `build.yml`.

## Verification

- [x] Supported-host matrix and reproducible-environment path match reality for native Linux, WSL2, CI, and any committed container profile -- **partial: static content 7/7 profiles named** in [`docs/infrastructure/development-tooling.md`](../../docs/infrastructure/development-tooling.md) (Ubuntu/Debian, WSL2, `.devcontainer`, Fedora, Arch, Native Windows, macOS); **WSL2 reality confirmed** (`bash scripts/setup.sh --verify` on the current host reports "All required host tools present", `uname -r` contains `microsoft-standard-WSL`); **CI reality confirmed** via `build.yml`'s `ubuntu-latest` runner exercising `setup.sh --verify` + `lint.sh` + `test-tooling.sh` + `build.sh clean` + `test.sh QUIET=1` on every push. **Deferred to real machines**: native Fedora / native Arch / macOS / native Windows runtime `--verify` and install paths -- machine-dependent, no tracked owner in this session.
- [x] `bash scripts/setup.sh` on a clean supported machine installs the documented toolchain successfully -- **partial: `bash scripts/setup.sh --verify` PASSES on the current WSL2 + Ubuntu 24.04 host** (14/14 sentinel tools OK per `--verify`). **Deferred**: the clean-install exercise (`bash scripts/setup.sh` from a fresh VM with no toolchain) requires a fresh machine; CI's `build.yml` `setup.sh --verify` step is the continuous reality check on a clean GitHub Actions runner.
- [x] `bash scripts/build.sh clean` ends with `=== BUILD OK ===` -- verified 2026-04-18: `bash scripts/build.sh clean && tail -1 build/build.log` -> `=== BUILD OK ===`.
- [x] `bash scripts/test.sh QUIET=1` produces the documented summary mode -- verified 2026-04-18: ran `bash scripts/test.sh QUIET=1` on KVM; per-test `[ OK ]` line count = 0, summary line `PASS: 1534 tests passed` present. Both host-side gate (`test.sh` line ~217) and kernel-side gate (`test_runner.c` `g_quiet`) enforce the suppression.
- [x] `bash scripts/install-hooks.sh` configures `core.hooksPath=.githooks` (`.githooks/pre-commit` + `.githooks/post-commit` activate immediately); opt-in pre-push toggles via `--enable-pre-push` / `--disable-pre-push` sentinel; `--remove` unsets `core.hooksPath`, drops the sentinel, and prunes legacy `.git/hooks/` symlinks -- verified: the `[hook lifecycle]` subshell in `scripts/test-tooling.sh` exercises install + idempotent re-install + `core.hooksPath` assertion + enable-pre-push + sentinel detection + `--status` ENABLED + disable-pre-push + `--remove` in a throwaway git repo (7 named assertions, all PASS). Live spot check on this repo: `install-hooks.sh --status` reports `Hooks path: .githooks (expected: .githooks)` and `Always-on hooks: post-commit pre-commit`.
- [x] `README.md`, `CONTRIBUTING.md`, and `docs/infrastructure/development-tooling.md` no longer advertise stale primary flows such as `make test`, `run-tests.sh`, or ISO-only QEMU paths unless they are explicitly marked legacy -- verified: `scripts/test-tooling.sh` has 21 assertions in the `[stale-command rejection in operator docs]` and `[workflow YAML]` groups that reject `make test` (as primary), `run-tests.sh`, and `build/os-build.iso` across README, CONTRIBUTING, CLAUDE, dev-tooling.md, pages.yml, labeler.yml, stale.yml. All 21 PASS. Negative test committed 2026-04-18 (`ba3122bb`): injecting `` `make test` `` into `README.md` produced the named failure.
- [x] GitHub Actions `build.yml` invokes the documented wrapper flow and uploads the expected artifacts/logs -- verified via static inspection: [`build.yml`](../../.github/workflows/build.yml) lines 93 (`bash scripts/setup.sh --verify`), 102 (`bash scripts/lint.sh`), 109 (`bash scripts/test-tooling.sh --quiet`), 113 (`bash scripts/build.sh clean`), 152 (`bash scripts/test.sh QUIET=1`), 159 + 169 (`actions/upload-artifact@v4`). The wrapper flow is the canonical §3 contract end-to-end.
- [x] `bash scripts/tooling-doctor.sh` reports PASS on a healthy environment and actionable failure output on a broken one -- verified 2026-04-18: `bash scripts/tooling-doctor.sh --quiet` on the current host -> `tooling-doctor: HEALTHY (31 checks, 0 warning(s))`, exit 0. Negative test with `PATH=/tmp/empty-path:/usr/bin` produced `FAIL setup.sh --verify: One or more required tools missing` with an actionable `fix:` line pointing at `bash scripts/setup.sh --verify` / `bash scripts/setup.sh` -- named failure, actionable remediation text, exit non-zero.
- [x] `bash scripts/test-tooling.sh` passes on a healthy tree and fails when a wrapper path or sentinel is intentionally broken -- verified: 393/393 PASS on clean tree (2026-06-16); negative-tested per commit `ba3122bb`.

> **Verified:** 2026-04-18 | 9/9 items | 7 fully verified + 2 partial (WSL2 + CI reality confirmed, fresh-machine install deferred to CI + future machine audits) | tooling-doctor HEALTHY 31/31, test-tooling 81/81, build.sh clean OK, test.sh QUIET=1 0/1534 per-test lines

**Test runner:** `scripts\debug\run-all-tests.bat` (Windows-side wrapper/QEMU smoke path only; host-side tooling assertions live in `bash scripts/test-tooling.sh`)
