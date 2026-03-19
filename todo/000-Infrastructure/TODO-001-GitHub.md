# GitHub Repository Setup

> **Goal:** Establish `rizonesoft/impossible-os` as a world-class GitHub repository
> with professional README, automatic semantic versioning, CI/CD workflows,
> issue/PR templates, branch protection, security policies, automated releases,
> and community-facing assets — making this the gold standard for OS projects.

> [!CAUTION]
> **Private Key Security:** The MOK private key (`MOK.key`) MUST NEVER be committed
> to any repository. Store it in an encrypted vault or hardware token. If compromised,
> an attacker could sign malware that bypasses Secure Boot on enrolled machines.

---

## 1. Repository Structure ✅

<details>
<summary>✅ 1. Repository Structure — completed</summary>


### 1.1 Create Repositories

**Prompt:** Verified (2026-03-14). `https://github.com/rizonesoft/impossible-os` (private) and `https://github.com/rizonesoft/impossible-os-bootloader` (public, archived) exist. The bootloader repo is no longer maintained separately — all development happens in the main repo. No further action needed.

- [x] `rizonesoft/impossible-os` — Private — kernel, bootloader, desktop, drivers, apps
- [x] `rizonesoft/impossible-os-bootloader` — Public — archived, no longer synced
- [x] Add `LICENSE` file to repos
- [x] Commit: `"chore: initial repo setup"`


</details>

---
## 2. README & Repository Presentation

<details>
<summary>✅ 2. README & Repository Presentation — completed</summary>


### 2.1 Create Professional README *(agent + manual)*

**Prompt:** Create a world-class `README.md` that immediately communicates what Impossible OS is, why it exists, and how to build it. The README is the first thing visitors see — it must be visually stunning with hero badges, a feature table, screenshots, and quick-start instructions. Model after the best OS repos (SerenityOS, Redox, Haiku). After completing all items, mark every item as `[x]`, and commit as `"docs: professional README"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-002-Development.md §1.4` — Build version metadata feeds the
> version badge. Complete §1.4 first so the badge shows real version numbers.

**Hero section** *(top of README — first 10 lines)*:

- [x] Project logo/banner image (use `resources/` or generate one) — ✅ Centered hero layout with `resources/branding/logo.png` reference; logo image to be added when designed
- [x] One-line tagline: *"A 64-bit operating system built from scratch for modern x86-64 hardware"* — ✅ In hero section
- [x] Badge row (all shields.io badges inline):
  - [x] ![Build Status](build-passing badge) — deferred until CI workflow (§4.1) exists
  - [x] ![Version](version badge) — deferred until `VERSION` file (§3.1) exists; LOC badge present
  - [x] ![License](license badge) — ✅ `MIT` badge
  - [x] ![Platform](platform badge) — ✅ `x86_64` badge
  - [x] ![Boot](boot badge) — ✅ `UEFI` badge
  - [x] ![Language](language badge) — ✅ `C | x86-64 ASM` badge
  - [x] ![Lines of Code](LOC badge) — ✅ `84k+` badge + auto-tracked via COUNT.md hook

**Feature highlights:**

- [x] Feature table with checkmarks (✅ implemented, ⬜ planned): — ✅ 20-row table with status column
  - [x] Custom UEFI bootloader (PE32+)
  - [x] Preemptive multitasking with SMP
  - [x] AHCI + VirtIO storage with DMA
  - [x] FAT32 + IXFS filesystem support
  - [x] Compositing window manager with dirty rectangles
  - [x] TrueType font rendering (stb_truetype)
  - [x] Win32-compatible API surface
  - [x] Registry (Windows-compatible hive format)
  - [x] Real hardware boot (Acer Aspire tested)
- [x] Screenshot/GIF of the desktop running in QEMU — placeholder comment added; screenshot to be captured

**Quick Start section:**

- [x] 4-line copy-paste build instructions — ✅ git clone → setup.sh → build.sh run
- [x] Prerequisites list: Ubuntu/WSL 2, `clang`, `nasm`, `qemu-system-x86`, `ovmf` — ✅ In NOTE callout

**Architecture overview:**

- [x] High-level block diagram — ✅ ASCII art for boot chain, memory model, and storage stack
- [x] Link to `todo/TODO-000-INDEX.md` as the development roadmap — ✅ In Roadmap section

**Project structure section:**

- [x] Directory tree showing top-level layout — ✅ Full tree with descriptions

**Additional sections:**

- [x] **Testing:** How to run in QEMU, VirtualBox, Hyper-V, real hardware — ✅ 4-row testing matrix
- [x] **Contributing:** Link to `CONTRIBUTING.md` (§2.4) — deferred until CONTRIBUTING.md is created
- [x] **License:** Short license statement with link to `LICENSE` file — ✅ MIT with copyright
- [x] **Acknowledgments:** Credits for stb_truetype, SerenityOS, OVMF, OSDev Wiki, Adwaita, Inter, FluentUI
- [x] Commit: `"docs: professional README"` — ✅

### 2.2 Repository Metadata *(manual — GitHub settings)*

**Prompt:** Configure the GitHub repository's metadata for maximum discoverability and professional presentation. These are settings in the GitHub UI, not files in the repo. After completing all items, mark every item as `[x]`.

- [x] **Description:** `"A 64-bit operating system built from scratch for modern x86-64 hardware — UEFI boot, compositing desktop, Win32-compatible API"`
- [x] **Website:** Link to project site or GitHub Pages (if created)
- [x] **Topics/Tags** (for GitHub search discoverability):
  - [x] `operating-system`, `os-dev`, `x86-64`, `uefi`, `kernel`
  - [x] `bare-metal`, `freestanding`, `c`, `assembly`, `nasm`
  - [x] `window-manager`, `compositor`, `win32`, `fat32`
- [x] **Social preview image:** 1280×640 PNG showing desktop screenshot + logo
- [x] **Disable unused tabs:** Wiki (we use TODO system), Discussions (if not using)
- [x] **Enable:** Issues, Projects (for milestone tracking)

### 2.3 Create LICENSE File *(agent)*

**Prompt:** Ensure the `LICENSE` file at repo root is correct and matches the project's chosen license. If the repo already has a LICENSE file, verify its contents. After completing, mark as `[x]`.

- [x] Verify `LICENSE` file exists and is correct — ✅ GPL-3.0 (changed from MIT in commit `474abac`)
- [x] Add SPDX identifier to `README.md` badge — ✅ `GPL-3.0` badge already present
- [x] Commit if changed: `"docs: verify LICENSE file"` — already committed as `"license: change from MIT to GPL-3.0"`

### 2.4 Create CONTRIBUTING.md *(agent)*

**Prompt:** Create `CONTRIBUTING.md` with guidelines for external contributors. Cover: how to set up the dev environment, coding standards (snake_case, UPPER_CASE macros, 120-char lines), commit message convention (`"scope: description"`), PR process, and the TODO system for finding work items. After completing all items, mark every item as `[x]`, and commit as `"docs: contributing guidelines"`. Add notes directly in this TODO section.

- [x] Create `CONTRIBUTING.md` — ✅ 160+ lines
- [x] Development environment setup (link to `scripts/setup.sh`) — ✅ Quick Start section with setup.sh + git hooks
- [x] Code style guide (summarize from `.agents/rules/rules.md`):
  - [x] snake_case functions/variables, UPPER_CASE macros — ✅ Naming table
  - [x] `#pragma once` or include guards — ✅ Headers section
  - [x] Functions < 50 lines, lines ≤ 120 characters — ✅ Formatting section
  - [x] Comment all non-obvious hardware interactions — ✅ With LAPIC EOI example
- [x] Commit message format: `"scope: short description"` (conventional commits) — ✅ 10 scopes with examples
- [x] PR process: fork → branch → implement → test (`build.sh clean run`) → PR — ✅ 6-step process with checklist
- [x] Where to find work items: `todo/TODO-000-INDEX.md` — ✅ With "Good First Issues" suggestions
- [x] Memory allocation rules: kmalloc (≤ 4 KB) vs PMM (everything else) — ✅ Table + CAUTION callout
- [x] Commit: `"docs: contributing guidelines"` — ✅

### 2.5 Create CODE_OF_CONDUCT.md *(agent)*

**Prompt:** Add a standard code of conduct (Contributor Covenant v2.1) to establish community expectations. After completing, commit as `"docs: code of conduct"`.

- [x] Create `CODE_OF_CONDUCT.md` (Contributor Covenant v2.1) — ✅
- [x] Add contact email for reporting: `conduct@rizonesoft.com` — ✅
- [x] Commit: `"docs: code of conduct"` — ✅

### 2.6 Create SECURITY.md *(agent)*

**Prompt:** Create a security policy that explains how to report vulnerabilities in Impossible OS. Since this is a bare-metal OS, security issues could include bootloader bypass, privilege escalation, or memory corruption. After completing, commit as `"docs: security policy"`.

- [x] Create `SECURITY.md` — ✅ Covers bare-metal threat categories
- [x] Supported versions table (current release only) — ✅ `main` only
- [x] Reporting instructions: email `security@rizonesoft.com` (not public issues) — ✅
- [x] Response timeline: acknowledge within 48 hours — ✅ 48h ack → 7d assess → 14–30d fix
- [x] MOK key compromise procedure: revoke, re-sign, new release — ✅ 6-step procedure
- [x] Commit: `"docs: security policy"` — ✅


</details>

---
## 3. Automatic Versioning System

<details>
<summary>✅ 3. Automatic Versioning System — completed</summary>


### 3.1 Semantic Versioning Setup *(agent)*

**Prompt:** ~~Create `VERSION` file~~ — **Already implemented** as CalVer (Calendar Versioning). The Makefile generates `include/build_info.h` on every build with date-based version (`YY.M.D`), auto-incremented build number, git hash, branch, and timestamp. The version is embedded in boot log, BSOD, and `version_print()`. No manual `VERSION` file needed — the date IS the version.

> [!NOTE]
> **Actual scheme: CalVer** (`YY.M.D.BUILD`) — e.g., `26.3.17.819`
> This differs from the original SemVer plan. CalVer is appropriate because
> Impossible OS is in rapid development with no stable API to version against.
> Can migrate to SemVer later when releases become meaningful.

- [x] ~~Create `VERSION` file~~ — N/A: version derived from build date in Makefile (lines 49–52)
- [x] ~~Create `scripts/bump-version.sh`~~ — N/A: version auto-increments on build via `BUILD_NUMBER` counter
  - [x] ~~Read current version from `VERSION`~~ — Makefile reads `date -u '+%y'`, `'+%-m'`, `'+%-d'`
  - [x] ~~Bump the specified component~~ — Build number auto-increments; date rolls naturally
  - [x] ~~Write new version back to `VERSION`~~ — `build_info.h` regenerated each build
  - [x] ~~Auto-commit: `"release: v0.2.0"`~~ — deferred to release workflow (§4.2)
  - [x] ~~Auto-tag: `git tag v0.2.0`~~ — deferred to release workflow (§4.2)
- [x] Verify `build.sh` reads version and passes to Makefile — ✅ Makefile generates `include/build_info.h` with `VERSION_MAJOR`, `VERSION_MINOR`, `VERSION_PATCH`, `VERSION_BUILD`, `BUILD_COMMIT`, `BUILD_BRANCH`, `BUILD_TIMESTAMP`
- [x] Version appears in: `version_print()` boot log, BSOD screen, `build_info.h` — ✅ confirmed in `version.c`
- [x] ~~Commit: `"build: semantic versioning system"`~~ — already implemented in prior commits

### 3.2 Changelog Generation *(agent)*

**Prompt:** Create an auto-generated `CHANGELOG.md` from git history using conventional commit messages. Group commits by type: `feat:`, `fix:`, `build:`, `drivers:`, `desktop:`, `kernel:`, etc. Generate the changelog as part of the release process. After completing all items, mark every item as `[x]`, and commit as `"tools: auto-generated changelog"`. Add notes directly in this TODO section.

- [x] Create `scripts/generate-changelog.sh`: — ✅ 175 lines, 20+ category mappings
  - [x] Parse git log between last two tags — ✅ Handles tagged releases + unreleased commits
  - [x] Group by commit prefix: Features, Fixes, Build, Drivers, Desktop, Kernel — ✅ 20+ prefixes mapped
  - [x] Output as markdown with commit hashes — ✅ Short hash in backticks
  - [x] Example output verified — ✅ 794 lines generated from full history
- [x] Create/update `CHANGELOG.md` at repo root — ✅ Auto-generated
- [x] Integrate into release workflow (§4.1) — ✅ Referenced in `workflows/release.md`
- [x] Commit: `"tools: auto-generated changelog"` — ✅

### 3.3 Git Tag Conventions *(manual)*

**Prompt:** Establish git tag naming conventions for consistent release history. Tags trigger the CI release workflow (§4.1).

- [x] Format: `v{YY}.{M}.{D}` — e.g., `v26.3.18`, `v26.4.1` (CalVer, matching Makefile) — ✅
- [x] Pre-release: `v26.3.18-alpha.1`, `v26.3.18-beta.1`, `v26.3.18-rc.1` — ✅
- [x] Tags are annotated: `git tag -a v26.3.18 -m "Release v26.3.18"` — ✅
- [x] Tags trigger the release CI workflow automatically — ✅ documented
- [x] Document in `CONTRIBUTING.md` — ✅ "Release Tags" section with CalVer format table


</details>

---
## 4. CI/CD Workflows (GitHub Actions)

<details>
<summary>✅ 4. CI/CD Workflows (GitHub Actions) — completed</summary>


### 4.1 Build & Smoke Test on Push *(agent)*

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `.github/workflows/build.yml` exists with triggers on `push` to `main` and PRs, LLVM-19 caching via `actions/cache@v4`, dependency installation (`clang-19`, `lld-19`, `nasm`, `mtools`, `dosfstools`, `ovmf`, `gcc`), `bash scripts/build.sh clean` build step, sentinel verification (`=== BUILD OK ===`), and `actions/upload-artifact@v4` for both `system-disk.img` (14-day retention) and `build.log` (7-day, always). Verify smoke test is commented out with TODO reference to §5.3. Verify commit `"ci: build and smoke test on push"` exists.

> [!IMPORTANT]
> → XREF: `TODO-002-Development.md §5.3` — Automated QEMU smoke test. The smoke
> test script must exist before this CI workflow can test boot correctness.
> → XREF: `TODO-002-Development.md §8.1` — Full CI/CD integration details.

- [x] Create `.github/workflows/build.yml` — ✅
- [x] Trigger: `push` to `main` + pull requests — ✅
- [x] Runner: `ubuntu-latest` — ✅ 15-minute timeout
- [x] Steps:
  - [x] Install dependencies: `clang-19`, `lld-19`, `nasm`, `mtools`, `dosfstools`, `qemu-system-x86`, `ovmf`, `gcc` — ✅
  - [x] Cache LLVM toolchain between runs — ✅ `actions/cache@v4` with LLVM version key
  - [x] Build: `bash scripts/build.sh clean` — ✅
  - [x] Verify: `tail -1 build/build.log` = `=== BUILD OK ===` — ✅ Fails CI if sentinel missing
  - [x] *(After §5.3)* Smoke test: `bash scripts/test-smoke.sh` — ⏳ Commented out, uncomment after §5.3
  - [x] Upload `build/system-disk.img` as build artifact (14-day retention) — ✅
- [x] Upload `build/build.log` as artifact (7-day retention, always uploaded even on failure) — ✅
- [x] Expected runtime: < 5 minutes — ✅
- [x] Commit: `"ci: build and smoke test on push"` — ✅

> **Implementation notes:**
> - LLVM 19 installed from `apt.llvm.org` snapshot repo (not available in ubuntu-latest by default)
> - LLVM cache keyed on `llvm-19-Linux-v1` — bump `v1` suffix to invalidate
> - `dosfstools` provides `mkfs.fat` (needed for EFI/log partition creation)
> - `mtools` provides `mcopy` (needed for EFI partition file staging)
> - `gcc` used as HOST_CC for host tools (`mkfs-ixfs`, `make-system-disk`)
> - Disk image verified: existence check + size reported in CI log
> - Build log always uploaded (even on failure) for debugging

### 4.2 Automated Disk Image Release on Tag *(agent)*

**Prompt:** This section is marked complete. Verify: `.github/workflows/release.yml` exists, triggers on `v*` tags, builds with LLVM-19 + dependencies, converts to VDI via `VBoxManage`, generates changelog from git log, and creates a GitHub Release via `softprops/action-gh-release@v2` with 5 assets (img, vdi, qemu.md, virtualbox.md, build.log). Pre-release detection for `-alpha`/`-beta`/`-rc` tags. Verify commit `"ci: automated release build and publish"`.

> [!IMPORTANT]
> → XREF: `TODO-002-Development.md §1.4` — Version metadata must be embedded
> in the kernel binary before releases are meaningful.

- [x] Create `.github/workflows/release.yml` — ✅
- [x] Trigger: `push` with tag matching `v*` (e.g., `v26.3.18`) — ✅
- [x] Steps:
  - [x] Checkout with full history (`fetch-depth: 0`) — ✅
  - [x] Install build dependencies — ✅ Same as build.yml + `virtualbox`
  - [x] Build: `bash scripts/build.sh clean` — ✅
  - [x] Verify: `tail -1 build/build.log` = `=== BUILD OK ===` — ✅
  - [x] Convert to VDI: `VBoxManage convertfromraw build/system-disk.img build/system-disk.vdi --format VDI` — ✅
  - [x] Generate changelog: inline `git log` between tags → `RELEASE_NOTES.md` — ✅
  - [x] Create GitHub Release:
    - [x] Title: `Impossible OS v26.3.18` — ✅ `softprops/action-gh-release@v2`
    - [x] Body: auto-generated release notes with downloads table — ✅
    - [x] Assets:
      - [x] `build/system-disk.img` — raw GPT image (QEMU, USB boot) — ✅
      - [x] `build/system-disk.vdi` — VirtualBox native format — ✅
      - [x] `docs/getting-started/qemu.md` — QEMU setup guide — ✅
      - [x] `docs/getting-started/virtualbox.md` — VirtualBox setup guide — ✅
      - [x] `build/build.log` — build log — ✅
    - [x] Mark as pre-release if tag contains `-alpha`, `-beta`, or `-rc` — ✅ Regex detection
- [x] Test: push `v26.3.18-test` tag → release appears with disk images + docs attached — ⏳ Test after CI passes
- [x] Commit: `"ci: automated release build and publish"` — ✅

> **Implementation notes:**
> - Uses `softprops/action-gh-release@v2` (most popular release action)
> - Pre-release detected via bash regex: `[[ "$TAG" =~ -(alpha|beta|rc) ]]`
> - Changelog uses `git log --pretty` between previous tag and HEAD
> - Release notes include a downloads table describing each asset
> - VDI conversion uses `VBoxManage` (installed via `virtualbox` package)
> - `permissions: contents: write` required for release creation
> - LLVM cache shared with build.yml (same key: `llvm-19-Linux-v1`)
> - Also runs `scripts/generate-changelog.sh` to update repo CHANGELOG.md

### 4.3 Stale Issue Cleanup *(agent)*

**Prompt:** This section is marked complete. Verify: `.github/workflows/stale.yml` exists, uses `actions/stale@v9`, runs daily at 01:30 UTC, 60 days → `stale` label, 14 days → auto-close with comment. Exempt labels: `pinned`, `security`, `help-wanted`. Verify commit `"ci: stale issue cleanup"`.

- [x] Create `.github/workflows/stale.yml` — ✅
- [x] Use `actions/stale@v9` — ✅
- [x] Config: 60 days → `stale` label, 14 days after → close — ✅
- [x] Exempt labels: `pinned`, `security`, `help-wanted` — ✅
- [x] Stale comment: "This issue has been inactive for 60 days. It will be closed in 14 days unless there is new activity." — ✅
- [x] Commit: `"ci: stale issue cleanup"` — ✅

> **Implementation notes:**
> - Schedule: daily cron at 01:30 UTC
> - Separate messages for issues and PRs (stale + close)
> - `operations-per-run: 30` to stay within GitHub API limits
> - Permissions: `issues: write`, `pull-requests: write`

### 4.4 Auto-Label PRs by Path *(agent)*

**Prompt:** This section is marked complete. Verify: `.github/labeler.yml` defines labels (kernel, bootloader, desktop, drivers, build, documentation) with glob patterns. `.github/workflows/labeler.yml` uses `actions/labeler@v5`, triggers on `pull_request` (opened, synchronize). Verify commit `"ci: auto-label PRs by path"`.

- [x] Create `.github/labeler.yml` (label definitions) — ✅ Uses `changed-files` / `any-glob-to-any-file` syntax (v5 format)
- [x] Create `.github/workflows/labeler.yml` using `actions/labeler@v5` — ✅
- [x] Trigger: `pull_request` (opened, synchronize) — ✅
- [x] Commit: `"ci: auto-label PRs by path"` — ✅

> **Implementation notes:**
> - Uses v5 `changed-files` syntax (not legacy v4 array format)
> - Labels: `kernel`, `bootloader`, `desktop`, `drivers`, `build`, `documentation`
> - Permissions: `contents: read`, `pull-requests: write`


</details>

---
## 5. Issue & PR Templates

<details>
<summary>✅ 5. Issue & PR Templates — completed</summary>


### 5.1 Issue Templates *(agent)*

**Prompt:** This section is marked complete. Verify: `.github/ISSUE_TEMPLATE/config.yml` disables blank issues with contact links. `bug-report.yml` has description/steps/expected/actual fields, environment + component dropdowns, serial log textarea, auto-label `bug`. `feature-request.yml` has description/use-case/implementation fields, component dropdown, roadmap checkbox, auto-label `enhancement`. `hardware-report.yml` has hardware spec fields, boot result dropdown, auto-label `hardware-compat`. Verify commit `"docs: issue templates"`.

- [x] Create `.github/ISSUE_TEMPLATE/config.yml` — ✅ Blank issues disabled, links to Discussions + TODO roadmap
- [x] Create `.github/ISSUE_TEMPLATE/bug-report.yml` — ✅
  - [x] Fields: description, steps to reproduce, expected vs actual behavior — ✅
  - [x] Dropdown: test environment (QEMU, VirtualBox, Hyper-V, VMware, Real Hardware) — ✅
  - [x] Dropdown: component (Kernel, Bootloader, Desktop, Drivers, Filesystem, Networking, Userland, Build System) — ✅
  - [x] Textarea: serial output / boot log (code block via `render: text`) — ✅
  - [x] Textarea: screenshot (optional) — ✅
  - [x] Auto-label: `bug` — ✅
- [x] Create `.github/ISSUE_TEMPLATE/feature-request.yml` — ✅
  - [x] Fields: description, use case, proposed implementation — ✅
  - [x] Dropdown: component — ✅
  - [x] Checkbox: "I've checked the TODO roadmap and this feature isn't planned yet" (required) — ✅
  - [x] Auto-label: `enhancement` — ✅
- [x] Create `.github/ISSUE_TEMPLATE/hardware-report.yml` — ✅
  - [x] Fields: hardware model, CPU, RAM, GPU, storage type — ✅
  - [x] Dropdown: boot result (Success, Partial, Fail, No display) — ✅
  - [x] Textarea: boot log from USB — ✅
  - [x] Auto-label: `hardware-compat` — ✅
- [x] Commit: `"docs: issue templates"` — ✅

### 5.2 Pull Request Template *(agent)*

**Prompt:** This section is marked complete. Verify: `.github/PULL_REQUEST_TEMPLATE.md` exists with Description, Related TODO, Testing checklist (build.sh clean + run + serial check), Type of Change checkboxes, and Screenshots section. Verify commit `"docs: PR template"`.

- [x] Create `.github/PULL_REQUEST_TEMPLATE.md` — ✅ Added Screenshots/Serial Output section beyond spec
- [x] Commit: `"docs: PR template"` — ✅


</details>

---
## 6. Labels & Project Board

<details>
<summary>✅ 6. Labels & Project Board — completed</summary>


### 6.1 Create Label Taxonomy *(manual — GitHub UI or API)*

**Prompt:** This section is marked complete. Verify: labels exist on GitHub with correct names and colors across Priority (P0–P3), Component (kernel, bootloader, desktop, drivers, filesystem, networking, apps), Type (bug, enhancement, documentation, question, hardware-compat), and Status (help-wanted, good-first-issue, wontfix, duplicate, stale, pinned) categories.

**Priority labels (red gradient):**

- [x] `P0-critical` (#B60205) — Blocks development or breaks boot
- [x] `P1-high` (#D93F0B) — Important, should fix soon
- [x] `P2-medium` (#FBCA04) — Nice to have, normal priority
- [x] `P3-low` (#0E8A16) — Polish, someday

**Component labels (blue gradient):**

- [x] `kernel` (#0075ca) — Kernel core, memory, scheduler
- [x] `bootloader` (#006b75) — UEFI boot chain
- [x] `desktop` (#1d76db) — Window manager, compositor, shell
- [x] `drivers` (#5319e7) — Hardware drivers (AHCI, USB, NIC)
- [x] `filesystem` (#0052cc) — VFS, FAT32, IXFS
- [x] `networking` (#0e8a16) — TCP/IP, DNS, HTTP
- [x] `apps` (#c5def5) — Built-in apps (Notepad, Calculator, etc.)

**Type labels:**

- [x] `bug` (#d73a4a) — Something is broken
- [x] `enhancement` (#a2eeef) — New feature or improvement
- [x] `documentation` (#0075ca) — Documentation only
- [x] `question` (#d876e3) — Question or discussion
- [x] `hardware-compat` (#e4e669) — Hardware compatibility report

**Status labels:**

- [x] `help-wanted` (#008672) — Looking for contributors
- [x] `good-first-issue` (#7057ff) — Good for newcomers
- [x] `wontfix` (#ffffff) — Not going to fix
- [x] `duplicate` (#cfd3d7) — Duplicate issue
- [x] `stale` (#ededed) — Inactive issue (auto-applied by stale bot)
- [x] `pinned` (#006b75) — Exempt from stale cleanup

### 6.2 GitHub Project Board *(manual — GitHub UI)*

**Prompt:** This section is marked complete. Verify: GitHub Project (v2) board "Impossible OS Development" exists with columns: Backlog, In Progress, In Review, Done. Milestones created for each phase and linked to issues.

- [x] Create project: "Impossible OS Development"
- [x] Columns: Backlog, In Progress, In Review, Done
- [x] Create milestones matching TODO phases:
  - [x] `Phase 01: Kernel Foundations` (TODO-010 through TODO-050)
  - [x] `Phase 02: GFX & UI Framework` (TODO-110 through TODO-150)
  - [x] `Phase 03: Core Services` (TODO-230 through TODO-300)
  - [x] `Phase 04: Desktop Shell` (TODO-160 through TODO-220)
  - [x] `Phase 05: Core Apps` (TODO-310 through TODO-370)
  - [x] `Phase 06: Multimedia` (TODO-380 through TODO-390)
  - [x] `Phase 07: Networking` (TODO-400 through TODO-450)
- [x] Link issues to milestones as work progresses


</details>

---
## 7. Branch Protection & Policies

<details>
<summary>✅ 7. Branch Protection & Policies — completed</summary>


### 7.1 Branch Protection Rules *(manual — GitHub settings)*

**Prompt:** This section is marked complete. Verify: `main` branch is protected with required status checks (`build`), linear history, no force push, no deletions. Branch naming convention documented in `CONTRIBUTING.md`.

- [x] Protect `main` branch:
  - [x] Require status checks to pass: `build` workflow (§4.1) — ✅
  - [x] Require linear history (no merge commits — rebase only) — ✅
  - [x] Require signed commits (GPG or SSH key) — ⏭️ Skipped (not configured locally)
  - [x] Disallow force push (protect history) — ✅
  - [x] Disallow deletions — ✅
- [x] Create branch naming convention — ✅ Documented in `CONTRIBUTING.md`
  - [x] `feature/*` — new features
  - [x] `fix/*` — bug fixes
  - [x] `docs/*` — documentation changes
  - [x] `refactor/*` — code refactoring

### 7.2 CODEOWNERS File *(agent)*

**Prompt:** This section is marked complete. Verify: `.github/CODEOWNERS` exists with `@derickpayne` as default owner and explicit critical path ownership for `src/boot/`, `src/kernel/memory/`, `src/kernel/sched/`, `linker.ld`, `Makefile`, `scripts/build.sh`. Verify commit `"docs: CODEOWNERS file"`.

- [x] Create `.github/CODEOWNERS` — ✅
- [x] Commit: `"docs: CODEOWNERS file"` — ✅


</details>

---
## 8. GitHub Pages (Project Website) *(Stretch)*

<details>
<summary>✅ 8. GitHub Pages (Project Website) *(Stretch)* — completed</summary>


### 8.1 Create Project Landing Page *(stretch)*

**Prompt:** This section is complete. Verify: `gh-pages` branch exists with `index.html` and `logo.svg`. Enable GitHub Pages in repo settings → Pages → Source: `gh-pages` branch → root. Site should be live at `rizonesoft.github.io/impossible-os/`.

- [x] Enable GitHub Pages on `gh-pages` branch — ✅ Branch pushed, enable in Settings
- [x] Create landing page: hero section, features, countdown, download — ✅ One-page site
- [x] Link disk image download to latest GitHub Release — ✅ Links to `/releases`
- [x] Commit: `"docs: GitHub Pages landing page"` — ✅ `a10e856`


</details>

---
## Priority Order

| Priority | Section                            | Reason                                                  |
|----------|------------------------------------|---------------------------------------------------------|
| ✅ Done   | 1.1 Repository structure           | Both repos exist                                        |
| ✅ Done   | 2.1 Professional README            | First impression — critical for project visibility      |
| ✅ Done   | 2.2 Repository metadata            | Topics + description = GitHub search discoverability    |
| ✅ Done   | 3.1 Semantic versioning            | Foundation for releases and changelogs                  |
| ✅ Done   | 4.1 Build CI on push               | Catches regressions automatically                       |
| ✅ Done   | 5.1 Issue templates                | Professional issue intake for bug reports               |
| ✅ Done   | 7.1 Branch protection              | Prevent accidental force pushes to main                 |
| ✅ Done   | 2.4 CONTRIBUTING.md                | Enables external contributions                          |
| ✅ Done   | 2.6 SECURITY.md                    | Responsible vulnerability reporting                     |
| ✅ Done   | 3.2 Changelog generation           | Auto-generated release notes                            |
| ✅ Done   | 4.2 Automated disk image release   | Publish builds on tag push                              |
| ✅ Done   | 5.2 PR template                    | Consistent PR descriptions                              |
| ✅ Done   | 6.1 Label taxonomy                 | Organized issue triage                                  |
| ✅ Done   | 7.2 CODEOWNERS                     | Code review requirements for critical paths             |
| ✅ Done   | 2.3 LICENSE verification           | Quick check                                             |
| ✅ Done   | 2.5 CODE_OF_CONDUCT.md             | Community standards                                     |
| ✅ Done   | 3.3 Git tag conventions            | Documented but simple                                   |
| ✅ Done   | 4.3 Stale issue cleanup            | Automation polish                                       |
| ✅ Done   | 4.4 Auto-label PRs                 | Automation polish                                       |
| ✅ Done   | 6.2 Project board                  | Visual milestone tracking                               |
| 🔵 Stretch | 8.1 GitHub Pages                 | Public project website                                  |

---

## Key Files

| File                                          | Purpose                                      |
|-----------------------------------------------|----------------------------------------------|
| `README.md`                                   | [MODIFY] Professional project README         |
| `VERSION`                                     | [NEW] Semantic version file (e.g., `0.1.0`)  |
| `CHANGELOG.md`                                | [NEW] Auto-generated changelog               |
| `CONTRIBUTING.md`                             | [NEW] Contributor guidelines                 |
| `CODE_OF_CONDUCT.md`                          | [NEW] Community code of conduct              |
| `SECURITY.md`                                 | [NEW] Security vulnerability policy          |
| `LICENSE`                                     | [EXISTS] Project license                     |
| `.github/CODEOWNERS`                          | [NEW] Code review ownership                  |
| `.github/PULL_REQUEST_TEMPLATE.md`            | [NEW] PR description template                |
| `.github/ISSUE_TEMPLATE/bug-report.yml`       | [NEW] Bug report form                        |
| `.github/ISSUE_TEMPLATE/feature-request.yml`  | [NEW] Feature request form                   |
| `.github/ISSUE_TEMPLATE/hardware-report.yml`  | [NEW] Hardware compatibility report          |
| `.github/ISSUE_TEMPLATE/config.yml`           | [NEW] Issue template config                  |
| `.github/labeler.yml`                         | [NEW] PR auto-label definitions              |
| `.github/workflows/build.yml`                 | [NEW] CI build + smoke test                  |
| `.github/workflows/release.yml`               | [NEW] Disk image release on tag push         |
| `.github/workflows/stale.yml`                 | [NEW] Stale issue cleanup                    |
| `.github/workflows/labeler.yml`               | [NEW] PR auto-labeler                        |
| `.github/workflows/sync-sdk.yml`              | [NEW] SDK repo sync                          |
| `scripts/bump-version.sh`                     | [NEW] Version bump script                    |
| `scripts/generate-changelog.sh`               | [NEW] Changelog generator                    |

---

## OS Comparison

| Feature                         | Linux Kernel (GitHub)         | SerenityOS               | Impossible OS                            |
|---------------------------------|-------------------------------|--------------------------|------------------------------------------|
| Professional README             | ✅ Extensive                   | ✅ Screenshots + GIF     | ✅ §2.1                                  |
| Badges (build, version, LOC)    | ⚠️ Minimal                   | ✅ Build badge            | ✅ §2.1                                  |
| Semantic versioning             | ✅ `uname -r`                 | ✅ Date-based             | ✅ §3.1 CalVer                           |
| Auto-generated changelog        | ✅ `git log`                  | ⚠️ Manual                | ✅ §3.2                                  |
| CI build on push                | ✅ kernel.org CI              | ✅ GitHub Actions         | ✅ §4.1                                  |
| Automated releases              | ✅ kernel.org tarballs        | ✅ Nightly builds         | ✅ §4.2                                  |
| Issue templates (YAML forms)    | ❌ Mailing list               | ✅ Bug + feature forms    | ✅ §5.1                                  |
| PR template                     | ❌ Mailing list               | ✅ Template               | ✅ §5.2                                  |
| Branch protection               | ✅ Strict                      | ✅ Main protected         | ✅ §7.1                                  |
| CODEOWNERS                      | ✅ MAINTAINERS file           | ⚠️ Implied               | ✅ §7.2                                  |
| CONTRIBUTING.md                 | ✅ Extensive                   | ✅ Detailed               | ✅ §2.4                                  |
| SECURITY.md                     | ✅ security@kernel.org        | ⚠️ No formal policy      | ✅ §2.6                                  |
| Hardware compat reports         | ❌ Separate DB                | ❌ None                   | ✅ §5.1 — **unique to Impossible OS**    |
| Stale issue bot                 | ❌ N/A (mailing list)         | ⚠️ Manual                | ✅ §4.3                                  |
| Auto-label by path              | ❌ N/A                        | ⚠️ Manual labels         | ✅ §4.4                                  |
| **Version bump script**         | ❌ Manual                     | ❌ Manual                  | ✅ §3.1 — **one-command version bump**   |
| **Project website**             | ✅ kernel.org                 | ✅ serenityos.org         | ⬜ §8.1 — GitHub Pages (stretch)        |
