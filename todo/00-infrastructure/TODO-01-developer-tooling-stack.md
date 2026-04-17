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
- [`scripts/install-hooks.sh`](../../scripts/install-hooks.sh) -- local git-hook installer
- [`scripts/hooks/pre-push`](../../scripts/hooks/pre-push) -- enforced local pre-push gate
- [`.githooks/`](../../.githooks/) -- repo-tracked git hooks that currently coexist with the install-hooks path
- [`scripts/machines/`](../../scripts/machines/) -- per-hypervisor / per-scenario runners
- [`scripts/test-smoke.sh`](../../scripts/test-smoke.sh) -- legacy smoke flow still referenced in docs and needs an explicit keep/retire decision
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
| 💎  |   3   | Build, test, lint, and run wrapper contract           | §1, §2         |  [ ]   |
| 💎  |   4   | Machine launcher and debug profile matrix             | §2, §3         |  [ ]   |
| 💎  |   5   | Git hooks and local automation lifecycle              | §1, §3         |  [ ]   |
| 💎  |   6   | GitHub Actions and artifact policy alignment          | §2, §3, §4, §5 |  [ ]   |
| ⭐  |   7   | Tooling doctor and regression pack                    | §1-§6          |  [ ]   |

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

- [ ] Document the canonical entry points and arguments for `scripts/build.sh`, `scripts/test.sh`, `scripts/lint.sh`, `scripts/run-qemu.sh`, and `scripts/debug.sh`
- [ ] Standardize success/failure signals: `build/build.log` sentinel, serial log paths, exit codes, and quiet/summary modes where applicable
- [ ] Define which commands are authoritative in docs and hooks (`bash scripts/build.sh`, `bash scripts/test.sh`, never raw `make` for builds)
- [ ] Add a single tooling reference page listing outputs and artifacts: disk image path, build log path, serial log path, screenshots/diffs if present
- [ ] Audit repo-facing wrapper surfaces and fix or retire stale entry points that still reference `build/os-build.iso`, `make all`, `run-tests.sh`, or `make test` as the primary flow (`scripts/run-qemu.sh`, `scripts/test-smoke.sh`, `docs/infrastructure/development-tooling.md`, `CONTRIBUTING.md`)
- [ ] Ensure wrapper docs mention the existing machine-specific scripts instead of encouraging direct ad hoc QEMU invocations
- [ ] Commit: `"docs/tooling: codify build, test, lint, and run wrapper contract"`

**Test checkpoint:** The documented wrapper commands match reality: `bash scripts/build.sh clean` ends with `=== BUILD OK ===`, `bash scripts/test.sh QUIET=1` produces a summary-only run, and wrapper exit codes are consistent with success/failure.

---

## 4. Machine Launcher and Debug Profile Matrix

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

## 5. Git Hooks and Local Automation Lifecycle

Hooks are part of the tooling contract, not a hidden convenience script.

- [ ] Document the local hook lifecycle owned by `scripts/install-hooks.sh` and `scripts/hooks/pre-push`: install, remove, update, and expected blocking behavior
- [ ] Reconcile repo-tracked `.githooks/` with `scripts/install-hooks.sh`: document whether both remain, which hook families use `core.hooksPath`, and which path is canonical for contributors
- [ ] Decide whether the repo stays on shell-managed hooks or adopts a declarative hook manager layer; if adopting, file the exact migration under this section instead of leaving it implied
- [ ] Define which checks are mandatory pre-push versus optional local quality-of-life checks
- [ ] Add a repo-visible hook reference explaining why push is blocked on failed build/test and where to re-run the same checks manually
- [ ] Wire hook docs into `README.md` / `CONTRIBUTING.md` / `CLAUDE.md` so all three describe the same install path
- [ ] Commit: `"tooling: define git hook lifecycle and local automation policy"`

**Test checkpoint:** `bash scripts/install-hooks.sh` installs the hook idempotently, `--remove` cleans it up, and a failing local build or test blocks `git push` with the documented error path.

---

## 6. GitHub Actions and Artifact Policy Alignment

CI must mirror local tooling, not fork it.

- [ ] Audit `.github/workflows/build.yml`, `release.yml`, `pages.yml`, `labeler.yml`, and `stale.yml` against the local script contract and remove drift in commands, sentinels, and artifact paths
- [ ] Define which workflows are required on PRs, which are release-only, and which are maintenance automation
- [ ] Add explicit artifact/report policy: which logs/images are uploaded, retention periods, and failure-time diagnostics
- [ ] Document when GitHub-hosted runners are sufficient and when a self-hosted runner or manual hardware validation is required
- [ ] Add operator notes for manual dispatches or future matrix expansion without baking undocumented behavior into YAML only
- [ ] Commit: `"ci/tooling: align GitHub Actions with local developer tooling contract"`

**Test checkpoint:** CI workflows invoke the same wrapper scripts as local docs. A failed build uploads the expected logs/artifacts, and a reviewer can map CI behavior directly back to the documented local commands.

---

## 7. Tooling Doctor and Regression Pack

This is the refinement step: make the tooling self-diagnosing instead of forcing contributors to reverse-engineer failures.

> [!TIP]
> Windows and Linux projects usually document setup and CI, but they rarely ship one repo-local "doctor" path that validates toolchain, hook install, runner prerequisites, and wrapper availability in one pass.

- [ ] Add a `scripts/tooling-doctor.sh` entry point that checks required host tools, key script executability, hook install status, and expected workflow files without mutating the repo
- [ ] Add a host-side regression pack (`scripts/test-tooling.sh` or equivalent) covering argument parsing, sentinel paths, hook install/remove idempotence, and CI config sanity checks
- [ ] Extend doctor coverage to runtime prerequisites named in §2 and §4: OVMF presence, `/dev/kvm` availability, `qemu-img`, `VBoxManage`/PowerShell where relevant, and whether the current host matches a supported profile or only a best-effort one
- [ ] Add a machine-readable doctor mode (`--json` or equivalent report file) so lightweight CI and bug reports can consume the same diagnostics without screen-scraping prose
- [ ] Make the doctor output actionable: print missing package/tool names, broken script paths, and the exact fix entry point
- [ ] Wire the regression pack into a lightweight CI path so wrapper drift is caught before release or contributor onboarding breaks
- [ ] Link doctor and regression-pack usage from `README.md` and developer docs
- [ ] Commit: `"tooling: add doctor command and regression pack for developer workflow"`

**Test checkpoint:** On a healthy setup, `bash scripts/tooling-doctor.sh` reports PASS across toolchain, hooks, and workflow files. On a missing dependency or broken hook install, it reports the exact failed check and recovery step.

---

## OS Comparison

| ⭐ | Feature                       | 🪟 Win11 projects         | 🐧 Linux projects          | 🚀 Impossible OS         |
| --- | ----------------------------- | ------------------------- | -------------------------- | ------------------------- |
| 💎 | Bootstrap script              | ⚠️ WDK/HLK heavy setup    | ⚠️ Distro docs + scripts   | ✅ §1 -- setup.sh + --verify sentinel |
| 💎 | Reproducible host profiles    | ⚠️ EWDK/Dev Box or VMs    | ✅ Devcontainers common    | ✅ §2 matrix + .devcontainer + shim procedure |
| 💎 | Canonical build/test wrappers | ✅ Common in mature repos | ✅ Common in mature repos  | ⬜ §3                    |
| 💎 | Named VM/debug profiles       | ⚠️ Often ad hoc           | ⚠️ Often ad hoc            | ⬜ §4                    |
| 💎 | Managed local hooks           | ⚠️ Varies by repo         | ✅ Common in many repos    | ⬜ §5                    |
| 💎 | Workflow/artifact policy      | ✅ Standard CI practice   | ✅ Standard CI practice    | ⬜ §6                    |
| ⭐ | One-command tooling doctor    | ❌ Rare in OS repos       | ❌ Rare in OS repos        | ⬜ §7                    |

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
  - in a temporary git repo or worktree, `bash scripts/install-hooks.sh` installs `.git/hooks/pre-push`, a second install is idempotent, and `--remove` deletes it cleanly
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
- [ ] `bash scripts/install-hooks.sh` installs `scripts/hooks/pre-push`; `--remove` removes it cleanly
- [ ] `README.md`, `CONTRIBUTING.md`, and `docs/infrastructure/development-tooling.md` no longer advertise stale primary flows such as `make test`, `run-tests.sh`, or ISO-only QEMU paths unless they are explicitly marked legacy
- [ ] GitHub Actions `build.yml` invokes the documented wrapper flow and uploads the expected artifacts/logs
- [ ] `bash scripts/tooling-doctor.sh` reports PASS on a healthy environment and actionable failure output on a broken one
- [ ] `bash scripts/test-tooling.sh` passes on a healthy tree and fails when a canonical wrapper path or sentinel is intentionally broken

**Test runner:** `scripts\debug\run-all-tests.bat` (Windows-side wrapper/QEMU smoke path only; host-side tooling assertions live in `bash scripts/test-tooling.sh`)
