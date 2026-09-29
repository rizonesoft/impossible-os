---
schema_version: 1
id: documentation-site
domain: 00-infrastructure
status: draft
title: "TODO-10 -- Documentation Site and Documentation Corpus"
file_patterns:
  - "scripts/site/**"
  - "gh-pages/**"
  - "docs/**"
  - "project.json"
  - "tools/vendor/**"
  - ".github/workflows/pages.yml"
---

# TODO-10 -- Documentation Site and Documentation Corpus

> **Validated:** 2026-09-28 | validate-todo-file clean (structure / IO table / XREF / test wiring)
> **Gap-audited:** 2026-09-28 | gap-audit + codex-gap-audit; 8 findings filed (§23 link check, new §24 versioned docs, new §25 search + accessibility, reciprocal D12 T06 §2/§8)

> **Goal:** Every roadmap file under `todo/` gets real documentation, published at [impossibleos.co/docs](https://impossibleos.co/docs/) and generated from Markdown in `docs/`, and no published fact (release date, repository owner, counts, design tokens, links) can drift from its source. A TODO file is the plan; its docs page is what a user, contributor or operator reads to understand what shipped.

> [!IMPORTANT]
> **Current state (2026-09-27):** `scripts/site/build.py` builds the landing page (`gh-pages/` templates) and renders all `docs/**/*.md` into the site; `.github/workflows/pages.yml` runs `--check`, builds and deploys on every push (Pages now deploys from the workflow, not the stale `gh-pages` branch). `project.json` holds project facts; README.md carries `<!-- project:key -->` regions re-synced by `.githooks/post-commit`. Coverage is computed from `<!-- docs: covers=... -->` directives and tracked against the shrink-only baseline `docs/.coverage-baseline.json`. Lint Check 30 fails a commit on a dead doc link or anchor, a stale region, a live repo URL naming the wrong owner, a README line-count badge that disagrees with COUNT.md, a stale `include/desktop/theme_tokens.h`, or a new undocumented roadmap file. What is missing is the content: most roadmap files have no documentation page yet (see the coverage page), there is no written contract for what a docs page contains, and nothing notices when the code a page describes changes.

## Inputs

- [`scripts/site/build.py`](../../scripts/site/build.py) -- the site generator and drift checker
- [`scripts/site/gen_theme_header.py`](../../scripts/site/gen_theme_header.py) -- `docs/design/tokens.json` to `include/desktop/theme_tokens.h`
- [`scripts/site/render-brand.sh`](../../scripts/site/render-brand.sh) -- README wordmarks and desktop preview
- [`gh-pages/docs-template.html`](../../gh-pages/docs-template.html) -- the docs page template
- [`project.json`](../../project.json) -- single source of project facts
- [`docs/.coverage-baseline.json`](../../docs/.coverage-baseline.json) -- undocumented roadmap files, shrink-only
- [`.github/workflows/pages.yml`](../../.github/workflows/pages.yml) -- check, build and deploy
- [`scripts/lint.sh`](../../scripts/lint.sh) -- Check 30 runs `build.py --check --skip-stats`
- [`tools/vendor/markdown_it/`](../../tools/vendor/markdown_it/) -- vendored markdown-it-py 3.0.0 (MIT), chosen over a hand-written converter per the vendor-first rule; mdurl 0.1.2 alongside
- -> XREF: `TODO-09-repository-transfer-rizonetech.md §8` -- the move back to `rizonesoft` and the public flip that made the docs site public
- -> XREF: `TODO-01-developer-tooling-stack.md` -- host tooling conventions and `scripts/test-tooling.sh`
- [`.claude/skills/create-todo/SKILL.md`](../../.claude/skills/create-todo/SKILL.md) -- its docs-page step moves to the §3 contract (operator-gated, control plane)

## Outcome

- Every roadmap file has at least one docs page that meets the §3 contract, and `docs/.coverage-baseline.json` is empty.
- A new roadmap file cannot be committed without its docs page (Check 30 already refuses it once the baseline stops listing it).
- A docs page that describes code which has since changed is flagged (§22).
- The site has a sitemap, per-page last-updated dates and a scheduled external-link check (§23).
- Every OS and SDK release keeps a docs snapshot pinned to its own commit, and the SDK API reference is published (§24).
- Search finds API identifiers anywhere on a page, and the docs UI passes keyboard and screen-reader checks (§25).

## Implementation Order

| ⭐  | Order | Deliverable                                                              | Depends On | Status |
| --- | :---: | ------------------------------------------------------------------------ | ---------- | :----: |
| ⭐  |   1   | §1 Site generator, project facts, coverage and drift gate                | --         |  [x]   |
| 💎  |   2   | §2 Map existing docs pages to their roadmap files                        | §1         |  [x]   |
| ⭐  |   3   | §3 Documentation page contract, template and create-todo step            | §1         |  [x]   |
| 💎  |   4   | §4 Document: Infrastructure (9 roadmap files)                            | §2, §3     |  [x]   |
| 💎  |   5   | §5 Document: Boot platform, part 1 (10 roadmap files)                    | §2, §3     |  [x]   |
| 💎  |   6   | §6 Document: Boot platform, part 2 (10 roadmap files)                    | §2, §3     |  [x]   |
| 💎  |   7   | §7 Document: Boot platform, part 3 (9 roadmap files)                     | §2, §3     |  [x]   |
| 💎  |   8   | §8 Document: Kernel core, part 1 (12 roadmap files)                      | §2, §3     |  [x]   |
| 💎  |   9   | §9 Document: Kernel core, part 2 (12 roadmap files)                      | §2, §3     |  [x]   |
| 💎  |  10   | §10 Document: Kernel core, part 3 (12 roadmap files)                     | §2, §3     |  [x]   |
| 💎  |  11   | §11 Document: Memory and concurrency (11 roadmap files)                  | §2, §3     |  [x]   |
| 💎  |  12   | §12 Document: Drivers and hardware, part 1 (13 roadmap files)            | §2, §3     |  [x]   |
| 💎  |  13   | §13 Document: Drivers and hardware, part 2 (12 roadmap files)            | §2, §3     |  [x]   |
| 💎  |  14   | §14 Document: Storage and filesystems (14 roadmap files)                 | §2, §3     |  [x]   |
| 💎  |  15   | §15 Document: Networking (11 roadmap files)                              | §2, §3     |  [x]   |
| 💎  |  16   | §16 Document: Desktop foundation and graphics, part 1 (14 roadmap files) | §2, §3     |  [x]   |
| 💎  |  17   | §17 Document: Graphics and UI, part 2 (9 roadmap files)                  | §2, §3     |  [x]   |
| 💎  |  18   | §18 Document: Desktop shell (14 roadmap files)                           | §2, §3     |  [x]   |
| 💎  |  19   | §19 Document: Platform services (15 roadmap files)                       | §2, §3     |  [x]   |
| 💎  |  20   | §20 Document: Applications and accessories (15 roadmap files)            | §2, §3     |  [x]   |
| 💎  |  21   | §21 Document: SDK, release, ports and research (21 roadmap files)        | §2, §3     |  [ ]   |
| ⭐  |  22   | §22 Doc freshness: `sources=` and a stale-page warning                   | §3         |  [x]   |
| 💎  |  23   | §23 Site polish: sitemap, last-updated, link health, OpenGraph           | §1         |  [ ]   |
| 💎  |  24   | §24 Versioned release docs: retention, pinned refs, SDK reference        | §1, §23    |  [ ]   |
| 💎  |  25   | §25 Docs search completeness and accessibility                           | §1         |  [ ]   |
| 💎  |  26   | §26 Document: Host tools (8 roadmap files)                               | §2, §3     |  [ ]   |

> 💎 = parity work -- matches what Windows 11 and Linux already do.
> ⭐ = exclusive work -- Impossible OS is superior or first.

---

## 1. Site Generator, Project Facts, Coverage, and Drift Gate

One generator owns every published surface, so no fact is maintained in two places by hand.

- [x] `scripts/site/build.py`: renders `gh-pages/` templates (`{{key}}` from `project.json`) and `docs/**/*.md` into `build/site/` with generated navigation, per-page table of contents, GitHub-style alerts, Mermaid, and a client-side search index
- [x] `project.json` as the single source of owner, repo URL, site URL and release date (2028-08-08); README `<!-- project:key -->` regions, including computed `stat_*` counts, re-synced by `.githooks/post-commit`
- [x] Coverage page from `<!-- docs: covers=... -->` directives, with the shrink-only baseline `docs/.coverage-baseline.json` (`--update-baseline` only removes entries)
- [x] Drift checks: dead doc links and anchors (15 found and fixed on first run), unknown template keys, stale regions, live repo URLs naming a non-canonical owner, README line-count badge vs COUNT.md, `theme_tokens.h` vs `docs/design/tokens.json`
- [x] Lint Check 30 runs `build.py --check --skip-stats` on every commit (~1 s); `.github/workflows/pages.yml` runs the full `--check`, builds and deploys; Pages switched from the stale `gh-pages` branch to workflow deploys
- [x] Vendored markdown-it-py 3.0.0 and mdurl 0.1.2 (MIT) under `tools/vendor/`, with PROVENANCE and CREDITS rows and a COUNT.md vendored-tree exclusion
- [x] Check 30 judges the commit, not the working tree: `--staged` snapshots the index; post-commit syncs from a pinned HEAD snapshot (`--sync-head` / `--emit-head --ref`, batched `cat-file`) (Codex review 2026-09-27)
- [x] Link and image existence is judged against tracked files only (ignored build artifacts never satisfy a link); same-page and percent-encoded anchors are validated (Codex review)
- [x] Raw HTML `href`/`src` are rewritten and validated through a tag scanner that skips comments and other attributes' values, decoding entities and escaping once (Codex re-adversarial)
- [x] The coverage baseline cannot grow: additions are measured against the previously committed baseline (Pages checks out `fetch-depth: 2`); the one exception is re-adding a falsely claimed path with a written `growth_reasons` entry (Codex review)
- [x] A dirty working README no longer skips the sync: the hook commits a synced README blob through a private index (5 re-adversarial rounds)
  - Held under the real `index.lock`, with a compare-and-swap on HEAD, real parents (shallow-safe), and signal-safe single-owner lock cleanup.
- [x] Design-line scope check (`docs/design/scope.json`) and an icon render stamp that binds every SVG source and rendered PNG (`resources/icons/color/SOURCES.sha256`)
- [x] The Pages workflow triggers on every published input (icon, brand and wallpaper assets, the theme header)
- [x] Commit: `"site: generated docs site, project facts, coverage and drift gate"`

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `bash scripts/lint.sh` reports no Check 30 error; the Pages workflow run for the commit is green and `https://impossibleos.co/docs/coverage.html` lists every roadmap file. Test on: WSL2 dev host; GitHub Actions `ubuntu-latest`.

> **Test runner:** host-side `python3 scripts/site/tests/test_build.py` (16 tests, wired into `scripts/test-tooling.sh`) | validation: `python3 scripts/site/build.py --check`, lint Check 30, the Pages workflow

> **Notes:**
> - **What shipped:** the site generator and drift gate (`scripts/site/build.py`, `gen_theme_header.py`), project facts in `project.json`, README regions synced by `.githooks/post-commit`, lint Check 30, and the generated Pages deploy.
> - **Review hardening:** commit-scoped checks (index and HEAD snapshots), tracked-file link rules including raw HTML, shrink-only baseline against the previous commit, and a transactional README amend under the real index lock.
> - **Canonical doc:** [`docs/infrastructure/documentation-site.md`](../../docs/infrastructure/documentation-site.md).

> **Verified:** 2026-09-27 | commit `ad3829185` | 13/13 items | build OK | tests 16/16 PASS
> **Quality reviewed:** 2026-09-27 | Codex 9x (adversarial, consistency, perf, re-adversarial x6) | 6H+10M fixed | scope: N/A (host tooling and docs; no kernel, boot or desktop code)

---

## 2. Map Existing Docs Pages to Their Roadmap Files

Many existing pages already document a roadmap file but do not declare it, so coverage undercounts them.

- [x] Add a first-line `covers=` directive to each existing page under `docs/` (not `docs/design/`, already mapped)
  - Name every roadmap file the page genuinely documents: it explains the subsystem, format, protocol, tool or procedure that TODO implements
  - A passing mention or a link does not count
- [x] Leave a page without a directive when it documents no single roadmap file (indexes, guides)
- [x] Run `python3 scripts/site/build.py --update-baseline` so the baseline drops every newly covered file
- [x] Mapped existing pages to their roadmap files (2026-09-27); after review: 45 pages carry directives, coverage 36 of 232, baseline 196
  - Review removed one false claim (`test-policy.md` does not document the kernel test harness; re-listed with a written reason) and added three missed ones (`bare-metal-gotchas.md`, `github-setup.md`, `docs/design/icons.md`)
- [x] Commit: `"docs: declare which roadmap files each existing docs page covers"`

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK` and a higher `N/232 TODO files documented` than before; the coverage page shows the new links. Test on: WSL2 dev host.

> **Test runner:** N/A (docs directives only) | validation: `python3 scripts/site/build.py --check` reports 36/232 documented with the 196-entry baseline

> **Notes:**
> - **What shipped:** first-line `covers=` directives on 45 docs pages (42 existing plus 3 design pages), shrinking the baseline from 231 to 196.
> - **Review:** one false claim removed and three missed mappings added; the removed claim is re-listed with a written `growth_reasons` entry, the only sanctioned baseline addition.

> **Verified:** 2026-09-27 | commit `ad3829185` | 4/4 items | build OK | 36/232 documented
> **Quality reviewed:** 2026-09-27 | Codex 3x (adversarial, consistency, perf) | 5M fixed | scope: N/A (docs-only)

---

## 3. Documentation Page Contract, Template, and create-todo Step

Every later section writes pages against this contract, so it must exist first and be unambiguous.

- [x] Wrote `docs/contributing/docs-page-contract.md` (`order=1`, `sources=scripts/site/build.py`) defining what a roadmap docs page contains, in this order:
  - H1 title naming the subsystem as a user would; the `covers=` directive on line 1
  - Overview: what it is and why it exists, answer first, 2-4 sentences
  - How it works: architecture, data flow, key structures, with a Mermaid diagram where a picture helps
  - Interfaces: public functions, syscalls, file formats, Registry keys or config options, each with its source link (`../../src/...` or `../../include/...`)
  - Using it: operator or developer guide with commands and expected output
  - Limits and status: what is not implemented yet, linking the owning roadmap section; never claim unshipped behaviour
  - Windows 11 and Linux comparison: one short paragraph or table, consistent with the TODO's OS Comparison table
  - See also: the roadmap file, related docs pages
- [x] Style rules in the contract: one line per paragraph, no em or en dashes, question-shaped H2s where natural, every figure sourced (code, test output, or spec), 400-1500 words per page, split larger topics into linked pages
  - Plus link-not-copy: point at the header, section or spec instead of pasting a struct or table
- [x] Freshness rule in the contract: a page that documents code declares `sources=path[,path]` and `reviewed=YYYY-MM-DD`, and the template carries both
  - The template ships them blank (`covers= sources= reviewed=`), which the parser accepts, so it renders without a false coverage claim
  - The check itself shipped with the doc-freshness section: stale pages warn in lint Check 30 and show on the coverage page.
- [x] Added `docs/contributing/_template.md`, a copy-ready skeleton of the contract, and `docs/contributing/index.md`
- [x] Mapped each domain to its docs folder in the contract (a table), adding each missing folder with an `index.md` when its first page lands
  - 00 infrastructure, 01 boot, 02 kernel, 03 memory, 04 hardware, 05 storage, 06 and 09 desktop, 07 networking, 08 graphics
  - 10 services, 11 and 13 apps (as §20 places them), 12 sdk, 14 host-tools, 15 release, 16 ports, 17 hardening (no roadmap files yet), 18 research; design specs stay in `docs/design/`
- [/] Upgrade create-todo's existing docs-page step (`.claude/skills/create-todo/SKILL.md:87`) to the contract and template, dropping its provisional fallback: operator-gated (control plane), filed in `overnight-runner-improvements-v20.md`
  - A new roadmap file ships with a docs page from `_template.md` whose Limits section states nothing is implemented yet
  - Needed because Check 30 refuses an undocumented new roadmap file
- [x] Added the contract to `docs/index.md` (category row and quick link) and linked it from `CONTRIBUTING.md` (new "Documentation Pages" subsection under Code Style)
- [x] Commit: `"docs: documentation page contract, template, and create-todo docs step"`

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; the contract page renders at `/docs/contributing/docs-page-contract.html`; a dry run of the create-todo docs step produces a page that passes the check. Test on: WSL2 dev host.

> **Test runner:** `python3 scripts/site/tests/test_build.py PageContract` (via `scripts/test-tooling.sh`) | 4 tests, 0 failures; validation: `python3 scripts/site/build.py --check` prints `site: OK`

> **Notes:**
> - Shipped `docs/contributing/docs-page-contract.md` (parts, directive keys, freshness, style, folder map, add-a-page steps), `_template.md` and `index.md`, linked from `docs/index.md` and `CONTRIBUTING.md`.
> - The template ships a blank directive (`covers= sources= reviewed=`) and `../contributing/` links, so it renders unclaimed and still resolves after being copied into any `docs/<folder>/`.
> - `PageContract` tests pin every `todo/NN-*` domain to one folder and check every page destination in §4-§21 against it; §21's two release files moved from `docs/sdk/` to `docs/release/`.
> - Dry run: a template copy covering `TODO-03-kernel-test-harness.md` passed the check apart from the expected stale-baseline entry, which the contract's step 4 now clears first.
> - Scope boundary: the create-todo step upgrade is parked operator-gated (control plane); `overnight-runner-improvements-v20.md` carries it.

> **Verified:** 2026-09-28 | commit `3ec5b3c03` | 7/8 items (1 parked operator-gated) | build OK | tests 4/4 PASS | add-a-page dry run (TODO-03 page in a scratch clone) reaches `site: OK` with and without `--skip-stats`
> **Quality reviewed:** 2026-09-28 | Codex 4x (adversarial x2, consistency, perf) | 6M+1L fixed, 0 open | scope: N/A (docs-only; no kernel, boot or desktop code)

---

## 4. Document: Infrastructure

> **Spawned-by:** root

Write docs pages that meet the §3 contract for the 9 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/infrastructure/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/00-infrastructure/TODO-01-developer-tooling-stack.md` (TODO-01 -- Developer Tooling Stack): `developer-tooling-stack.md`
  - `todo/00-infrastructure/TODO-02-ai-development-system.md` (TODO-02 -- AI Development System): `ai-development-system.md`
  - `todo/00-infrastructure/TODO-03-kernel-test-harness.md` (TODO-03 -- Kernel Test Harness): `kernel-test-harness.md`
- [x] Pages in `docs/infrastructure/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/00-infrastructure/TODO-04-usermode-test-framework.md` (TODO-04 -- User-Mode Test Framework): `usermode-test-framework.md`
  - `todo/00-infrastructure/TODO-05-desktop-ui-test-framework.md` (TODO-05 -- Desktop & UI Test Framework): `desktop-ui-test-framework.md`
  - `todo/00-infrastructure/TODO-06-todo-metadata-layer.md` (TODO-06 -- TODO Metadata Layer and Derived Graph): `todo-metadata-layer.md`
- [x] Pages in `docs/infrastructure/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/00-infrastructure/TODO-07-lsp-mcp-bridge.md` (TODO-07 -- LSP to MCP Bridge): `lsp-mcp-bridge.md`
  - `todo/00-infrastructure/TODO-08-automation-hardening.md` (TODO-08 -- Automation Hardening (Skill / Hook / MCP / Codex Integration)): `automation-hardening.md`
  - `todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md` (TODO-09 -- Repository Transfer to rizonetech): `repository-transfer.md`
- [x] The seven files §2 had mapped kept their reference pages, none of which followed the §3 part order, so each gained a contract-shaped overview page linking them
  - Rewriting 13,943-word `development-tooling.md` or the transfer runbooks into the contract order would have destroyed their reference structure; the overview carries Limits, comparison and See also, and links the reference as the deep dive.
  - `github-setup.md`: corrected the stale "`www` CNAME still targets `rizonetech.github.io`" line (a public resolver returned `rizonesoft.github.io` on 2026-09-28).
- [x] Added every new page to `docs/infrastructure/index.md` (new Roadmap Overviews table), then ran `python3 scripts/site/build.py --update-baseline` (196 to 194 entries)
- [x] Commit: `"docs: infrastructure documentation pages"` (`dfcd162db`)

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check --skip-stats` prints `site: OK` with 38/232 TODO files documented; `python3 scripts/site/build.py` renders all nine pages under `build/site/docs/infrastructure/`

> **Notes:**
> - **What shipped:** nine contract-shaped overview pages in `docs/infrastructure/`, one per infrastructure roadmap file, plus a Roadmap Overviews table in the folder index.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so §22's freshness check flags it when the scripts or headers it describes change.
> - **Downstream:** the reference pages §2 mapped keep their `covers=` claims; the overviews link them rather than copying them.
> - **Found on the way:** `github-setup.md` still said the `www` CNAME pointed at `rizonetech` (fixed); the TODO-09 validation tail asserts pre-move-back state (filed operator-gated in TODO-09 §8).
> - **Scope boundary:** the eight host-tools pages moved to §26.

> **Verified:** 2026-09-28 | commit `dfcd162db` | 6/6 items | build OK | site: OK, 38/232 documented; tests 34646 kernel + 17 user-mode PASS; smoke PASS
> **Quality reviewed:** 2026-09-28 | Codex 5x (adversarial x2, adversarial post-ship, consistency, perf) | 13M fixed, 0 open | scope: N/A (docs-only; re-adversarial skipped: docs-only fixes)


---

## 5. Document: Boot platform, part 1

> **Spawned-by:** root

Write docs pages that meet the §3 contract for the 10 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/boot/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md` (TODO-01 -- Boot Protocol ABI & Handoff Contract): `boot-protocol-abi-overview.md`
  - `todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md` (TODO-02 -- UEFI Bootloader Hardening & Secure Boot): `uefi-hardening-overview.md`
  - `todo/01-boot-platform/TODO-03-bootloader-error-recovery.md` (TODO-03 -- Bootloader Error Recovery & ELF Hardening): `bootloader-error-recovery.md`
- [x] Pages in `docs/boot/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/01-boot-platform/TODO-04-firmware-table-platform-inventory.md` (TODO-04 -- Firmware Table & Platform Inventory): `firmware-platform-inventory.md`
  - `todo/01-boot-platform/TODO-05-boot-device-discovery.md` (TODO-05 -- Boot Device Discovery & Fallback Chain): `boot-device-discovery.md`
  - `todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md` (TODO-06 -- Boot Media, Image Pipeline & Installer Handoff): `boot-media-image-pipeline.md`
- [x] Pages in `docs/boot/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md` (TODO-07 -- Boot Entry Store, Menu & Policy): `boot-entries-menu-policy.md`
  - `todo/01-boot-platform/TODO-08-alternate-boot-protocols.md` (TODO-08 -- Alternate Boot Protocols & Compatibility Boundary): `alternate-boot-protocols.md`
  - `todo/01-boot-platform/TODO-09-cpu-boot-sequencing.md` (TODO-09 -- CPU Boot Sequencing & AP Hardening): `cpu-boot-sequencing.md`
- [x] Pages in `docs/boot/` for the next 1 roadmap files, each with its `covers=` directive
  - `todo/01-boot-platform/TODO-10-bare-metal-hardening.md` (TODO-10 -- Bare Metal Boot Hardening): `bare-metal-hardening.md`
- [x] The eight files §2 had mapped kept their reference pages, none in the §3 part order, so each gained a contract-shaped overview page linking them; TODO-05 and TODO-09 had no page at all
  - Every command, script and serial line the pages quote was checked against the tree (the drafted `bootcfg` flags, a `Booted from:` log line and a `uefi_runtime.c` loader-variable claim were wrong and were corrected).
- [x] Created `docs/boot/index.md` (the folder had 19 pages and no index) with Roadmap Overviews and Reference Documents tables, added Boot Platform to `docs/index.md`, then ran `python3 scripts/site/build.py --update-baseline` (194 to 192 entries)
- [x] Roadmap drift found while writing, fixed in place: TODO-01 §1 IO row `[/]` to `[x]` (its follow-up shipped as §21), TODO-04 Verification OVMF item `[/]` to `[x]` (JSON disk write live), TODO-06 OS Comparison UKI and build-host rows
- [x] Commit: `"docs: boot platform, part 1 documentation pages"` (`305f71470`)

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check --skip-stats` prints `site: OK` with 40/232 TODO files documented; `python3 scripts/site/build.py --out /tmp/s5site` renders all ten pages plus `index.html` under `docs/boot/`

> **Notes:**
> - **What shipped:** ten contract-shaped overview pages in `docs/boot/`, one per boot-platform roadmap file 01 to 10, plus the folder's first `index.md`.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so §22's freshness check flags it when the code it describes changes.
> - **Downstream:** the 19 reference pages keep their `covers=` claims; the overviews link them as deep dives rather than copying them.
> - **Found on the way:** three stale roadmap claims (TODO-01 §1 IO row, TODO-04 Verification, TODO-06 OS Comparison) corrected in the same commit.

> **Verified:** 2026-09-28 | commit `305f71470` | 8/8 items | build OK | site: OK, 40/232 documented; tests 34646 kernel + 17 user-mode PASS; smoke PASS
> **Quality reviewed:** 2026-09-28 | Codex 4x (adversarial, adversarial post-ship, consistency, perf) | 14M fixed, 0 open | scope: N/A (docs-only; re-adversarial skipped: docs-only fixes)

---

## 6. Document: Boot platform, part 2

Write docs pages that meet the §3 contract for the 10 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/boot/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/01-boot-platform/TODO-11-interrupt-timer-arch.md` (TODO-11 -- Interrupt Architecture & Unified Timer Subsystem): `interrupt-timer-architecture.md`
  - `todo/01-boot-platform/TODO-12-early-entropy-random-seed.md` (TODO-12 -- Early Entropy & Random Seed Handoff): `early-entropy-random-seed.md`
  - `todo/01-boot-platform/TODO-13-tpm-measured-boot-attestation.md` (TODO-13 -- TPM Measured Boot, PCR Replay & Attestation): `tpm-measured-boot.md`
- [x] Pages in `docs/boot/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/01-boot-platform/TODO-14-boot-diagnostics.md` (TODO-14 -- Boot Diagnostics, Heartbeat & Spinner): `boot-diagnostics.md`
  - `todo/01-boot-platform/TODO-15-visual-post-display.md` (TODO-15 -- Visual POST Display (VPD)): `visual-post-display.md`
  - `todo/01-boot-platform/TODO-16-nvme-storage.md` (TODO-16 -- NVMe Storage Driver (Boot-Critical)): `nvme-boot-storage.md`
- [x] Pages in `docs/boot/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/01-boot-platform/TODO-17-xhci-usb-boot.md` (TODO-17 -- xHCI, USB Storage & USB HID (Boot-Critical)): `xhci-usb-boot.md`
  - `todo/01-boot-platform/TODO-18-usb-hid-keyboard-mouse.md` (TODO-18 -- USB HID Boot-Protocol Keyboard & Mouse): `usb-hid-boot-protocol.md`
  - `todo/01-boot-platform/TODO-19-usb-boot-hardening.md` (TODO-19 -- USB Boot Hardening & Fail-Safe Pipeline): `usb-boot-hardening.md`
- [x] Pages in `docs/boot/` for the next 1 roadmap files, each with its `covers=` directive
  - `todo/01-boot-platform/TODO-20-usb-zero-delay-handover.md` (TODO-20 -- Zero-Delay USB Boot (Pre-ExitBootServices Driver Loading)): `usb-zero-delay-handover.md`
- [x] TODO-13 and TODO-14 had reference pages outside the §3 part order (`pcr-allocation.md`, `boot-timeline-schema.md`), so each gained an overview linking them; the other eight had no page
  - Every quoted function, constant, serial line, anchor and `sources=` path was checked against the tree before review; Codex adversarial then found 8 medium overclaims (fresh xHCI ring allocation on inherit, headless-token enrollment, canary seeding order, no-MSI hot-plug, and four more), all fixed.
- [x] Added the ten pages to `docs/boot/index.md` Roadmap Overviews, then ran `python3 scripts/site/build.py --update-baseline` (192 to 184 entries)
- [x] Roadmap drift found while writing, fixed in place in TODO-15, TODO-16 and TODO-17
  - TODO-15 and TODO-16 "Current state" callouts said their test files were missing; TODO-15 cited a nonexistent `Last POST code:` serial line; TODO-17 sections 1-4 cited POST codes `0xD700`-`0xD703` that never shipped.
- [x] Commit: `"docs: boot platform, part 2 documentation pages"` (`9f9356c89`)

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check --skip-stats` prints `site: OK` with 48/232 TODO files documented

> **Notes:**
> - **What shipped:** ten contract-shaped overview pages in `docs/boot/`, one per boot-platform roadmap file 11 to 20, each listed in `docs/boot/index.md`.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so §22's freshness check flags it when the code it describes changes.
> - **Downstream:** the TPM and diagnostics overviews link the existing `pcr-allocation.md` and `boot-timeline-schema.md` reference pages rather than copying them.
> - **Found on the way:** stale test-file, serial-line and POST-code claims in TODO-15, TODO-16 and TODO-17 corrected in the same commit.

> **Verified:** 2026-09-28 | commit `9f9356c89` | 8/8 items | build OK | site: OK, 48/232 documented; tests 34646 kernel + 17 user-mode PASS; smoke PASS
> **Quality reviewed:** 2026-09-28 | Codex 5x (adversarial x2, adversarial post-ship, consistency, perf) | 15M fixed, 0 open | scope: N/A (docs-only; re-adversarial skipped: docs-only fixes)

---

## 7. Document: Boot platform, part 3

Write docs pages that meet the §3 contract for the 9 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/boot/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/01-boot-platform/TODO-21-ab-boot-rollback.md` (TODO-21 -- A/B Dual-Slot Boot & Automatic Rollback): `ab-boot-rollback.md`
  - `todo/01-boot-platform/TODO-22-recovery-partition.md` (TODO-22 -- Recovery Partition & Self-Repair): `recovery-partition.md`
  - `todo/01-boot-platform/TODO-23-boot-watchdog.md` (TODO-23 -- Boot Watchdog & Hang Detection): `boot-watchdog.md`
- [x] Pages in `docs/boot/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/01-boot-platform/TODO-24-blackbox-service-partition.md` (TODO-24 -- BlackBox Service Partition): `blackbox-service-partition.md`
  - `todo/01-boot-platform/TODO-25-network-pxe-http-boot.md` (TODO-25 -- Network / PXE / HTTP Boot): `network-boot.md`
  - `todo/01-boot-platform/TODO-26-hibernation-resume-fast-startup-handoff.md` (TODO-26 -- Hibernation Resume & Fast Startup Boot Handoff): `hibernation-resume-handoff.md`
- [x] Pages in `docs/boot/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/01-boot-platform/TODO-27-uefi-advanced.md` (TODO-27 -- UEFI Advanced Features): `uefi-advanced.md`
  - `todo/01-boot-platform/TODO-28-boot-validation-certification-matrix.md` (TODO-28 -- Boot Validation & Hardware Certification Matrix): `boot-validation-matrix.md`
  - `todo/01-boot-platform/TODO-29-boot-perf-health-observability.md` (TODO-29 -- Boot Performance & Health Observability): `boot-performance-health.md`
- [x] TODO-24, TODO-28 and TODO-29 had reference pages outside the §3 part order (`black-box-artifacts.md`, `../hardware/boot-lab.md`, the health and trend schemas), so each gained an overview linking them; the other six had no page
  - Every quoted function, constant, serial line, anchor and `sources=` path was checked against the tree; the drafts' partition counts (three or five) were wrong, since the default build passes `--ab` and produces six.
- [x] Added the nine pages to `docs/boot/index.md` Roadmap Overviews, then ran `python3 scripts/site/build.py --update-baseline` (184 to 178 entries)
- [x] Roadmap drift found while writing, fixed in place in TODO-21, TODO-27 and `tools/make-system-disk.c`
  - TODO-21 §2 Notes still called `--ab` dormant; the tool's `--ab` help said 4 partitions.
  - TODO-27 §2 named a nonexistent `firmware_cmd.c` and sent two follow-ups to a "§8" that did not exist; that is now TODO-27 §8, with reciprocal XREFs to `07-networking/TODO-03 §7` and the gzip owner `02-kernel-core/TODO-03 §4`.
  - Review found FAT32 replace-existing deletes the old file before the rename can fail, so the boot-trend publish is not atomic; filed in `05-storage-filesystems/TODO-04 §18`.
- [x] Commit: `"docs: boot platform, part 3 documentation pages"` (`58653c408`)

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check --skip-stats` prints `site: OK` with 54/232 TODO files documented

> **Notes:**
> - **What shipped:** nine contract-shaped overview pages in `docs/boot/`, one per boot-platform roadmap file 21 to 29, each listed in `docs/boot/index.md`.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so §22's freshness check flags it when the code it describes changes.
> - **Downstream:** the BlackBox, boot-lab and health/trend reference pages keep their `covers=` claims; the overviews link them rather than copying them.
> - **Found on the way:** TODO-27 gained §8 as the real owner of its dangling "§8" follow-ups; TODO-21 notes and the `make-system-disk` help text were corrected.
> - **Filed:** FAT32 replace-existing is not crash-safe (`05-storage-filesystems/TODO-04 §18`); `boot_reliability.py`'s default `--row-id` is not a matrix row (`01-boot-platform/TODO-28 §9`).

> **Verified:** 2026-09-28 | commit `58653c408` | 7/7 items | build OK | site: OK, 54/232 documented; tests 34646 kernel + 17 user-mode PASS; smoke PASS
> **Quality reviewed:** 2026-09-28 | Codex 5x (adversarial x2, adversarial post-ship, consistency, perf) | 13M fixed, 0 open | scope: N/A (docs-only; re-adversarial skipped: docs-only fixes)

---

## 8. Document: Kernel core, part 1

Write docs pages that meet the §3 contract for the 12 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/kernel/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/02-kernel-core/TODO-01-kernel-init-sequencing.md` (TODO-01 -- Kernel Init Sequencing): `kernel-init-sequencing.md`
  - `todo/02-kernel-core/TODO-02-kernel-configuration-policy.md` (TODO-02 -- Kernel Configuration & Policy Plane): `kernel-configuration-policy.md`
  - `todo/02-kernel-core/TODO-03-kernel-libraries.md` (TODO-03 -- Kernel Embedded Libraries): `kernel-libraries.md`
- [x] Pages in `docs/kernel/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/02-kernel-core/TODO-04-system-logging.md` (TODO-04 -- System Logging): `system-logging.md`
  - `todo/02-kernel-core/TODO-05-object-manager.md` (TODO-05 -- Object Manager): `object-manager.md`
  - `todo/02-kernel-core/TODO-06-executive-support-runtime.md` (TODO-06 -- Executive Support Runtime): `executive-support-runtime.md`
- [x] Pages in `docs/kernel/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/02-kernel-core/TODO-07-irql-model-dpcs.md` (TODO-07 -- IRQL Model & DPCs): `irql-dpc.md`
  - `todo/02-kernel-core/TODO-08-time-filetime-management.md` (TODO-08 -- Time & FILETIME Management): `time-filetime.md`
  - `todo/02-kernel-core/TODO-09-x86-64-architecture.md` (TODO-09 -- x86-64 Architecture Enhancements): `x86-64-architecture.md`
- [x] Pages in `docs/kernel/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/02-kernel-core/TODO-10-kernel-security-hardening.md` (TODO-10 -- Kernel Security Hardening): `kernel-security-hardening.md`
  - `todo/02-kernel-core/TODO-11-peb-teb-user-abi.md` (TODO-11 -- PEB / TEB & User-Mode ABI): `peb-teb-user-abi.md`
  - `todo/02-kernel-core/TODO-12-native-api-ssdt.md` (TODO-12 -- Native API Layer (Nt/Zw)): `native-api-ssdt.md`
- [x] No kernel-core file had a page before, so all twelve are new; `docs/kernel/index.md` lost its placeholder table of subdirectories that never existed
  - Every quoted function, constant, serial line, anchor and `sources=` path was checked against the tree; the drafts overcounted shipped sections in four files and missed shipped KUSD policy bits and the LZ4 log-rotation consumer.
- [x] Added the twelve pages to `docs/kernel/index.md` Roadmap Overviews, then ran `python3 scripts/site/build.py --update-baseline` (178 to 166 entries)
- [x] Roadmap drift found while writing, fixed in place in TODO-01, 05, 08, 10, 11, 12 and `include/kernel/nt/service_numbers.h`
  - TODO-01 said 27 subsystems (30 today); TODO-05 said 13 object types (15 with Token and Job); TODO-12 and the header comment said 475 services (477); TODO-08 named a nonexistent `KeQueryPerformanceCounter` and showed timezone and NTFS rows as not done.
  - TODO-10 section 13's guard-table capacity and SMP items and TODO-11 section 11's KUSD policy-bits item had shipped; each now `[x]` with evidence.
- [x] Commit: `"docs: kernel core, part 1 documentation pages"` (`076d48750`)

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check --skip-stats` prints `site: OK` with 66/232 TODO files documented

> **Notes:**
> - **What shipped:** twelve contract-shaped overview pages in `docs/kernel/`, one per kernel-core roadmap file 01 to 12, each listed in `docs/kernel/index.md`.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so §22's freshness check flags it when the code it describes changes.
> - **Downstream:** the limits sections link existing owner items (TODO-12 §3 stack arguments, §6 IOSB write-back, TODO-10 §13 user-process stack guards) rather than filing duplicates.
> - **Found on the way:** count and status drift corrected in six kernel-core roadmap files and one header comment.

> **Verified:** 2026-09-28 | commit `076d48750` | 8/8 items | build OK | site: OK, 66/232 documented; tests 34646 kernel + 17 user-mode PASS
> **Quality reviewed:** 2026-09-28 | Codex 5x (adversarial x2, adversarial post-ship, consistency, perf) | 20M fixed, 0 open | scope: kernel-quality-auditor on the header comment, 0 findings (re-adversarial skipped: docs-only fixes)

---

## 9. Document: Kernel core, part 2

Write docs pages that meet the §3 contract for the 12 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/kernel/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/02-kernel-core/TODO-13-atom-nls-locale-subsystem.md` (TODO-13 -- Atom, NLS & Locale Subsystem): `atom-nls-locale.md`
  - `todo/02-kernel-core/TODO-14-registry-completion.md` (TODO-14 -- Registry System Completion): `registry.md`
  - `todo/02-kernel-core/TODO-15-security-reference-monitor.md` (TODO-15 -- Security Reference Monitor): `security-reference-monitor.md`
- [x] Pages in `docs/kernel/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/02-kernel-core/TODO-16-kernel-notification-facility.md` (TODO-16 -- Kernel Notification Facility): `kernel-notification-facility.md`
  - `todo/02-kernel-core/TODO-17-binary-system.md` (TODO-17 -- Binary Format System (exec_load / ELF / PE32+ / EIF)): `binary-format-system.md`
  - `todo/02-kernel-core/TODO-18-kernel-image-module-registry.md` (TODO-18 -- Kernel Image & Module Registry): `kernel-image-module-registry.md`
- [x] Pages in `docs/kernel/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/02-kernel-core/TODO-19-code-integrity-trust-policy.md` (TODO-19 -- Code Integrity & Trust Policy): `code-integrity-trust-policy.md`
  - `todo/02-kernel-core/TODO-20-eif-full-implementation.md` (TODO-20 -- EIF Full Implementation): `eif-executable-format.md`
  - `todo/02-kernel-core/TODO-21-process-model-extensions.md` (TODO-21 -- Process Model Extensions): `process-model-extensions.md`
- [x] Pages in `docs/kernel/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/02-kernel-core/TODO-22-environment-variables.md` (TODO-22 -- Environment Variables & Process Arguments): `environment-variables.md`
  - `todo/02-kernel-core/TODO-23-exception-dispatch-seh.md` (TODO-23 -- Exception Dispatch & SEH): `exception-dispatch-seh.md`
  - `todo/02-kernel-core/TODO-24-alpc-message-ports.md` (TODO-24 -- ALPC / Message Ports): `alpc-message-ports.md`
- [x] No file in this range had a page before, so all twelve are new; every quoted symbol was checked to exist in the tree and every roadmap link fragment against the target heading's GitHub slug
  - The drafts overcounted the ALPC stubs (ten, not eleven), the privilege LUIDs (25) and the PE export tables (14 kernel32, 96 ntdll), and cited a nonexistent `reg_resolve_path()` and a `CurrentControlSet` link.
  - Review corrected the per-process page-table story (every process has its own PML4; what is missing is per-segment mapping), the exec no-rollback contract, the env block's ASCII-only fold, and the `cmd.exe` shell that does exist.
- [x] Added the twelve pages to `docs/kernel/index.md` Roadmap Overviews, then ran `python3 scripts/site/build.py --update-baseline` (166 to 154 entries)
- [x] Roadmap drift found while writing, fixed in place in TODO-14, 16, 17 and 23
  - TODO-14 put `registry.c` at 2,536 lines (4,293); TODO-16 and TODO-17 quoted serial lines that do not match the code, and TODO-17 §5 cited POST16 codes `0xD807`/`0xD808` that never shipped.
  - TODO-23's Outcome named a nonexistent `src/kernel/rtl/seh.c` and called kernel SEH a SCOPE_TABLE walker; it is a registration list in `except.c`.
- [x] Commit: `"docs: kernel core, part 2 documentation pages"`

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check` prints `site: OK` with 78/232 TODO files documented

> **Notes:**
> - **What shipped:** twelve contract-shaped overview pages in `docs/kernel/`, one per kernel-core roadmap file 13 to 24, each listed in `docs/kernel/index.md`.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so §22's freshness check flags it when the code it describes changes.
> - **Downstream:** limits sections link existing owner sections (TODO-12 §7 environment inheritance, `09-desktop-shell/TODO-07` §1 CNG primitives) rather than filing duplicates.
> - **Found on the way:** `build.py --check` validates fragments only for links into `docs/`, not into `todo/`, so a wrong roadmap anchor passes the site gate; six were caught by hand here and the check is filed in §23.
> - **Filed:** journal replay that invalidates the log before checking its copy (`02-kernel-core/TODO-35` §1); ring-3 ALPC QoS was already owned by TODO-35 §5.

> **Verified:** 2026-09-28 | commit `8483ac3bc` | 8/8 items | build OK | site: OK, 78/232 documented; tests 34646 kernel + 17 user-mode PASS
> **Quality reviewed:** 2026-09-28 | Codex 5x (adversarial x2, adversarial post-ship, consistency, perf) | 22M fixed, 0 open | scope: N/A (docs-only; no source changed; re-adversarial skipped: docs-only fixes)

---

## 10. Document: Kernel core, part 3

Write docs pages that meet the §3 contract for the 12 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/kernel/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md` (TODO-25 -- Kernel Resource Accounting & Quotas): `kernel-resource-accounting-quotas.md`
  - `todo/02-kernel-core/TODO-26-power-management.md` (TODO-26 -- Power Management (S-States, D-States, Thermal & Idle)): `power-management.md`
  - `todo/02-kernel-core/TODO-27-crash-dump-generation.md` (TODO-27 -- Crash Dump Generation): `crash-dump-generation.md`
- [x] Pages in `docs/kernel/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/02-kernel-core/TODO-28-bsod-ux-enhancements.md` (TODO-28: BSOD / Panic Screen & Crash Experience): `panic-screen-crash-experience.md`
  - `todo/02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md` (TODO-29 -- Kernel Debugger (KD Protocol)): `kernel-debugger-kd.md`
  - `todo/02-kernel-core/TODO-30-system-health-recovery-orchestrator.md` (TODO-30 -- System Health & Recovery Orchestrator): `system-health-recovery.md`
- [x] Pages in `docs/kernel/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/02-kernel-core/TODO-31-kernel-bulletproofing.md` (TODO-31 -- Kernel Bulletproofing): `kernel-bulletproofing.md`
  - `todo/02-kernel-core/TODO-32-kernel-logging-v2-lockless.md` (TODO-32 -- Kernel Logging v2: Lockless, Priority-Lanes, Fail-Proof): `kernel-logging-v2.md`
  - `todo/02-kernel-core/TODO-33-higher-half-kernel-relocation.md` (TODO-33 -- Higher-Half Kernel Relocation): `higher-half-kernel.md`
- [x] Pages in `docs/kernel/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/02-kernel-core/TODO-34-serial-log-signal-to-noise.md` (TODO-34 -- Serial Log Signal-to-Noise and Log-Cleanliness Gate): `serial-log-cleanliness.md`
  - `todo/02-kernel-core/TODO-35-unblocked-deferral-backfill.md` (TODO-35 -- Unblocked-Deferral Backfill (2026-07-27 cohort)): `unblocked-deferral-backfill.md`
  - `todo/02-kernel-core/TODO-A-SSDT-Master-Table.md` (SSDT Master Table): `ssdt-master-table.md`
- [x] TODO-33 was claimed by `docs/infrastructure/kernel-address-space.md`, a design doc outside the contract (no limits or comparison part, over 2,000 words), so it gained a contract page; the other eleven had no page
  - Every quoted symbol, constant and serial string was checked in the tree and every roadmap fragment against its heading's slug; drafts named a nonexistent `panic_had_prior_crash()` (it is `panic_had_previous_crash()`) and a `KiDebugRoutine` variable (the hook is `ki_set_debug_routine()`).
  - Figures are sourced: the SSDT ledger counts were recounted from its rows, and the higher-half page quotes `__kernel_end` = `0x7da000` from `llvm-nm-19 build/kernel.exe` at `f26c21502` rather than the roadmap's July headroom figure.
- [x] Added the twelve pages to `docs/kernel/index.md` Roadmap Overviews, then ran `python3 scripts/site/build.py --update-baseline` (154 to 143 entries; TODO-33 was never in the baseline)
- [x] Roadmap drift found while writing, fixed in place in TODO-28, 29, 31 and TODO-A
  - TODO-28 put `panic.c` at 975 lines with a Selawik TTF font (3,528 lines, console bitmap font); TODO-29 put `serial.c` at ~110 lines fixed at 38400 baud (2,347 lines, keeps the firmware divisor).
  - TODO-31 quoted `SSDT_MAIN_COUNT = 475` (477); TODO-A's progress line read 146/477 wired while its rows count 214; review downgraded `NtSaveKey`/`NtSaveKeyEx`/`NtRestoreKey` (handler returns `STATUS_NOT_SUPPORTED`), leaving 155 `[x]` and 59 `[/]`.
- [x] Commit: `"docs: kernel core, part 3 documentation pages"`

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check` prints `site: OK` with 89/232 TODO files documented

> **Notes:**
> - **What shipped:** twelve contract-shaped overview pages in `docs/kernel/`, one per kernel-core roadmap file 25 to 35 plus the SSDT ledger, each listed in `docs/kernel/index.md`.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so the freshness check flags it when the code it describes changes.
> - **Status honesty:** five of the twelve roadmaps (KD, health orchestrator, logging v2, serial-log gate, deferral backfill) are almost wholly unbuilt, and their pages say so in the first paragraph rather than describing the plan as behaviour.
> - **Found on the way:** roadmap text had drifted from the code in TODO-28, 29, 31, 34 and TODO-A, and was corrected in place, including TODO-34's smoke gate (shipped in `6fbf3719a`) and TODO-31's `kernel_config_t` guards; no new work needed filing.

> **Verified:** 2026-09-28 | commit `d551d8272` | 8/8 items | build OK | site: OK, 89/232 documented; tests 34646 kernel + 17 user-mode PASS
> **Quality reviewed:** 2026-09-28 | Codex 5x (adversarial x2, adversarial post-ship, consistency, perf) | 22M fixed, 0 open | scope: N/A (docs-only; no source changed; re-adversarial skipped: docs-only fixes)

---

## 11. Document: Memory and concurrency

Write docs pages that meet the §3 contract for the 11 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/memory/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/03-memory-concurrency/TODO-01-vmm-memory-protection.md` (TODO-01 -- VMM Memory Protection & Diagnostics): `vmm-memory-protection.md`
  - `todo/03-memory-concurrency/TODO-02-memory-security.md` (TODO-02 -- Memory Security Hardening): `memory-security.md`
  - `todo/03-memory-concurrency/TODO-03-advanced-allocator.md` (TODO-03 -- Advanced Kernel Allocator): `advanced-allocator.md`
- [x] Pages in `docs/memory/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/03-memory-concurrency/TODO-04-pager-reclaim-working-set.md` (TODO-04 -- Pager, Reclaim, and Working Set Manager): `pager-reclaim-working-set.md`
  - `todo/03-memory-concurrency/TODO-05-advanced-virtual-memory.md` (TODO-05 -- Advanced Virtual Memory): `advanced-virtual-memory.md`
  - `todo/03-memory-concurrency/TODO-06-scheduler-enhancement.md` (TODO-06 -- Scheduler Enhancement): `scheduler.md`
- [x] Pages in `docs/memory/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/03-memory-concurrency/TODO-07-smp-phase2.md` (TODO-07 -- SMP Phase 2): `smp-phase2.md`
  - `todo/03-memory-concurrency/TODO-08-advanced-sync.md` (TODO-08 -- Advanced Synchronisation Primitives): `advanced-sync.md`
  - `todo/03-memory-concurrency/TODO-09-win32-ipc-extensions.md` (TODO-09 -- Win32 IPC Extensions & Async I/O): `win32-ipc-extensions.md`
- [x] Pages in `docs/memory/` for the next 2 roadmap files, each with its `covers=` directive
  - `todo/03-memory-concurrency/TODO-10-concurrency-diagnostics.md` (TODO-10 -- Concurrency & Memory Diagnostics): `concurrency-diagnostics.md`
  - `todo/03-memory-concurrency/TODO-11-warm-kernel-update-runtime.md` (TODO-11: Warm-Kernel-Update Runtime): `warm-kernel-update.md`
- [x] No 03 roadmap file had a page, so all eleven are new; every quoted symbol, constant and log string was checked in the tree and every roadmap link against its heading's slug
  - Most roadmap Implementation Order rows are still `[ ]` while related code shipped under other owners (guard pages, heap redzones, 1 GiB pages, sections, Job Objects, IOCP pool, barriers), so each page describes that code and keeps the roadmap section open.
- [x] Created `docs/memory/index.md` listing the eleven pages, added a Memory and Concurrency row to `docs/index.md`, then ran `python3 scripts/site/build.py --update-baseline` (143 to 132 entries)
- [x] Roadmap drift found while writing, fixed in place in TODO-04, 05, 06, 07, 10 and 11
  - TODO-04 claimed a working swap pager: `swap_init()` has no caller outside `test_swap.c`, so a normal boot never swaps; the current-state line now says so and names the §1 owner.
  - TODO-06 described a hardcoded LAPIC ICR (it is calibrated by `lapic_timer_calibrate()`); TODO-07 quoted TLB vector `0xE0` (`0xFE`) and nonexistent `smp/smp.h`, `smp/barriers.h`, `sched.c` paths; TODO-05 cited `src/kernel/lz4` and stale `nt_section.c` lines.
  - TODO-10 said the default stack is 64 KB (8 KiB, already guarded by `task_create()`); TODO-11 counted 10 warm-update suites (14).
- [x] Commit: `"docs: memory and concurrency documentation pages"`

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check` prints `site: OK` with 100/232 TODO files documented

> **Notes:**
> - **What shipped:** eleven contract-shaped overview pages in a new `docs/memory/` folder, one per memory and concurrency roadmap file, with a folder index and a docs home row.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so the freshness check flags it when the code it describes changes.
> - **Status honesty:** only TODO-06 §11 is complete on its own table (TODO-01 §12 is partial); every page says what is built, what exists under another owner and what is only planned.
> - **Found on the way:** the swap pager is never initialised at boot, `smp_rendezvous_begin()` has no production caller, and the mutex wait queue and pipe slot claims are still unlocked; all four already have owning roadmap items.

> **Verified:** 2026-09-28 | commit `6bdf71330` | 8/8 items | build OK | site: OK, 100/232 documented; tests 34646 kernel + 17 user-mode PASS; smoke PASS
> **Quality reviewed:** 2026-09-28 | Codex 5x (adversarial x2, adversarial post-ship, consistency, perf) | 19M fixed, 0 open | scope: N/A (docs-only; no source changed; re-adversarial skipped: docs-only fixes)

---

## 12. Document: Drivers and hardware, part 1

Write docs pages that meet the §3 contract for the 13 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/hardware/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/04-drivers-hardware/TODO-01-pci-pcie-pnp-resource-manager.md` (TODO-01 -- PCI/PCIe, PnP & Resource Manager): `pci-pnp-resource-manager.md`
  - `todo/04-drivers-hardware/TODO-02-apic-interrupt-routing.md` (TODO-02 -- APIC Architecture & Advanced Interrupt Routing): `apic-interrupt-routing.md`
  - `todo/04-drivers-hardware/TODO-03-acpi-power-management.md` (TODO-03 -- ACPI Full Subsystem & Power Management): `acpi-power-management.md`
- [x] Pages in `docs/hardware/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/04-drivers-hardware/TODO-04-security-hardware.md` (TODO-04 -- Security Hardware & DMA Safety): `security-hardware.md`
  - `todo/04-drivers-hardware/TODO-05-kernel-module-system.md` (TODO-05 -- Kernel Module System): `kernel-modules.md`
  - `todo/04-drivers-hardware/TODO-06-firmware-loader-device-blobs.md` (TODO-06 -- Firmware Loader & Device Blob Policy): `firmware-loader.md`
- [x] Pages in `docs/hardware/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/04-drivers-hardware/TODO-07-device-manager.md` (TODO-07 -- Device Manager & Driver Diagnostics): `device-manager.md`
  - `todo/04-drivers-hardware/TODO-08-core-driver-enhancements.md` (TODO-08 -- Core Built-in Driver Enhancements): `core-drivers.md`
  - `todo/04-drivers-hardware/TODO-09-hypervisor-abstraction.md` (TODO-09 -- Hypervisor Abstraction Layer): `hypervisor-abstraction.md`
- [x] Pages in `docs/hardware/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/04-drivers-hardware/TODO-10-usb-stack.md` (TODO-10 -- USB Stack Completion): `usb-stack.md`
  - `todo/04-drivers-hardware/TODO-11-input-system.md` (TODO-11 -- Input System Enhancement): `input-system.md`
  - `todo/04-drivers-hardware/TODO-12-i2c-touchpad.md` (TODO-12 -- I2C/SMBus Bus & Precision Touchpad): `i2c-touchpad.md`
- [x] Pages in `docs/hardware/` for the next 1 roadmap files, each with its `covers=` directive
  - `todo/04-drivers-hardware/TODO-13-storage-controller-device-drivers.md` (TODO-13 -- Storage Controller & Removable Media Drivers): `storage-controllers.md`
- [x] No 04 roadmap file had a page, so all thirteen are new; every quoted symbol, constant and log string was checked in the tree and every roadmap anchor resolves under the site check
- [x] Rewrote `docs/hardware/index.md` with a Roadmap Overviews table, then ran `python3 scripts/site/build.py --update-baseline` (132 to 119 entries)
  - Its old `cpu/`, `bus/`, `firmware/` and `interrupts/` rows named folders that never existed; they now link `specs/hardware/` directly.
- [x] Roadmap drift found while writing, fixed in place in TODO-03, 08 and 09
  - TODO-09 claimed a VMBus core and storvsc were complete: no VMBus code exists (only a comment at `src/kernel/idt.c:9`), so the claim and the §10 dependency now say so.
  - TODO-08 cited a ~220-line PCI stub and an existing `acpi_get_mcfg()` (neither true) and now records that the HPET (`hpet_ns()`) and a polled NVMe driver already ship.
  - TODO-03 had the OSL at `src/kernel/acpi/acpi_osl.c` (it is `src/kernel/acpi_osl.c`) and still listed the PCI config lock as missing; only a native `pci_write8()` is.
- [x] Commit: `"docs: drivers and hardware, part 1 documentation pages"`

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.


> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check` prints `site: OK` with 113/232 TODO files documented

> **Notes:**
> - **What shipped:** thirteen contract-shaped overview pages in `docs/hardware/`, one per drivers and hardware roadmap file 01 to 13, listed in a rewritten `docs/hardware/index.md`.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so the freshness check flags it when the code it describes changes.
> - **Status honesty:** no 04 roadmap has a complete section; each page separates what ships (often under a boot or kernel roadmap), what is partial and what is only planned.
> - **Found on the way:** ACPICA is linked but never initialised, SMEP and SMAP are always skipped, xHCI without MSI never enumerates a hot-plugged device, and the FADT MSI prohibition is not enforced; all are open items in their owning roadmaps.

> **Verified:** 2026-09-28 | commit `ab6850c3f` | 9/9 items | build OK | site: OK, 113/232 documented; tests 34646 kernel + 17 user-mode PASS; smoke PASS
> **Quality reviewed:** 2026-09-28 | Codex 4x (adversarial, adversarial post-ship, consistency, perf) | 12M fixed, 0 open | scope: N/A (docs-only; no source changed; re-adversarial skipped: docs-only fixes)

---

## 13. Document: Drivers and hardware, part 2

Write docs pages that meet the §3 contract for the 12 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/hardware/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/04-drivers-hardware/TODO-14-network-drivers.md` (TODO-14 -- Network Drivers): `network-drivers.md`
  - `todo/04-drivers-hardware/TODO-15-wifi-drivers.md` (TODO-15 -- WiFi Hardware Drivers): `wifi-drivers.md`
  - `todo/04-drivers-hardware/TODO-16-bluetooth.md` (TODO-16 -- Bluetooth Full Stack): `bluetooth.md`
- [x] Pages in `docs/hardware/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/04-drivers-hardware/TODO-17-gpu-display-drivers.md` (TODO-17 -- GPU & Display Drivers): `gpu-display-drivers.md`
  - `todo/04-drivers-hardware/TODO-18-audio-drivers.md` (TODO-18 -- Audio Drivers): `audio-drivers.md`
  - `todo/04-drivers-hardware/TODO-19-hardware-monitoring-sensors.md` (TODO-19 -- Hardware Monitoring, Sensors & Environmental Devices): `hardware-monitoring-sensors.md`
- [x] Pages in `docs/hardware/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/04-drivers-hardware/TODO-20-serial-parallel-debug-io.md` (TODO-20 -- Serial, Parallel, GPIO/SPI & Debug I/O Devices): `serial-parallel-debug-io.md`
  - `todo/04-drivers-hardware/TODO-21-game-controller-haptics.md` (TODO-21 -- Game Controllers, HID Force Feedback & Haptics): `game-controllers.md`
  - `todo/04-drivers-hardware/TODO-22-camera-imaging-devices.md` (TODO-22 -- Camera, Video Capture & Imaging Devices): `camera-imaging.md`
- [x] Pages in `docs/hardware/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/04-drivers-hardware/TODO-23-printing-scanning-device-path.md` (TODO-23 -- Printing, Scanning & Imaging Peripheral Device Path): `printing-scanning.md`
  - `todo/04-drivers-hardware/TODO-24-docking-thunderbolt-usb4-expansion.md` (TODO-24 -- Docking, Thunderbolt, USB4 & External Expansion): `docking-thunderbolt-usb4.md`
  - `todo/04-drivers-hardware/TODO-25-driver-hardware-certification-matrix.md` (TODO-25 -- Driver Hardware Certification Matrix): `driver-certification-matrix.md`
- [x] No roadmap file had a page, so all twelve are new; every quoted symbol, constant and log string was checked in the tree and every roadmap anchor resolves under the site check
- [x] Added every new page to the Roadmap Overviews table in `docs/hardware/index.md`, then ran `python3 scripts/site/build.py --update-baseline` (119 to 107 entries)
- [x] Roadmap drift found while writing, fixed in place in TODO-14, 15, 16, 17 and 19
  - TODO-14 and 15 named a receive hook (`ethernet_receive()`, `net_receive_ethernet()`) and `virtio_init_device()` that do not exist; they now name `net_rx()` and the real VirtIO calls, and TODO-14 §2 owns the missing `net_ops_t` table.
  - TODO-16 called `bt_hci_usb.c` an existing stub; it is not written. TODO-17 claimed DISPI mode-setting, `g_fb` and `memcpy_to_fb` (none exist) and marked its fallback Done.
  - TODO-19 said ACPI already covers battery and thermal policy; only a disabled EC transport exists.
- [x] Commit: `"docs: drivers and hardware, part 2 documentation pages"`

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check` prints `site: OK` with 125/232 TODO files documented

> **Notes:**
> - **What shipped:** twelve contract-shaped overview pages in `docs/hardware/`, one per drivers and hardware roadmap file 14 to 25, listed in `docs/hardware/index.md`.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so the freshness check flags it when the code it describes changes.
> - **Status honesty:** none of the twelve roadmaps has a shipped section; each page says what works today (the RTL8139, the GOP framebuffer, the console UART, the disabled EC) and why the rest does not.
> - **Found on the way:** the Ethernet layer is hard-wired to the RTL8139, so no second NIC can register until TODO-14 §2 adds a driver table.

> **Verified:** 2026-09-28 | commit `49050d9ba` | 8/8 items | build OK | site: OK, 125/232 documented; tests 34646 kernel + 17 user-mode PASS
> **Quality reviewed:** 2026-09-28 | Codex 4x (adversarial, adversarial post-ship, consistency, perf) | 14M fixed, 0 open | scope: N/A (docs-only; no source changed; re-adversarial skipped: docs-only fixes)

---

## 14. Document: Storage and filesystems

Write docs pages that meet the §3 contract for the 14 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/storage/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/05-storage-filesystems/TODO-01-block-storage-hardening.md` (TODO-01 -- Block Storage Hardening): `block-storage-hardening.md`
  - `todo/05-storage-filesystems/TODO-02-ntfs-readwrite.md` (TODO-02 -- NTFS Read/Write Driver): `ntfs.md`
  - `todo/05-storage-filesystems/TODO-03-volume-management-automount.md` (TODO-03 -- Volume Management & Auto-mount): `volume-management.md`
- [x] Pages in `docs/storage/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/05-storage-filesystems/TODO-04-fat32-hardening-vfs-semantics.md` (TODO-04 -- FAT32 Hardening & VFS Win32 Semantics): `fat32-vfs.md`
  - `todo/05-storage-filesystems/TODO-05-win32-file-io-api.md` (TODO-05 -- Win32 File I/O API & IRP Layer): `win32-file-io.md`
  - `todo/05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md` (TODO-06 -- IXFS Core Foundation & Win32 Compatibility): `ixfs-core.md`
- [x] Pages in `docs/storage/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/05-storage-filesystems/TODO-07-ixfs-advanced-enterprise.md` (TODO-07 -- IXFS Advanced Storage, Reliability & Enterprise): `ixfs-advanced.md`
  - `todo/05-storage-filesystems/TODO-08-exfat-readwrite.md` (TODO-08 -- exFAT Read/Write Driver): `exfat.md`
  - `todo/05-storage-filesystems/TODO-09-ext4-readwrite.md` (TODO-09 -- ext4 Read/Write Driver): `ext4.md`
- [x] Pages in `docs/storage/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/05-storage-filesystems/TODO-10-btrfs-readonly.md` (TODO-10 -- Btrfs Read-Only Driver): `btrfs.md`
  - `todo/05-storage-filesystems/TODO-11-optical-media.md` (TODO-11 -- Optical Media: ISO 9660, Joliet & UDF): `optical-media.md`
  - `todo/05-storage-filesystems/TODO-12-apple-filesystems-readonly.md` (TODO-12 -- Apple Filesystems: APFS & HFS+ (Read-Only)): `apple-filesystems.md`
- [x] Pages in `docs/storage/` for the next 2 roadmap files, each with its `covers=` directive
  - `todo/05-storage-filesystems/TODO-13-partition-tools-storage-suite.md` (TODO-13 -- Partition Management & Storage Tools): `partition-tools.md`
  - `todo/05-storage-filesystems/TODO-14-disk-benchmark-diagnostics.md` (TODO-14 -- Disk Benchmark & I/O Diagnostics): `disk-diagnostics.md`
- [x] No roadmap file had a page, so all fourteen are new; every quoted symbol, constant and log string was checked in the tree and every roadmap anchor resolves under the site check
- [x] Replaced the placeholder `docs/storage/index.md` with a Roadmap Overviews table listing every new page and a Specifications table, then ran `python3 scripts/site/build.py --update-baseline` (107 to 93 entries)
- [x] Roadmap drift found while writing, fixed in place in TODO-03, 09, 10, 11 and 12
  - TODO-03 §1 and §2 named the X: partition `Logs`; the code keys on the GPT name `BlackBox`. TODO-11 pointed its optical XREF at TODO-03 §8 (volume ioctls) instead of §7.
  - TODO-09 cited a stale line number for the ext2 probe and planned a read-only mount on unknown INCOMPAT feature bits, which the ext4 format forbids (refuse INCOMPAT, read-only on RO_COMPAT).
  - TODO-10 and TODO-12 cited `src/kernel/fs/ext4/` as a reference, which does not exist (the CRC32C helper is `kcrc32c()`).
- [x] Four code defects found while writing and reviewing, filed in the owning open sections rather than fixed (docs-only section)
  - TODO-03 §2: `scan_device()` and `probe_filesystem()` read one sector into a 512-byte stack buffer, which a 2048-byte optical sector overruns at boot.
  - TODO-06 §1: IXFS journal entries hold 4080 of 4096 bytes, and commit and replay zero each block's last 16 bytes (the block bitmap's last 128 allocation bits).
  - TODO-07 §16: an IXFS version 1 volume's read-only flag is checked only by write and create; unlink, truncate and mount-time journal replay still modify it.
  - TODO-05 §8: `vfs_seconds_to_filetime()` uses a base of 2024-12-30 10:40 UTC while its comment claims 2026-01-01.
- [x] Commit: `"docs: storage and filesystems documentation pages"`

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check` prints `site: OK` with 139/232 TODO files documented

> **Notes:**
> - **What shipped:** fourteen contract-shaped overview pages in `docs/storage/`, one per storage and filesystems roadmap file 01 to 14, listed in a rewritten `docs/storage/index.md`.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so the freshness check flags it when the code it describes changes.
> - **Status honesty:** only TODO-04 has shipped sections; the other pages say what works today (NTFS read, NCQ, VirtIO retry, ATAPI, IXFS journal and snapshots) and why the rest does not.
> - **Found on the way:** NTFS mounts read-only because its VFS write ops are stubs and its journal is never armed, although the write engine and replay code exist; booting with a data disc in an optical drive overruns the kernel stack.

> **Verified:** 2026-09-29 | commit `8299170f0` | 11/11 items | build OK | site: OK, 139/232 documented; tests 34646 kernel + 17 user-mode PASS; smoke PASS
> **Quality reviewed:** 2026-09-29 | Codex 4x (adversarial, adversarial post-ship, consistency, perf) | 3H+17M+2L fixed, 0 open | scope: N/A (docs-only; no source changed; re-adversarial skipped: docs-only fixes)

---

## 15. Document: Networking

Write docs pages that meet the §3 contract for the 11 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/networking/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/07-networking/TODO-01-tcp-network-infrastructure.md` (TODO-01 -- TCP Protocol & Network Infrastructure): `tcp-network-infrastructure.md`
  - `todo/07-networking/TODO-02-dns-sockets.md` (TODO-02 -- DNS Resolver & BSD Sockets API): `dns-sockets.md`
  - `todo/07-networking/TODO-03-http-tls.md` (TODO-03 -- HTTP/HTTPS Client & TLS): `http-tls.md`
- [x] Pages in `docs/networking/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/07-networking/TODO-04-ipv6-dual-stack.md` (TODO-04 -- IPv6 Dual-Stack): `ipv6.md`
  - `todo/07-networking/TODO-05-firewall.md` (TODO-05 -- Network Firewall & Packet Filter): `firewall.md`
  - `todo/07-networking/TODO-06-ntp-status-winsock.md` (TODO-06 -- NTP, Network Status & Win32 Winsock): `ntp-status-winsock.md`
- [x] Pages in `docs/networking/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/07-networking/TODO-07-web-browser.md` (TODO-07 -- Web Browser): `web-browser.md`
  - `todo/07-networking/TODO-08-ssh-ftp-clients.md` (TODO-08 -- SSH & FTP Clients): `ssh-ftp.md`
  - `todo/07-networking/TODO-09-email-client.md` (TODO-09 -- Email Client): `email-client.md`
- [x] Pages in `docs/networking/` for the next 2 roadmap files, each with its `covers=` directive
  - `todo/07-networking/TODO-10-pdf-viewer.md` (TODO-10 -- PDF Viewer & Document Reader): `pdf-viewer.md`
  - `todo/07-networking/TODO-11-syslog-forwarding.md` (TODO-11 -- Remote Syslog Forwarding (RFC 5424)): `syslog-forwarding.md`
- [x] Replaced the placeholder `docs/networking/index.md` (it listed `drivers/` and `protocols/` folders that never existed) with a Roadmap Overviews table, then ran `python3 scripts/site/build.py --update-baseline` (93 to 82 entries)
- [x] Roadmap drift found while writing, fixed in place across all eleven networking files
  - Implementation Order rows in TODO-06 to 09 carried pre-renumbering section labels, and the Order columns of TODO-01 (six rows) and TODO-05 (rows 5 to 9) pointed at the wrong bodies (the oracle reads Order as the section number); all now match the bodies.
  - TODO-08 named Monocypher's BLAKE2b `crypto_eddsa_*` for `ssh-ed25519` (needs the SHA-512 `crypto_ed25519_*`), a nonexistent ChaCha20-Poly1305 call with 32-byte keys and HKDF (OpenSSH's construction needs 64-byte keys and RFC 4253 derivation), and an unported MIT library that is vendored and compiled.
  - TODO-06 planned its own TSC slew and `uefi_set_time()` although `ke_ntp_adjtime()` ships; TODO-02 planned syscalls 39-49, now taken; TODO-09 placed `base64_decode()` in a nonexistent `tls.c` and stored passwords in plaintext against `11-apps/TODO-04` section 6.
  - TODO-10 (PDF) called stb_truetype bounds-checked for embedded fonts; its header disclaims untrusted fonts, so a bounds-checked path is now a filed prerequisite.
  - TODO-06's NTP client contract was rewritten against RFC 5905 during review (fresh request nonce, one outstanding exchange, LI and kiss-o'-death checks, sourced clocks keep the ten-year guard); TODO-11 syslog keeps event time; TODO-07 section 5 and TODO-10 section 6 now wait on the image-decoder fix.
  - Smaller fixes: `ethernet_receive()` is `net_rx()` (TODO-04), stale cross-section numbers (TODO-03, 04, 05, 07, 08), a claimed UDP checksum pattern that does not exist (TODO-01), and directory-less XREFs and an RTC timestamp for syslog (TODO-11).
- [x] Code defects found while writing, filed in owning sections rather than fixed (docs-only section)
  - `07-networking/TODO-01` section 1: `ipv4_handle()` accepts IHL below 5 and `total_len < hdr_len`, and `udp_handle()` trusts the UDP length, so one malformed broadcast makes the DHCP parser read past the frame.
  - `02-kernel-core/TODO-10` section 31: `SYS_NETINFO` copies `net_cfg` to an unchecked user pointer (added to the legacy-syscall audit list).
  - `04-drivers-hardware/TODO-14` section 7: RTL8139 wraps its Rx offset at 9708 bytes instead of the 8 KiB ring; `07-networking/TODO-06` section 1: the DHCP transaction ID is the constant `0x12345678`.
  - `07-networking/TODO-10` section 1 and `11-apps/TODO-05` section 1: both plan the same PDF parser, decompressor and renderer; one owner has to be chosen.
  - `02-kernel-core/TODO-12` new section 33: 37 fixed legacy `SYS_*` reservations above 48 across 22 roadmaps collide with each other and the live table; the networking and dialog roadmaps were made symbolic here.
  - `08-graphics-ui/TODO-01` section 5: the stb_image realloc shim in `image.c` copies `new_size` bytes from a smaller `kmalloc` block when a PNG's IDAT buffer grows past 4 KiB.
- [x] Commit: `"docs: networking documentation pages"`

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check` prints `site: OK` with 150/232 TODO files documented

> **Notes:**
> - **What shipped:** eleven contract-shaped overview pages in `docs/networking/`, one per networking roadmap file 01 to 11, listed in a rewritten `docs/networking/index.md`.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so the freshness check flags it when the network code it describes changes.
> - **Status honesty:** no networking roadmap section has shipped; pages describe what runs today (IPv4, ICMP, one-shot DHCP, send-only `ping`, `ifconfig`) and which vendored pieces already exist (Mbed TLS unbuilt, Monocypher built, stb zlib).
> - **Scope boundary:** the app windows for browser, mail, SSH, FTP and PDF are documented with the apps roadmaps (section 20); these pages cover the protocol and engine halves.

> **Verified:** 2026-09-29 | commit `5f71cff17` | 8/8 items | build OK | site: OK, 150/232 documented; tests 34646 kernel + 17 user-mode PASS; lint 0 errors
> **Accepted:** [H] NTP replies need root-distance and reference-time checks (reason: unbuilt NTP client design) -> XREF: 07-networking/TODO-06 §2 (item: "Reject replies whose server timing is poor" at line 103)
> **Accepted:** [H] NTP measurement spanning a local clock change must be discarded (reason: needs a time-service API) -> XREF: 07-networking/TODO-06 §2 (item: "Discard a measurement that spans a local clock change" at line 105)
> **Quality reviewed:** 2026-09-29 | Codex 30x (adversarial, consistency, perf, re-adversarial) | 6H+18M fixed, 2H accepted, 0 open | scope: N/A (docs-only; no source changed; parity research and test coverage N/A)

---

## 16. Document: Desktop foundation and graphics, part 1

Write docs pages that meet the §3 contract for the 14 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/desktop/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/06-desktop-foundation/TODO-01-wm-completion.md` (TODO-01 -- Window Manager Completion): `window-management.md`
  - `todo/06-desktop-foundation/TODO-02-compositor-optimization.md` (TODO-02 -- Compositor Optimization): `compositor.md`
  - `todo/06-desktop-foundation/TODO-03-input-system.md` (TODO-03 -- Input System): `input-system.md`
- [x] Pages in `docs/desktop/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/06-desktop-foundation/TODO-04-control-library.md` (TODO-04 -- Control Library Completion): `control-library.md`
  - `todo/06-desktop-foundation/TODO-05-desktop-shell.md` (TODO-05 -- Desktop Shell Completion): `desktop-shell.md`
  - `todo/06-desktop-foundation/TODO-06-desktop-icons.md` (TODO-06 -- Desktop Icon System): `desktop-icons.md`
- [x] Pages in `docs/graphics/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/08-graphics-ui/TODO-01-graphics-asset-foundation.md` (TODO-01 -- Advanced 2D Graphics and Visual Asset Foundation): `graphics-assets.md`
  - `todo/08-graphics-ui/TODO-02-text-font-internationalization.md` (TODO-02 -- Text, Font, and Internationalization Foundation): `text-fonts.md`
  - `todo/08-graphics-ui/TODO-03-theme-system.md` (TODO-03 -- Theme System): `theme-system.md`
- [x] Pages in `docs/graphics/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/08-graphics-ui/TODO-04-animation-engine.md` (TODO-04 -- Animation Engine): `animation-engine.md`
  - `todo/08-graphics-ui/TODO-05-widget-library-core.md` (TODO-05 -- Extended Widget Library: Core Controls): `widget-library.md`
  - `todo/08-graphics-ui/TODO-06-widget-dialogs.md` (TODO-06 -- Extended Widget Library: Complex Controls & Dialogs): `complex-controls-dialogs.md`
- [x] Pages in `docs/graphics/` for the next 2 roadmap files, each with its `covers=` directive
  - `todo/08-graphics-ui/TODO-07-ui-accessibility-automation-ime.md` (TODO-07 -- UI Accessibility, Automation, and IME Foundation): `accessibility-ime.md`
  - `todo/08-graphics-ui/TODO-08-window-manager.md` (TODO-08 -- Window Manager Enhancements): `window-manager.md`
- [x] New `docs/desktop/index.md`, rewritten placeholder `docs/graphics/index.md`, a Desktop row in `docs/index.md`, then `python3 scripts/site/build.py --update-baseline` (82 to 76 entries)
  - The old graphics index listed `rendering/` and `desktop/` subfolders that never existed.
  - Eight of the fourteen files were already covered by `docs/design/` spec pages; the new pages cover them too, as subsystem pages that link the spec rather than repeat it.
- [x] Roadmap drift found while writing, fixed in place
  - Implementation Order: the Order column (which the oracle reads as the section number) pointed at the wrong bodies in `08-graphics-ui/TODO-06` (six rows) and `TODO-08` (seven rows); section labels were swapped in `TODO-04` (rows 4 and 5) and `TODO-05` (rows 3 to 5); Depends On cells in both carried pre-renumbering numbers.
  - Stale cross-references: theme is `TODO-03` not `TODO-01`, animation `TODO-04` not `TODO-02`, the dropdown overlay `TODO-05` not `TODO-04`, the context menu engine `TODO-09` section 1 not `TODO-07`, IME `08-graphics-ui/TODO-07` not `TODO-16`, the USER syscalls `TODO-15` not `TODO-12`, and the taskbar and Start menu files are `08-graphics-ui/TODO-10` and `TODO-11`, not `09-desktop-shell`.
  - Code claims: `06-desktop-foundation/TODO-01` and `TODO-02` preambles now state the shipped Alt+F4, resize cursors, drag rectangle and frame timing; `TODO-03` Bug B names the real key route (`terminal_key_input()`, no WM focus check); `TODO-06` section 6 extends the shipped `icon_for_extension()`; `08-graphics-ui/TODO-08` no longer claims edge resize or `wm_get_focused()` exist.
- [x] Code defects found while writing, filed in owning open sections rather than fixed (docs-only section)
  - `08-graphics-ui/TODO-01` section 4: the Xcursor loader frees low frames by passing a frame number to `pmm_free_frame()`, copies oversize cursor images past their 24x24 slot, and the ICO and `icons.ires` parsers do not bound offsets.
  - `08-graphics-ui/TODO-02` section 1: a font that fails to parse is freed with `kfree()` although it came from `pmm_alloc_contiguous()`.
  - `08-graphics-ui/TODO-08` section 1: `wm_destroy_window()` leaves drag state set and the slot's controls alive; section 2 gains the missing edge-resize implementing item.
  - `08-graphics-ui/TODO-05` section 1 (control slot reuse, press-fires-click, `ctrl_set_text(NULL)`), `TODO-10` section 1 (taskbar hit-test geometry), `TODO-04` sections 3 and 6 (frame clock, spring arithmetic), `06-desktop-foundation/TODO-03` section 5 (stale terminal handle after close).
  - Cross-roadmap contract conflicts found in review, filed rather than designed: two hotkey tables (`06-desktop-foundation/TODO-03` section 4 versus `08-graphics-ui/TODO-08` section 5) and `wm_anim_snap()` taking a side where its caller passes a zone rectangle (`TODO-04` section 5); `TODO-06` section 8 now draws the existing status glyphs, as the dialog design requires, instead of adding colour icons.
- [x] Commit: `"docs: desktop foundation and graphics, part 1 documentation pages"`

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check` prints `site: OK` with 156/232 TODO files documented

> **Notes:**
> - **What shipped:** fourteen contract-shaped pages, six in the new `docs/desktop/` and eight in `docs/graphics/`, one per roadmap file, each listed in its folder index.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so the freshness check flags it when the desktop or graphics code it describes changes.
> - **Status honesty:** no section of these fourteen roadmaps has shipped; pages describe what runs today (full-repaint compositor, four controls, three static icons, Latin-1 TrueType text, generated but unused theme tokens) and link the owning sections.
> - **Scope boundary:** the design specs in `docs/design/` stay authoritative for visuals; these pages link them. Taskbar, Start menu and Win32k pages are section 17.

> **Verified:** 2026-09-29 | commit `893870691` | 9/9 items | build OK | site: OK, 156/232 documented; tests 34646 kernel + 17 user-mode PASS; smoke PASS
> **Quality reviewed:** 2026-09-29 | Codex 4x (adversarial, adversarial post-ship, consistency, perf) | 13M fixed, 0 open | scope: N/A (docs-only; no source changed; parity research and test coverage N/A; re-adversarial skipped: docs-only fixes)

---

## 17. Document: Graphics and UI, part 2

**Design:** n/a -- a documentation-writing section; the pages it writes describe the design, they do not draw UI

Write docs pages that meet the §3 contract for the 9 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/graphics/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/08-graphics-ui/TODO-09-desktop-shell-features.md` (TODO-09 -- Desktop Shell Features): `desktop-shell-features.md`
  - `todo/08-graphics-ui/TODO-10-taskbar.md` (TODO-10 -- Taskbar): `taskbar.md`
  - `todo/08-graphics-ui/TODO-11-startmenu-tray-notifications.md` (TODO-11 -- Start Menu, System Tray & Notifications): `start-menu-tray-notifications.md`
- [x] Pages in `docs/graphics/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/08-graphics-ui/TODO-12-clock-time.md` (TODO-12 -- Kernel Time & Taskbar Clock): `clock-time.md`
  - `todo/08-graphics-ui/TODO-13-boot-splash-recovery.md` (TODO-13 -- Boot Splash & F8 Recovery): `boot-splash-recovery.md`
  - `todo/08-graphics-ui/TODO-14-win32-gdi-user32-stubs.md` (TODO-14 -- Win32 GDI / USER32 Desktop API Stubs): `win32-gdi-user32.md`
- [x] Pages in `docs/graphics/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/08-graphics-ui/TODO-15-win32k-shadow-ssdt.md` (TODO-15 -- Win32k Shadow SSDT (NtGdi / NtUser)): `win32k-shadow-ssdt.md`
  - `todo/08-graphics-ui/TODO-16-win32k-shadow-native-api.md` (TODO-16 -- Win32k Shadow Native API (SSDT Table 1 Router)): `win32k-shadow-router.md`
  - `todo/08-graphics-ui/TODO-A-Win32k-Shadow-SSDT-Master-Table.md` (Win32k Shadow SSDT Master Table): `win32k-shadow-master-table.md`
- [x] Add every new page to its folder `index.md`, then run `python3 scripts/site/build.py --update-baseline` (76 to 71 entries)
  - Four of the nine files (TODO-09 to TODO-12) were already covered by `docs/design/shell.md`; the new pages cover them too, as subsystem pages that link the spec.
- [x] Roadmap drift found while writing, fixed in place
  - Implementation Order: the Order column did not match the section in `08-graphics-ui/TODO-09`, `TODO-10`, `TODO-12`, `TODO-13` and `TODO-14` (and four `TODO-09` labels named the wrong section); Depends On cells named pre-renumbering sections there and in `TODO-11`, `TODO-15` and `TODO-16`, whose bare `TODO-12` the graph parser read as the graphics clock roadmap (now `02-kernel-core/TODO-12`, checked with `extract_implementation_order()`).
  - Stale owners: the old `TODO-01`/`TODO-02`/`TODO-06`/`TODO-07` numbers for theme, animation, window manager and shell features; the native API as `02-kernel-core/TODO-05` in `TODO-15`/`TODO-16`; FILETIME as `TODO-17`; crash-loop protection as `TODO-21` (now `TODO-28`).
  - Code claims: `TODO-12` now says the FILETIME clock and a taskbar clock ship; `TODO-13` that the splash, fade, logo pipeline and a bootloader F8 exist; `TODO-14` that no dialogs or `wm_minimize()` exist yet; `TODO-15`/`TODO-16` that the shadow table exists, with the shipped routing and stub items marked done; the `TODO-12` section 1 and `TODO-13` section 5 checklists now build on the existing clock and boot mode instead of a second clock and a `g_boot_mode`; `SYS_*` numbers above 48 are no longer described as taken.
  - `TODO-A`: 29 range headings, not 28; the headings overlap but the 1,300 row indexes are unique.
- [x] Code defects and design conflicts found while writing, filed in owning open sections rather than fixed (docs-only section)
  - `08-graphics-ui/TODO-15` section 1: `SSDT_SHADOW_MAX` is 1,024, so 419 master-table rows at `0x1400`-`0x15A2` cannot register.
  - `08-graphics-ui/TODO-12` section 1: the planned Unix clock would be a second clock with its own anchor, a fixed PIT divisor and an unchecked `rtc_read()` seed; section 3: the taskbar clock's fallback formats a zero date without CMOS.
  - `08-graphics-ui/TODO-13` section 5: safe mode is resolved but only the code-integrity gate reads it, so it does not skip the desktop or network; the F8 plan now builds on the bootloader's F8.
  - `08-graphics-ui/TODO-10` section 1: the Start button is drawn at x 4-51 but hit-tested at x 2-49.
- [x] Commit: `"docs: graphics and ui, part 2 documentation pages"`
- [x] Post-ship review fixes (adversarial, consistency, perf legs)
  - `08-graphics-ui/TODO-13` section 5: the F8 choice must be made before `kernel_config_publish()` (`boot_hw.c`), because the published config is immutable and the safe-mode gates read it.
  - `desktop-shell-features.md` no longer tells readers to change `WallpaperMode` and reboot: every boot re-seeds the Registry defaults.
  - `08-graphics-ui/TODO-16`: the registration prerequisite is `08-graphics-ui/TODO-15` section 1, not the kernel NTSTATUS section; `TODO-12` section 3 checks the clock every second but repaints only when its text changes.

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check` prints `site: OK` with 161/232 TODO files documented

> **Notes:**
> - **What shipped:** nine contract-shaped pages in `docs/graphics/`, one per roadmap file (shell features, taskbar, Start menu, clock, boot splash, GDI/USER32, and the three Win32k shadow SSDT files), each in the folder index.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so the freshness check flags it when the shell, time, splash, PE or SSDT code it describes changes.
> - **Status honesty:** no section of these nine roadmaps has shipped; pages describe what runs today (a static taskbar and Start menu, the FILETIME clock, the splash and bootloader F8, an empty shadow table) and link the owning sections.
> - **Scope boundary:** the design specs in `docs/design/` stay authoritative for visuals; the kernel pages `docs/kernel/time-filetime.md` and `docs/kernel/native-api-ssdt.md` stay authoritative for the clock and the main SSDT, and these pages link them.

> **Verified:** 2026-09-29 | commit `9d820f058` | 8/8 items | build OK | site: OK, 161/232 documented; tests 34646 kernel + 17 user-mode PASS; smoke PASS
> **Quality reviewed:** 2026-09-29 | Codex 4x (adversarial, adversarial post-ship, consistency, perf) | 9M+1L fixed, 0 open | scope: N/A (docs-only; no source changed; parity research and test coverage N/A; re-adversarial skipped: docs-only fixes)

---

## 18. Document: Desktop shell

**Design:** n/a -- a documentation-writing section; the pages it writes describe the design, they do not draw UI

Write docs pages that meet the §3 contract for the 14 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/desktop/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/09-desktop-shell/TODO-01-clipboard.md` (TODO-01 -- Clipboard System): `clipboard.md`
  - `todo/09-desktop-shell/TODO-02-file-associations-resources.md` (TODO-02 -- File Associations, Shortcuts & System Resources): `file-associations.md`
  - `todo/09-desktop-shell/TODO-03-service-manager.md` (TODO-03 -- Service Manager & Core Daemons): `service-manager.md`
- [x] Pages in `docs/desktop/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/09-desktop-shell/TODO-04-recycle-zip-scheduler.md` (TODO-04 -- Recycle Bin, ZIP & Task Scheduler): `recycle-bin-zip-scheduler.md`
  - `todo/09-desktop-shell/TODO-05-file-search.md` (TODO-05 -- File Search & Indexing): `file-search.md`
  - `todo/09-desktop-shell/TODO-06-security-accounts.md` (TODO-06 -- Security & User Accounts): `security-accounts.md`
- [x] Pages in `docs/desktop/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/09-desktop-shell/TODO-07-cng-crypto.md` (TODO-07 -- CNG Crypto & Certificate Store): `cng-crypto.md`
  - `todo/09-desktop-shell/TODO-08-terminal.md` (TODO-08 -- Terminal Emulator): `terminal.md`
  - `todo/09-desktop-shell/TODO-09-file-manager.md` (TODO-09 -- File Manager): `file-manager.md`
- [x] Pages in `docs/desktop/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/09-desktop-shell/TODO-10-notepad.md` (TODO-10 -- Notepad Text Editor): `notepad.md`
  - `todo/09-desktop-shell/TODO-11-control-panel.md` (TODO-11 -- Control Panel & Settings): `control-panel.md`
  - `todo/09-desktop-shell/TODO-12-utilities.md` (TODO-12 -- Task Manager, Device Manager & Core Utilities): `utilities.md`
- [x] Pages in `docs/desktop/` for the next 2 roadmap files, each with its `covers=` directive
  - `todo/09-desktop-shell/TODO-13-explorer-shell-host.md` (TODO-13 -- Explorer Shell Host (explorer.exe)): `explorer-shell-host.md`
  - `todo/09-desktop-shell/TODO-14-desktop-test-late-phase-harness.md` (TODO-14 -- Desktop Test Late-Phase Harness and Artifact Bundle): `desktop-test-late-phase.md`
- [x] Add every new page to its folder `index.md`, then run `python3 scripts/site/build.py --update-baseline` (71 to 58 entries)
  - `docs/desktop/index.md` gains a Desktop Shell Roadmaps table; `docs/design/shell.md` already covered `TODO-09`, and the new page covers it too, as a subsystem page that links the spec.
- [x] Roadmap drift found while writing, fixed in place
  - Implementation Order: the Order column did not match the section in `09-desktop-shell/TODO-01` and `TODO-02` (rows now in section order, the build sequence kept in Depends On); the section labels were shifted in `TODO-03`, `TODO-06`, `TODO-07`, `TODO-10` and `TODO-12`; `TODO-13` row 2 named the wrong section.
  - Code claims: `TODO-01`, `TODO-05` and `TODO-07` listed planned APIs as shipped and syscall numbers above the highest assigned `SYS_*` (48); `TODO-04` said miniz was missing (vendored, unbuilt); `TODO-07` said SHA-256 was missing and ignored the vendored Mbed TLS and the shipped `ntfs_efs.c` parser; `TODO-06` Monocypher vendoring items are marked done.
  - Stale names and paths: `csprng_read` and `crypto_argon2i` (now `csprng_fill` and `crypto_argon2`), `vfs_mkdir`, `registry_get/set`, `src/shell/cmds.c` (the shell is `user/cmd.c`), `kmath_sin` (already in `libc/math.h`), the object manager path in `TODO-14`, and its `TEST_PENDING` claim for a passing synthesized test.
  - Inbound references: code signing is `TODO-07` section 7, not 9 (`15-installer-release/TODO-01`, `01-boot-platform/TODO-06`); the ZIP API is `TODO-04` section 4 and the scheduler section 6 (`11-apps/TODO-04`, `TODO-07`, `TODO-12`).
- [x] Code defects and design conflicts found while writing, filed in owning open sections rather than fixed (docs-only section)
  - `09-desktop-shell/TODO-08` section 1: Start > Terminal starts another `cmd.exe` on every click even when the terminal is open; the 200 by 2000 grid needs about 1,563 pages for a 16-byte cell; the test seams must survive the rewrite.
  - `09-desktop-shell/TODO-13` section 1: the desktop, taskbar and window manager run in the kernel, so `explorer.exe` ownership and its `C:\Windows` path need deciding first.
- [x] Commit: `"docs: desktop shell documentation pages"`
- [x] Post-ship review fixes (adversarial, consistency, perf legs and three consistency confirmation rounds)
  - Pages: `desktop-test-late-phase.md` names the real targets (`make test-visual`; `make test-desktop` is the kernel suite); `clipboard.md`, `terminal.md` and `input-system.md` say Ctrl+C does nothing today (`cmd.exe` sets no foreground group and pending signals are not delivered).
  - `09-desktop-shell/TODO-07` sections 1, 3 and 8 now wrap Mbed TLS and the shipped hashes, and section 8 follows the Windows `$EFS` contract of `ntfs_efs.c` (AES-256 sector mode, RSA-OAEP) with an interop fixture, a raw ciphertext write that breaks the dormant `ntfs_write_data()` recursion, and an offset-aware cipher API.
  - `09-desktop-shell/TODO-04` section 3 consumes the workspace-based miniz port of `02-kernel-core/TODO-03` section 4; `TODO-08` section 1 files `cmd.exe` claiming the console, with delivery owned by `02-kernel-core/TODO-21` section 14 and `10-platform-services/TODO-10` section 8; the last CNG signing references moved to section 7.

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check` prints `site: OK` with 174/232 TODO files documented

> **Notes:**
> - **What shipped:** fourteen contract-shaped pages in `docs/desktop/`, one per desktop shell roadmap file, each listed in a new Desktop Shell Roadmaps table in the folder index.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so the freshness check flags it when the shell, terminal, security, crypto or test code it describes changes.
> - **Status honesty:** only `TODO-14` sections 6 to 8 have shipped; the other pages describe what runs today (the 80 by 20 terminal, `cmd.exe` built-ins, the kernel CSPRNG, tokens, hashes and Monocypher, vendored but unbuilt miniz and Mbed TLS) and link the owning sections.
> - **Scope boundary:** the design specs in `docs/design/` stay authoritative for visuals, and the kernel, storage and networking pages stay authoritative for the CSPRNG, tokens, VFS, NTFS and TLS; these pages link them.

> **Verified:** 2026-09-29 | commit `800620e90` | 10/10 items | build OK | site: OK, 174/232 documented; tests 34646 kernel + 17 user-mode PASS; smoke PASS
> **Quality reviewed:** 2026-09-29 | Codex 7x (adversarial, adversarial post-ship, consistency x4, perf) | 3H+9M fixed, 0 open | scope: N/A (docs-only; no source changed; parity research and test coverage N/A; re-adversarial skipped: docs and roadmap-text fixes)

---

## 19. Document: Platform services

**Design:** n/a -- a documentation-writing section; the pages it writes describe the design, they do not draw UI

Write docs pages that meet the §3 contract for the 15 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/services/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/10-platform-services/TODO-01-audio-system.md` (TODO-01 -- Audio System & Media Player): `audio-system.md`
  - `todo/10-platform-services/TODO-02-paint-app.md` (TODO-02 -- Paint App & Image Tools): `paint-app.md`
  - `todo/10-platform-services/TODO-03-updates-packages.md` (TODO-03 -- System Updates & IPKG Package Manager): `updates-packages.md`
- [x] Pages in `docs/services/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/10-platform-services/TODO-04-restore-recovery.md` (TODO-04 -- System Restore, Recovery & Observability): `restore-recovery.md`
  - `todo/10-platform-services/TODO-05-screensaver-widgets-display.md` (TODO-05 -- Screensaver, Widgets & Display): `screensaver-widgets-display.md`
  - `todo/10-platform-services/TODO-06-accessibility.md` (TODO-06 -- Accessibility Features): `accessibility.md`
- [x] Pages in `docs/services/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/10-platform-services/TODO-07-win32-pe-loader.md` (TODO-07 -- Native Win32 Execution & PE Loader): `win32-pe-loader.md`
  - `todo/10-platform-services/TODO-08-win32-api-surface.md` (TODO-08 -- Win32 API Surface Completion): `win32-api-surface.md`
  - `todo/10-platform-services/TODO-09-compiler-sdk.md` (TODO-09 -- C/C++ Compiler & SDK): `compiler-sdk.md`
- [x] Pages in `docs/services/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/10-platform-services/TODO-10-linux-compat.md` (TODO-10 -- Linux ELF Compatibility Layer): `linux-compat.md`
  - `todo/10-platform-services/TODO-11-installer-iso.md` (TODO-11 -- OS Installer & ISO Build): `installer-iso.md`
  - `todo/10-platform-services/TODO-12-long-term-features.md` (TODO-12 -- Long-Term Features): `long-term-features.md`
- [x] Pages in `docs/services/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/10-platform-services/TODO-A-user32-export-master-table.md` (TODO-A -- user32.dll Export Master Table): `user32-exports.md`
  - `todo/10-platform-services/TODO-B-comctl32-export-master-table.md` (TODO-B -- comctl32.dll Export Master Table): `comctl32-exports.md`
  - `todo/10-platform-services/TODO-C-shell32-export-master-table.md` (TODO-C -- shell32.dll Export Master Table): `shell32-exports.md`
- [x] Add every new page to its folder `index.md`, then run `python3 scripts/site/build.py --update-baseline` (58 to 43 entries)
  - `docs/services/` is new: its `index.md` holds a Platform Services roadmap table, and `docs/index.md` gains a Platform Services category row.
- [x] Roadmap drift found while writing, fixed in place
  - Shipped work described as missing: `TODO-07` said there were no PE structs, no `STAR`/`LSTAR` setup and the GDT selectors in the wrong order (all three shipped; the loader shipped under `02-kernel-core/TODO-17`); `TODO-10` said no SYSV stack exists (`task_exec()` builds it for every exec) and that exec probes PE first (it matches magic bytes).
  - Planned work described as shipped: `TODO-01` said an AC97 driver exists and claimed syscalls 60 and 61 (collide with the Win32 ABI plan); `TODO-04` planned `LOG_SECURITY=4` (4 is `LOG_FATAL`) and relied on `registry_backup()` (`RegSaveKey()` returns `ERROR_NOT_SUPPORTED`); `TODO-05` said `kevent_log()` had shipped.
  - Implementation Order and numbering: `TODO-04` table rows used an old section numbering; the domain `INDEX.md` summaries for `TODO-09` and `TODO-10` were off by one; `TODO-09`, `TODO-10` and `TODO-12` notes cited wrong own sections.
  - Stale owners and paths: the export tables named `TODO-11` for the file now numbered `08-graphics-ui/TODO-14`, and `TODO-C` gave `CommandLineToArgvW` the wrong owner; `TODO-05`, `TODO-06`, `TODO-08`, `TODO-11` and `TODO-12` cited wrong sections or files for the lock screen, wallpaper, animation engine, TEB setup, OOBE, accounts and compositor.
  - Inbound references: `15-installer-release/TODO-01`, `TODO-02`, `TODO-03`, `TODO-04`, `01-boot-platform/TODO-06`, `11-apps/TODO-10`, `12-user-platform-sdk/TODO-03`, `TODO-04`, `TODO-06` and `18-future-research/TODO-03`, `TODO-05` now cite the right `TODO-04`, `TODO-07` to `TODO-12` sections.
- [x] Code defects found while writing, filed in owning open sections rather than fixed (docs-only section)
  - `10-platform-services/TODO-07` section 8: stale comments in `exec.c`, `syscall.h` (`SYS_EXEC` arguments) and `task.c`.
  - `10-platform-services/TODO-08` section 2: the test shim's two header comments disagree with its code (the process ID and tick count are TEB and `KUSER_SHARED_DATA` reads; only the file calls use `INT 0x80`).
  - `10-platform-services/TODO-07` section 4 (post-ship review): `pe_rollback()` unmaps only a contiguous prefix, and a failed page map leaks its frame.
- [x] Commit: `"docs: platform services documentation pages"`
- [x] Post-ship review fixes (adversarial, consistency and perf legs)
  - Pages: `win32-pe-loader.md` no longer claims complete rollback on failure; `long-term-features.md` names the sections other roadmaps depend on instead of calling all of them independent.
  - Roadmaps: `11-apps/TODO-10` calls `pdf_begin()` and `pdf_end()` once per document again; the last stale `TODO-07`, `TODO-08` and `TODO-12` section references in `12-user-platform-sdk/TODO-04`, `TODO-06` and `10-platform-services/TODO-11` are fixed; `TODO-12` no longer calls every section independent.

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check` prints `site: OK` with 189/232 TODO files documented

> **Notes:**
> - **What shipped:** fifteen contract-shaped pages in a new `docs/services/` folder, one per platform services roadmap file, listed in its index and linked from the docs home page.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so the freshness check flags it when the loader, exec, Registry, logging, GPT or input code it describes changes.
> - **Status honesty:** no section of these fifteen roadmaps has shipped; the pages describe what runs today (the kernel PE loader and import tables, the SYSV start-up stack, `events.jsonl`, crash reports, release images) and link the owning sections.
> - **Scope boundary:** kernel, boot, storage, graphics and desktop pages stay authoritative for the loader internals, SSDT, logging, GPT and widgets; these pages link them.

> **Verified:** 2026-09-29 | commit `23711536a` | 10/10 items | build OK | site: OK, 189/232 documented; tests 34646 kernel + 17 user-mode PASS
> **Quality reviewed:** 2026-09-29 | Codex 4x (adversarial, adversarial post-ship, consistency, perf) | 9M fixed, 0 open | scope: N/A (docs-only; no source changed; parity research and test coverage N/A; re-adversarial skipped: docs and roadmap-text fixes)

---

## 20. Document: Applications and accessories

**Design:** n/a -- a documentation-writing section; the pages it writes describe the design, they do not draw UI

Write docs pages that meet the §3 contract for the 15 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [x] Pages in `docs/apps/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/11-apps/TODO-01-web-browser.md` (TODO-01 -- Web Browser): `web-browser.md`
  - `todo/11-apps/TODO-02-ftp-wget-wifi.md` (TODO-02 -- FTP Client, wget/curl & WiFi): `ftp-wget-wifi.md`
  - `todo/11-apps/TODO-03-ssh-client.md` (TODO-03 -- SSH Client): `ssh-client.md`
- [x] Pages in `docs/apps/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/11-apps/TODO-04-email-client.md` (TODO-04 -- Email Client): `email-client.md`
  - `todo/11-apps/TODO-05-pdf-viewer.md` (TODO-05 -- PDF Viewer): `pdf-viewer.md`
  - `todo/11-apps/TODO-06-video-player.md` (TODO-06 -- Video Player): `video-player.md`
- [x] Pages in `docs/apps/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/11-apps/TODO-07-collaboration-apps.md` (TODO-07 -- Collaboration & Network Client Apps): `collaboration-apps.md`
  - `todo/11-apps/TODO-08-notepad.md` (TODO-08 -- Notepad): `notepad.md`
  - `todo/11-apps/TODO-09-calculator.md` (TODO-09 -- Calculator): `calculator.md`
- [x] Pages in `docs/apps/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/11-apps/TODO-10-wordpad.md` (TODO-10 -- WordPad (Rich Text Editor)): `wordpad.md`
  - `todo/11-apps/TODO-11-photos-image-viewer.md` (TODO-11 -- Photos (Image Viewer)): `photos.md`
  - `todo/11-apps/TODO-12-screenshot-archive.md` (TODO-12 -- Screenshot Tool & Archive Manager): `screenshot-archive.md`
- [x] Pages in `docs/apps/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/11-apps/TODO-13-calendar-utilities.md` (TODO-13 -- Calendar, Sticky Notes & Utility Apps): `calendar-utilities.md`
  - `todo/13-tools-accessories/TODO-01-obbrowse-namespace-browser.md` (TODO-01 -- ObBrowse: Object Namespace Browser): `obbrowse.md`
  - `todo/13-tools-accessories/TODO-02-event-viewer.md` (TODO-02 -- Event Viewer (Log Viewer)): `event-viewer.md`
- [x] Add every new page to its folder `index.md`, then run `python3 scripts/site/build.py --update-baseline` (43 to 28 entries)
  - `docs/apps/` is new: its `index.md` holds an Applications and Accessories roadmap table, and `docs/index.md` gains an Applications category row.
- [x] Roadmap drift found while writing, fixed in place
  - Wrong section numbers: `TODO-03` to `07-networking/TODO-08` (off by two) and the socket and Monocypher sections; `TODO-04` to `07-networking/TODO-09` (off by one); `TODO-09` and `TODO-13` to `09-desktop-shell/TODO-12` (calculator §2, calendar §6); the dialog, toast and miniz sections cited by `TODO-05`, `TODO-08`, `TODO-10`, `TODO-11`, `TODO-12`, `TODO-13`.
  - Wrong facts: `TODO-03` specified an HKDF key schedule over a BLAKE2b exchange hash that no SSH server speaks (now RFC 4253 section 7.2 over SHA-256); `13-tools-accessories/TODO-02` and `10-platform-services/TODO-04` gave the `events.jsonl` keys as `level`/`tag` (the writer emits `lvl`/`sub` and five more); `TODO-09` asked to add `kmath_sin` and friends, which ship; `TODO-11` claimed stb_image decodes WebP and TIFF.
  - Shipped work described as missing: the ObBrowse wrappers (`sys_opendirobj()`, `sys_querydirobj()`), `sysinfo.exe`, and the shell's `ping` in `user/cmd.c` (the roadmaps pointed at an empty `src/shell/`).
  - Wrong names: `reg_*` for `RegGetString`/`RegSetString`, `smbios_get_system_info` for `smbios_get_info`, `wm_get_focused` for `wm_get_focused_window`, `FONT_BODY`, `gfx_create_surface`, `csprng_read`, `vfs_delete`, and `include/kernel/vfs.h`; the `11-apps` and `13-tools-accessories` indexes had wrong domain numbers and dead links.
- [x] Ownership conflicts found while writing, filed on both sides rather than decided (docs-only section)
  - Engine vs app: browser (`07-networking/TODO-07`), FTP and wget/curl (`07-networking/TODO-08`, `TODO-03`), SSH host-trust store (`07-networking/TODO-08`), contacts store (`07-networking/TODO-09`).
  - Duplicated apps: Notepad (`09-desktop-shell/TODO-10`), Calculator, screenshot and archive manager (`09-desktop-shell/TODO-12`, `08-graphics-ui/TODO-09` §5); Wi-Fi parked on its owner `04-drivers-hardware/TODO-15`.
  - Unowned or unvetted: WebP and TIFF decoders (`TODO-11` §1), NetSurf/Dillo and pl_mpeg licence gates (`TODO-01` §6, `TODO-06` §1); WordPad's font lookup now points at `08-graphics-ui/TODO-02` §1-§2.
- [x] Commit: `"docs: applications and accessories documentation pages"`
- [x] Post-ship review fixes (adversarial, consistency and perf legs)
  - Pages: `calculator.md` no longer calls the double `kmath_cos`/`kmath_acos`/`kmath_pow`/`kmath_sqrt` ready (font-grade); `event-viewer.md` describes `dropped` as a per-window snapshot and rotation as rename-and-recreate; `pdf-viewer.md`, `obbrowse.md` and `video-player.md` state traversal bounds, the quadratic directory walk and an unmeasured frame target.
  - Roadmaps: `TODO-09` §4 parked on the kernel libraries' double-maths unification; `13-tools-accessories/TODO-02` §3 follows rotation by identity; `TODO-05` §2 and `07-networking/TODO-10` §3 bound the page-tree walk; ObBrowse §2 files the O(n^2) walk; `TODO-06` §2 uses SSE2 unpacks, not SSSE3 `_mm_shuffle_epi8`; the Photos table no longer claims §1 done.

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

> **Test runner:** N/A (docs pages only) | validation: `python3 scripts/site/build.py --check` prints `site: OK` with 204/232 TODO files documented

> **Notes:**
> - **What shipped:** fifteen contract-shaped pages in a new `docs/apps/` folder, one per apps and tools roadmap file, listed in its index and linked from the docs home page.
> - **How it integrates:** each page declares `covers=`, `sources=` and `reviewed=`, so the freshness check flags it when the image, font, network, crypto, Object Manager or logging code it describes changes.
> - **Status honesty:** no app in these roadmaps exists; the pages describe what runs today (image and font calls, `ping`, `sysinfo.exe`, the directory-object syscalls, `events.jsonl`) and link the owning sections.
> - **Scope boundary:** where a networking, desktop or graphics roadmap owns the engine or a duplicate app, the page names that owner and links its page instead of restating it.

> **Verified:** 2026-09-29 | commit `c98bf1cf8` | 10/10 items | build OK | site: OK, 204/232 documented; tests 34646 kernel + 17 user-mode PASS
> **Quality reviewed:** 2026-09-29 | Codex 4x (adversarial, adversarial post-ship, consistency, perf) | 1H+7M fixed, 0 open | scope: N/A (docs-only; no source changed; parity research and test coverage N/A; re-adversarial skipped: docs and roadmap-text fixes)

---

## 21. Document: SDK, release, ports and research

Write docs pages that meet the §3 contract for the 21 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Skip a file only if §2 already mapped a page to it and that page meets the contract; otherwise extend or write the page.

- [ ] Pages in `docs/sdk/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/12-user-platform-sdk/TODO-01-kernel-libraries.md` (TODO-01 -- Kernel Embedded Libraries)
  - `todo/12-user-platform-sdk/TODO-02-env-vars-process-abi.md` (TODO-02 -- Environment Variables & Process ABI)
  - `todo/12-user-platform-sdk/TODO-03-elf-reloc-kernel-modules.md` (TODO-03 -- ELF Relocations & Kernel Module System)
- [ ] Pages in `docs/sdk/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/12-user-platform-sdk/TODO-04-ntdll-user-runtime.md` (TODO-04 -- NTDLL & User-Mode Runtime)
  - `todo/12-user-platform-sdk/TODO-05-win32-subsystem.md` (TODO-05 -- Win32 Subsystem Server (CSRSS))
  - `todo/12-user-platform-sdk/TODO-06-sdk-distribution.md` (TODO-06 -- SDK Distribution & Developer Experience)
- [ ] Page in `docs/sdk/` for the next roadmap file, with its `covers=` directive
  - `todo/12-user-platform-sdk/TODO-07-win32-compat-matrix.md` (TODO-07 -- Win32 Compatibility Matrix & Bring-Up Ladder)
- [ ] Pages in `docs/release/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/15-installer-release/TODO-01-release-artifacts.md` (TODO-01 -- Disk Image, USB & Release Artifacts)
  - `todo/15-installer-release/TODO-02-unattended-install.md` (TODO-02 -- Unattended Installation & Deployment)
  - `todo/15-installer-release/TODO-03-update-server.md` (TODO-03 -- Update Server Infrastructure)
- [ ] Pages in `docs/release/` for the next 2 roadmap files, each with its `covers=` directive
  - `todo/15-installer-release/TODO-04-release-qa.md` (TODO-04 -- Release QA & Platform Certification)
  - `todo/15-installer-release/TODO-05-github-release-community.md` (TODO-05 -- GitHub Releases & Community Launch)
- [ ] Pages in `docs/ports/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/16-architecture-ports/TODO-01-arch-abstraction-layer.md` (TODO-01 -- Architecture Abstraction Layer)
  - `todo/16-architecture-ports/TODO-02-aarch64-kernel-port.md` (TODO-02 -- AArch64 Kernel Port)
  - `todo/16-architecture-ports/TODO-03-smp-scaling-processor-groups.md` (TODO-03 -- SMP Scaling & Processor Groups)
- [ ] Pages in `docs/research/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/18-future-research/TODO-01-multi-arch-port.md` (TODO-01 -- ARM64 & RISC-V Architecture Port)
  - `todo/18-future-research/TODO-02-hypervisor.md` (TODO-02 -- Type-1 Hypervisor (ImpossibleHV))
  - `todo/18-future-research/TODO-03-gpu-compositor.md` (TODO-03 -- GPU-Accelerated Compositor)
- [ ] Pages in `docs/research/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/18-future-research/TODO-04-secureboot-tpm.md` (TODO-04 -- Secure Boot, TPM 2.0 & Measured Boot)
  - `todo/18-future-research/TODO-05-ai-ml-runtime.md` (TODO-05 -- AI/ML Native Inference Runtime)
  - `todo/18-future-research/TODO-06-android-app-compatibility.md` (TODO-06 -- Android App Compatibility (Research Spike))
- [ ] Add every new page to its folder `index.md`, then run `python3 scripts/site/build.py --update-baseline`
- [ ] Commit: `"docs: sdk, release, ports and research documentation pages"`

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any file named in this section; each page renders on the local build (`python3 scripts/site/build.py`, then open `build/site/docs/`). Test on: WSL2 dev host.

---

## 22. Doc Freshness: `sources=` and a Stale-Page Warning

A page that was right when written goes wrong when its code changes. Neither Windows nor Linux documentation tracks this mechanically.

> [!TIP]
> Linux kernel-doc extracts API docs from source comments but cannot tell a narrative page is stale; Microsoft Learn relies on manual review dates. A per-page source list checked against git history catches narrative drift too.

- [x] Directive gains `sources=path[,path]` and `reviewed=YYYY-MM-DD`
  - Every source must be a TRACKED file or directory of the tree being checked (`build.py` `tracked_sources` / `check_sources`, via `freshness.tracked`), else an error; `reviewed=` must parse as a date.
- [x] `scripts/site/freshness.py` and `build.py --freshness` judge each page by content
  - Stale means the sources differ (`git diff`) between the commit that last changed the page's CONTENT and the tree being checked, so reverts, merges and deletions under a source directory are handled.
  - The baseline follows renames on the first-parent line with merge diffs; staged renames and a plain `mv` keep the old baseline.
  - An uncommitted edit of the page is `editing` (not during a merge); a shallow clone is `unknown`.
- [x] Stale pages are warnings, never errors, shown in lint and on the coverage page
  - `build.py` prints `WARN:` lines even under `--quiet`, and lint Check 30 forwards them on a passing check.
  - The coverage page gains a Freshness table; Pages runs on every push with full history, so the table cannot lag behind a path filter.
- [x] `sources=` and `reviewed=2026-09-28` on the 43 existing pages that document code (4 process pages need none)
  - Landing page feature cards get the same check with a per-card baseline; pages written by the domain sections add them under the contract rule.
- [x] Post-ship review fixes (4 rounds), each with a git-backed regression test
  - A merge cannot clear a page or card by dropping or renaming its sources: HEAD's directive sources still count (`with_head_sources`, rename-aware).
  - Duplicate or empty card titles are errors; card sources share the page normalisation (no trailing slash).
  - An unborn HEAD reports `new`; a git failure (log or the card `cat-file --batch` reader) reports `unknown` with the reason and a warning, never `new`.
  - Bounded history: one `git log -n1` per page (widened only past pure renames) and one `cat-file --batch` process for every card version.
- [/] The `sources=` rule in `docs/contributing/docs-page-contract.md`: parked on the page-contract section, which writes that file (item "Freshness rule in the contract")
  - The rule is already documented in `docs/infrastructure/documentation-site.md` and CLAUDE.md.
- [x] Commit: `"site: doc freshness, sources= directive and stale-page warning"`

**Test checkpoint:** touching a file listed in a page's `sources=` in a scratch commit makes `python3 scripts/site/build.py --freshness` list that page; reverting clears it. Test on: WSL2 dev host.

> **Test runner:** host-side `python3 scripts/site/tests/test_build.py` (class `Freshness`, 25 git-backed cases, plus `FeatureCards`) | validation: `python3 scripts/site/build.py --freshness`, lint Check 30 warnings

> **Notes:**
> - **What shipped:** `scripts/site/freshness.py` judges each page and feature card by content: stale when its `sources=` differ between the commit that last changed it and the tree being checked.
> - **How it integrates:** `build.py` validates sources against tracked files, prints `WARN:` lines that lint Check 30 now forwards, adds a Freshness table to the coverage page, and has a `--freshness` listing.
> - **Downstream:** Pages runs on every push with full history; 43 existing pages and all 9 cards carry `sources=` and `reviewed=`; domain sections add them under the contract rule.
> - **Canonical doc:** [`docs/infrastructure/documentation-site.md`](../../docs/infrastructure/documentation-site.md) "How do we notice when a page goes out of date?".
> - **Scope boundary:** warnings only, by design; the page-contract text for `sources=` is parked on the contract section.

> **Verified:** 2026-09-28 | commit `b5ffa270b` | 5/6 items | build OK | tests 46/46 PASS
> **Quality reviewed:** 2026-09-28 | Codex 14x (design, test-coverage, adversarial x4, re-adversarial x2, consistency x3, perf x3) | 29M fixed, 0 open | scope: N/A (host tooling and docs; no kernel, boot or desktop code)

---

## 23. Site Polish: Sitemap, Last-Updated, Link Health, OpenGraph

> **Spawned-by:** root

- [ ] Emit `sitemap.xml` and `robots.txt` for the landing page, the design page and every docs page, with `lastmod` from git; the Pages workflow must fetch full history (`fetch-depth: 0`) for this
- [ ] Show "Last updated <date>" in each docs page footer from `git log -1 --format=%cs -- <page>`
- [ ] Add Open Graph and canonical URL tags to docs pages from `project.json`
- [ ] Scheduled external-link check (the `linkcheck` equivalent): a weekly workflow HEAD/GETs every external `http(s)` link in `docs/**` and `gh-pages/`, with retry and an allowlist; never per-commit, since Check 30 covers internal links only
- [x] Landing page feature claims verified against the code and rendered from data
  - Every claim checked (mapper pass plus Codex): removed false ones (per-CPU run queues, CFS, "no legacy IDE polling", GPU acceleration, TCP "in progress", VirtIO-net, `WriteFile` in the SDK, a shipped shim chain, "zero legacy bloat"); planned work is now called planned, including Linux binary support in the pillars and meta text.
  - Cards live in `gh-pages/features.json` (`owners`, `sources`) and render into `{{feature_cards}}`; the "N roadmap sections to go" line is computed with the todo-graph producer's `extract_implementation_order`, and a dead owner, untracked source or dash fails `build.py --check`. Test: `FeatureCards` in `scripts/site/tests/test_build.py`.
- [x] GitHub About box (description, homepage, topics) derives from `project.json` and is drift-checked
  - `scripts/site/repo_meta.py`: `tagline` is the description, `site_url` the homepage, `topics` the topics; `--apply` writes them with `gh repo edit` and re-diffs, `--check` compares the live repo. Applied 2026-09-27 (removed the em dash, `http` homepage, added 5 topics).
  - Offline `validate()` (shape, dashes, https, GitHub topic rule, case-folded duplicates) runs in `build.py --check`; `.github/workflows/repo-metadata.yml` runs the live check on change and daily, so a GitHub UI edit turns it red. 4 tests in `scripts/site/tests/test_build.py`.
- [ ] Validate fragments on links from `docs/` into tracked non-docs Markdown (`todo/`, `specs/`) against GitHub heading slugs; `build.py` checks fragments only for `docs/` targets (`scripts/site/build.py:336-339`)
  - Measured 2026-09-28 with a GitHub-slug checker: 6 dead roadmap anchors in section 9's drafts (fixed) and 6 more in existing pages (`docs/boot/boot-menu.md`, `boot-protocol-changelog.md`, `bootstrap.md`, `docs/infrastructure/development-tooling.md`)
  - GitHub keeps underscores and drops other punctuation, so a hand-slugged `load_base` or `PS_PROTECTION` heading is the usual miss
- [ ] Commit: `"site: sitemap, last-updated dates, external link check, OpenGraph"`

**Test checkpoint:** the deployed site serves `https://impossibleos.co/sitemap.xml` listing every docs page; each docs page footer shows its last-updated date; the link-check workflow reports a planted dead external link in a fixture and passes on the real tree. Test on: WSL2 dev host; GitHub Actions `ubuntu-latest`.

---

## 24. Versioned Release Docs: Retention, Pinned Refs, SDK Reference

> **Spawned-by:** §23 (split)

A release snapshot is only useful if it survives later deploys and still points at the code it describes. Today every deploy publishes a fresh whole-site artifact (`.github/workflows/pages.yml`), `site-live.yml` repairs drift by redeploying `main`, and `scripts/site/build.py` hardcodes `main` in source, image and edit links (`build.py:310`, `:335`, `:435`, `:699`).

- [ ] Release snapshots: when a `v*` tag is pushed, build the docs at that tag into `docs/<version>/` of the published site and add a version picker; `main` stays the default
- [ ] Version manifest (for example `gh-pages/versions.json`) is the authoritative list of retained releases; every deploy AND every `site-live.yml` repair assembles `main` plus each listed release tree
- [ ] `scripts/site/verify_live.py` verifies every retained release tree, not just the files of the current `main` build
- [ ] Add a release ref and URL base to rendering: source, directory, image and edit links pin to the release commit; nav, search index and canonical URLs are scoped to that version
- [ ] Two version namespaces: OS `v*` and SDK `sdk/v*` (created by D12 T06 §8 `release-sdk.sh`) each trigger a snapshot under their own path
- [ ] Publish the SDK API reference that D12 T06 §2 `gendoc` writes to `sdk/docs/api-reference/` under `docs/sdk/api/` for `main` and per SDK tag -> XREF: `D12 T06 §2`
- [ ] Tests: two releases then a `main` deploy and a repair leave both releases served; a release page links to its tag commit after `main` deletes the referenced file; an `sdk/v*` fixture tag publishes reference pages matching its headers
- [ ] Commit: `"site: retained, version-pinned release docs and SDK API reference"`

**Test checkpoint:** after two test tags and a later `main` deploy, `verify_live.py` passes for `main` and both release trees; a release page's source link resolves at its tag; `docs/sdk/api/` renders on the local build. Test on: WSL2 dev host; GitHub Actions `ubuntu-latest`.

---

## 25. Docs Search Completeness and Accessibility

> **Spawned-by:** §23 (split)

Search drops content today: the indexer keeps only plain `text` tokens (inline code and code blocks are skipped, `build.py:374`) and truncates each page to 4,000 characters (`build.py:955`), so documented API names such as `boot_health_publish_json` return no hits. Result selection is visual only (`gh-pages/docs-template.html:205`), which a screen reader cannot follow.

- [ ] Improve search: index H2/H3 text with anchors so a hit jumps to the section, and rank title > heading > body; keep `search.json` under 2 MB
- [ ] Index inline-code identifiers, code examples and late-page text; meet the size budget with per-section records or index shards, never by silently dropping content
- [ ] Search box follows the W3C ARIA combobox pattern: `role=combobox`, `aria-expanded`, `aria-controls`, `aria-activedescendant` on arrow keys, results in a `listbox` with `option` roles
- [ ] Static accessibility checks in `build.py`'s check mode: every image has non-empty alt text, heading levels do not skip, every page has exactly one H1
- [ ] Tests: a search fixture finds an inline-code identifier and a match past character 4,000; the a11y check rejects a missing alt and an H2 to H4 skip
- [ ] Commit: `"site: complete search index and accessible search and pages"`

**Test checkpoint:** searching `boot_health_publish_json` on the local build returns its page; keyboard-only navigation of search results is announced by a screen reader (NVDA or Orca); the check mode reports a planted missing alt text. Test on: WSL2 dev host; Windows with NVDA.

---

## 26. Document: Host tools

> **Spawned-by:** §4 (split)

Write docs pages that meet the §3 contract for the 8 roadmap files below. Read each roadmap file and the code it names; document what exists today and link the roadmap sections for what does not. Split out of §4 because 17 pages is more than one worker context; the host-tools files share one folder and no dependency on the infrastructure pages.

- [ ] Pages in `docs/host-tools/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/14-host-tools/TODO-01-sdk-build-system.md` (TODO-01 -- SDK Build System)
  - `todo/14-host-tools/TODO-02-ixfs-mount.md` (TODO-02 -- IXFS Mount (Linux))
  - `todo/14-host-tools/TODO-03-addr2line.md` (TODO-03 -- ixfs-addr2line (Enhanced Address Resolver))
- [ ] Pages in `docs/host-tools/` for the next 3 roadmap files, each with its `covers=` directive
  - `todo/14-host-tools/TODO-04-crash-decode.md` (TODO-04 -- crash-decode (Post-Mortem Crash Analyzer))
  - `todo/14-host-tools/TODO-05-serial-analyze.md` (TODO-05 -- serial-analyze (Boot Log Analyzer))
  - `todo/14-host-tools/TODO-06-disk-inspect.md` (TODO-06 -- disk-inspect (Disk Image Browser))
- [ ] Pages in `docs/host-tools/` for the next 2 roadmap files, each with its `covers=` directive
  - `todo/14-host-tools/TODO-07-ixfs-fsck.md` (TODO-07 -- ixfs-fsck (Filesystem Consistency Checker))
  - `todo/14-host-tools/TODO-08-blackbox-log-extractor.md` (TODO-08 -- BlackBox Log Extractor)
- [ ] Add `docs/host-tools/index.md` (the folder's first pages), add every new page to it, then run `python3 scripts/site/build.py --update-baseline`
- [ ] Commit: `"docs: host tools documentation pages"`

**Test checkpoint:** `python3 scripts/site/build.py --check` prints `site: OK`; `docs/.coverage-baseline.json` no longer lists any `todo/14-host-tools/` file; each page renders on the local build. Test on: WSL2 dev host.

---

## OS Comparison

| ⭐  | Feature                          | 🪟 Win11                 | 🐧 Linux                   | 🚀 Impossible OS          |
| --- | -------------------------------- | ------------------------ | -------------------------- | ------------------------- |
| 💎  | Docs generated from in-tree text | ⚠️ Learn, separate repos | ✅ Sphinx `Documentation/` | ✅ §1 `docs/` to site     |
| ⭐  | Build fails on dead doc links    | ❌ Not enforced          | ⚠️ Warnings only           | ✅ §1 Check 30 error      |
| ⭐  | Every subsystem has a docs page  | ⚠️ Public APIs only      | ⚠️ Uneven                  | ⬜ §4-§21, §26 coverage   |
| ⭐  | Facts derived from one source    | ❌ Manual                | ❌ Manual                  | ✅ §1 `project.json`      |
| ⭐  | Stale narrative page detection   | ❌ Review dates          | ❌ Not tracked             | ✅ §22 `sources=` warns   |
| 💎  | Versioned docs per release       | ✅ Per version           | ✅ Per kernel version      | ⬜ §24 retained snapshots |
| 💎  | Site search                      | ✅ Full search           | ✅ Sphinx search           | ⚠️ §1 basic, §25 full     |
| 💎  | External link rot check          | ✅ Learn link validation | ✅ Sphinx `linkcheck`      | ⬜ §23 weekly workflow    |
| 💎  | Published API reference          | ✅ Learn API reference   | ✅ kernel-doc              | ⬜ §24 from D12 T06 §2    |
| 💎  | Accessible docs UI               | ✅ WCAG conformance      | ⚠️ Theme-dependent         | ⬜ §25 ARIA + checks      |

> **After §1-§3:** the pipeline, the gate and the page contract exist; coverage is measured and cannot regress.
> **After §4-§21 and §26:** every roadmap file is documented and the baseline is empty.
> **After §22-§25:** stale pages are flagged and the site matches mainstream docs portals on navigation, versions, search and accessibility.

---

## Unit Tests

> Host-side tests, not kernel tests: `scripts/site/tests/test_build.py` (stdlib `unittest`), wired into `scripts/test-tooling.sh` as a nested suite so CI runs it.

- [x] Create `scripts/site/tests/test_build.py` (46 tests pass 2026-09-28) with:
  - `github_slug("1. Layout at a Glance")` == `"1-layout-at-a-glance"`; duplicate headings get `-1`, `-2` suffixes
  - `page_url("index.md")` == `""`, `page_url("boot/index.md")` == `"boot/"`, `page_url("boot/x.md")` == `"boot/x.html"`
  - `sync_regions()` rewrites a stale `<!-- project:release_date_long -->` region and reports an unknown key as an error
  - `merge_stats()` keeps the original `stat_*` values while other regions are rewritten
  - A fixture docs tree with a dead relative link and a dead anchor yields exactly those two errors
  - `check_baseline()` errors on a new undocumented file and on a stale baseline entry; `--update-baseline` never adds
  - `hex_to_css("#80FFFFFF")` == `"rgba(255, 255, 255, 0.502)"`; `gen_theme_header.argb("#60CDFF")` == `"0xFF60CDFFu"`
  - `gen_theme_header.check()` is empty on the committed tree
  - Scripts that do not parse, GitHub About-box validation and diff, and feature cards (open-section count, escaping, dead owner, untracked source, dash)
  - Freshness on a throwaway repo: stale then fresh after a revert, deletion under a source directory, committed and uncommitted pure renames, a merge resolution as baseline, raw file names, literal pathspecs, editing vs merge, per-card baselines, `reviewed` bumps, merged card sources
- [ ] Extend `scripts/site/tests/test_build.py` with the §24 cases (release retention across deploys, ref-pinned links, `sdk/v*` namespace) and the §25 cases (inline-code search hit, match past 4,000 chars, missing alt, heading skip)
- [x] Register the suite in `scripts/test-tooling.sh` (runs on every tooling pass; it takes about 2 s, so it is not path-scoped)
- [x] Commit: `"test: site generator unit tests"` (landed with the section 1-2 review)

## Verification

- [ ] `python3 scripts/site/build.py --check` -> `site: OK (... N/M TODO files documented)` with N equal to M (every roadmap file) at completion
- [ ] `bash scripts/lint.sh` -> no Check 30 error
- [ ] `bash scripts/test-tooling.sh --quiet` -> the site suite passes
- [ ] The latest `GitHub Pages` run is green and `https://impossibleos.co/docs/` serves the current `main`
- [ ] `docs/.coverage-baseline.json` lists no files
- [ ] Commit: `"00-infrastructure/TODO-10: documentation site and corpus complete"`

**Test runner:** N/A (host-side site tooling; no kernel suite) | validation: `python3 scripts/site/build.py --check` + `scripts/site/tests/test_build.py` via `scripts/test-tooling.sh`
