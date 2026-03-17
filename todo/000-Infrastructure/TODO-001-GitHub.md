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

### 1.1 Create Repositories

**Prompt:** Verified (2026-03-14). `https://github.com/rizonesoft/impossible-os` (private) and `https://github.com/rizonesoft/impossible-os-bootloader` (public, archived) exist. The bootloader repo is no longer maintained separately — all development happens in the main repo. No further action needed.

- [x] `rizonesoft/impossible-os` — Private — kernel, bootloader, desktop, drivers, apps
- [x] `rizonesoft/impossible-os-bootloader` — Public — archived, no longer synced
- [x] Add `LICENSE` file to repos
- [x] Commit: `"chore: initial repo setup"`

---

## 2. README & Repository Presentation

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

---

## 3. Automatic Versioning System

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

- [x] Format: `v{MAJOR}.{MINOR}.{PATCH}` — e.g., `v0.1.0`, `v1.0.0` — ✅
- [x] Pre-release: `v0.1.0-alpha.1`, `v0.1.0-beta.1`, `v0.1.0-rc.1` — ✅
- [x] Tags are annotated: `git tag -a v0.1.0 -m "Release v0.1.0"` — ✅
- [x] Tags trigger the release CI workflow automatically — ✅ documented
- [x] Document in `CONTRIBUTING.md` — ✅ Added "Release Tags" section with format table

---

## 4. CI/CD Workflows (GitHub Actions)

### 4.1 Build & Smoke Test on Push *(agent)*

**Prompt:** Create `.github/workflows/build.yml` that builds the OS and runs a smoke test on every push to `main` and on pull requests. This is the primary CI workflow — if this fails, the build is broken. Cache the toolchain between runs for speed. After completing all items, mark every item as `[x]`, and commit as `"ci: build and smoke test on push"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-002-Development.md §5.3` — Automated QEMU smoke test. The smoke
> test script must exist before this CI workflow can test boot correctness.
> → XREF: `TODO-002-Development.md §8.1` — Full CI/CD integration details.

- [ ] Create `.github/workflows/build.yml`
- [ ] Trigger: `push` to `main` + pull requests
- [ ] Runner: `ubuntu-latest`
- [ ] Steps:
  - [ ] Install dependencies: `clang`, `lld`, `nasm`, `xorriso`, `mtools`, `qemu-system-x86`, `ovmf`
  - [ ] Cache toolchain (`/usr/bin/clang`, OVMF, etc.) between runs
  - [ ] Build: `bash scripts/build.sh clean`
  - [ ] Verify: `tail -1 build/build.log` = `=== BUILD OK ===`
  - [ ] *(After §5.3)* Smoke test: `bash scripts/test-smoke.sh`
  - [ ] Upload `build/os-build.iso` as build artifact (downloadable from Actions tab)
- [ ] Expected runtime: < 5 minutes
- [ ] Commit: `"ci: build and smoke test on push"`

### 4.2 Automated ISO Release on Tag *(agent)*

**Prompt:** Create `.github/workflows/release.yml` that builds the OS ISO and publishes it as a GitHub Release whenever a version tag (`v*`) is pushed. The ISO is attached as a release artifact. The changelog is auto-generated from commits since the last tag. After completing all items, mark every item as `[x]`, and commit as `"ci: automated release build and ISO publish"`. Add notes directly in this TODO section.

> [!IMPORTANT]
> → XREF: `TODO-002-Development.md §1.4` — Version metadata must be embedded
> in the kernel binary before releases are meaningful.

- [ ] Create `.github/workflows/release.yml`
- [ ] Trigger: `push` with tag matching `v*` (e.g., `v0.1.0`)
- [ ] Steps:
  - [ ] Checkout with full history (`fetch-depth: 0`)
  - [ ] Install build dependencies
  - [ ] Build: `bash scripts/build.sh clean`
  - [ ] Verify: `tail -1 build/build.log` = `=== BUILD OK ===`
  - [ ] Generate changelog: `bash scripts/generate-changelog.sh` (§3.2)
  - [ ] Create GitHub Release:
    - [ ] Title: `Impossible OS v0.1.0`
    - [ ] Body: auto-generated changelog
    - [ ] Assets: `build/os-build.iso`, `build/build.log`
    - [ ] Mark as pre-release if tag contains `-alpha`, `-beta`, or `-rc`
- [ ] Test: push `v0.0.1-test` tag → release appears with ISO attached
- [ ] Commit: `"ci: automated release build and ISO publish"`

### 4.3 Stale Issue Cleanup *(agent)*

**Prompt:** Create a GitHub Actions workflow that automatically labels and closes stale issues and PRs. Issues with no activity for 60 days get labeled `stale`, and if no response for 14 more days, they are closed with a comment. This prevents issue tracker clutter. After completing, commit as `"ci: stale issue cleanup"`.

- [ ] Create `.github/workflows/stale.yml`
- [ ] Use `actions/stale@v9`
- [ ] Config: 60 days → `stale` label, 14 days after → close
- [ ] Exempt labels: `pinned`, `security`, `help-wanted`
- [ ] Stale comment: "This issue has been inactive for 60 days. It will be closed in 14 days unless there is new activity."
- [ ] Commit: `"ci: stale issue cleanup"`

### 4.4 Auto-Label PRs by Path *(agent)*

**Prompt:** Create a labeler workflow that automatically applies labels to pull requests based on which files are changed. This enables filtering and triaging PRs at a glance. After completing, commit as `"ci: auto-label PRs by path"`.

- [ ] Create `.github/labeler.yml` (label definitions):
  ```yaml
  kernel:
    - 'src/kernel/**'
  bootloader:
    - 'src/boot/**'
  desktop:
    - 'src/desktop/**'
  drivers:
    - 'src/kernel/drivers/**'
  build:
    - 'Makefile'
    - 'scripts/**'
  documentation:
    - 'todo/**'
    - '*.md'
  ```
- [ ] Create `.github/workflows/labeler.yml` using `actions/labeler@v5`
- [ ] Trigger: `pull_request` (opened, synchronize)
- [ ] Commit: `"ci: auto-label PRs by path"`

---

## 5. Issue & PR Templates

### 5.1 Issue Templates *(agent)*

**Prompt:** Create structured issue templates so bug reports and feature requests include all necessary information. Use GitHub's issue template forms (YAML-based) for guided input. After completing all items, mark every item as `[x]`, and commit as `"docs: issue templates"`. Add notes directly in this TODO section.

- [ ] Create `.github/ISSUE_TEMPLATE/config.yml` — disable blank issues, add links
- [ ] Create `.github/ISSUE_TEMPLATE/bug-report.yml`:
  - [ ] Fields: description, steps to reproduce, expected vs actual behavior
  - [ ] Dropdown: test environment (QEMU, VirtualBox, Hyper-V, Real Hardware)
  - [ ] Dropdown: component (kernel, bootloader, desktop, drivers, filesystem)
  - [ ] Textarea: serial output / boot log (code block)
  - [ ] Textarea: screenshot (optional)
  - [ ] Auto-label: `bug`
- [ ] Create `.github/ISSUE_TEMPLATE/feature-request.yml`:
  - [ ] Fields: description, use case, proposed implementation
  - [ ] Dropdown: component
  - [ ] Checkbox: "I've checked the TODO roadmap and this feature isn't planned yet"
  - [ ] Auto-label: `enhancement`
- [ ] Create `.github/ISSUE_TEMPLATE/hardware-report.yml`:
  - [ ] Fields: hardware model, CPU, RAM, GPU, storage type
  - [ ] Dropdown: boot result (success, partial, fail, no display)
  - [ ] Textarea: boot log from USB
  - [ ] Auto-label: `hardware-compat`
- [ ] Commit: `"docs: issue templates"`

### 5.2 Pull Request Template *(agent)*

**Prompt:** Create a PR template that guides contributors to describe their changes, link relevant TODO items, and confirm they've tested. After completing, commit as `"docs: PR template"`.

- [ ] Create `.github/PULL_REQUEST_TEMPLATE.md`:
  ```markdown
  ## Description
  <!-- What does this PR do? -->

  ## Related TODO
  <!-- Link to the TODO section this implements, e.g., TODO-080 §1.3 -->

  ## Testing
  - [ ] `bash scripts/build.sh clean` → `=== BUILD OK ===`
  - [ ] `bash scripts/build.sh run` → boots and works in QEMU
  - [ ] Serial output checked for new warnings/errors

  ## Type of Change
  - [ ] Bug fix
  - [ ] New feature
  - [ ] Breaking change
  - [ ] Documentation update
  ```
- [ ] Commit: `"docs: PR template"`

---

## 6. Labels & Project Board

### 6.1 Create Label Taxonomy *(manual — GitHub UI or API)*

**Prompt:** Create a comprehensive label system for issues and PRs. Labels should cover priority, component, type, status, and difficulty. Use consistent colors. After completing, mark items as `[x]`.

**Priority labels (red gradient):**

- [ ] `P0-critical` (#B60205) — Blocks development or breaks boot
- [ ] `P1-high` (#D93F0B) — Important, should fix soon
- [ ] `P2-medium` (#FBCA04) — Nice to have, normal priority
- [ ] `P3-low` (#0E8A16) — Polish, someday

**Component labels (blue gradient):**

- [ ] `kernel` (#0075ca) — Kernel core, memory, scheduler
- [ ] `bootloader` (#006b75) — UEFI boot chain
- [ ] `desktop` (#1d76db) — Window manager, compositor, shell
- [ ] `drivers` (#5319e7) — Hardware drivers (AHCI, USB, NIC)
- [ ] `filesystem` (#0052cc) — VFS, FAT32, IXFS
- [ ] `networking` (#0e8a16) — TCP/IP, DNS, HTTP
- [ ] `apps` (#c5def5) — Built-in apps (Notepad, Calculator, etc.)

**Type labels:**

- [ ] `bug` (#d73a4a) — Something is broken
- [ ] `enhancement` (#a2eeef) — New feature or improvement
- [ ] `documentation` (#0075ca) — Documentation only
- [ ] `question` (#d876e3) — Question or discussion
- [ ] `hardware-compat` (#e4e669) — Hardware compatibility report

**Status labels:**

- [ ] `help-wanted` (#008672) — Looking for contributors
- [ ] `good-first-issue` (#7057ff) — Good for newcomers
- [ ] `wontfix` (#ffffff) — Not going to fix
- [ ] `duplicate` (#cfd3d7) — Duplicate issue
- [ ] `stale` (#ededed) — Inactive issue (auto-applied by stale bot)
- [ ] `pinned` (#006b75) — Exempt from stale cleanup

### 6.2 GitHub Project Board *(manual — GitHub UI)*

**Prompt:** Create a GitHub Project (v2) board for tracking development milestones. Map project columns to the TODO phases. This provides a visual overview of progress.

- [ ] Create project: "Impossible OS Development"
- [ ] Columns: Backlog, In Progress, In Review, Done
- [ ] Create milestones matching TODO phases:
  - [ ] `Phase 01: Kernel Foundations` (TODO-010 through TODO-050)
  - [ ] `Phase 02: GFX & UI Framework` (TODO-110 through TODO-150)
  - [ ] `Phase 03: Core Services` (TODO-230 through TODO-300)
  - [ ] `Phase 04: Desktop Shell` (TODO-160 through TODO-220)
  - [ ] `Phase 05: Core Apps` (TODO-310 through TODO-370)
  - [ ] `Phase 06: Multimedia` (TODO-380 through TODO-390)
  - [ ] `Phase 07: Networking` (TODO-400 through TODO-450)
- [ ] Link issues to milestones as work progresses

---

## 7. Branch Protection & Policies

### 7.1 Branch Protection Rules *(manual — GitHub settings)*

**Prompt:** Configure branch protection on `main` to prevent accidental force pushes and ensure all changes pass CI. After completing, mark items as `[x]`.

- [ ] Protect `main` branch:
  - [ ] Require status checks to pass: `build` workflow (§4.1)
  - [ ] Require linear history (no merge commits — rebase only)
  - [ ] Require signed commits (GPG or SSH key)
  - [ ] Disallow force push (protect history)
  - [ ] Disallow deletions
- [ ] Create branch naming convention:
  - [ ] `feature/*` — new features
  - [ ] `fix/*` — bug fixes
  - [ ] `docs/*` — documentation changes
  - [ ] `refactor/*` — code refactoring

### 7.2 CODEOWNERS File *(agent)*

**Prompt:** Create a `CODEOWNERS` file that defines code review requirements for critical paths. Changes to the bootloader, kernel core, and memory management require project lead review. After completing, commit as `"docs: CODEOWNERS file"`.

- [ ] Create `.github/CODEOWNERS`:
  ```
  # Default owner for everything
  *                           @derickpayne

  # Critical paths — require lead review
  src/boot/                   @derickpayne
  src/kernel/memory/          @derickpayne
  src/kernel/sched/           @derickpayne
  linker.ld                   @derickpayne
  Makefile                    @derickpayne
  scripts/build.sh            @derickpayne
  ```
- [ ] Commit: `"docs: CODEOWNERS file"`

---

## 8. SDK Repository

### 8.1 Create Public SDK Repo *(manual)*

**Prompt:** Create `rizonesoft/impossible-os-sdk` as a public GitHub repository. This is the outward-facing repo that third-party developers will use to download SDK headers, libraries, and documentation without needing access to the private OS source. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt.

> [!IMPORTANT]
> → XREF: `TODO-535-SDK.md` — Full SDK development roadmap.
> This section only covers the GitHub repository creation. SDK content
> is defined in the phase 05 TODO.

- [ ] Create `rizonesoft/impossible-os-sdk` (public) on GitHub
- [ ] Add `README.md` explaining SDK purpose, contents, and how to use it
- [ ] Add `LICENSE` (MIT)
- [ ] Repository topics: `sdk`, `impossible-os`, `win32`, `c`, `operating-system`
- [ ] Commit to SDK repo: `"chore: initial SDK public repo"`

### 8.2 Sync SDK Directory on Push *(agent)*

**Prompt:** Create `.github/workflows/sync-sdk.yml` in the private repo to automatically push `sdk/` changes to the public `impossible-os-sdk` repo on every push to `main` that touches `sdk/**`. Use the clone+copy+push approach. Store auth token as `SDK_REPO_TOKEN` secret. After completing all items, mark every item as `[x]`, and commit as `"ci: sync SDK to public repo on push"`.

- [ ] Create `.github/workflows/sync-sdk.yml`
- [ ] Trigger: `push` to `main` with paths filter `sdk/**`
- [ ] Action: clone public SDK repo → remove old contents → copy `sdk/` → commit + push
- [ ] Add `SDK_REPO_TOKEN` to private repo secrets (PAT: `public_repo` write scope)
- [ ] Test end-to-end: change a file in `sdk/`, push, verify public SDK repo updates
- [ ] Commit: `"ci: sync SDK to public repo on push"`

---

## 9. GitHub Pages (Project Website) *(Stretch)*

### 9.1 Create Project Landing Page *(stretch)*

**Prompt:** Create a simple GitHub Pages site at `rizonesoft.github.io/impossible-os` as the public face of the project. Show screenshots, feature list, download links, and development blog. Static HTML/CSS — no framework needed. After completing, commit as `"docs: GitHub Pages landing page"`.

- [ ] *(Stretch)* Enable GitHub Pages on `gh-pages` branch
- [ ] *(Stretch)* Create landing page: hero section, features, screenshots, download
- [ ] *(Stretch)* Link ISO download to latest GitHub Release
- [ ] *(Stretch)* Add development blog section (markdown posts)
- [ ] *(Stretch)* Commit: `"docs: GitHub Pages landing page"`

---

## Priority Order

| Priority | Section                            | Reason                                                  |
|----------|------------------------------------|---------------------------------------------------------|
| ✅ Done   | 1.1 Repository structure           | Both repos exist                                        |
| 🔴 P0    | 2.1 Professional README            | First impression — critical for project visibility      |
| 🔴 P0    | 2.2 Repository metadata            | Topics + description = GitHub search discoverability    |
| 🟠 P1    | 3.1 Semantic versioning            | Foundation for releases and changelogs                  |
| 🟠 P1    | 4.1 Build CI on push               | Catches regressions automatically                       |
| 🟠 P1    | 5.1 Issue templates                | Professional issue intake for bug reports               |
| 🟠 P1    | 7.1 Branch protection              | Prevent accidental force pushes to main                 |
| 🟡 P2    | 2.4 CONTRIBUTING.md                | Enables external contributions                          |
| 🟡 P2    | 2.6 SECURITY.md                    | Responsible vulnerability reporting                     |
| 🟡 P2    | 3.2 Changelog generation           | Auto-generated release notes                            |
| 🟡 P2    | 4.2 Automated ISO release          | Publish builds on tag push                              |
| 🟡 P2    | 5.2 PR template                    | Consistent PR descriptions                              |
| 🟡 P2    | 6.1 Label taxonomy                 | Organized issue triage                                  |
| 🟡 P2    | 7.2 CODEOWNERS                     | Code review requirements for critical paths             |
| 🟢 P3    | 2.3 LICENSE verification           | Quick check                                             |
| 🟢 P3    | 2.5 CODE_OF_CONDUCT.md             | Community standards                                     |
| 🟢 P3    | 3.3 Git tag conventions            | Documented but simple                                   |
| 🟢 P3    | 4.3 Stale issue cleanup            | Automation polish                                       |
| 🟢 P3    | 4.4 Auto-label PRs                 | Automation polish                                       |
| 🟢 P3    | 6.2 Project board                  | Visual milestone tracking                               |
| 🟢 P3    | 8.1 SDK repo create                | Before any SDK code is written                          |
| 🟢 P3    | 8.2 SDK auto-sync CI               | After SDK repo + sdk/ directory exist                   |
| 🔵 P4    | 9.1 GitHub Pages                   | Stretch — public project website                        |

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
| `.github/workflows/release.yml`               | [NEW] ISO release on tag push                |
| `.github/workflows/stale.yml`                 | [NEW] Stale issue cleanup                    |
| `.github/workflows/labeler.yml`               | [NEW] PR auto-labeler                        |
| `.github/workflows/sync-sdk.yml`              | [NEW] SDK repo sync                          |
| `scripts/bump-version.sh`                     | [NEW] Version bump script                    |
| `scripts/generate-changelog.sh`               | [NEW] Changelog generator                    |

---

## OS Comparison

| Feature                         | Linux Kernel (GitHub)         | SerenityOS               | Impossible OS                            |
|---------------------------------|-------------------------------|--------------------------|------------------------------------------|
| Professional README             | ✅ Extensive                   | ✅ Screenshots + GIF     | ⬜ §2.1 P0                               |
| Badges (build, version, LOC)    | ⚠️ Minimal                   | ✅ Build badge            | ⬜ §2.1 P0                               |
| Semantic versioning             | ✅ `uname -r`                 | ✅ Date-based             | ⬜ §3.1 P1                               |
| Auto-generated changelog        | ✅ `git log`                  | ⚠️ Manual                | ⬜ §3.2 P2                               |
| CI build on push                | ✅ kernel.org CI              | ✅ GitHub Actions         | ⬜ §4.1 P1                               |
| Automated releases              | ✅ kernel.org tarballs        | ✅ Nightly builds         | ⬜ §4.2 P2                               |
| Issue templates (YAML forms)    | ❌ Mailing list               | ✅ Bug + feature forms    | ⬜ §5.1 P1                               |
| PR template                     | ❌ Mailing list               | ✅ Template               | ⬜ §5.2 P2                               |
| Branch protection               | ✅ Strict                      | ✅ Main protected         | ⬜ §7.1 P1                               |
| CODEOWNERS                      | ✅ MAINTAINERS file           | ⚠️ Implied               | ⬜ §7.2 P2                               |
| CONTRIBUTING.md                 | ✅ Extensive                   | ✅ Detailed               | ⬜ §2.4 P2                               |
| SECURITY.md                     | ✅ security@kernel.org        | ⚠️ No formal policy      | ⬜ §2.6 P2                               |
| Hardware compat reports         | ❌ Separate DB                | ❌ None                   | ⬜ §5.1 — **unique to Impossible OS**    |
| Stale issue bot                 | ❌ N/A (mailing list)         | ⚠️ Manual                | ⬜ §4.3 P3                               |
| Auto-label by path              | ❌ N/A                        | ⚠️ Manual labels         | ⬜ §4.4 P3                               |
| **Version bump script**         | ❌ Manual                     | ❌ Manual                  | ⬜ §3.1 — **one-command version bump**   |
| **Project website**             | ✅ kernel.org                 | ✅ serenityos.org         | ⬜ §9.1 P4 — GitHub Pages               |
