---
schema_version: 1
id: github-release-community
domain: 15-installer-release
status: active
title: "TODO-05 -- GitHub Releases & Community Launch"
---

# TODO-05 -- GitHub Releases & Community Launch

> **Goal:** Transform Impossible OS from an internal project into a public open-source
> product -- GitHub release workflow, changelog discipline, contribution guide overhaul,
> issue and PR template redesign, README rewrite, project website, community channels
> (GitHub Discussions + Discord), and a public-facing roadmap with GitHub Milestones
> sync.

> [!IMPORTANT]
> **Upload machinery** (`gh release upload` for disk image, ISO, VM images, SDK ZIP)
> is specced in `15-installer-release/TODO-03 §2` (`scripts/upload-release.sh`) and
> `12-user-platform-sdk/TODO-06 §8` (`scripts/release-sdk.sh`). The `create-release.sh`
> here **orchestrates** those scripts; do not re-specify artifact upload logic.
>
> **`gen-changelog.sh`** is specced in `15-installer-release/TODO-03 §3`; §2 here
> consumes it to draft the `CHANGELOG.md` update -- do not re-specify the script.
>
> **`scripts/promote-release.sh`** (stable promotion, git tag push) is specced in
> `15-installer-release/TODO-03 §3`; `create-release.sh` (§1 here) runs after
> `promote-release.sh` completes.
>
> **Code signing** (`sign-release.sh`, Ed25519) is specced in `TODO-01 §5`; §1 here
> requires it to have run before `create-release.sh`.
>
> **Existing files** (`README.md`, `CONTRIBUTING.md`, `CHANGELOG.md`,
> `CODE_OF_CONDUCT.md`, `.github/PULL_REQUEST_TEMPLATE.md`, `.github/ISSUE_TEMPLATE/`)
> already exist in the repo; every section here **overwrites/enhances** them in-place.

---

## Inputs

- `15-installer-release/TODO-03-update-server.md §2 §3` (→ XREF) -- `upload-release.sh`; `promote-release.sh`; `gen-changelog.sh`; §1 §2 orchestration
- `15-installer-release/TODO-01-release-artifacts.md §1 §5` (→ XREF) -- `OS_VERSION_STRING`; signed artifacts; §1 release workflow
- `15-installer-release/TODO-04-release-qa.md §1` (→ XREF) -- `release-checklist.md` PR template; §2 sign-off gate before `create-release.sh`
- `12-user-platform-sdk/TODO-06-sdk-distribution.md §8` (→ XREF) -- `release-sdk.sh`; SDK ZIP artifact; §1 coordinate
- `12-user-platform-sdk/TODO-07-win32-compat-matrix.md §12` (→ XREF) -- `compat-check.sh` score; §5 §8 roadmap metric
- `README.md`, `CONTRIBUTING.md`, `CHANGELOG.md`, `CODE_OF_CONDUCT.md` -- existing files to overhaul
- `.github/ISSUE_TEMPLATE/`, `.github/PULL_REQUEST_TEMPLATE.md` -- existing templates to replace
- `scripts/build.sh` -- quick-start commands; §3 §5
- `00-infrastructure/TODO-01-developer-tooling-stack.md §1-§7` (→ XREF) -- canonical host setup, wrapper, hook, workflow, and doctor contract consumed by `README.md` and `CONTRIBUTING.md`

---

## Outcome

Visiting `https://github.com/rizonesoft/impossible-os` shows a polished README with a
desktop screenshot, 3-command quick start, and download links. New contributors find
`CONTRIBUTING.md`, structured issue templates, and a `good-first-issue` label queue.
Every stable release tags the repo, posts a GitHub Release with all signed artifacts,
and announces on Discord. The public roadmap maps TODO domains to GitHub Milestones so
outsiders can track progress without reading 100+ TODO files.

---

## Implementation Order

| Step | Section                                         | 💎/⭐ | Dependency                                            |
| ---- | ----------------------------------------------- | ----- | ----------------------------------------------------- |
| 1    | Contribution guide overhaul (`CONTRIBUTING.md`) | 💎    | `scripts/build.sh`; existing file                     |
| 2    | Issue & PR templates (`.github/`)               | ⭐    | §1; existing templates                                |
| 3    | Changelog discipline (`CHANGELOG.md`)           | 💎    | `gen-changelog.sh` (TODO-03 §3)                       |
| 4    | GitHub release workflow (`create-release.sh`)   | 💎    | `TODO-01 §5` signing; `D15T03 §2 §3`; SDK `D12T06 §8` |
| 5    | README overhaul                                 | ⭐    | §4 (download links); desktop screenshot               |
| 6    | Project website (`docs/website/`)               | ⭐    | §5 content; §4 release links                          |
| 7    | Community channels (Discussions + Discord)      | 💎    | §6 live; §4 release announcements webhook             |
| 8    | Roadmap publication + GitHub Milestones sync    | ⭐    | §7; domain `INDEX.md` files                           |

---

## 1. GitHub Release Workflow `[Sonnet]`

**Source:** `scripts/create-release.sh`; `.github/workflows/release.yml`

- [ ] Scope the stable-tag pre-release cleanup in `release.yml:355-383` to versions older than the stable tag; today it deletes every listed pre-release and its tag, newer release candidates included
- [ ] Start from the shipped `.github/workflows/release.yml` rather than a second tag-triggered workflow
  - It already handles `v*` CalVer tags, pre-release cleanup and `scripts/generate-changelog.sh`; `15-installer-release/TODO-03` §2 also specs `upload-release.yml` on the same tag.
- [ ] **`scripts/create-release.sh <version> <channel>`**:
  1. Preflight: verify `release-checklist.md` PR was merged (check git log for `release: {version}` commit); verify `build/impossible-os-{version}.img.zst.sha256` exists (artifacts built + signed by `TODO-01 §5`)
  2. Create annotated tag: `git tag -a "v{version}" -m "Impossible OS {version}"` + `git push origin "v{version}"`
  3. Determine release flags: `channel=beta` → `--prerelease`; `channel=stable` → full release
  4. `gh release create "v{version}" --title "Impossible OS {version}" --notes-file docs/changelog/{version}.md [--prerelease if beta] --draft`
  5. Upload OS artifacts (delegate to `scripts/upload-release.sh {version}` from `TODO-03 §2`)
  6. Upload SDK ZIP (delegate to `scripts/release-sdk.sh` from `TODO-06 §8`, pass `--upload-only` flag)
  7. Publish draft: `gh release edit "v{version}" --draft=false`
  8. Post Discord announcement webhook (§7): `curl -X POST $DISCORD_RELEASES_WEBHOOK -d "{\"content\":\"...\"}"` with version, download URL, changelog summary
  9. Print: `"Release v{version} ({channel}) published: https://github.com/rizonesoft/impossible-os/releases/tag/v{version}"`
- [ ] **`.github/workflows/release.yml`**: triggered on `push` to tags matching `v*`; steps: checkout, `bash scripts/build.sh clean`, `bash scripts/sign-release.sh`, `bash scripts/create-release.sh ${{ github.ref_name }} ${{ inputs.channel }}`; requires secrets `GH_RELEASE_TOKEN`, `CODESIGN_PRIV_KEY`, `CLOUDFLARE_R2_*`, `DISCORD_RELEASES_WEBHOOK`
- [ ] **SDK version synchronization**: `create-release.sh` reads `HKLM\SYSTEM\SDK\InstalledVersion` equivalent from `sdk/VERSION` file; asserts it matches OS version before proceeding

---

## 2. Changelog Discipline `[Sonnet]`

**Source:** `CHANGELOG.md` (overhaul existing); `scripts/gen-changelog.sh` (from `D10T03 §5`)

- [ ] **`CHANGELOG.md` format** -- [Keep a Changelog](https://keepachangelog.com/) style:
  ```markdown
  # Changelog

  All notable changes to Impossible OS are documented here.
  Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)

  ## [Unreleased]
  ...

  ## [1.0.22100] -- 2026-03-26
  ### Added
  - Answer file support for unattended installation (#123)
  ### Fixed
  - PMM double-free on NUMA systems (#118)
  ### Changed
  - Update client now parses JSON endpoint instead of INI (#115)
  ### Removed
  - Legacy `SYS_DEBUG_OLD` syscall stub (#110)

  [1.0.22100]: https://github.com/rizonesoft/impossible-os/compare/v1.0.21000...v1.0.22100
  [Unreleased]: https://github.com/rizonesoft/impossible-os/compare/v1.0.22100...HEAD
  ```
- [ ] **PR requirement**: every PR must add an entry to `## [Unreleased]`; PR template (§4) checklist includes `[ ] CHANGELOG.md updated`; CI linter checks that `## [Unreleased]` section is non-empty on PRs that add/change code
- [ ] **`scripts/gen-changelog.sh <old-tag> <new-tag>`** (specced in `D10T03 §5`; used here): `gh pr list --state merged --base main --search "merged:{old-date}..{new-date}" --json title,number,url` → group by PR label (`kernel`/`boot`/`desktop`/`drivers`/etc.) → output draft `docs/changelog/{version}.md`; maintainer reviews + promotes to `CHANGELOG.md`
- [ ] **Changelog lint in CI** (`.github/workflows/pr-check.yml`): `python scripts/lint-changelog.py` -- check `CHANGELOG.md` parses; warn if `## [Unreleased]` is empty on code-change PRs; error on version entry missing comparison link

---

## 3. Contribution Guide Overhaul `[Sonnet]`

**Source:** `CONTRIBUTING.md` (overhaul existing file)

- [ ] Reconcile the DCO sign-off and "no direct pushes to `main`" items with the repo's zero-trailer commit policy and main-only workflow (`CONTRIBUTING.md`, `CLAUDE.md`) before writing them into the guide
- [ ] **Sections** (rewrite `CONTRIBUTING.md` to cover):
  - **Canonical source of truth**: `D00 T01 §1-§7` owns setup/build/test/hook/workflow command truth; `CONTRIBUTING.md` summarizes the supported path and links back instead of inventing parallel commands
  - **Development setup**: WSL2 + Ubuntu 22.04 recommended; `bash scripts/setup.sh` installs all dependencies (clang-19, nasm, lld-19, QEMU, OVMF); Windows native not supported (WSL2 only); macOS not supported
  - **Build commands**: table (`bash scripts/build.sh`, `clean`, `run`, `clean run`, `qa`); verify with `tail -1 build/build.log`
  - **QEMU test run**: `bash scripts/test.sh` for the regression suite; `bash scripts/build.sh run` for interactive boot
  - **Code style**: line length ≤ 120, function length < 50 lines, `snake_case` functions + vars, `UPPER_CASE` macros, `#pragma once`, no angle-bracket includes; link to `CONTRIBUTING.md §Coding Conventions`
  - **Commit message format**: conventional commits -- `scope: short description`; common scopes table (kernel, boot, desktop, drivers, gfx, fs, net, build, docs, agent)
  - **PR process**: feature branch → PR → CI green → code review → squash-merge; PR title must follow conventional commit format; no direct pushes to `main`
  - **DCO sign-off**: `Signed-off-by: Name <email>` in each commit; `git commit -s` adds automatically; DCO bot checks PRs
  - **Issue labels** table: `P0` (blocker), `P1` (important), `P2` (nice-to-have), `P3` (stretch), `bug`, `enhancement`, `good-first-issue`, `help-wanted`, `kernel`, `desktop`, `drivers`, `docs`
  - **Good-first-issue criteria**: `[ ]` items in `todo/` files tagged `[Sonnet]` with clear inputs; no syscall changes; < 100 lines estimated; documented in `good-first-issue` GitHub label description
  - **TODO files**: new TODO files must use the `create-todo` skill; direct `[ ]` item additions without a TODO file require no special process

---

## 4. Issue & PR Templates `[Sonnet]`

**Source:** `.github/ISSUE_TEMPLATE/` (replace existing); `.github/PULL_REQUEST_TEMPLATE.md` (replace existing)

- [ ] **`.github/ISSUE_TEMPLATE/bug_report.yml`** (structured form):
  ```yaml
  name: Bug Report
  description: Report a defect in Impossible OS
  labels: ["bug", "triage"]
  body:
    - type: input
      id: os-version
      label: OS Version
      placeholder: "e.g. 1.0.22100"
    - type: dropdown
      id: platform
      label: Platform
      options: [QEMU, Hyper-V, VirtualBox, Real Hardware]
    - type: textarea
      id: reproduce
      label: Steps to reproduce
    - type: textarea
      id: expected
      label: Expected behaviour
    - type: textarea
      id: actual
      label: Actual behaviour
    - type: textarea
      id: serial-log
      label: Serial log (attach build/serial.log or relevant excerpt)
      required: false
    - type: dropdown
      id: priority
      label: Suggested priority
      options: [P0 - Crash/data loss, P1 - Major regression, P2 - Minor issue, P3 - Polish]
  ```
- [ ] **`.github/ISSUE_TEMPLATE/feature_request.yml`**:
  - Fields: title, description, use case, Windows 11 equivalent, Linux equivalent, which TODO domain it belongs to (dropdown), priority suggestion
- [ ] **`.github/PULL_REQUEST_TEMPLATE.md`** (replace existing):
  ```markdown
  ## Summary
  <!-- What does this PR do? One paragraph. -->

  ## Related issue
  Closes #<!-- issue number -->

  ## Checklist
  - [ ] `bash scripts/build.sh clean` passes
  - [ ] `bash scripts/test.sh` passes (no new test failures)
  - [ ] `CHANGELOG.md` updated under `## [Unreleased]`
  - [ ] project conventions consulted -- no violations
  - [ ] New TODO files created with `create-todo` skill (if applicable)
  - [ ] No angle-bracket includes added
  - [ ] `Signed-off-by:` present in all commits
  ```
- [ ] **`.github/ISSUE_TEMPLATE/config.yml`**: blank-issues disabled; link to `CONTRIBUTING.md` + Discord for questions

---

## 5. README Overhaul `[Sonnet]`

**Source:** `README.md` (overhaul existing file)

- [ ] **README structure**:
  ````markdown
  # Impossible OS

  > A production-quality 64-bit OS built from scratch.
  > Custom UEFI bootloader, kernel, and compositing desktop -- no Linux, no Windows.

  [![CI](badge)] [![License: GPL-3.0](badge)] [![Release](badge)]

  ![Desktop Screenshot](docs/screenshots/desktop-v{version}.png)

  ## Quick Start

  ```bash
  git clone https://github.com/rizonesoft/impossible-os
  bash scripts/setup.sh        # install dependencies (Ubuntu/WSL2)
  bash scripts/build.sh run    # build + launch in QEMU
  ```

  ## Download

  | Format                  | Link                  | SHA-256            |
  | ----------------------- | --------------------- | ------------------ |
  | Disk image (`.img.zst`) | [v{latest}](releases) | [sha256](releases) |
  | Bootable ISO            | [v{latest}](releases) | [sha256](releases) |
  | VirtualBox OVA          | [v{latest}](releases) | [sha256](releases) |
  | SDK                     | [v{latest}](releases) | [sha256](releases) |

  ## Feature Status
  <!-- feature status table, kept in sync with README -->

  ## Architecture

  <!-- ASCII art boot chain + memory model -->

  ## Contributing

  See [CONTRIBUTING.md](CONTRIBUTING.md). New contributors: look for
  [good-first-issue](issues?label=good-first-issue) items.

  ## License

  GPL-3.0 -- Copyright © 2026 [Rizonetech (Pty) Ltd](https://rizonetech.com)
  ````
- [ ] **Desktop screenshot**: capture QEMU framebuffer at `1280×720` showing desktop with taskbar, wallpaper, and at least one open window; save as `docs/screenshots/desktop-v{version}.png`; script `scripts/take-screenshot.sh` (QEMU `-screenshot` option on a specific frame or `sendkey` sequence to open a window)
- [ ] **CI status badge**: `[![CI](https://github.com/rizonesoft/impossible-os/actions/workflows/build.yml/badge.svg)](...)` -- auto-shows green/red from GHA
- [ ] **Tooling-command sync**: README quick start and test guidance must reuse the canonical commands from `D00 T01 §1-§7`; no stale `make test`, `run-tests.sh`, or obsolete ISO-only launcher wording
- [ ] **Auto-update download table**: `scripts/update-readme-links.sh` -- reads `build/release-{version}.json`; replaces `{latest}` placeholders in README with current version + URLs; called by `create-release.sh` (§1)
- [ ] **README status sync**: feature status table in README stays current; `scripts/sync-readme-status.sh` generates README table from source to avoid drift

---

## 6. Project Website `[Sonnet]`

**Source:** `docs/website/` directory; deployed to `gh-pages` branch → `impossible-os.dev`

- [ ] **Static site structure** (`docs/website/`):
  ```
  docs/website/
  ├── index.html           ← Home: hero, features, screenshot, download CTA
  ├── docs.html            ← Documentation: API reference, guides index
  ├── changelog.html       ← Auto-rendered from CHANGELOG.md
  ├── blog/
  │   └── {date}-{title}.md ← Release announcements, handwritten posts
  ├── css/style.css        ← Clean minimal design, dark-mode support
  └── js/main.js           ← Fetch latest version from update API for CTA button
  ```
- [ ] **Home page content**: hero section ("A production-quality OS. Built from scratch."), animated terminal showing `bash scripts/build.sh run`, feature grid (6 highlight cards: UEFI bootloader, 64-bit kernel, compositing desktop, networking, Win32 API, native installer), latest release screenshot, download CTA button (dynamically fetches latest version from `https://impossible-os.dev/api/version?channel=stable` and shows version + download link), footer with GitHub + Discord links
- [ ] **Docs page**: links to `docs/guides/` (getting started, enterprise deployment, API reference, porting guide)
- [ ] **Changelog page**: fetches `CHANGELOG.md` from GitHub raw URL and renders Markdown via `marked.js`
- [ ] **Blog**: Markdown posts in `docs/website/blog/`; rendered via `marked.js`; first post: "Announcing Impossible OS v1.0" release post written by maintainer
- [ ] **GitHub Actions deploy workflow** (`.github/workflows/website.yml`): trigger on push to `main` and on tag `v*`; copy `docs/website/` to `gh-pages` branch via `peaceiris/actions-gh-pages`; custom domain `impossible-os.dev` via `CNAME` file; Cloudflare DNS proxies the domain

---

## 7. Community Channels `[Sonnet]`

**Sources:** GitHub repository settings; Discord server; `CODE_OF_CONDUCT.md` (enhance existing)

- [ ] **GitHub Discussions**: enable in repo settings; configure categories:
  - `Q&A` -- how-to questions; maintainers answer; resolved threads auto-lock after 30 days
  - `Ideas` -- feature proposals; labelled `good-idea` by maintainer if accepted → converts to issue
  - `Show and Tell` -- screenshots, videos, apps built for Impossible OS
  - `General` -- anything else; contribution discussions
- [ ] **Discord server**:
  - Channels: `#announcements` (read-only), `#releases` (webhook-only), `#general`, `#kernel-dev`, `#userland`, `#hardware`, `#help`, `#good-first-issues` (bot auto-posts new `good-first-issue` GitHub issues)
  - Server invite linked in `README.md` + `CONTRIBUTING.md` + website footer
  - Roles: `@Contributor` (anyone with merged PR), `@Maintainer`, `@Community`
- [ ] **Discord release webhook**: `DISCORD_RELEASES_WEBHOOK` secret; `create-release.sh §1` POSTs on each release:
  ```
  🚀 **Impossible OS v{version}** is out!
  {type} release -- {N} new changes
  Download: https://github.com/rizonesoft/impossible-os/releases/tag/v{version}
  Changelog: https://impossible-os.dev/changelog#{version}
  ```
- [ ] **`CODE_OF_CONDUCT.md`** (enhance existing): adopt Contributor Covenant 2.1 verbatim; add project-specific contact: `conduct@impossible-os.dev` (GitHub-forwarded email alias); link from `CONTRIBUTING.md` and all issue templates
- [ ] **`#good-first-issues` bot**: GitHub Action (`.github/workflows/discord-notify.yml`): on `issues` event with label `good-first-issue` added → POST to `#good-first-issues` Discord channel webhook with issue title + URL

---

## 8. Roadmap Publication + GitHub Milestones Sync `[Sonnet]`

**Source:** `docs/roadmap.md`; `scripts/sync-milestones.sh`; `scripts/sync-issues.sh`

- [ ] **`docs/roadmap.md`** (public-facing, non-technical):
  - **Current milestone (v1.0)**: what is included -- kernel, storage, networking, compositing desktop, Win32 API foundations, installer, release pipeline, SDK; link to `v1.0-kernel` + `v1.0-desktop` GitHub Milestones
  - **Next milestone (v1.1)**: planned improvements -- full Win32 compat Tier 1–7, unattended deployment, audio system, enterprise features; link to `v1.1-compat` + `v1.1-enterprise` Milestones
  - **Long-term vision (v2.0)**: Win32 compat Tier 8, Linux compatibility layer, touch + gamepad input, software OpenGL, multi-user sessions
  - **Compat progress bar**: embed `Win32 compat score: {N}%` (from `scripts/compat-check.sh` last run output stored in `build/compat-score.txt`); updated by CI
- [ ] **GitHub Milestones → TODO domain mapping**:
  ```
  v1.0-kernel       → 01-boot-platform, 02-kernel-core, 03-memory-concurrency, 04-drivers-hardware
  v1.0-desktop      → 05-storage-filesystems, 06-desktop-foundation, 07-networking, 08-graphics-ui, 09-desktop-shell
  v1.0-apps         → 10-platform-services, 11-apps
  v1.0-release      → 15-installer-release
  v1.1-compat       → 12-user-platform-sdk
  ```
- [ ] **`scripts/sync-milestones.sh`**: for each milestone, `gh milestone create` (if not exists) with description = domain `INDEX.md` first paragraph + due date; `gh milestone edit` if exists; idempotent (safe to run repeatedly)
- [ ] **`scripts/sync-issues.sh`** (lightweight -- P0/P1 only):
  - Scan all `todo/**/*.md` for `- [ ]` items with adjacent priority tag `🔴` (P0) or `🟠` (P1)
  - For each such item: if no existing GitHub Issue with matching title: `gh issue create --title "{item text}" --label "P{N}" --milestone "v{...}" --body "Source: {file} §{section}"`
  - Guard: dry-run mode by default; `--apply` flag required to actually create issues; prevents issue spam
  - Limit: max 10 new issues per run; print skipped count

---

## OS Comparison


| ⭐  | Feature                                                             | 🪟 Win11                                          | 🐧 Linux                                                    | 🚀 Impossible OS                                                      |
| --- | ------------------------------------------------------------------- | ------------------------------------------------- | ----------------------------------------------------------- | --------------------------------------------------------------------- |
| 💎  | Automated release workflow                                          | ✅ Internal pipeline; not public                  | ✅ `make release` + distro infra;                           | ⬜ §1 -- `create-release.sh`; GHA on `v*` tag                         |
| 💎  | Keep-a-Changelog + changelog lint in CI                             | ✅ Windows Blog; no structured changelog          | ✅ kernel.org `CHANGES`; distro changelogs                  | ⬜ §2 -- `CHANGELOG.md` Keep-a-Changelog; lint-changelog.py in CI     |
| 💎  | Structured contribution guide + DCO                                 | ✅ `CONTRIBUTING.md` on GitHub repos; not         | ✅ `Documentation/process/` in kernel; `CONTRIBUTING.md` in | ⬜ §3 -- overhaul `CONTRIBUTING.md`; setup, style, DCO,               |
| ⭐  | Structured YAML issue forms + PR checklist                          | ✅ GitHub YAML forms on MS                        | ✅ Many kernel/distro repos use forms                       | ⬜ §4 -- YAML `bug_report.yml` + `feature_request.yml`; PR            |
| ⭐  | README with live download CTA                                       | ❌ N/A (Windows is not on                         | ✅ Distro READMEs; no dynamic download                      | ⬜ §5 -- README download table; `main.js` fetches                     |
| ⭐  | Static project website with dynamic version CTA                     | ✅ `microsoft.com` -- commercial; not open-source | ✅ `kernel.org`, distro websites                            | ⬜ §6 -- `docs/website/`; GitHub Pages; Cloudflare CNAME              |
| 💎  | Discord + GitHub Discussions community                              | ✅ Windows Insider Hub; not Discord               | ✅ Kernel mailing list; many distros                        | ⬜ §7 -- Discussions categories; Discord server +                     |
| ⭐  | Public roadmap with auto-sync from TODO system to GitHub Milestones | ✅ Windows Roadmap on Learn.microsoft.com; no     | ✅ kernel.org merge window schedule; no                     | ⬜ §8 -- `docs/roadmap.md`; `sync-milestones.sh`; compat progress bar |

Impossible OS's `⭐` advantage: the release workflow, changelog, README, and roadmap are
all connected -- `create-release.sh` updates the README download table, posts to Discord,
and the public roadmap shows live Win32 compat progress as a percentage bar sourced
directly from CI. A new contributor can go from `git clone` to a running OS in 3
commands, and the `#good-first-issues` Discord bot means help-wanted items surface
immediately to community members without polling GitHub.

---

## Verification

- [ ] **Release workflow**: `scripts/create-release.sh 1.0.22100 stable` (dry run with `GH_RELEASE_TOKEN` set to test PAT): annotated tag created, draft GH Release created with correct title, all artifact upload calls made, Discord webhook fires, release published; verify at `https://github.com/rizonesoft/impossible-os/releases`
- [ ] **Changelog lint**: PR with code change + empty `## [Unreleased]` → CI fails with `CHANGELOG.md: [Unreleased] section is empty`; PR with entry in `[Unreleased]` → lint passes
- [ ] **CONTRIBUTING.md**: fresh Ubuntu/WSL2 environment, follow `CONTRIBUTING.md` setup steps exactly → OS builds and runs in QEMU without additional intervention
- [ ] **Issue templates**: open new issue on GitHub → two template options appear; fill bug report form → all fields validate; submit → issue created with `bug` + `triage` labels
- [ ] **PR template**: open a PR → checklist appears with all 7 items unchecked; confirm no merge without at least CI checks passing
- [ ] **README**: `README.md` renders on GitHub with screenshot visible, quick-start code blocks correct, download table populated; CI badge shows green; Discord invite link valid
- [ ] **Website**: push to `main` → GHA deploys `docs/website/` to `gh-pages`; `https://impossible-os.dev` loads home page; download CTA button shows correct latest version; `https://impossible-os.dev/changelog` renders `CHANGELOG.md`
- [ ] **Discord announcement**: `create-release.sh` fires webhook → message appears in `#releases` within 5 s; message contains version, type, and correct download URL
- [ ] **Milestones sync**: `scripts/sync-milestones.sh` (dry run) prints 5+ milestones with correct descriptions; `--apply` creates them on GitHub; re-run → idempotent (no duplicates)
- [ ] **sync-issues dry run**: `scripts/sync-issues.sh` (default dry-run) prints P0/P1 items found in TODO files; `--apply` creates at most 10 issues; re-run with `--apply` → no duplicates created
- [ ] Commit: `"community: GitHub release workflow, changelog discipline, CONTRIBUTING.md overhaul, YAML issue templates, README overhaul, project website, Discord integration, roadmap + milestone sync"`
