# Repository Transfer Preflight Inventory and Risk Register

> Owner: [`todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md`](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md) §1.
> Baseline captured: **2026-04-27** (commit-time snapshot; re-baseline immediately before pressing the transfer button per §4).
> Purpose: every observable surface that must be re-checked after the move from `rizonesoft/impossible-os` to `rizonetech/impossible-os` is captured here once so §5/§7 can compare directly instead of remembering. Move-back to a personal account (§8) re-uses the same shape.

This document is *observation only*. It does not change Pages, DNS, or any GitHub setting. Edits that ship configuration belong in §2-§9 of the TODO.

---

## 1. Repository Identity

| Field | Value |
|---|---|
| `nameWithOwner` | `rizonesoft/impossible-os` |
| Node ID | `R_kgDORgIuVg` |
| Owner login | `rizonesoft` (user, NOT an organization -- `isInOrganization: false`) |
| Visibility | `PRIVATE` (`isPrivate: true`) |
| Default branch | `main` |
| Description | "A 64-bit operating system built from scratch for modern x86-64 hardware -- UEFI boot, compositing desktop, Win32-compatible API" |
| Homepage URL | `https://rizonesoft.com` |
| License | GPL-3.0 (`gpl-3.0`, "GNU GPLv3") |
| Topics | `bare-metal`, `freestanding`, `operating-system`, `os-development`, `uefi-boot` |
| Created | `2026-03-06T15:11:41Z` |
| Last push | `2026-04-27T19:49:50Z` |
| Disk size | 32820 KB |
| Stargazers / watchers / forks | 0 / 0 / 0 |
| Open issues | 0 |
| Issues enabled | yes |
| Discussions enabled | no |
| Wiki enabled | yes (unused) |
| Projects (classic) enabled | yes (unused) |
| Security policy file enabled | yes (`SECURITY.md`) |
| Blank issues enabled | yes |
| Archived / fork / template | no / no / no |
| Merge methods allowed | merge, squash, rebase (all three) |
| Auto-delete branch on merge | no |
| SSH URL | `git@github.com:rizonesoft/impossible-os.git` |
| HTTPS URL | `https://github.com/rizonesoft/impossible-os` |

**Local remotes (`git remote -v`):**

```
origin      https://github.com/rizonesoft/impossible-os.git              (fetch + push)
bootloader  https://github.com/rizonesoft/impossible-os-bootloader.git   (fetch + push)
```

The `bootloader` remote is a separate repository (`rizonesoft/impossible-os-bootloader`) and is OUT OF SCOPE for this TODO. TODO-09 transfers `impossible-os` only; whether the bootloader repo follows is a later decision.

---

## 2. GitHub Pages State

| Field | Value |
|---|---|
| Status | `built` |
| `build_type` | `legacy` (publishes from a branch, NOT from the workflow) |
| Source branch / path | `gh-pages` / `/` |
| Custom domain (CNAME) | `impossibleos.co` |
| Custom 404 page | yes |
| Public Pages flag | yes |
| HTML URL (per API) | `http://impossibleos.co/` (HTTP, not HTTPS -- see HTTPS-enforcement note below) |
| `https_enforced` | **`false`** |
| `https_certificate.state` | `approved` |
| `https_certificate.domains` | `impossibleos.co`, `www.impossibleos.co` |
| `https_certificate.expires_at` | `2026-06-16` |
| Pages environment | `github-pages` (id `13147855799`, created `2026-03-18T12:19:40Z`) |
| Environment protection | `branch_policy` (custom branch policies; admins can bypass) |
| Latest Pages workflow run | `id 24294368082` (`2026-04-12T00:11:46Z`) **failed** -- `Failed to CreateArtifact: Artifact storage quota has been hit`. |
| Latest successful workflow run | `id 23255656274` (`2026-03-18T16:33:14Z`). The 2026-04-12 attempt is the only run after that and the only failure; no further runs have fired since because the workflow's `paths: ['gh-pages/**']` trigger has not matched and no `workflow_dispatch` was issued. |
| Latest deployment to env | `id 4340497396` from `gh-pages` branch sha `35e7dcd5` (`2026-04-12T00:27:47Z`) |

**Two findings flagged for §3 / §5:**

1. **Pages source mismatch with TODO-09 introductory callout.** The TODO's `Current state` says "GitHub Pages is deployed by `.github/workflows/pages.yml`". In reality, the API reports `build_type: legacy` from the `gh-pages` branch -- the live site is whatever was last pushed to `refs/heads/gh-pages` (sha `35e7dcd5`, 2026-04-12). The workflow IS configured but its last run failed. §3 must decide whether to switch the source to "GitHub Actions" cleanly during/after the transfer or keep the legacy branch source. Until then, both must keep working.
2. **HTTPS enforcement is OFF.** Cert is approved and the apex serves on HTTPS, but `https_enforced: false` means the default redirect from `https://rizonesoft.github.io/impossible-os/` lands on `http://impossibleos.co/` (plain HTTP), which then upgrades. This is a hardening gap that should be fixed in §5 under the new owner.

**Pages workflow failure root cause:** `Failed to CreateArtifact: Artifact storage quota has been hit.` for the `rizonesoft` user account. Transfer to the `rizonetech` enterprise org will get a new artifact storage budget, which paradoxically may *fix* this. Worth verifying in §7 by re-running the workflow after transfer.

---

## 3. Live Domain Baseline (curl + DNS)

Captured 2026-04-27 ~19:54 UTC.

### HTTP HEAD

| URL | Status | Final URL | Notes |
|---|---|---|---|
| `https://impossibleos.co/` | `200` | same | served directly; `last-modified: 2026-04-12T00:27:55Z`, `etag: "69dae70b-b9bc"`, `content-length: 47548`, `server: GitHub.com` |
| `https://www.impossibleos.co/` | `301 -> 200` | `https://impossibleos.co/` | Pages-issued redirect |
| `https://rizonesoft.github.io/impossible-os/` | `301 -> 200` | `http://impossibleos.co/` (HTTP -- because `https_enforced: false`) -> apex serves over HTTPS via Fastly |
| `http://impossibleos.co/` | `200` | same | upgrades to HTTPS via apex cert |

### DNS

| Name | Type | Value |
|---|---|---|
| `impossibleos.co` | A (apex) | `185.199.108.153`, `185.199.109.153`, `185.199.110.153`, `185.199.111.153` -- the 4 GitHub Pages anycast IPv4 addresses (current as of GitHub Pages docs). |
| `impossibleos.co` | AAAA | **NOT SET** -- IPv6 not currently configured at the apex. §3 (TODO) explicitly tracks this as an optional decision. |
| `www.impossibleos.co` | CNAME | `rizonesoft.github.io.` (visible in resolver chain via `getent hosts`; CNAME target then resolves to the GitHub Pages anycast addresses, both IPv4 `185.199.10[8-11].153` and IPv6 `2606:50c0:800[0-3]::153`). |

**TTL inspection:** `dig` is not installed on this host (`/bin/bash: line 3: dig: command not found`). Operators must run `dig +nocmd impossibleos.co A` and `dig +nocmd www.impossibleos.co CNAME` from a host with BIND tools (or `nslookup -debug`) immediately before transfer to capture the authoritative TTL. §3 already plans to drop the `www` CNAME TTL pre-transfer; the apex A-record TTL also needs the snapshot for the §7 "TTL is restored to normal value after transfer window" check.

**Apex HTTPS certificate:** the GitHub-managed cert covers BOTH `impossibleos.co` and `www.impossibleos.co`, expires `2026-06-16`. Transfer must complete and the new owner's Pages settings must accept the custom domain BEFORE that expiry, otherwise GitHub may not auto-renew.

---

## 4. GitHub Actions State

| Field | Value |
|---|---|
| Actions enabled | yes |
| `allowed_actions` | `all` (no restriction list) |
| `sha_pinning_required` | no |
| Default workflow permissions (`GITHUB_TOKEN`) | `read` |
| Can approve own PR reviews | no |
| Required workflow approvals | (none -- repo is solo) |
| Runner groups | default (GitHub-hosted only; no self-hosted) |

**Active workflows (`gh workflow list --all`):**

| Name | File | ID | State |
|---|---|---|---|
| Build | `.github/workflows/build.yml` | 247801843 | active |
| Label PRs | `.github/workflows/labeler.yml` | 247848080 | active |
| Deploy GitHub Pages | `.github/workflows/pages.yml` | 247912708 | active |
| Release | `.github/workflows/release.yml` | 247815431 | active |
| Stale Issues | `.github/workflows/stale.yml` | 247847233 | active |
| TODO graph | `.github/workflows/todo-graph.yml` | 265120126 | active |
| Visual Regression | `.github/workflows/visual-regression.yml` | 264848357 | active |
| pages-build-deployment | (auto-generated by Pages legacy source) | 247909841 | active |

The TODO's Inputs list names five workflows; the actual count is **seven** repo-defined workflows plus the auto-generated `pages-build-deployment`. §6 must include `todo-graph.yml` and `visual-regression.yml` in the URL/badge sweep if either references the old owner.

**Workflows that reference the old owner explicitly:** must be checked file-by-file in §6 (this preflight does not edit). The fast scan to run during §6:

```bash
rg -n 'rizonesoft' .github/workflows/
```

**Pre-transfer concurrency note:** all current workflows are triggered on `push` to `main` or on `workflow_dispatch`. None depend on owner-scoped GitHub Apps or marketplace-restricted actions. The `actions/*` family used by `pages.yml` (checkout / configure-pages / upload-pages-artifact / deploy-pages) is universally available; under the `rizonetech` enterprise plan, no allowlist needs adjustment.

---

## 5. Repository Settings That Do Not Reliably Travel

Items GitHub may or may not bring across cleanly during a transfer; verify directly under the new owner.

### Branch protection / rulesets

| Source | Status |
|---|---|
| Classic branch protection on `main` | **NONE** (`HTTP 404 Branch not protected`). |
| Repository ruleset "Default Branch Security" | **active** (id `14058331`, target `~DEFAULT_BRANCH`, created 2026-03-18). Rules: `required_status_checks` (`Build Impossible OS`, strict, `do_not_enforce_on_create: true`, integration `15368`), `non_fast_forward`, `required_linear_history`, `deletion`. Bypass actor: repo `Maintain` role (`actor_id: 5`, mode `always`). |

§5 (TODO) must reapply this ruleset under the new owner if GitHub does not migrate it. The required-status-check `context: "Build Impossible OS"` matches the `Build` workflow's job name; check that the job name has not changed during transfer.

### Secrets and variables

| Type | Names |
|---|---|
| Repository secrets | `BOOTLOADER_REPO_TOKEN` (created 2026-03-14T14:40:49Z) |
| Repository variables | (none) |
| Environment secrets | (none in `github-pages`; check post-transfer in case org policy adds any) |

`BOOTLOADER_REPO_TOKEN` is the single critical secret; the release workflow consumes it. **Validate post-transfer by running the release workflow on a no-op tag.** If GitHub fails to migrate the secret value (it usually preserves the name but not always the value), regenerate the token and re-push it.

### Environments

| Name | Created | Protection | Branch policy |
|---|---|---|---|
| `github-pages` | 2026-03-18T12:19:40Z | `branch_policy` (admins can bypass) | custom branch policies allowed; protected branches not required |

### Webhooks, deploy keys, integrations

| Surface | State |
|---|---|
| Webhooks | none |
| Deploy keys | none |
| GitHub App installations on the repo | (not enumerable without org-scope token; verify in UI under both old and new owner) |
| Dependabot alerts (`vulnerability-alerts`) | **DISABLED** |
| Automated security fixes | `enabled: false` |
| Code scanning, secret scanning | (not directly enumerable on a private repo through `gh api`; verify in the UI) |

§5 should turn Dependabot alerts on under the new owner -- this is a free hardening for a freestanding-kernel repo even though dependency surface is small.

### Releases / packages

| Field | Value |
|---|---|
| Latest release | `Impossible OS v26.3.18-alpha.821` (pre-release, tag `v26.3.18-alpha.821`, 2026-03-18T09:35:11Z) |
| Packages | (none enumerated; `gh api repos/.../packages` returns empty) |

---

## 6. Access Model

| Surface | State |
|---|---|
| Direct collaborators on `rizonesoft/impossible-os` | only `rizonesoft` (admin / maintain / push / pull / triage). |
| Outside collaborators | none. |
| Teams | not applicable (user-owned, not an org). |
| CODEOWNERS handles | only `@derickpayne` (in 14 lines of `.github/CODEOWNERS`). |
| Protected-branch bypass actors | `RepositoryRole` id `5` (Maintain role), bypass mode `always`. |
| Pages / DNS administrator | repo owner (`rizonesoft`) for Pages; DNS handled outside GitHub by the registered DNS provider for `impossibleos.co`. |

**Org-level baseline for the receiving side (`rizonetech`):**

| Field | Value |
|---|---|
| Org login | `Rizonetech` (canonical capitalization; URLs are case-insensitive, code uses lowercase). |
| ID | `279138845`. |
| Plan | `enterprise` (`filled_seats: 1`, `seats: 50`, `space: 976562499` bytes). |
| Public repos / private repos | 0 / 0 / 0 (clean target). |
| Members can create repositories | yes (`all` repo creation type). |
| Forking private repos | disabled. |
| Current user (`rizonesoft`) membership | `active` / `admin` / direct membership. |
| Existing `rizonetech/impossible-os` | **does not exist** (`GraphQL: Could not resolve to a Repository`). Transfer destination is clean. |

§2 (TODO) is the section that confirms org Actions / Pages / Copilot policy parity before pressing the transfer button.

---

## 7. Hard-Coded Owner References (Inventory)

Captured by `rg -n 'rizonesoft/impossible-os|github\.com/rizonesoft|rizonesoft\.github\.io|hits\.sh/rizonesoft' README.md CONTRIBUTING.md SECURITY.md CODE_OF_CONDUCT.md AGENTS.md CLAUDE.md docs gh-pages .github todo scripts` -- the path set was expanded to include `todo/` and `scripts/` after the initial pass missed [`scripts/todo-graph/render.py:42`](../../scripts/todo-graph/render.py#L42) (`REPO_URL_BASE = "https://github.com/rizonesoft/impossible-os/blob/main"`), the actual generator constant for TODO graph links. **Re-runs of this baseline MUST use this expanded path set**, not the narrower one.

**Two snapshots are tracked because this preflight doc itself is a source of references:**

| Snapshot | Total matches | Files | Captured |
|---|---|---|---|
| **Pre-preflight-doc baseline (expanded path, theoretical)** | 303 | ~37 | 2026-04-27, theoretical state before this doc, the [TODO-09 file](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md) self-references, and the [`github-setup.md`](github-setup.md) overview pointer were committed. Represents the inventory the [URL/badge/docs/generated-link sweep](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#6-url-badge-docs-and-generated-link-sweep) must reduce. (Computed as 364 - 61, where 61 = 43 from this preflight doc + 17 from the [TODO-09 file](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md) self-references + 1 from the github-setup overview-pointer edit.) |
| **Post-preflight-doc baseline (expanded path, measured)** | 364 | 39 | 2026-04-27, measured immediately after this doc, the [Preflight Inventory section](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#1-preflight-inventory-and-risk-register) stamp, and the github-setup pointer were committed. An operator re-running the rg command at transfer time will see this number; the [URL/badge/docs/generated-link sweep](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#6-url-badge-docs-and-generated-link-sweep) MUST classify the preflight doc, the [TODO-09 file](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md), and the `Pre-doc / Post-doc` rows in this very table as intentionally retained historical-record sources and exclude them from "active GitHub repo" references that need owner-rewriting. |

**Per-pattern breakdown (post-doc expanded baseline):**

| Pattern | Hits |
|---|---|
| `rizonesoft/impossible-os` | 339 |
| `github.com/rizonesoft` | 298 |
| `rizonesoft.github.io` | 23 |
| `hits.sh/rizonesoft` | 5 |
| **Total raw matches (with overlap)** | **364** |

**Files containing at least one match (39 in post-doc expanded baseline; the originally-scanned 28 narrow-path files plus [`docs/infrastructure/repository-transfer-preflight.md`](repository-transfer-preflight.md), 8 files under `todo/` and 2 files under `scripts/`):**

```
README.md                                 docs/index.md
CONTRIBUTING.md                           docs/getting-started/virtualbox.md
docs/infrastructure/github-setup.md       docs/infrastructure/development-tooling.md
docs/infrastructure/ai-system.md          docs/infrastructure/todo-graph.md
docs/boot/boot-protocol-changelog.md      docs/infrastructure/repository-transfer-preflight.md
.github/ISSUE_TEMPLATE/feature-request.yml .github/ISSUE_TEMPLATE/config.yml
gh-pages/index.html                       gh-pages/err/index.html
gh-pages/err/errors.js                    gh-pages/err/0000/index.html
gh-pages/err/0001/index.html              gh-pages/err/0002/index.html
gh-pages/err/0003/index.html              gh-pages/err/0004/index.html
gh-pages/err/0005/index.html              gh-pages/err/0006/index.html
gh-pages/err/0007/index.html              gh-pages/err/0008/index.html
gh-pages/err/0009/index.html              gh-pages/err/000a/index.html
gh-pages/err/000b/index.html              gh-pages/err/000c/index.html
gh-pages/err/000d/index.html              todo/00-infrastructure/INDEX.md
todo/00-infrastructure/TODO-02-ai-development-system.md
todo/00-infrastructure/TODO-06-todo-metadata-layer.md
todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md
todo/12-user-platform-sdk/TODO-06-sdk-distribution.md
todo/15-installer-release/TODO-01-release-artifacts.md
todo/15-installer-release/TODO-03-update-server.md
todo/15-installer-release/TODO-05-github-release-community.md
scripts/test-ai-system.sh                 scripts/todo-graph/render.py
```

**`scripts/` matches need explicit handling, not "intentional retention":**

- [`scripts/todo-graph/render.py:42`](../../scripts/todo-graph/render.py#L42) -- `REPO_URL_BASE = "https://github.com/rizonesoft/impossible-os/blob/main"`. This is the generator constant for TODO graph node URLs in [`docs/infrastructure/todo-graph.md`](todo-graph.md). The [URL/badge/docs/generated-link sweep](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#6-url-badge-docs-and-generated-link-sweep) must update this constant in lockstep with the transfer; the regenerated graph then re-emits with the new owner. Do NOT manually edit the generated graph block. Risk row R10 names this file directly.
- [`scripts/test-ai-system.sh:396`](../../scripts/test-ai-system.sh#L396) -- a `t_info` log line ("verify repo/org Settings -> Copilot access = disabled for rizonesoft/impossible-os"). The [URL/badge/docs/generated-link sweep](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#6-url-badge-docs-and-generated-link-sweep) should update this string to track the active owner; this is informational rather than a URL.

Notes for §6:

- `SECURITY.md`, `CODE_OF_CONDUCT.md`, `AGENTS.md`, `CLAUDE.md` returned **zero** matches in this scan -- they identify the project / Claude Code system but do not name the GitHub URL. They should not need owner-related edits.
- `gh-pages/err/*/index.html` is the largest group (15 files); the error-page header/footer link block is the most likely shared template that produces hundreds of references.
- `rizonesoft.github.io` appears 23 times under the expanded scan (5 in the narrow-path baseline -- the canonical / Open Graph / JSON-LD / visitor-counter spots on `gh-pages/index.html` plus one other landing-page surface; the additional 18 are spread across `todo/` and self-references in this preflight doc).
- `hits.sh/rizonesoft` appears 5 times under the expanded scan (1 in the narrow-path baseline -- the active visitor-counter badge URL in `gh-pages/index.html`; the other 4 are self-references in this preflight doc and the [Preflight Inventory section stamp](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#1-preflight-inventory-and-risk-register) prose that quote the pattern).

This inventory does NOT classify which references are the active GitHub repo (must change in §6) versus the brand `rizonesoft.com` (must NOT change, since the company name does not move). §6 walks the classification.

---

## 8. Remote Consumers (Notify or Re-Point)

| Consumer | Current state | Action |
|---|---|---|
| Local clones (this dev host) | `origin` remote points at `https://github.com/rizonesoft/impossible-os.git` | §4 updates after transfer; redirect covers transient gap. |
| Existing release tarballs / artifacts | hosted at `https://github.com/rizonesoft/impossible-os/releases/...` -- one pre-release `v26.3.18-alpha.821` published 2026-03-18 | GitHub maintains release artifact URLs through the transfer redirect. |
| Public Pages link (`https://impossibleos.co/`) | live; preferred public surface | unchanged before/during/after transfer (see §3 of TODO). |
| Default Pages URL (`https://rizonesoft.github.io/impossible-os/`) | live; redirects to apex over HTTP | will move to `https://rizonetech.github.io/impossible-os/`; OLD URL relies on GitHub repo redirect. |
| `BOOTLOADER_REPO_TOKEN` consumers | the release workflow only | re-validated post-transfer. |
| External docs / social profiles / pinned GitHub Apps | unknown to this preflight | operator must enumerate manually before §4; this preflight cannot see private third-party references. |
| Visitor-counter `hits.sh` badge | namespace `hits.sh/rizonesoft.github.io` -- counter accumulates per-namespace | counter resets when namespace switches to `rizonetech.github.io`; acceptable for a private repo. |

---

## 9. Risk Register

Severity scale: **Critical** (Pages or repo unreachable for any duration), **High** (broken automation / lost setting / wrong policy), **Medium** (cosmetic or recoverable churn), **Low** (advisory).

| # | Risk | Severity | Likelihood | Mitigation | Owner |
|---|---|---|---|---|---|
| R1 | Pages custom-domain detachment when GitHub asks the new owner to re-verify the domain on transfer. Apex `impossibleos.co` could go dark for hours. | Critical | Medium | Verify the domain at the org level BEFORE transfer (§2). Keep the legacy `gh-pages` branch source intact through the transfer (do NOT switch to "GitHub Actions" source on the same day). Watch `gh api repos/rizonetech/impossible-os/pages` immediately after transfer for `protected_domain_state` regressing from `null`. | §2 + §3 + §4 |
| R2 | DNS propagation delay on `www.impossibleos.co` CNAME flip from `rizonesoft.github.io` -> `rizonetech.github.io`. | High | High | Drop TTL pre-transfer (§3). Apply CNAME flip only after the new Pages endpoint serves (§4). Apex A records are unchanged because they target the GitHub anycast IPs, not the owner. | §3 + §4 |
| R3 | Pages HTTPS certificate re-provisioning delay -- GitHub typically issues a fresh cert when the custom domain is re-verified. | High | Medium | Tolerate transient HTTPS warning on the default Pages URL; apex serves through Fastly with the existing cert (`expires 2026-06-16`). Do NOT enable `https_enforced` until the cert reissues. | §5 |
| R4 | Pages workflow on `main` branch is currently FAILING (`Artifact storage quota`) -- the move could mask the failure as "transfer broke Pages". | High | Already realized | Document baseline failure state here; expect new owner's enterprise-plan storage to allow the workflow to succeed; verify with `gh run list -w pages.yml -R rizonetech/impossible-os` post-transfer. The legacy `gh-pages` branch source keeps the live site up regardless. | §3 + §7 |
| R5 | Lost or unreadable `BOOTLOADER_REPO_TOKEN` after transfer -- secret values do not always migrate. | High | Low-Medium | Re-validate post-transfer by triggering a release workflow dry run. If broken, regenerate token and push to repo secrets. | §5 |
| R6 | Ruleset "Default Branch Security" not migrated to new owner -- main branch becomes unprotected. | High | Medium | §5 verifies the ruleset survives; if not, recreate it from this baseline (rules listed verbatim in §5). | §5 |
| R7 | Org-level Actions policy disables existing workflows under `rizonetech` (allowlist or token-permissions tighter than user-account default). | High | Medium | §2 confirms org Actions policy permits `actions/checkout`, `actions/configure-pages`, `actions/upload-pages-artifact`, `actions/deploy-pages`, plus all build/release/labeler/stale/todo-graph/visual-regression action versions BEFORE pressing transfer. | §2 |
| R8 | CODEOWNERS / collaborators / Maintain-role bypass actor not preserved across owners. | Medium | Medium | Solo-owner repo so collaborator loss is bounded (only `rizonesoft` is on the list). §5 verifies CODEOWNERS handle `@derickpayne` is still resolvable under the new owner; recreate ruleset bypass actor to map to a `rizonetech` team member. | §5 |
| R9 | Badges in `README.md` and `docs/` break because they URL-bake the old owner. **39 files / 364 matches** under the expanded scan (the historical narrow-path baseline was 28 files / 283 matches). | Medium | High | The [URL/badge/docs/generated-link sweep](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#6-url-badge-docs-and-generated-link-sweep) (already inventoried in section 7 above) replaces active references; redirect is convenience only. | §6 |
| R10 | Generated TODO graph links in [`docs/infrastructure/todo-graph.md`](todo-graph.md) go stale -- those are auto-emitted by [`scripts/todo-graph/render.py`](../../scripts/todo-graph/render.py) (the `REPO_URL_BASE` constant at line 42 is the single source of truth for graph node URLs; `build.py` orchestrates and writes but does NOT hold the URL constant). | Medium | High | The [URL/badge/docs/generated-link sweep](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#6-url-badge-docs-and-generated-link-sweep) must update `render.py:42` and re-run `python3 scripts/todo-graph/build.py` to regenerate the doc. Do NOT manually edit the generated graph block. | §6 + §9 |
| R11 | Recreating the old repository path `rizonesoft/impossible-os` during the redirect window destroys GitHub's automatic repo redirect. | High | Low (operator discipline) | §4 explicitly forbids recreating the path during the redirect window. Documented here as a tripwire. | §4 |
| R12 | Org-level Copilot / autonomous coding-agent policy diverges from project doctrine, silently enabling cloud-agent PRs. | High | Medium | §2 / §5 / §7 explicitly check that org Copilot Access policies do NOT enable coding-agent / autonomous PR flow for the repo. Pre-transfer check with admin user, post-transfer check via UI. (No public API surface for this -- manual UI inspection required.) | §2 + §5 + §7 |
| R13 | Visibility flip during transfer -- transferring a private repo to an org that allows public repos still requires explicit visibility change; no risk of accidental flip on transfer alone, but R12 + visibility flip together would expose private artifacts. | Medium | Low | Visibility stays `PRIVATE` through the transfer; §8 governs the eventual public flip with its own preflight gate. | §4 + §8 |
| R14 | DNS provider's TTL drop is a noop if the provider treats the apex as an ALIAS/ANAME with provider-side caching. | Low | Low-Medium | §3 calls this out; operator must check the DNS provider's actual TTL behavior, not just the resolver-visible TTL. | §3 |
| R15 | `https_enforced: false` left ON post-transfer leaves a plain-HTTP redirect chain from `https://rizonetech.github.io/impossible-os/`. | Low | Medium | §5 turns HTTPS enforcement on after the new cert reissues; this is a hardening fix that the move enables, not a regression. | §5 |

---

## §2 Receiving Org (`rizonetech`) Readiness Probe

> Captures the pre-transfer readiness of the `rizonetech` organization. Combines public-API observations (token scopes `gist, read:org, repo, workflow`) with operator-only items that need either `gh auth refresh -h github.com -s admin:org` or a UI walkthrough. Each item names the authoritative source so the [URL/badge/docs/generated-link sweep](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#6-url-badge-docs-and-generated-link-sweep) and post-transfer audit have a concrete reference.

### Confirmed from public API (no `admin:org` needed)

| Field | Value | Source |
|---|---|---|
| `login` | `Rizonetech` (canonical case; URLs case-insensitive, code uses `rizonetech`) | `gh api orgs/rizonetech` |
| `plan` | `enterprise` | `gh api orgs/rizonetech` |
| `private_repos` plan limit | 999999 | §1 baseline (`gh api orgs/rizonetech --jq '.plan.private_repos'`) |
| `seats` | 50 (1 filled) | §1 baseline |
| `members_can_create_repositories` | yes | `gh api orgs/rizonetech` |
| `members_allowed_repository_creation_type` | `all` (internal / private / public) | `gh api orgs/rizonetech` |
| `members_can_fork_private_repositories` | **disabled** | `gh api orgs/rizonetech` |
| `default_repository_permission` | `read` | `gh api orgs/rizonetech` |
| `web_commit_signoff_required` | no | `gh api orgs/rizonetech` |
| `two_factor_requirement_enabled` | **no** -- 2FA is not org-required | `gh api orgs/rizonetech` |
| `is_verified` | no (org-name verification, separate from custom-domain verification) | `gh api orgs/rizonetech` |
| `has_organization_projects` / `has_repository_projects` | yes / yes | `gh api orgs/rizonetech` |
| `saml_provider` | null (no SSO enforcement) | `gh api orgs/rizonetech --jq '.saml_provider'` |
| Org members (public) | one entry: `rizonesoft` (admin) | `gh api orgs/rizonetech/members` |
| Org public teams | zero | `gh api orgs/rizonetech/teams` |
| Org repositories | zero (clean transfer destination -- no `rizonetech/impossible-os` collision) | `gh api orgs/rizonetech/repos` |
| Org GitHub App installations | zero (`total_count: 0`) | `gh api orgs/rizonetech/installations` |
| Org credential authorizations | empty | `gh api orgs/rizonetech/credential-authorizations` |

### Confirmed: operator + current admin membership

| Field | Value | Source |
|---|---|---|
| Operator `rizonesoft` membership | `state: active`, `role: admin`, `direct_membership: true` | `gh api user/memberships/orgs/rizonetech` (recorded in §6 Access Model above) |

### Org security defaults for NEW repositories (read off `gh api orgs/rizonetech`)

| Default toggle | Current value | Implication for the transferred repo |
|---|---|---|
| `advanced_security_enabled_for_new_repositories` | `false` | Source repo is private; advanced security is a paid GHAS add-on; not required for transfer. |
| `dependabot_alerts_enabled_for_new_repositories` | `false` | Source repo also has Dependabot disabled (§5 baseline). Transfer is parity-compatible. R5 in the risk register flags this as a hardening opportunity post-transfer, not a blocker. |
| `dependabot_security_updates_enabled_for_new_repositories` | `false` | Same as above. |
| `dependency_graph_enabled_for_new_repositories` | `false` | Free feature; consider enabling post-transfer. |
| `secret_scanning_enabled_for_new_repositories` | `false` | Private-repo secret scanning is a paid GHAS feature; not blocking. |
| `secret_scanning_push_protection_enabled_for_new_repositories` | `false` | Same. |

These are **org defaults for newly created repos** -- transferred repos preserve their own settings. The transfer does not regress against the source-repo baseline (which is also "all off"), but the org defaults being all-off means the transfer is **not** an automatic hardening event. Hardening (turning Dependabot alerts on, etc.) is a deliberate post-transfer step under [the Post-Transfer Settings audit](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#5-post-transfer-settings-workflows-secrets-and-environments-audit).

### Operator-only items (require `admin:org` token scope OR UI walkthrough)

| # | Item | What blocks API check | Authoritative source for the operator |
|---|---|---|---|
| O1 | **Verify `impossibleos.co` for GitHub Pages** at the org level. This is the Pages-specific custom-domain ownership check that prevents the org from publishing a site on a domain the org does not own; it is a SEPARATE flow from organization-identity domain verification (the SAML/billing-domain feature) -- DO NOT conflate them. The Pages flow proves DNS ownership of `impossibleos.co` for the receiving org, so the new owner's Pages settings accept the custom domain without re-issuing the cert and `https_enforced` can later be turned on cleanly. Per <https://docs.github.com/pages/configuring-a-custom-domain-for-your-github-pages-site/verifying-your-custom-domain-for-github-pages>. | Pages custom-domain verification is a UI-only flow today; no public REST endpoint. | UI: <https://github.com/organizations/rizonetech/settings/pages> -> "Verified custom domains" (or the org-Pages domain-verification panel; GitHub renames this surface periodically). The flow shows a TXT record to add to the DNS zone for `impossibleos.co`, then verifies. Walk before pressing the [transfer button](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#4-repository-transfer-runbook-and-rollback-window) so the receiving org's Pages accept the domain immediately. **Optional separate check:** organization-identity domain verification at <https://github.com/organizations/rizonetech/settings/security> ("Verified and approved domains" -- per <https://docs.github.com/organizations/managing-organization-settings/verifying-or-approving-a-domain-for-your-organization>) is used for SAML / email / billing display; not required for Pages, but harmless to also verify if the org plans to send branded email or display a verified badge. |
| O2 | Configure org Actions policy: GitHub-hosted runners enabled, allowed actions list includes `actions/checkout`, `actions/configure-pages`, `actions/upload-pages-artifact`, `actions/deploy-pages` (Pages workflow), plus build / release / labeler / stale / todo-graph / visual-regression actions. Default `GITHUB_TOKEN` permission must allow Pages workflow's `pages: write` + `id-token: write`. | Endpoint requires `admin:org`. | UI: <https://github.com/organizations/rizonetech/settings/actions>. CLI after refresh: `gh api orgs/rizonetech/actions/permissions`, `gh api orgs/rizonetech/actions/permissions/selected-actions`, `gh api orgs/rizonetech/actions/permissions/workflow`. Source-repo baseline (§4 above): `allowed_actions: all`, `default_workflow_permissions: read`. Match or relax at the org level before transfer; do NOT tighten and break the workflow. |
| O3 | Confirm org Pages policy permits per-repository Pages and custom domains. (Some enterprise plans expose a "Pages public/private/disabled" toggle at the org level.) | Org Pages policy is part of `admin:org` settings. | UI: <https://github.com/organizations/rizonetech/settings/pages>. Confirm "Public" or "Private and Public" is selected; confirm custom domains are allowed. |
| O4 | Confirm org-level merge / branch / ruleset / security policies are at least as permissive as the source-repo settings: ruleset import path for "Default Branch Security" (id `14058331`, captured in §5 above), merge methods (merge / squash / rebase all allowed at source), 2FA enforcement (currently OFF). | Org rulesets endpoint requires `admin:org`. | UI: <https://github.com/organizations/rizonetech/settings/repository-defaults>, <https://github.com/organizations/rizonetech/settings/rules>. CLI after refresh: `gh api orgs/rizonetech/rulesets`. The org may be empty (no ruleset preset) -- that's fine; the source-repo ruleset migrates with the repo. |
| O5 | **Disable Copilot cloud coding-agent / autonomous-agent PR enablement** for the org and the soon-to-be-transferred repo. Required by [Autonomous-Agent Boundary Policy](ai-system.md#autonomous-agent-boundary-policy). | Copilot Access policy has no public-API surface today; UI-only. | UI: <https://github.com/organizations/rizonetech/settings/copilot/access>. Set to "No access" or "Selected members" with `rizonesoft` excluded from cloud-agent permission. Re-verify post-transfer in the [Post-Transfer Settings audit](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#5-post-transfer-settings-workflows-secrets-and-environments-audit) and the [Validation suite](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#7-validation-suite-pages-actions-releases-clone-hooks-graph). |
| O6 | Identify or create teams that replace personal/collaborator permissions: maintainers, reviewers, release operators, security contacts, Pages/DNS administrators. | Org team management is `admin:org`. | UI: <https://github.com/organizations/rizonetech/teams>. Currently zero teams (verified above). Solo-dev pattern: a single `maintainers` team with `rizonesoft` is sufficient until external collaborators are onboarded. CODEOWNERS migration: the source-repo file routes everything to `@derickpayne` (the user); under an org owner, that handle still resolves but consider adding a `@rizonetech/maintainers` route once the team exists. |

### Pre-transfer checklist (concrete commands + UI walkthroughs for the operator)

The block below covers ALL six O-items: CLI commands for items with REST endpoints, and explicit UI walkthroughs for items where GitHub does not expose a public API (O1 Pages-domain verification, O3 org Pages policy, O5 Copilot Access). Walk every item -- skipping the UI-only ones (especially O5) lets the operator complete the visible CLI block and press transfer with policy gaps. After running `gh auth refresh -h github.com -s admin:org` once to upgrade the local token, the operator can replace the CLI walkthroughs:

```bash
# O1 -- Pages custom-domain verification for impossibleos.co (UI-only)
#   Walk: https://github.com/organizations/rizonetech/settings/pages
#   Add TXT record per the displayed challenge, then click Verify.
#   Do NOT confuse with org-identity domain verification under /settings/security.

# O2 -- Actions policy (must match or relax source-repo settings)
gh api orgs/rizonetech/actions/permissions
gh api orgs/rizonetech/actions/permissions/workflow
gh api orgs/rizonetech/actions/permissions/selected-actions  # only if allowed_actions != "all"

# O3 -- org Pages policy (UI-only; no stable public REST endpoint)
#   Walk: https://github.com/organizations/rizonetech/settings/pages
#   Confirm: "Public" or "Private and Public" selected; custom domains allowed.

# O4 -- rulesets / merge defaults / 2FA-required policy
gh api orgs/rizonetech/rulesets                                     # typically empty until first ruleset is imported
#   UI follow-up: https://github.com/organizations/rizonetech/settings/repository-defaults
#                 https://github.com/organizations/rizonetech/settings/rules

# O5 -- Copilot cloud-coding-agent / autonomous-agent PR enablement (UI-only -- MANDATORY)
#   Walk: https://github.com/organizations/rizonetech/settings/copilot/access
#   Set: "No access" OR "Selected members" with rizonesoft excluded from cloud-agent permission.
#   Required by docs/infrastructure/ai-system.md "Autonomous-Agent Boundary Policy".
#   Re-verify post-transfer in the post-transfer settings audit and the validation suite.

# O6 -- teams (currently zero)
gh api orgs/rizonetech/teams
#   UI: https://github.com/organizations/rizonetech/teams
```

If any item returns unexpected values OR the UI shows a state different from the documented expectation, stop the transfer and resolve before pressing the [transfer button](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#4-repository-transfer-runbook-and-rollback-window).

---

## §3 Custom-Domain Continuity Plan

> Plans the Pages custom-domain transition so `https://impossibleos.co/` stays the canonical user-facing URL before, during, and after the [transfer button](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#4-repository-transfer-runbook-and-rollback-window). The default owner-based Pages URL (`https://rizonesoft.github.io/impossible-os/` -> `https://rizonetech.github.io/impossible-os/`) IS allowed to change; the apex is not.

### Canonical domain policy

| Surface | Status |
|---|---|
| `https://impossibleos.co/` (apex) | **Canonical** -- the only URL contributors / docs / external references should bake in long-term. |
| `https://www.impossibleos.co/` | Permanent `301` redirect to apex (already configured: §3 baseline shows `301 -> https://impossibleos.co/`). |
| `https://<owner>.github.io/impossible-os/` | **Non-canonical**. Currently `rizonesoft.github.io` -> `301 -> http://impossibleos.co/`; will become `rizonetech.github.io` after transfer. The default Pages URL is a transient implementation detail. |

### Source-of-truth changes shipped in this section

| Change | Why |
|---|---|
| Add tracked [`gh-pages/CNAME`](../../gh-pages/CNAME) containing `impossibleos.co\n` | The `gh-pages` BRANCH (current legacy Pages source) already has a `CNAME` blob (sha `1441672e`, content `impossibleos.co`). Adding the same file to `main`'s `gh-pages/` folder gives the workflow-deployed artifact (when the failed `pages.yml` quota issue resolves) the same custom-domain hint, so switching the Pages source mode from "branch" to "Actions" later doesn't drop the domain. The `actions/upload-pages-artifact@v3` step in `.github/workflows/pages.yml` includes everything under `gh-pages/` in the artifact, so the CNAME ships with the deploy. |
| Update [`gh-pages/index.html`](../../gh-pages/index.html) lines 13 / 19 / 36 / 1089 | Replace `https://rizonesoft.github.io/impossible-os/` with `https://impossibleos.co/` in the canonical link, og:url, JSON-LD `url`, and the hits.sh visitor-counter namespace. The visitor-counter switch (`hits.sh/rizonesoft.github.io/impossible-os.svg` -> `hits.sh/impossibleos.co.svg`) makes the counter owner-independent across this transfer AND any future move-back. Counter resets when the namespace changes; acceptable per §1's R-table. |
| `gh-pages/err/` audit | All 14 error-page `index.html` files already use `https://impossibleos.co/err/<code>/` as their canonical URL (§1's "error pages already use https://impossibleos.co/..." callout was correct). Each err page has one `https://github.com/rizonesoft/impossible-os` link in the nav -- those are owner-specific repo links, owned by the [URL/badge/docs/generated-link sweep](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#6-url-badge-docs-and-generated-link-sweep) (§6), NOT by §3. |
| The `author.url` `https://rizonesoft.com` in the JSON-LD block (line 40) | INTENTIONALLY RETAINED -- `rizonesoft.com` is the company brand homepage, not the active GitHub repo URL. The §6 classification rule keeps brand references; only repo URLs flip. |

### DNS plan

| Record | Current state (per §1 baseline) | Pre-transfer change | Post-transfer change |
|---|---|---|---|
| `impossibleos.co` A (apex) | 4 GitHub Pages anycast IPv4: `185.199.108.153`, `185.199.109.153`, `185.199.110.153`, `185.199.111.153`. | **No change.** GitHub Pages anycast IPs are owner-independent. | **No change** unless GitHub publishes a new Pages IP set (verify against <https://docs.github.com/pages/configuring-a-custom-domain-for-your-github-pages-site/managing-a-custom-domain-for-your-github-pages-site>). |
| `impossibleos.co` AAAA (apex) | NOT SET (IPv4 only) | **Decision: keep unset for now.** GitHub publishes IPv6 addresses (`2606:50c0:8000::153` ... `2606:50c0:8003::153`) and recommends adding them for IPv6 parity, but the upstream Linux/Windows test platforms used by Impossible OS do not measurably benefit from IPv6 reachability for a docs site, and adding records expands the rollback surface. Re-evaluate after the §4 transfer is verified green; if added, log them in §1's DNS table. | Re-evaluate after transfer. |
| `www.impossibleos.co` CNAME | `rizonesoft.github.io.` (verified by `getent hosts`). Returns `301 -> https://impossibleos.co/`. | **Drop TTL** if the DNS provider exposes the control. Target a low TTL (60-300s) at least 24h before transfer so the CNAME flip in §4 propagates within a single TTL window. Some providers treat apex/CNAME as ALIAS/ANAME with provider-side caching that ignores the published TTL -- check the provider's documented behavior before assuming. | **CNAME flip** from `rizonesoft.github.io` -> `rizonetech.github.io` AFTER the transfer button is pressed AND `https://rizonetech.github.io/impossible-os/` is serving (verified via `curl -I`). DO NOT flip earlier; the new owner's Pages endpoint must exist first or `www` goes dark. |

### Pages certificate continuity

| Field | Current | Plan |
|---|---|---|
| Cert state | `approved` | After transfer, GitHub may re-provision the cert when the custom domain is re-verified under the new owner. **Tolerate transient HTTPS warning** on the apex during re-provisioning -- typically minutes to single-digit hours per GitHub Pages docs. |
| `https_certificate.expires_at` | `2026-06-16` | Cert covers BOTH `impossibleos.co` and `www.impossibleos.co`. Transfer must complete BEFORE this expiry, otherwise GitHub may not auto-renew. |
| `https_enforced` | **`false`** | Leave OFF until the new cert reissues post-transfer; turning it on while the cert is reissuing can break the default Pages URL's redirect chain. The [Post-Transfer Settings audit](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#5-post-transfer-settings-workflows-secrets-and-environments-audit) is the right place to flip it on once the cert is stable. |

### Acceptable transient states (during/just-after transfer)

- The default Pages URL `https://rizonesoft.github.io/impossible-os/` may stop redirecting cleanly for ~minutes while GitHub's repo-redirect machinery catches up; the apex serves through Fastly with the existing cert and is unaffected.
- The new default Pages URL `https://rizonetech.github.io/impossible-os/` may show a TLS warning briefly while the cert reissues for the new owner.
- `www.impossibleos.co` may show an old answer for one TTL after the CNAME flip.
- The hits.sh visitor counter resets to zero on the namespace switch (`rizonesoft.github.io/impossible-os` -> `impossibleos.co`); historical counts are not retained. Acceptable for a private repo.

### Validation commands

The same set used in §1's baseline; running them post-transfer is the §3 acceptance check (and the basis for the [Validation suite](../../todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md#7-validation-suite-pages-actions-releases-clone-hooks-graph)):

```bash
curl -sIL --max-time 10 https://impossibleos.co/                        # expect 200, last-modified matching latest gh-pages deploy
curl -sIL --max-time 10 https://www.impossibleos.co/                    # expect 301 -> https://impossibleos.co/
curl -sIL --max-time 10 https://rizonetech.github.io/impossible-os/     # expect 301 -> apex (or HTTPS-enforced apex if §5 flipped the toggle)
getent hosts impossibleos.co                                            # 4 anycast IPv4 addresses (185.199.10[8-11].153)
getent hosts www.impossibleos.co                                        # CNAME chain ends at rizonetech.github.io after §4
# With BIND tools available:
dig +nocmd +noall +answer impossibleos.co A
dig +nocmd +noall +answer impossibleos.co AAAA      # currently empty; non-empty if §3 AAAA decision was revisited
dig +nocmd +noall +answer www.impossibleos.co CNAME
```

If any line returns an unexpected status code or DNS answer, stop and resolve before pressing the transfer button (or rolling back per §4).

---

## How to Re-Run This Inventory

The exact commands used to capture this baseline:

```bash
# Repository identity
gh repo view rizonesoft/impossible-os --json nameWithOwner,owner,visibility,isPrivate,defaultBranchRef,description,homepageUrl,isArchived,isFork,isTemplate,isInOrganization,hasIssuesEnabled,hasDiscussionsEnabled,hasWikiEnabled,hasProjectsEnabled,isSecurityPolicyEnabled,isBlankIssuesEnabled,repositoryTopics,id,sshUrl,url,createdAt,pushedAt,licenseInfo,deleteBranchOnMerge,squashMergeAllowed,rebaseMergeAllowed,mergeCommitAllowed

# Pages + environment + deployments
gh api repos/rizonesoft/impossible-os/pages
gh api repos/rizonesoft/impossible-os/environments
gh api repos/rizonesoft/impossible-os/deployments --jq '.[0:3]'
gh run list -R rizonesoft/impossible-os -w pages.yml -L 3

# Live domain + DNS (require BIND tools or Python)
for u in https://impossibleos.co/ https://www.impossibleos.co/ https://rizonesoft.github.io/impossible-os/ http://impossibleos.co/; do
  curl -sIL --max-time 10 "$u" | head -25
done
getent hosts impossibleos.co
getent hosts www.impossibleos.co
dig +nocmd impossibleos.co A          # if available
dig +nocmd impossibleos.co AAAA       # if available
dig +nocmd www.impossibleos.co CNAME  # if available

# Actions + workflows + secrets + variables + branch protection + rulesets
gh api repos/rizonesoft/impossible-os/actions/permissions
gh api repos/rizonesoft/impossible-os/actions/permissions/workflow
gh workflow list -R rizonesoft/impossible-os --all
gh secret list -R rizonesoft/impossible-os
gh variable list -R rizonesoft/impossible-os
gh api repos/rizonesoft/impossible-os/branches/main/protection
gh api repos/rizonesoft/impossible-os/rulesets

# Webhooks / deploy keys / vulnerability alerts / dependabot
gh api repos/rizonesoft/impossible-os/hooks
gh api repos/rizonesoft/impossible-os/keys
gh api repos/rizonesoft/impossible-os/vulnerability-alerts --silent && echo enabled || echo disabled
gh api repos/rizonesoft/impossible-os/automated-security-fixes

# Access
gh api repos/rizonesoft/impossible-os/collaborators

# Receiving org readiness
gh api orgs/rizonetech --jq '{login,plan,public_repos,total_private_repos,owned_private_repos,members_can_create_repositories,billing_email}'
gh repo view rizonetech/impossible-os --json nameWithOwner   # MUST 404
gh api user/memberships/orgs/rizonetech

# Hard-coded owner references (expanded path set: includes todo + scripts;
# render.py:42 holds REPO_URL_BASE so scripts/ is mandatory).
#
# Step 1 -- compare against the post-doc baseline (expected: 39 files / 364 matches):
PATTERN='rizonesoft/impossible-os|github\.com/rizonesoft|rizonesoft\.github\.io|hits\.sh/rizonesoft'
PATHS='README.md CONTRIBUTING.md SECURITY.md CODE_OF_CONDUCT.md AGENTS.md CLAUDE.md docs gh-pages .github todo scripts'
rg -l "$PATTERN" $PATHS | wc -l                                    # file count
rg -n "$PATTERN" $PATHS | wc -l                                    # total matching lines (matches "364" semantics in this doc)
for p in 'rizonesoft/impossible-os' 'github\.com/rizonesoft' 'rizonesoft\.github\.io' 'hits\.sh/rizonesoft'; do
  echo "[$p] $(rg -c "$p" $PATHS 2>/dev/null | awk -F: '{s+=$NF} END {print s}')"
done                                                                # per-pattern counts
#
# Step 2 -- locator (only run after Step 1 shows drift; emits hundreds of lines):
rg -n "$PATTERN" $PATHS
```

Re-run the full block immediately before §4 transfer to detect any drift since the baseline date.
