# TODO-01 -- Developer Tooling Stack

> **Goal:** Consolidate the repository's host-side developer workflow into one explicit contract: setup, build, run, debug, test, hooks, and GitHub automation all use the same paths, flags, logs, and operator expectations. Today the scripts work, but the rules live across `README.md`, `CLAUDE.md`, ad hoc shell wrappers, and workflow YAML. This TODO turns that into one maintained roadmap so local developer flow, CI, and release-adjacent tooling stop drifting.

> [!IMPORTANT]
> **Current state:** The repo already has a substantial tooling surface: `scripts/setup.sh`, `scripts/build.sh`, `scripts/test.sh`, `scripts/run-qemu.sh`, `scripts/debug.sh`, machine launchers under `scripts/machines/`, a pre-push hook in `scripts/hooks/pre-push`, and GitHub Actions workflows in `.github/workflows/`. The root and infrastructure indexes now point at this TODO, but the actual contract is still split across docs, scripts, workflow YAML, and adjacent consumer TODOs. No section in the roadmap yet owns the build wrapper contract, hook lifecycle, runner/artifact policy, or a host-side regression pack for these scripts.

---

## Inputs

- [`scripts/setup.sh`](../../scripts/setup.sh) -- host bootstrap entry point
- [`scripts/setup-deps.sh`](../../scripts/setup-deps.sh) -- dependency install helper
- [`scripts/build.sh`](../../scripts/build.sh) -- canonical build wrapper
- [`scripts/test.sh`](../../scripts/test.sh) -- canonical test wrapper
- [`scripts/lint.sh`](../../scripts/lint.sh) -- repo lint entry point
- [`scripts/run-qemu.sh`](../../scripts/run-qemu.sh) -- default QEMU launcher
- [`scripts/debug.sh`](../../scripts/debug.sh) -- developer debug entry point
- [`scripts/install-hooks.sh`](../../scripts/install-hooks.sh) -- local git-hook installer
- [`scripts/hooks/pre-push`](../../scripts/hooks/pre-push) -- enforced local pre-push gate
- [`scripts/machines/`](../../scripts/machines/) -- per-hypervisor / per-scenario runners
- [`.github/workflows/build.yml`](../../.github/workflows/build.yml) -- build artifact and sentinel policy
- [`.github/workflows/release.yml`](../../.github/workflows/release.yml) -- release automation boundary
- [`.github/CODEOWNERS`](../../.github/CODEOWNERS) -- ownership and review routing
- [`README.md`](../../README.md) -- public operator-facing setup/build/run docs
- [`CLAUDE.md`](../../CLAUDE.md) -- canonical local workflow rules
- -> XREF: `T04 §3, §4, §11` -- user-mode test launcher, CI-friendly output, and final test wiring extend the same tooling contract
- -> XREF: `T05 §7` -- visual-regression CI consumes the same runner and artifact policy
- -> XREF: `T03 §4` -- repo-wide deferred-test sweep depends on stable local test orchestration
- -> XREF: `D14 T01 §1-§3` -- host SDK/build-system work is complementary, but this TODO owns repo-local developer wrappers
- -> XREF: `D15 T01 §2, §5` -- release packaging and signing consume the tooling contract but remain release-domain owned

## Outcome

- One canonical contract for setup/build/test/run/debug wrappers and their logs, flags, and failure sentinels.
- Hook installation, local automation, and CI workflow behavior documented and owned in one place.
- GitHub Actions workflows aligned with the local script contract instead of duplicating behavior ad hoc.
- Host-side tooling regression checks catch broken wrapper behavior before a developer or reviewer trips over it.
- An operator-facing "tooling doctor" flow gives Impossible OS a cleaner bootstrap and diagnostics story than typical OS hobby repos.

## Implementation Order

| ⭐  | Order | Deliverable                                     | Depends On     | Status |
| --- | :---: | ----------------------------------------------- | -------------- | :----: |
| 💎  |   1   | Host bootstrap and dependency contract          | --             |  [ ]   |
| 💎  |   2   | Build, test, lint, and run wrapper contract     | §1             |  [ ]   |
| 💎  |   3   | Machine launcher and debug profile matrix       | §2             |  [ ]   |
| 💎  |   4   | Git hooks and local automation lifecycle        | §1, §2         |  [ ]   |
| 💎  |   5   | GitHub Actions and artifact policy alignment    | §2, §3, §4     |  [ ]   |
| ⭐  |   6   | Tooling doctor and regression pack              | §1-§5          |  [ ]   |

> 💎 = parity work: Windows and Linux projects both rely on stable setup/build/test/CI contracts.
> ⭐ = exclusive work: Impossible OS can provide a single operator-facing developer workflow with self-diagnosis instead of scattered scripts and tribal knowledge.

---

## 1. Host Bootstrap and Dependency Contract

Make setup reproducible and explicit so a new machine converges on the same toolchain and package set without guesswork.

- [ ] Audit `scripts/setup.sh` and `scripts/setup-deps.sh` into one documented contract: supported distros, required packages, optional packages, and no-op/idempotent reruns
- [ ] Define exact outputs and sentinel checks: which tools must exist after setup (`clang-19`, `ld.lld-19`, `nasm`, `qemu-system-x86_64`, `ovmf`, `mtools`)
- [ ] Add a dedicated docs page under `docs/infrastructure/` for host bootstrap, with `README.md` and `CLAUDE.md` linking to the same canonical instructions
- [ ] Add explicit boundary notes for what stays in `D14 T01 §1-§3` versus this repo-local setup flow
- [ ] Commit: `"docs/tooling: define host bootstrap and dependency contract"`

**Test checkpoint:** On a clean supported Linux machine, `bash scripts/setup.sh` completes and the documented required tools are present on `PATH`. A second run is idempotent and reports no destructive drift.

---

## 2. Build, Test, Lint, and Run Wrapper Contract

The wrappers are the real interface developers use. Their arguments, outputs, sentinels, and log paths must be explicit and stable.

- [ ] Document the canonical entry points and arguments for `scripts/build.sh`, `scripts/test.sh`, `scripts/lint.sh`, `scripts/run-qemu.sh`, and `scripts/debug.sh`
- [ ] Standardize success/failure signals: `build/build.log` sentinel, serial log paths, exit codes, and quiet/summary modes where applicable
- [ ] Define which commands are authoritative in docs and hooks (`bash scripts/build.sh`, `bash scripts/test.sh`, never raw `make` for builds)
- [ ] Add a single tooling reference page listing outputs and artifacts: disk image path, build log path, serial log path, screenshots/diffs if present
- [ ] Ensure wrapper docs mention the existing machine-specific scripts instead of encouraging direct ad hoc QEMU invocations
- [ ] Commit: `"docs/tooling: codify build, test, lint, and run wrapper contract"`

**Test checkpoint:** The documented wrapper commands match reality: `bash scripts/build.sh clean` ends with `=== BUILD OK ===`, `bash scripts/test.sh QUIET=1` produces a summary-only run, and wrapper exit codes are consistent with success/failure.

---

## 3. Machine Launcher and Debug Profile Matrix

Local and CI runs need named machine profiles instead of script archaeology across `scripts/machines/`.

- [ ] Inventory `scripts/machines/` launchers and classify them by platform and purpose: WHPX, TCG, KVM, VirtualBox, storage, filesystem, and secure-boot scenarios
- [ ] Create one canonical matrix document mapping each launcher to intended use, expected accelerator, artifacts, and known limitations
- [ ] Define the boundary between `scripts/run-qemu.sh`, `scripts/debug.sh`, and machine-specific runners so the default path and specialist paths are obvious
- [ ] Add explicit debugger entry points and serial-log expectations for each supported profile
- [ ] Add a bare-metal bring-up row covering disk-image handoff, serial capture path, and expected log/artifact locations so the matrix does not stop at VM-only guidance
- [ ] Mark release-validation VM flows as XREFs to `D15 T04 §2, §4` and installer/provisioning-specific VM flows as XREFs to `D15 T02 §7` instead of duplicating them here
- [ ] Commit: `"docs/tooling: machine launcher and debug profile matrix"`

**Test checkpoint:** A developer can choose the right launcher or bring-up path for WHPX, TCG, VirtualBox, secure-boot, and bare-metal scenarios from one table. Each documented path produces the expected serial/artifact location without ambiguity.

---

## 4. Git Hooks and Local Automation Lifecycle

Hooks are part of the tooling contract, not a hidden convenience script.

- [ ] Document the local hook lifecycle owned by `scripts/install-hooks.sh` and `scripts/hooks/pre-push`: install, remove, update, and expected blocking behavior
- [ ] Decide whether the repo stays on shell-managed hooks or adopts a declarative hook manager layer; if adopting, file the exact migration under this section instead of leaving it implied
- [ ] Define which checks are mandatory pre-push versus optional local quality-of-life checks
- [ ] Add a repo-visible hook reference explaining why push is blocked on failed build/test and where to re-run the same checks manually
- [ ] Wire hook docs into `README.md` / `CONTRIBUTING.md` / `CLAUDE.md` so all three describe the same install path
- [ ] Commit: `"tooling: define git hook lifecycle and local automation policy"`

**Test checkpoint:** `bash scripts/install-hooks.sh` installs the hook idempotently, `--remove` cleans it up, and a failing local build or test blocks `git push` with the documented error path.

---

## 5. GitHub Actions and Artifact Policy Alignment

CI must mirror local tooling, not fork it.

- [ ] Audit `.github/workflows/build.yml`, `release.yml`, `pages.yml`, `labeler.yml`, and `stale.yml` against the local script contract and remove drift in commands, sentinels, and artifact paths
- [ ] Define which workflows are required on PRs, which are release-only, and which are maintenance automation
- [ ] Add explicit artifact/report policy: which logs/images are uploaded, retention periods, and failure-time diagnostics
- [ ] Document when GitHub-hosted runners are sufficient and when a self-hosted runner or manual hardware validation is required
- [ ] Add operator notes for manual dispatches or future matrix expansion without baking undocumented behavior into YAML only
- [ ] Commit: `"ci/tooling: align GitHub Actions with local developer tooling contract"`

**Test checkpoint:** CI workflows invoke the same wrapper scripts as local docs. A failed build uploads the expected logs/artifacts, and a reviewer can map CI behavior directly back to the documented local commands.

---

## 6. Tooling Doctor and Regression Pack

This is the refinement step: make the tooling self-diagnosing instead of forcing contributors to reverse-engineer failures.

> [!TIP]
> Windows and Linux projects usually document setup and CI, but they rarely ship one repo-local "doctor" path that validates toolchain, hook install, runner prerequisites, and wrapper availability in one pass.

- [ ] Add a `scripts/tooling-doctor.sh` entry point that checks required host tools, key script executability, hook install status, and expected workflow files without mutating the repo
- [ ] Add a host-side regression pack (`scripts/test-tooling.sh` or equivalent) covering argument parsing, sentinel paths, hook install/remove idempotence, and CI config sanity checks
- [ ] Make the doctor output actionable: print missing package/tool names, broken script paths, and the exact fix entry point
- [ ] Wire the regression pack into a lightweight CI path so wrapper drift is caught before release or contributor onboarding breaks
- [ ] Link doctor and regression-pack usage from `README.md` and developer docs
- [ ] Commit: `"tooling: add doctor command and regression pack for developer workflow"`

**Test checkpoint:** On a healthy setup, `bash scripts/tooling-doctor.sh` reports PASS across toolchain, hooks, and workflow files. On a missing dependency or broken hook install, it reports the exact failed check and recovery step.

---

## OS Comparison

| ⭐ | Feature                       | 🪟 Win11 projects         | 🐧 Linux projects        | 🚀 Impossible OS         |
| --- | ----------------------------- | ------------------------ | ------------------------ | ------------------------ |
| 💎 | Bootstrap script              | ⚠️ WDK/HLK heavy setup   | ⚠️ Distro docs + scripts | ⬜ §1                    |
| 💎 | Canonical build/test wrappers | ✅ Common in mature repos | ✅ Common in mature repos | ⬜ §2                    |
| 💎 | Named VM/debug profiles       | ⚠️ Often ad hoc          | ⚠️ Often ad hoc          | ⬜ §3                    |
| 💎 | Managed local hooks           | ⚠️ Varies by repo        | ✅ Common in many repos  | ⬜ §4                    |
| 💎 | Workflow/artifact policy      | ✅ Standard CI practice  | ✅ Standard CI practice  | ⬜ §5                    |
| ⭐ | One-command tooling doctor    | ❌ Rare in OS repos      | ❌ Rare in OS repos      | ⬜ §6                    |

> **After §1-§5:** Impossible OS reaches the same baseline as well-run Windows and Linux projects for setup, wrappers, hooks, and CI policy.
> **After §6:** the repo gains a cleaner operator experience than either baseline by shipping a first-class doctor/regression path for its own developer tooling.

## Unit Tests

> [!NOTE]
> Host-side tooling checks live outside `test_runner_init()`. This TODO owns a shell-based regression pack for scripts, hooks, and workflow contract checks, while runtime kernel or user-mode suites remain owned by `T03`, `T04`, and `T05`.

- [ ] Create `scripts/test-tooling.sh` with concrete assertions:
  - `bash scripts/setup.sh --help` exits 0 and points at the documented bootstrap path
  - `bash scripts/build.sh clean` writes `build/build.log`, and `tail -1 build/build.log` equals `=== BUILD OK ===`
  - `bash scripts/test.sh QUIET=1` keeps the summary path while suppressing per-test PASS spam on a healthy tree
  - in a temporary git repo or worktree, `bash scripts/install-hooks.sh` installs `.git/hooks/pre-push`, a second install is idempotent, and `--remove` deletes it cleanly
  - workflow sanity checks grep `build.yml`, `release.yml`, `pages.yml`, `labeler.yml`, and `stale.yml` for the canonical wrapper paths they are supposed to invoke
- [ ] Wire the tooling regression pack into an existing lightweight local/CI path
- [ ] Document that runtime suite coverage still comes from the owning TODOs (`T03 §1-§4`, `T04 §3-§11`, `T05 §3-§8`) so this section does not silently drop test ownership
- [ ] Commit: `"test/tooling: add developer tooling regression pack"`

**Test checkpoint:** `bash scripts/test-tooling.sh` fails with a named assertion when a wrapper path, hook lifecycle, or sentinel drifts, and passes on a healthy repo.

## Verification

- [ ] `bash scripts/setup.sh` on a clean supported machine installs the documented toolchain successfully
- [ ] `bash scripts/build.sh clean` ends with `=== BUILD OK ===`
- [ ] `bash scripts/test.sh QUIET=1` produces the documented summary mode
- [ ] `bash scripts/install-hooks.sh` installs `scripts/hooks/pre-push`; `--remove` removes it cleanly
- [ ] GitHub Actions `build.yml` invokes the documented wrapper flow and uploads the expected artifacts/logs
- [ ] `bash scripts/tooling-doctor.sh` reports PASS on a healthy environment and actionable failure output on a broken one
- [ ] `bash scripts/test-tooling.sh` passes on a healthy tree and fails when a canonical wrapper path or sentinel is intentionally broken

**Test runner:** `scripts\debug\run-all-tests.bat` (Windows-side wrapper/QEMU smoke path only; host-side tooling assertions live in `bash scripts/test-tooling.sh`)
