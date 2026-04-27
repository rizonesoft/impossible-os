---
schema_version: 1
id: repository-transfer-rizonetech
domain: 00-infrastructure
status: draft
title: "TODO-09 -- Repository Transfer to rizonetech"
file_patterns:
  - ".github/**"
  - "gh-pages/**"
  - "docs/infrastructure/github-setup.md"
  - "README.md"
  - "CONTRIBUTING.md"
  - "SECURITY.md"
  - "CODE_OF_CONDUCT.md"
  - "AGENTS.md"
  - "CLAUDE.md"
---

# TODO-09 -- Repository Transfer to rizonetech

> **Goal:** Move the Impossible OS repository from the current `rizonesoft/impossible-os` owner to the `rizonetech` GitHub organization with no avoidable disruption to GitHub Pages, release links, badges, workflows, branch protection, custom domain ownership, or contributor workflow. Keep the move reversible so the repository can later transfer back to the personal account when the project is ready to become public.

> [!IMPORTANT]
> **Current state (baselined 2026-04-27 in [`docs/infrastructure/repository-transfer-preflight.md`](../../docs/infrastructure/repository-transfer-preflight.md)):** Local remotes point at `https://github.com/rizonesoft/impossible-os.git` and `https://github.com/rizonesoft/impossible-os-bootloader.git`. The repository is `PRIVATE`, owned by the `rizonesoft` user (NOT an org). GitHub Pages serves with `build_type: legacy` from the `gh-pages` BRANCH (sha `35e7dcd5`, last successful 2026-04-12); the workflow [`.github/workflows/pages.yml`](../../.github/workflows/pages.yml) is configured but its 2026-04-12 run failed with `Artifact storage quota has been hit` and has not run successfully since. The custom domain `https://impossibleos.co/` returns `200 OK` directly (cert valid through 2026-06-16), `https://www.impossibleos.co/` returns `301 -> https://impossibleos.co/`, and `https://rizonesoft.github.io/impossible-os/` returns `301 -> http://impossibleos.co/` (HTTP because `https_enforced: false`). DNS for the apex points at the four GitHub Pages anycast IPv4 addresses (no AAAA set); `www.impossibleos.co` CNAMEs to `rizonesoft.github.io`. Repo state: 7 active workflows, 1 secret (`BOOTLOADER_REPO_TOKEN`), 0 variables, 0 webhooks, 0 deploy keys, ruleset "Default Branch Security" active on `main`, no classic branch protection, Dependabot alerts disabled. Hard-coded owner references span **39 files / 364 matches** under the expanded scan path (`README.md CONTRIBUTING.md SECURITY.md CODE_OF_CONDUCT.md AGENTS.md CLAUDE.md docs gh-pages .github todo scripts`); the narrow-path 28-files / 283-matches count is the historical baseline and is superseded -- the expanded scan picks up `scripts/todo-graph/render.py:42` (`REPO_URL_BASE` constant for TODO graph links) and 35 references across 8 `todo/` files that the narrow path missed. Receiving destination `rizonetech/impossible-os` does not exist; the `rizonetech` enterprise org has 50 seats / 1 filled and the operator (`rizonesoft`) is admin. The landing page still has canonical, Open Graph, JSON-LD, and visitor-counter references to `rizonesoft.github.io/impossible-os/`; the error pages already use `https://impossibleos.co/...`.

## Inputs

- [`gh-pages/`](../../gh-pages/) -- deployed static site; must carry the custom-domain and canonical-link truth during and after transfer
- [`.github/workflows/pages.yml`](../../.github/workflows/pages.yml) -- GitHub Pages deployment workflow; requires `pages: write` and `id-token: write`
- [`.github/workflows/build.yml`](../../.github/workflows/build.yml) -- CI status badge and build gate referenced from README
- [`.github/workflows/release.yml`](../../.github/workflows/release.yml) -- release artifact automation and public download links
- [`.github/workflows/labeler.yml`](../../.github/workflows/labeler.yml) -- maintenance automation that depends on repository permissions
- [`.github/workflows/stale.yml`](../../.github/workflows/stale.yml) -- maintenance automation that depends on repository permissions
- [`.github/CODEOWNERS`](../../.github/CODEOWNERS) -- review routing; org teams may need replacement handles
- [`README.md`](../../README.md) -- badges, clone URLs, release links, and public GitHub references
- [`CONTRIBUTING.md`](../../CONTRIBUTING.md) -- contributor workflow and GitHub URL references
- [`SECURITY.md`](../../SECURITY.md) -- vulnerability reporting and owner identity
- [`CODE_OF_CONDUCT.md`](../../CODE_OF_CONDUCT.md) -- maintainer contact references if any
- [`docs/infrastructure/github-setup.md`](../../docs/infrastructure/github-setup.md) -- canonical GitHub repository setup documentation
- [`docs/infrastructure/development-tooling.md`](../../docs/infrastructure/development-tooling.md) -- workflow inventory and Pages workflow references
- [`docs/infrastructure/ai-system.md`](../../docs/infrastructure/ai-system.md) -- GitHub UI and organization-level AI-policy notes
- [`AGENTS.md`](../../AGENTS.md) -- external reviewer pointer that names repository policy and may reference owner context
- [`CLAUDE.md`](../../CLAUDE.md) -- doctrine and commit policy; only update if the owner move changes a rule, not for URL churn alone
- [`todo/TODO-00-INDEX.md`](../TODO-00-INDEX.md) and [`todo/00-infrastructure/INDEX.md`](INDEX.md) -- roadmap discoverability
- GitHub repository settings for `rizonesoft/impossible-os` -- transfer, Pages, branch protection, environments, Actions, secrets, variables, rulesets, Discussions, Issues, security features, and visibility
- GitHub organization settings for `https://github.com/rizonetech/` -- ownership, verified domain readiness, Pages permission, Actions policy, Copilot/cloud-agent policy, team membership, billing/plan capability, and repository visibility rules
- DNS provider records for `impossibleos.co` and `www.impossibleos.co` -- apex A records, optional AAAA records, `www` CNAME, TTL, and any ALIAS/ANAME provider behavior
- -> XREF: [`00-infrastructure/TODO-01 section 6 GitHub Actions and artifact policy alignment`](TODO-01-developer-tooling-stack.md#6-github-actions-and-artifact-policy-alignment) -- transfer consumes the workflow and artifact policy but does not redefine it
- -> XREF: [`00-infrastructure/TODO-02 section 8 Autonomous-Agent Boundary Policy`](TODO-02-ai-development-system.md#8-autonomous-agent-boundary-policy) -- org-level Copilot/cloud-agent settings must preserve the no-autonomous-agent stance
- -> XREF: [`00-infrastructure/TODO-02 section 9 AI Workflow Regression Suite`](TODO-02-ai-development-system.md#9-ai-workflow-regression-suite) -- URL/owner changes must not break the AGENTS authority-byte-compare and AI-policy checks
- -> XREF: [`00-infrastructure/TODO-06 section 3 Validator`](TODO-06-todo-metadata-layer.md#3-validator-stale-xref--dangling-dep--orphan--cycle--bat--status--schema) -- this TODO must keep graph metadata valid

## Outcome

- Repository ownership transfers from `rizonesoft/impossible-os` to `rizonetech/impossible-os` with a preflight checklist, a time-boxed transfer runbook, and a rollback path.
- `https://impossibleos.co/` stays the primary public Pages URL before, during, and after the transfer.
- `www.impossibleos.co` CNAME is updated from `rizonesoft.github.io` to `rizonetech.github.io` only when the receiving org's Pages endpoint is ready.
- GitHub Pages custom-domain verification, HTTPS enforcement, Pages deployment environment, and Actions permissions are confirmed under `rizonetech`.
- Repository redirects are treated as convenience only. Docs, badges, clone examples, release links, and generated graph links are updated to the new canonical owner where appropriate.
- Organization policy preserves the Impossible OS AI boundary: no autonomous-agent PR flow, no cloud coding-agent enablement, no new parallel instruction tree.
- A move-back plan exists for returning to the personal account when the repository becomes public, including DNS, Pages, visibility, redirects, and post-transfer validation.
- Every operator action that cannot be expressed in git is captured in `docs/infrastructure/github-setup.md` so the final state is reproducible.

## Implementation Order

|  ⭐  | Order | Section | Deliverable                                                       | Depends On                     | Status |
| --- | :---: | :------: | ----------------------------------------------------------------- | ------------------------------ | :----: |
|  ⭐  |   1   |    1    | Preflight inventory and risk register                             | --                             |  [x]   |
|  ⭐  |   2   |    2    | `rizonetech` organization readiness and policy parity             | §1                             |  [/]   |
|  ⭐  |   3   |    3    | GitHub Pages custom-domain continuity plan                        | §1, §2                         |  [/]   |
|  ⭐  |   4   |    4    | Repository transfer runbook and rollback window                   | §1, §2, §3                     |  [ ]   |
|  ⭐  |   5   |    5    | Post-transfer settings, workflows, secrets, environments audit    | §4                             |  [ ]   |
|  ⭐  |   6   |    6    | URL, badge, docs, and generated-link sweep                        | §4, §5                         |  [ ]   |
|  ⭐  |   7   |    7    | Validation suite: Pages, Actions, releases, clone, hooks, graph   | §5, §6                         |  [ ]   |
|  ⭐  |   8   |    8    | Move-back and public-visibility runbook                           | §1, §2, §3, §4, §5, §6, §7     |  [ ]   |
|  ⭐  |   9   |    9    | Documentation sync and closure                                    | §1, §2, §3, §4, §5, §6, §7, §8 |  [ ]   |

> **Order vs section number:** The file is ordered by execution flow. Section 8 is intentionally specified before the first transfer happens so the team can confirm the move is reversible rather than discovering account or Pages constraints after the repository is already in the organization.

---

## 1. Preflight Inventory and Risk Register

Capture the exact current state before changing any GitHub owner setting. The output is the baseline used to prove that the move did not break Pages, CI, release automation, or project policy.

- [x] Record current repository identity: owner `rizonesoft` (user, not org -- `isInOrganization: false`), visibility `PRIVATE`, default branch `main`, node id `R_kgDORgIuVg`, HTTPS / SSH URLs, topics (`bare-metal`, `freestanding`, `operating-system`, `os-development`, `uefi-boot`), description, homepage `https://rizonesoft.com`, Issues=on, Discussions=off, Wiki=on (unused), Projects=on (unused), security policy file present, GPL-3.0 license, all three merge methods allowed. Captured in [`docs/infrastructure/repository-transfer-preflight.md`](../../docs/infrastructure/repository-transfer-preflight.md) §1.
- [x] Record current Pages state: `build_type: legacy` from branch `gh-pages` path `/`, environment `github-pages` (id `13147855799`), custom domain `impossibleos.co`, custom 404 enabled, **`https_enforced: false`**, cert `approved` through `2026-06-16` covering both apex and `www`. Last successful workflow run 2026-03-18; the 2026-04-12 run failed (`Artifact storage quota has been hit`); legacy branch source keeps the live site up. No tracked `gh-pages/CNAME` file -- domain stored in Pages settings only. Preflight §2.
- [x] Record live domain baseline (curl HEAD captured 2026-04-27 ~19:54 UTC):
  - `https://impossibleos.co/` returns `200 OK`, `last-modified: 2026-04-12T00:27:55Z`, etag `"69dae70b-b9bc"`, content-length `47548`.
  - `https://www.impossibleos.co/` returns `301 -> https://impossibleos.co/`.
  - `https://rizonesoft.github.io/impossible-os/` returns `301 -> http://impossibleos.co/` (HTTP -- consequence of `https_enforced: false`).
  - `impossibleos.co` apex A records: `185.199.108.153`, `185.199.109.153`, `185.199.110.153`, `185.199.111.153` (the 4 GitHub Pages anycast addresses); no AAAA set.
  - `www.impossibleos.co` CNAME currently targets `rizonesoft.github.io.`. Preflight §3.
- [x] Record current GitHub Actions state: enabled, `allowed_actions: all`, `sha_pinning_required: false`, default `GITHUB_TOKEN` permission `read`, no required workflow approvals, GitHub-hosted runners only. **7 active workflows** (the TODO Inputs list named only 5 -- adds `todo-graph.yml` and `visual-regression.yml`) plus the auto-generated `pages-build-deployment`. Preflight §4.
- [x] Record repository settings that do not reliably travel: classic branch protection on `main` is **NOT** set (`HTTP 404 Branch not protected`); ruleset "Default Branch Security" (id `14058331`) IS active with `required_status_checks` (`Build Impossible OS`, strict, `do_not_enforce_on_create`), `non_fast_forward`, `required_linear_history`, `deletion`; bypass actor `RepositoryRole 5 (Maintain)`. Repo secrets: `BOOTLOADER_REPO_TOKEN` only. Variables: none. Webhooks: none. Deploy keys: none. Vulnerability alerts: **disabled**. Automated security fixes: disabled. Environment `github-pages` has `branch_policy` protection. Preflight §5.
- [x] Record current access model: only `rizonesoft` is a direct collaborator (admin/maintain/push/pull/triage). No outside collaborators. No teams (user-owned repo). CODEOWNERS routes everything plus 6 critical paths to `@derickpayne`. Pages/DNS administered by repo owner; DNS provider holds `impossibleos.co` records. Receiving org `rizonetech` (canonical login `Rizonetech`, id `279138845`) is `enterprise` plan with 50 seats / 1 filled; current operator is direct-membership admin; `rizonetech/impossible-os` does not yet exist. Preflight §6.
- [x] Record all hard-coded owner references via `rg -n 'rizonesoft/impossible-os|github\.com/rizonesoft|rizonesoft\.github\.io|hits\.sh/rizonesoft' README.md CONTRIBUTING.md SECURITY.md CODE_OF_CONDUCT.md AGENTS.md CLAUDE.md docs gh-pages .github todo scripts` (path set expanded after Codex flagged that the original narrower path missed `scripts/todo-graph/render.py:42` -- the `REPO_URL_BASE` constant for TODO graph links). **303 matches across ~37 files (pre-preflight-doc theoretical) / 364 matches across 39 files (post-preflight-doc measured)**; the +61 delta is intentional self-references in this preflight doc + the TODO-09 stamp + the github-setup pointer. `scripts/` contributes 2 matches: `render.py:42` (active generator constant -- §6 must patch) and `test-ai-system.sh:396` (informational log line). `todo/` contributes 35 matches across 8 files (mostly project narrative; some inventoried as active references). `SECURITY.md` / `CODE_OF_CONDUCT.md` / `AGENTS.md` / `CLAUDE.md` returned **zero** GitHub-URL matches and should not need owner edits. Preflight §7 lists both baselines + the full file set + per-pattern counts + the explicit `scripts/` handling.
- [x] Record remote consumers: local dev clone (`origin` on this host), one prerelease `v26.3.18-alpha.821` (artifact URLs auto-redirect through GitHub), the `BOOTLOADER_REPO_TOKEN` consumer (`release.yml` only), and the `hits.sh/rizonesoft.github.io` visitor-counter namespace (counter resets on namespace change -- acceptable for a private repo). External docs / social profiles / pinned GitHub Apps are not enumerable from inside the repo and must be walked manually before §4. Preflight §8.
- [x] Risk register with severity + mitigation + owner section -- 15 risks captured ranging from Critical (R1: Pages custom-domain detachment on transfer) through Low (R14: provider-side DNS TTL caching, R15: `https_enforced` left off). Includes the new finding R4 (Pages workflow already failing on artifact quota -- transfer to enterprise org likely fixes it) and R11 (recreating old repo path during redirect window destroys redirect). Preflight §9.
- [x] Commit: `"todo: document repository-transfer preflight inventory"`

**Test checkpoint:** [`docs/infrastructure/repository-transfer-preflight.md`](../../docs/infrastructure/repository-transfer-preflight.md) lists every setting needed to recreate the repository in `rizonetech` and contains a "How to Re-Run This Inventory" block with the exact `gh api` / `gh repo view` / `curl` / `getent` / `rg` commands used to capture the baseline. §4 must re-run that block immediately before pressing the transfer button to detect drift, not rely on the 2026-04-27 snapshot.

> **Test runner:** N/A (operator preflight inventory, no test surface) | validation: re-run the "How to Re-Run This Inventory" command block in `repository-transfer-preflight.md` and diff against the captured tables; live URL/DNS/`gh api` outputs must match within transfer-window tolerance.
> **Notes:**
> - Shipped: [`docs/infrastructure/repository-transfer-preflight.md`](../../docs/infrastructure/repository-transfer-preflight.md) (~360 lines, 9 sections + count-first reproduction-command block) baselining identity / Pages / DNS / Actions / settings / access / owner-refs / consumers / 15-row risk register on 2026-04-27 from live `gh api` / `curl` / `getent` / `rg` outputs.
> - Surfaced three latent findings the TODO callout had wrong or missing: Pages source is `build_type: legacy` from the `gh-pages` branch (NOT the workflow); the workflow's most recent run (2026-04-12) failed on `Artifact storage quota`; and the original narrow-path owner-reference scan missed the active `REPO_URL_BASE` constant in `scripts/todo-graph/render.py:42`. Updated §0 `Current state` callout to match observed reality, expanded the rg path set to include `todo` + `scripts`, added R4 (workflow failure) and rewrote R10 to name the actual generator file.
> - Cross-doc wiring: [`docs/infrastructure/github-setup.md`](../../docs/infrastructure/github-setup.md) Overview now points at the preflight doc, so §5 / §7 / §8 (move-back) re-baseline against a single source of truth. Codex 3x review adoptions in commit `baad0045`.
> - Canonical doc: [`docs/infrastructure/repository-transfer-preflight.md`](../../docs/infrastructure/repository-transfer-preflight.md).
> - Scope boundary: §1 captures *current* state only; §2 owns receiving-org policy parity, §3 owns Pages/DNS continuity changes, §4 owns the actual transfer, §5 owns post-transfer audit. The preflight doc is a baseline reference, not a runbook.
>
> **Verified:** 2026-04-27 | commit `baad0045` | 9/9 items | build N/A (docs-only) | lint clean, todo-graph 8/8
> **Quality reviewed:** 2026-04-27 | Codex 3x (adversarial, consistency, perf) | 1H+2M+1L fixed, 0 open | scope: N/A (docs-only -- no domain code-quality skill applies; re-adversarial skipped: docs+stamp-only fixes, no SMP/IRQ/state-machine touch)

---

## 2. `rizonetech` Organization Readiness and Policy Parity

Prepare the receiving organization before transfer so the repository lands into a compatible policy envelope.

- [x] Confirm the `rizonetech` organization exists, has the intended owners, and the human operator has permission to receive transferred repositories. **Verified:** `gh api orgs/rizonetech` returns `login: "Rizonetech"` (canonical), id `279138845`; `gh api user/memberships/orgs/rizonetech` returns `state: active`, `role: admin`, `direct_membership: true` for `rizonesoft`. Cross-referenced from §1 baseline.
- [x] Confirm the org plan supports the repository's current visibility plus GitHub Pages requirements. If the repo remains private, verify that Pages for private repositories is available under the active plan before transfer. **Verified:** `gh api orgs/rizonetech --jq '.plan'` returns `name: "enterprise"`, `private_repos: 999999`, `seats: 50`, `space: 976562499`. Enterprise plan covers private-repo Pages.
- [ ] Verify `impossibleos.co` in the receiving organization if GitHub requires account-level domain verification for custom Pages domains. **Operator-only (O1):** requires `gh auth refresh -h github.com -s admin:org` then `gh api orgs/rizonetech/verified-domains`, OR walk <https://github.com/organizations/rizonetech/settings/security> "Verified domains" before pressing the §4 transfer button. Captured in [`docs/infrastructure/repository-transfer-preflight.md` §2 O1](../../docs/infrastructure/repository-transfer-preflight.md#operator-only-items-require-adminorg-token-scope-or-ui-walkthrough).
- [ ] Configure organization Actions policy so existing workflows can run: GitHub-hosted runners allowed, required marketplace actions allowed (`actions/checkout`, `actions/configure-pages`, `actions/upload-pages-artifact`, `actions/deploy-pages`, and any release/build workflow actions), and workflow permissions compatible with Pages deploy. **Operator-only (O2):** source-repo baseline already captured in §1 (allowed_actions=all, default_workflow_permissions=read); operator must match-or-relax at org level via <https://github.com/organizations/rizonetech/settings/actions> or post-`admin:org` `gh api orgs/rizonetech/actions/permissions`. Concrete commands documented in preflight §2.
- [ ] Confirm org-level Pages policy allows repository Pages and custom domains. **Operator-only (O3):** UI-only check at <https://github.com/organizations/rizonetech/settings/pages> (no stable public REST endpoint). Confirm "Public" or "Private and Public" is selected and custom domains are allowed.
- [x] Confirm org-level **security defaults** are compatible with repository settings: Dependabot, secret scanning, code scanning, private vulnerability reporting. **Verified:** `gh api orgs/rizonetech` security defaults for new repos are all `false` (`advanced_security`, `dependabot_alerts`, `dependabot_security_updates`, `dependency_graph`, `secret_scanning`, `secret_scanning_push_protection`); `two_factor_requirement_enabled: false`; no SAML; `web_commit_signoff_required: false`. Source-repo baseline (§1) is also "all off", so transfer is parity-compatible. (Note: org rulesets / merge-method defaults / 2FA-required-policy still need operator probe -- tracked as a separate item below under O4.)
- [ ] Confirm org-level **rulesets, merge-method defaults, and 2FA enforcement** are compatible with the source-repo settings: ruleset import path for "Default Branch Security" (id `14058331`, captured in §1), merge methods (merge / squash / rebase all allowed at source), 2FA enforcement (currently OFF). **Operator-only (O4):** `gh api orgs/rizonetech/rulesets` requires `admin:org` scope. Walk <https://github.com/organizations/rizonetech/settings/repository-defaults> and <https://github.com/organizations/rizonetech/settings/rules>; the org may be empty (no preset ruleset), in which case the source-repo ruleset migrates with the repo per §5. The source-repo ruleset itself migrates with the repo on transfer; this O4 check verifies the org's *envelope* is at least as permissive as the source.
- [ ] Preserve the AI-system boundary at org level: disable Copilot cloud coding-agent / autonomous-agent PR enablement for this repository, or document the exact UI setting that keeps it disabled after transfer. **Operator-only (O5):** Copilot Access policy has no public-API surface today. Walk <https://github.com/organizations/rizonetech/settings/copilot/access>, set "No access" or "Selected members" excluding `rizonesoft` from cloud-agent permission. Re-verify in §5 / §7. Cross-referenced from [Autonomous-Agent Boundary Policy](../../docs/infrastructure/ai-system.md#autonomous-agent-boundary-policy).
- [ ] Create or identify teams that replace personal/collaborator permissions: maintainers, reviewers, release operators, security contacts, and Pages/DNS administrators. **Operator-only (O6):** `gh api orgs/rizonetech/teams` returns empty (verified). Solo-dev minimum: one `maintainers` team containing `rizonesoft`. CODEOWNERS currently routes everything to `@derickpayne` (the user) -- that handle still resolves under org ownership, but consider adding a `@rizonetech/maintainers` route once the team exists. <https://github.com/organizations/rizonetech/teams>.
- [x] Confirm no existing `rizonetech/impossible-os` repository blocks the transfer. If one exists, stop and decide whether to rename/archive/delete it before proceeding. **Verified:** `gh repo view rizonetech/impossible-os` returns `GraphQL: Could not resolve to a Repository`; `gh api orgs/rizonetech/repos` returns `[]`; clean transfer destination.
- [x] Commit: `"docs/github: capture rizonetech organization readiness"`

**Test checkpoint:** The receiving organization is ready before the transfer button is pressed. No required setting depends on "we will fix it after it lands" except values that GitHub only exposes after transfer. **Probe-time status:** 4 of 10 items verified from public API + §1 baseline; 6 items map to operator-only steps O1-O6 (admin:org CLI scope OR UI walkthrough) -- all captured by name in [`docs/infrastructure/repository-transfer-preflight.md` §2](../../docs/infrastructure/repository-transfer-preflight.md#2-receiving-org-rizonetech-readiness-probe) with concrete commands and UI URLs. Section flips from `[/]` to `[x]` once the operator confirms all six O-items in chat or by re-running the §2 commands with `admin:org` plus walking the UI-only checks (O1 Pages-domain verification, O3 org Pages policy, O5 Copilot Access).

> **Test runner:** N/A (operator readiness probe, no test surface) | validation: re-run the "Pre-transfer checklist" command block in `repository-transfer-preflight.md` §2 after `gh auth refresh -h github.com -s admin:org`; UI items O1 / O3 / O5 walked manually and noted in chat.
> **Notes:**
> - Shipped: appended `## §2 Receiving Org (rizonetech) Readiness Probe` to [`docs/infrastructure/repository-transfer-preflight.md`](../../docs/infrastructure/repository-transfer-preflight.md) with three tables (public-API confirmed, security-defaults parity, operator-only items O1-O6) plus a copy-paste pre-transfer block that mixes `gh api` commands with explicit UI walkthroughs for the three GitHub features without REST endpoints (O1 Pages-domain verification, O3 org Pages policy, O5 Copilot Access).
> - Local CLI token has scopes `gist, read:org, repo, workflow` -- enough to confirm 4 of 10 items publicly; the remaining 6 need either `gh auth refresh -h github.com -s admin:org` or UI walkthrough at named URLs.
> - Cross-doc wiring: each operator-only item links back to the canonical TODO sections that depend on it (§4 transfer button, §5 post-transfer audit, §7 validation suite). Codex 3x review adoptions in commit `b7e3392d`.
> - Canonical doc: [`docs/infrastructure/repository-transfer-preflight.md` §2](../../docs/infrastructure/repository-transfer-preflight.md#2-receiving-org-rizonetech-readiness-probe).
> - Scope boundary: §2 only **probes and documents** org readiness; the actual `admin:org`-scoped configuration changes (Actions policy, ruleset import, Copilot Access toggle, team creation) are operator actions before §4. §5 owns the post-transfer audit that re-validates each item.
>
> **Verified:** 2026-04-27 | commit `b7e3392d` | 4/10 items | build N/A (docs-only) | lint clean, todo-graph 8/8
> **Quality reviewed:** 2026-04-27 | Codex 3x (adversarial, consistency, perf) | 2H+1M+1L fixed, 0 open | scope: N/A (docs-only -- no domain code-quality skill applies; re-adversarial skipped: docs+stamp-only fixes, no SMP/state-machine touch)

---

## 3. GitHub Pages Custom-Domain Continuity Plan

Make the custom domain the stable user-facing surface. The default owner-based Pages URL is allowed to change; `https://impossibleos.co/` must not.

- [x] Decide the canonical domain policy: apex `https://impossibleos.co/` remains canonical; `https://www.impossibleos.co/` redirects to apex; owner-based GitHub Pages URLs are non-canonical. **Documented in [`docs/infrastructure/repository-transfer-preflight.md` §3](../../docs/infrastructure/repository-transfer-preflight.md#3-custom-domain-continuity-plan) "Canonical domain policy" table.**
- [x] Add a tracked [`gh-pages/CNAME`](../../gh-pages/CNAME) file containing exactly `impossibleos.co`. **Verified:** the `gh-pages` BRANCH (current legacy Pages source) already carries a CNAME blob `1441672e` with content `impossibleos.co`; adding the same file to `main`'s `gh-pages/` folder gives the workflow-deployed artifact (when the artifact-quota issue resolves) the same custom-domain hint, so switching the Pages source mode from "branch" to "Actions" later doesn't drop the domain. The `actions/upload-pages-artifact@v3` step in [`pages.yml`](../../.github/workflows/pages.yml) bundles everything under `gh-pages/` into the artifact, so the CNAME ships with the deploy.
- [x] Update [`gh-pages/index.html`](../../gh-pages/index.html) canonical, Open Graph URL, JSON-LD URL, and visitor-counter URL away from `rizonesoft.github.io/impossible-os/` to `https://impossibleos.co/`. **Done:** four edits at lines 13 (canonical), 19 (og:url), 36 (JSON-LD `url`), 1089 (hits.sh visitor counter switched to `hits.sh/impossibleos.co.svg` -- owner-independent across this transfer AND any future move-back). `author.url: https://rizonesoft.com` at line 40 retained intentionally (brand homepage, not active GitHub repo URL).
- [x] Audit [`gh-pages/err/`](../../gh-pages/err/) for consistent canonical URLs and relative links. **Verified:** all 14 error-page `index.html` files (`err/index.html` + `err/000[0-d]/index.html`) already use `https://impossibleos.co/err/<code>/` as their canonical URL; relative `../../` links resolve correctly. The single `https://github.com/rizonesoft/impossible-os` "GitHub" nav link in each err page is owner-specific repo URL, owned by the [URL/badge/docs/generated-link sweep](#6-url-badge-docs-and-generated-link-sweep) (§6), NOT §3.
- [ ] Reduce DNS TTL for `www.impossibleos.co` before transfer if the DNS provider allows it, so the CNAME switch to `rizonetech.github.io` propagates quickly. **Operator-only:** target 60-300s TTL at least 24h pre-transfer. Some DNS providers treat apex/CNAME as ALIAS/ANAME with provider-side caching that ignores the published TTL -- verify provider docs before assuming. Captured in preflight §3 DNS plan.
- [x] Keep apex A records pointed at the four GitHub Pages IPv4 addresses unless GitHub changes the documented Pages IP set. **Verified:** apex resolves to `185.199.108.153 / 109.153 / 110.153 / 111.153` (the published GitHub Pages anycast set per <https://docs.github.com/pages/configuring-a-custom-domain-for-your-github-pages-site/managing-a-custom-domain-for-your-github-pages-site>). Owner-independent; no change required.
- [x] Decide whether to add GitHub Pages AAAA records for IPv6 parity. **Decided: keep unset for now.** Re-evaluate after §4 transfer is verified green; adding IPv6 AAAA records expands rollback surface and the docs site has no measurable IPv6-only client demand today. Documented in preflight §3 DNS plan.
- [x] Prepare the `www` DNS change but do not apply it until `rizonetech/impossible-os` Pages settings accept `impossibleos.co` and the new default Pages host is live. **Prepared:** the §3 DNS plan documents the exact pre-condition (`curl -I https://rizonetech.github.io/impossible-os/` returns a successful response under the new owner) and the change (CNAME `rizonesoft.github.io` -> `rizonetech.github.io`). The actual flip is a §4 operator action.
- [x] Prepare validation commands. **Done:** `curl` / `getent` / `dig` block embedded in preflight §3 "Validation commands". Same set referenced from §1's baseline + §7's validation suite.
- [x] Document acceptable transient states: GitHub Pages certificate provisioning may show a temporary HTTPS warning; default `github.io` URLs may redirect differently; DNS may take one TTL to converge. **Done:** preflight §3 "Acceptable transient states" lists 4 named transients (default-Pages-URL redirect drift, new default-Pages-URL TLS warning during cert reissue, `www` CNAME TTL convergence, hits.sh counter reset on namespace switch).
- [x] Commit: `"pages: prepare custom-domain continuity for repository transfer"`

**Test checkpoint:** Before the repository moves, `https://impossibleos.co/` is already the canonical link surface in the site content and the DNS change needed after transfer is known exactly. **Probe-time status:** 9 of 10 items complete; 1 item (DNS TTL drop) is operator-only at the DNS provider. Section flips from `[/]` to `[x]` once the operator confirms the TTL drop in chat.

> **Test runner:** N/A (Pages content + DNS plan, no kernel/usermode/desktop test surface) | validation: re-run the §3 "Validation commands" block in `repository-transfer-preflight.md` post-transfer; expect 200 on apex / 301 on www / 301 on default Pages URL / DNS A records unchanged / `www` CNAME flipped to `rizonetech.github.io`.
> **Notes:**
> - Shipped: tracked [`gh-pages/CNAME`](../../gh-pages/CNAME) (`impossibleos.co\n`), four URL fixes in [`gh-pages/index.html`](../../gh-pages/index.html) (canonical / og:url / JSON-LD / hits.sh), and a `## §3 Custom-Domain Continuity Plan` appendix in [`docs/infrastructure/repository-transfer-preflight.md`](../../docs/infrastructure/repository-transfer-preflight.md) covering canonical policy / source-of-truth changes / DNS plan / cert continuity / transient states / validation commands.
> - The hits.sh visitor counter switched namespace from `rizonesoft.github.io/impossible-os` to `impossibleos.co` -- owner-independent and survives both this transfer and any future move-back. Counter resets to zero (acceptable per §1 R-table).
> - Cross-doc wiring: each operator action (TTL drop, AAAA decision, www CNAME flip) names the §4/§5/§7 section that owns the actual execution. Codex 3x review adoptions in commit `<this-commit>`.
> - Canonical doc: [`docs/infrastructure/repository-transfer-preflight.md` §3](../../docs/infrastructure/repository-transfer-preflight.md#3-custom-domain-continuity-plan).
> - Scope boundary: §3 only **prepares** the custom-domain continuity (source-of-truth content + DNS plan + transient-state taxonomy); §4 owns the actual transfer + the `www` CNAME flip; §5 owns flipping `https_enforced` on once the new cert is stable; §6 owns the GitHub repo URLs in `gh-pages/err/`.
>
> **Verified:** 2026-04-27 | commit `<this-commit>` | 9/10 items | build N/A (Pages content + docs) | lint clean, todo-graph 8/8

---

## 4. Repository Transfer Runbook and Rollback Window

Perform the actual transfer as a short, observable operation with a clear stop/rollback point.

- [ ] Freeze repository changes during transfer: no merges, releases, Pages deploys, branch protection changes, or DNS changes until the post-transfer audit completes.
- [ ] Create a fresh backup marker: latest commit SHA on `main`, latest tag, latest release ID, last successful build workflow run, last successful Pages deployment run, and `git bundle` or verified remote mirror if desired.
- [ ] Confirm there is no repository at `https://github.com/rizonetech/impossible-os` immediately before transfer.
- [ ] Confirm the old path `rizonesoft/impossible-os` will not be recreated during the redirect window. Recreating the old path can break GitHub's automatic repository redirect.
- [ ] Transfer repository ownership from `rizonesoft` to `rizonetech` through GitHub Settings.
- [ ] Accept the transfer from the `rizonetech` organization side if GitHub requires explicit acceptance.
- [ ] Update local remotes after the transfer:
  - `origin` fetch/push -> `https://github.com/rizonetech/impossible-os.git`
  - keep `bootloader` remote unchanged unless a separate decision transfers `impossible-os-bootloader`
- [ ] Trigger or re-run Pages deployment from `rizonetech/impossible-os` after Pages settings are confirmed.
- [ ] Apply the `www.impossibleos.co` CNAME switch from `rizonesoft.github.io` to `rizonetech.github.io` once the new Pages endpoint is active.
- [ ] Keep a rollback window open until all Section 7 validation checks pass. Rollback means transferring back to the previous owner and restoring `www` CNAME if needed, not deleting/recreating repositories.
- [ ] Commit: `"docs/github: add repository transfer runbook"`

**Test checkpoint:** The repository lands at `https://github.com/rizonetech/impossible-os`, local `git fetch` succeeds from the new remote, the old GitHub repo URL redirects, and the custom domain still serves the landing page.

---

## 5. Post-Transfer Settings, Workflows, Secrets, and Environments Audit

GitHub transfers repository data, but the project should not trust complex settings until they are checked directly.

- [ ] Verify repository visibility is unchanged from the intended transfer state.
- [ ] Verify default branch, branch protection, rulesets, required checks, required reviews, signed commit rules if any, force-push/delete restrictions, and bypass users/teams.
- [ ] Verify team and collaborator permissions match the access model from Section 1.
- [ ] Verify `.github/CODEOWNERS` still references valid users/teams after transfer; update owner handles or teams if needed.
- [ ] Verify Actions policy and workflow token permissions. Pages workflow must retain `contents: read`, `pages: write`, and `id-token: write`.
- [ ] Verify repository secrets and variables required by release automation, signing, notifications, or integrations. GitHub may not expose secret values; validate by running the workflow that consumes them or by comparing names to the preflight inventory.
- [ ] Verify Pages environment exists, deployment branch/source is correct, environment protection does not block deploys, and the latest deployment is attached to `rizonetech/impossible-os`.
- [ ] Verify release workflow can read tags and attach artifacts without old-owner assumptions.
- [ ] Verify issue templates, discussions, labels, milestones, project links, and security policy are still present.
- [ ] Verify webhooks, deploy keys, GitHub Apps, branch badges, package settings, and integration installs that may be owner-scoped.
- [ ] Verify org-level autonomous-agent/Copilot policy for this repository remains disabled as required by the AI-system documentation.
- [ ] Commit: `"docs/github: record post-transfer settings audit"`

**Test checkpoint:** The settings audit closes only after direct inspection or a successful workflow run proves each setting survived or was intentionally recreated.

---

## 6. URL, Badge, Docs, and Generated-Link Sweep

Stop relying on redirects for first-party content. Redirects are useful for old external links, but repo-owned files should name the new canonical owner.

- [ ] Update README badges and links from `rizonesoft/impossible-os` to `rizonetech/impossible-os` where they describe the active repository.
- [ ] Update clone commands from `https://github.com/rizonesoft/impossible-os.git` to `https://github.com/rizonetech/impossible-os.git`.
- [ ] Update GitHub Actions badge URLs, release URLs, stars URLs, commit URLs, issue/discussion URLs, and source links.
- [ ] Update `docs/infrastructure/github-setup.md` to describe the new canonical repository owner and transfer history.
- [ ] Update `docs/infrastructure/development-tooling.md` workflow table rows and Pages references if they still name the old owner.
- [ ] Update `gh-pages/index.html` GitHub links to `rizonetech/impossible-os` after transfer.
- [ ] Update `gh-pages/err/**/*.html` GitHub links to `rizonetech/impossible-os` after transfer.
- [ ] Regenerate or patch generated TODO graph links in [`docs/infrastructure/todo-graph.md`](../../docs/infrastructure/todo-graph.md) so graph nodes point at the new repository owner.
- [ ] Search and classify remaining `rizonesoft` references:
  - Keep references that identify the company/brand or historical owner.
  - Update references that point at the active GitHub repository.
  - Document intentionally retained references in the commit message or notes.
- [ ] Avoid doctrine churn: do not edit `CLAUDE.md` or `AGENTS.md` unless a real policy statement changes.
- [ ] Commit: `"docs/github: update canonical repository owner to rizonetech"`

**Test checkpoint:** `rg -n "rizonesoft/impossible-os|github.com/rizonesoft|rizonesoft.github.io" README.md CONTRIBUTING.md SECURITY.md CODE_OF_CONDUCT.md docs gh-pages .github todo` returns only intentional historical or brand references.

---

## 7. Validation Suite: Pages, Actions, Releases, Clone, Hooks, Graph

Treat the transfer as complete only after the same surfaces a contributor uses are verified end to end.

- [ ] Git validation:
  - Fresh clone from `https://github.com/rizonetech/impossible-os.git` succeeds.
  - Existing clone with updated `origin` can fetch and push if the operator has permission.
  - Old repository URL redirects to the new repository URL.
- [ ] Pages validation:
  - `https://impossibleos.co/` returns `200 OK`.
  - `https://www.impossibleos.co/` redirects to `https://impossibleos.co/`.
  - `https://rizonetech.github.io/impossible-os/` redirects to the custom domain or serves the expected Pages site.
  - HTTPS certificate is valid for the custom domain.
  - Latest Pages workflow run completed successfully under the new owner.
- [ ] DNS validation:
  - Apex A records match GitHub Pages current IPs.
  - `www.impossibleos.co` CNAME points to `rizonetech.github.io`.
  - TTL is restored to the normal value after the transfer window.
- [ ] Workflow validation:
  - Build workflow runs on a no-op or normal push.
  - Pages workflow runs on `workflow_dispatch`.
  - Release workflow dry-run path or documented non-release verification path passes where available.
  - Labeler/stale workflows are syntactically valid and permitted under org policy.
- [ ] Local tooling validation:
  - `bash scripts/lint.sh`
  - `bash scripts/test-tooling.sh`
  - `python3 scripts/todo-graph/build.py --quiet`
  - `python3 scripts/todo-graph/validate.py --warnings-only`
- [ ] Project policy validation:
  - `AGENTS.md` authority block remains byte-compatible with its canonical source if the regression suite checks it.
  - No new `.codex/`, `.cursor/`, `.github/agents/`, `.github/chatmodes/`, `.github/workflows/copilot-setup-steps.yml`, or autonomous-agent enablement files were added.
  - Org UI policy still blocks autonomous coding-agent PR flow for this repository.
- [ ] Public link validation:
  - README badges render.
  - Release links resolve.
  - Site "GitHub" and "Releases" buttons resolve.
  - Generated TODO graph links resolve to `rizonetech/impossible-os`.
- [ ] Commit: `"docs/github: record repository transfer validation"`

**Test checkpoint:** Transfer validation is green only when the public site, workflows, local graph tooling, and policy guardrails all pass under the new owner.

---

## 8. Move-Back and Public-Visibility Runbook

Define the return path before it is needed. Moving back later should be a planned ownership change, not a scramble during the public launch.

- [ ] Define the move-back trigger: repository ready to become public, release posture approved, security/private-history review complete, and public documentation ready.
- [ ] Confirm the personal account can receive the repository back and supports the desired public Pages setup.
- [ ] Confirm no repository exists at the old destination path that would block the transfer back.
- [ ] Before moving back, repeat the Section 1 preflight inventory from the `rizonetech` state.
- [ ] Transfer repository from `rizonetech/impossible-os` back to the intended personal account path.
- [ ] Update local remotes and first-party docs back to the final canonical owner, if the final public owner is not `rizonetech`.
- [ ] Update `www.impossibleos.co` CNAME back to the receiving account's `<owner>.github.io` Pages hostname if the custom domain remains on GitHub Pages.
- [ ] Re-verify the custom domain on the receiving account before or immediately after transfer.
- [ ] Flip repository visibility to public only after:
  - secrets are audited,
  - private-only notes are removed or accepted,
  - release workflow and issue templates are public-ready,
  - branch protection/rulesets are active,
  - security policy is public-ready,
  - Pages custom domain is stable.
- [ ] Decide how long to rely on GitHub repository redirects after move-back and document any old-owner URLs that should remain supported externally.
- [ ] Commit: `"docs/github: add move-back and public-visibility runbook"`

**Test checkpoint:** The final public move is considered ready only after the custom domain, repository visibility, first-party URLs, and security posture are validated in that order.

---

## 9. Documentation Sync and Closure

Close the roadmap by making the final owner state discoverable and removing transfer-only ambiguity from contributor docs.

- [ ] Update [`docs/infrastructure/github-setup.md`](../../docs/infrastructure/github-setup.md) with the final repository owner, transfer date, Pages custom-domain settings, DNS shape, branch protection notes, and org policy notes.
- [ ] Update [`todo/00-infrastructure/INDEX.md`](INDEX.md) if this TODO moves from `draft` to `active` or `done`.
- [ ] Update [`todo/TODO-00-INDEX.md`](../TODO-00-INDEX.md) only if this work should be promoted to root Active Epics.
- [ ] Add a "Repository Ownership" or "Canonical GitHub URL" note to contributor-facing docs if repeated confusion appears during transfer.
- [ ] Run URL searches and preserve a short final owner-reference inventory in the TODO notes or commit message.
- [ ] Run TODO graph build/validate after the final docs edit.
- [ ] Mark Implementation Order rows complete only when the corresponding operator action and validation evidence exist.
- [ ] Commit: `"todo: close repository transfer to rizonetech"`

**Test checkpoint:** A new contributor can find the canonical repository URL, clone it, view the Pages site, and follow the docs without encountering stale owner names except where history is intentional.

---

## OS Comparison

| ⭐ | Feature                                  | 🪟 Win11             | 🐧 Linux              | 🚀 Impossible OS                    |
|----|------------------------------------------|---------------------|----------------------|------------------------------------|
| ⭐ | Repo-owner transfer runbook              | ❌ N/A (closed src) | ⚠️ Ad-hoc per project | ⏳ §1 baseline + §2 org-readiness probe shipped (`docs/infrastructure/repository-transfer-preflight.md`); §3-§9 still planned |
| ⭐ | Rollback-window discipline               | ❌ N/A              | ⚠️ Project-dependent  | ⬜ Planned -- §4 keeps rollback open until §7 green |
| ⭐ | Pages custom-domain continuity plan      | ❌ N/A              | ⚠️ Project-dependent  | ⏳ §3 shipped: tracked CNAME, 4 URL fixes in `gh-pages/index.html`, DNS plan + transient-state taxonomy + validation block in preflight §3; one operator-only TTL drop remaining |
| 💎 | Forward move-back / visibility flip plan | ❌ N/A              | ❌ N/A                | ⬜ Planned -- §8 defines return path before transfer |
| ⭐ | Post-transfer settings audit             | ❌ N/A              | ⚠️ Tribal-knowledge   | ⬜ Planned -- §5 verifies branch protection / secrets / Pages env / Actions policy directly |
| 💎 | AI-policy boundary preserved across move | ❌ N/A              | ❌ N/A                | ⬜ Planned -- §2, §5, §7 keep no-autonomous-agent stance under new org |

> **After §1-§4:** Inventory, org readiness, Pages continuity plan, and a time-boxed transfer runbook with rollback exist before any GitHub setting is touched.
> **After §5-§7:** Settings audit, first-party URL sweep, and end-to-end validation suite confirm the move did not silently break Pages, Actions, releases, badges, or graph tooling.
> **After §8-§9:** A move-back and public-visibility runbook is documented before it is needed, and `docs/infrastructure/github-setup.md` carries the final state for future contributors.

---

## Unit Tests

> Tests for this TODO are **operator-validation**, not kernel-side or host-tooling-side. There is no `TEST_CAT_*` registration, no `src/kernel/test/test_*.c`, no `user/test/test_*.c`, and no `scripts/test-tooling.sh` sub-test to add. The sole verification surface is the live GitHub repository, GitHub Pages, DNS, and the existing tooling already covered by `bash scripts/test-tooling.sh` and `python3 scripts/todo-graph/validate.py`. Section 7 is the validation suite for this TODO.

- [ ] No new automated tests are introduced by this TODO.
- [ ] Section 7 enumerates the operator-executed checks (git fetch, `curl -I`, DNS lookup, workflow re-run, `bash scripts/lint.sh`, `bash scripts/test-tooling.sh`, `python3 scripts/todo-graph/build.py`, `python3 scripts/todo-graph/validate.py`) that stand in for unit tests here.
- [ ] Commit: covered by §7's validation commit; no separate test commit.

---

## Verification

> **Test runner:** N/A (operator runbook -- no kernel/usermode/desktop test surface) | validation: §7 validation suite (`curl -I https://impossibleos.co/`, `curl -I https://www.impossibleos.co/`, `curl -I https://rizonetech.github.io/impossible-os/`, fresh clone from new origin, latest Pages deployment under `rizonetech`, `bash scripts/lint.sh`, `bash scripts/test-tooling.sh`, `python3 scripts/todo-graph/build.py --quiet`, `python3 scripts/todo-graph/validate.py --warnings-only`).

- [ ] Repository lives at `https://github.com/rizonetech/impossible-os`; old `rizonesoft/impossible-os` URL redirects.
- [ ] `git fetch` succeeds from the new origin in a fresh clone and in an updated existing clone.
- [ ] `https://impossibleos.co/` returns `200 OK` with a valid HTTPS certificate.
- [ ] `https://www.impossibleos.co/` redirects to `https://impossibleos.co/`.
- [ ] `https://rizonetech.github.io/impossible-os/` either redirects to the custom domain or serves the expected Pages site.
- [ ] Apex DNS A records match GitHub Pages IPs; `www.impossibleos.co` CNAME resolves to `rizonetech.github.io`.
- [ ] Latest Pages, build, and (if dispatched) release workflow runs are green under the new owner.
- [ ] `bash scripts/lint.sh` passes after the URL/badge/doc sweep.
- [ ] `bash scripts/test-tooling.sh` passes (no regression from URL changes).
- [ ] `python3 scripts/todo-graph/build.py --quiet` and `python3 scripts/todo-graph/validate.py --warnings-only` both pass.
- [ ] `rg -n "rizonesoft/impossible-os|github.com/rizonesoft|rizonesoft.github.io" README.md CONTRIBUTING.md SECURITY.md CODE_OF_CONDUCT.md docs gh-pages .github todo` returns only intentional historical or brand references.
- [ ] Org-level policy still blocks autonomous coding-agent PR flow for the repository (Settings -> Copilot -> Access policies; manual UI check captured in §5/§7 evidence).
- [ ] Verify on: GitHub.com (production), DNS provider's authoritative resolvers, and the Linux WSL2 dev host. Bare-metal / Windows test platforms not applicable -- this TODO is repository-operations only.
- [ ] Commit: `"00-infrastructure/TODO-09: repository transfer to rizonetech complete"`
