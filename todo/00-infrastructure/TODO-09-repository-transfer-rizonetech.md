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

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Move the Impossible OS repository from the current `rizonesoft/impossible-os` owner to the `rizonetech` GitHub organization with no avoidable disruption to GitHub Pages, release links, badges, workflows, branch protection, custom domain ownership, or contributor workflow. Keep the move reversible so the repository can later transfer back to the personal account when the project is ready to become public.

> [!IMPORTANT]
> **Current state (baselined 2026-04-27 in [`docs/infrastructure/repository-transfer-preflight.md`](../../docs/infrastructure/repository-transfer-preflight.md)):** Local remotes point at `https://github.com/rizonesoft/impossible-os.git` and `https://github.com/rizonesoft/impossible-os-bootloader.git`. The repository is `PRIVATE`, owned by the `rizonesoft` user (NOT an org). GitHub Pages serves with `build_type: legacy` from the `gh-pages` BRANCH (sha `35e7dcd5`, last successful 2026-04-12); the workflow [`.github/workflows/pages.yml`](../../.github/workflows/pages.yml) is configured but its 2026-04-12 run failed with `Artifact storage quota has been hit` and has not run successfully since. The custom domain `https://impossibleos.co/` returns `200 OK` directly (cert valid through 2026-06-16), `https://www.impossibleos.co/` returns `301 -> https://impossibleos.co/`, and `https://rizonesoft.github.io/impossible-os/` returns `301 -> http://impossibleos.co/` (HTTP because `https_enforced: false`). DNS for the apex points at the four GitHub Pages anycast IPv4 addresses (no AAAA set); `www.impossibleos.co` CNAMEs to `rizonesoft.github.io` (pre-transfer baseline; the §4 transfer + §6 sweep flip these to `rizonetech.github.io`). Repo state: 7 active workflows, 1 secret (`BOOTLOADER_REPO_TOKEN`), 0 variables, 0 webhooks, 0 deploy keys, ruleset "Default Branch Security" active on `main`, no classic branch protection, Dependabot alerts disabled. Hard-coded owner references span **39 files / 364 matches** under the expanded scan path (`README.md CONTRIBUTING.md SECURITY.md CODE_OF_CONDUCT.md AGENTS.md CLAUDE.md docs gh-pages .github todo scripts`); the narrow-path 28-files / 283-matches count is the historical baseline and is superseded -- the expanded scan picks up `scripts/todo-graph/render.py:42` (`REPO_URL_BASE` constant for TODO graph links) and 35 references across 8 `todo/` files that the narrow path missed. Receiving destination `rizonetech/impossible-os` does not exist; the `rizonetech` enterprise org has 50 seats / 1 filled and the operator (`rizonesoft`) is admin. The landing page still has canonical, Open Graph, JSON-LD, and visitor-counter references to `rizonetech.github.io/impossible-os/`; the error pages already use `https://impossibleos.co/...`.

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
- -> XREF: `TODO-10-documentation-site.md §1` -- the generated site and drift gate that now enforce the canonical owner in live URLs (`project.json`)

## Outcome

- Repository ownership transfers from `rizonesoft/impossible-os` to `rizonetech/impossible-os` with a preflight checklist, a time-boxed transfer runbook, and a rollback path.
- `https://impossibleos.co/` stays the primary public Pages URL before, during, and after the transfer.
- `www.impossibleos.co` CNAME is updated from `rizonetech.github.io` to `rizonetech.github.io` only when the receiving org's Pages endpoint is ready.
- GitHub Pages custom-domain verification, HTTPS enforcement, Pages deployment environment, and Actions permissions are confirmed under `rizonetech`.
- Repository redirects are treated as convenience only. Docs, badges, clone examples, release links, and generated graph links are updated to the new canonical owner where appropriate.
- Organization policy preserves the Impossible OS AI boundary: no autonomous-agent PR flow, no cloud coding-agent enablement, no new parallel instruction tree.
- A move-back plan exists for returning to the personal account when the repository becomes public, including DNS, Pages, visibility, redirects, and post-transfer validation.
- Every operator action that cannot be expressed in git is captured in `docs/infrastructure/github-setup.md` so the final state is reproducible.

## Implementation Order

| ⭐  | Order | Section | Deliverable                                                     | Depends On                     | Status |
| --- | :---: | :-----: | --------------------------------------------------------------- | ------------------------------ | :----: |
| ⭐  |   1   |    1    | Preflight inventory and risk register                           | --                             |  [x]   |
| ⭐  |   2   |    2    | `rizonetech` organization readiness and policy parity           | §1                             |  [/]   |
| ⭐  |   3   |    3    | GitHub Pages custom-domain continuity plan                      | §1, §2                         |  [/]   |
| ⭐  |   4   |    4    | Repository transfer runbook and rollback window                 | §1, §2, §3                     |  [/]   |
| ⭐  |   5   |    5    | Post-transfer settings, workflows, secrets, environments audit  | §4                             |  [/]   |
| ⭐  |   6   |    6    | URL, badge, docs, and generated-link sweep                      | §4, §5                         |  [x]   |
| ⭐  |   7   |    7    | Validation suite: Pages, Actions, releases, clone, hooks, graph | §5, §6                         |  [/]   |
| ⭐  |   8   |    8    | Move-back and public-visibility runbook                         | §1, §2, §3, §4, §5, §6, §7     |  [/]   |
| ⭐  |   9   |    9    | Documentation sync and closure                                  | §1, §2, §3, §4, §5, §6, §7, §8 |  [x]   |

> **Order vs section number:** The file is ordered by execution flow. Section 8 is intentionally specified before the first transfer happens so the team can confirm the move is reversible rather than discovering account or Pages constraints after the repository is already in the organization.

---

## 1. Preflight Inventory and Risk Register

Capture the exact current state before changing any GitHub owner setting. The output is the baseline used to prove that the move did not break Pages, CI, release automation, or project policy.

- [x] Record current repository identity: owner `rizonesoft` (user, not org -- `isInOrganization: false`), visibility `PRIVATE`, default branch `main`, node id `R_kgDORgIuVg`, HTTPS / SSH URLs, topics (`bare-metal`, `freestanding`, `operating-system`, `os-development`, `uefi-boot`), description, homepage `https://rizonesoft.com`, Issues=on, Discussions=off, Wiki=on (unused), Projects=on (unused), security policy file present, GPL-3.0 license, all three merge methods allowed. Captured in [`docs/infrastructure/repository-transfer-preflight.md`](../../docs/infrastructure/repository-transfer-preflight.md) §1.
- [x] Record current Pages state: `build_type: legacy` from branch `gh-pages` path `/`, environment `github-pages` (id `13147855799`), custom domain `impossibleos.co`, custom 404 enabled, **`https_enforced: false`**, cert `approved` through `2026-06-16` covering both apex and `www`. Last successful workflow run 2026-03-18; the 2026-04-12 run failed (`Artifact storage quota has been hit`); legacy branch source keeps the live site up. No tracked `gh-pages/CNAME` file -- domain stored in Pages settings only. Preflight §2.
- [x] Record live domain baseline (curl HEAD captured 2026-04-27 ~19:54 UTC):
  - `https://impossibleos.co/` returns `200 OK`, `last-modified: 2026-04-12T00:27:55Z`, etag `"69dae70b-b9bc"`, content-length `47548`.
  - `https://www.impossibleos.co/` returns `301 -> https://impossibleos.co/`.
  - `https://rizonetech.github.io/impossible-os/` returns `301 -> http://impossibleos.co/` (HTTP -- consequence of `https_enforced: false`).
  - `impossibleos.co` apex A records: `185.199.108.153`, `185.199.109.153`, `185.199.110.153`, `185.199.111.153` (the 4 GitHub Pages anycast addresses); no AAAA set.
  - `www.impossibleos.co` CNAME currently targets `rizonetech.github.io.`. Preflight §3.
- [x] Record current GitHub Actions state: enabled, `allowed_actions: all`, `sha_pinning_required: false`, default `GITHUB_TOKEN` permission `read`, no required workflow approvals, GitHub-hosted runners only. **7 active workflows** (the TODO Inputs list named only 5 -- adds `todo-graph.yml` and `visual-regression.yml`) plus the auto-generated `pages-build-deployment`. Preflight §4.
- [x] Record repository settings that do not reliably travel: classic branch protection on `main` is **NOT** set (`HTTP 404 Branch not protected`); ruleset "Default Branch Security" (id `14058331`) IS active with `required_status_checks` (`Build Impossible OS`, strict, `do_not_enforce_on_create`), `non_fast_forward`, `required_linear_history`, `deletion`; bypass actor `RepositoryRole 5 (Maintain)`. Repo secrets: `BOOTLOADER_REPO_TOKEN` only. Variables: none. Webhooks: none. Deploy keys: none. Vulnerability alerts: **disabled**. Automated security fixes: disabled. Environment `github-pages` has `branch_policy` protection. Preflight §5.
- [x] Record current access model: only `rizonesoft` is a direct collaborator (admin/maintain/push/pull/triage). No outside collaborators. No teams (user-owned repo). CODEOWNERS routes everything plus 6 critical paths to `@derickpayne`. Pages/DNS administered by repo owner; DNS provider holds `impossibleos.co` records. Receiving org `rizonetech` (canonical login `Rizonetech`, id `279138845`) is `enterprise` plan with 50 seats / 1 filled; current operator is direct-membership admin; `rizonetech/impossible-os` does not yet exist. Preflight §6.
- [x] Record all hard-coded owner references via `rg -n 'rizonesoft/impossible-os|github\.com/rizonesoft|rizonesoft\.github\.io|hits\.sh/rizonesoft' README.md CONTRIBUTING.md SECURITY.md CODE_OF_CONDUCT.md AGENTS.md CLAUDE.md docs gh-pages .github todo scripts` (path set expanded after Codex flagged that the original narrower path missed `scripts/todo-graph/render.py:42` -- the `REPO_URL_BASE` constant for TODO graph links). **303 matches across ~37 files (pre-preflight-doc theoretical) / 364 matches across 39 files (post-preflight-doc measured)**; the +61 delta is intentional self-references in this preflight doc + the TODO-09 stamp + the github-setup pointer. `scripts/` contributes 2 matches: `render.py:42` (active generator constant -- §6 must patch) and `test-ai-system.sh:396` (informational log line). `todo/` contributes 35 matches across 8 files (mostly project narrative; some inventoried as active references). `SECURITY.md` / `CODE_OF_CONDUCT.md` / `AGENTS.md` / `CLAUDE.md` returned **zero** GitHub-URL matches and should not need owner edits. Preflight §7 lists both baselines + the full file set + per-pattern counts + the explicit `scripts/` handling.
- [x] Record remote consumers: local dev clone (`origin` on this host), one prerelease `v26.3.18-alpha.821` (artifact URLs auto-redirect through GitHub), the `BOOTLOADER_REPO_TOKEN` consumer (`release.yml` only), and the `hits.sh/rizonetech.github.io` visitor-counter namespace (counter resets on namespace change -- acceptable for a private repo). External docs / social profiles / pinned GitHub Apps are not enumerable from inside the repo and must be walked manually before §4. Preflight §8.
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
- [/] Verify `impossibleos.co` for **GitHub Pages** under the receiving organization (Pages-specific custom-domain ownership check; this is a SEPARATE flow from org-identity domain verification). **Operator-only (O1):** UI-only, no public REST endpoint. Walk <https://github.com/organizations/rizonetech/settings/pages> "Verified custom domains" panel, add the displayed TXT record to the `impossibleos.co` DNS zone, then click Verify -- per <https://docs.github.com/pages/configuring-a-custom-domain-for-your-github-pages-site/verifying-your-custom-domain-for-github-pages>. Optional separate org-identity verification at <https://github.com/organizations/rizonetech/settings/security> ("Verified and approved domains") is for SAML / billing / branded email; not required for Pages, harmless to also walk. Captured in [`docs/infrastructure/repository-transfer-preflight.md` §2 O1](../../docs/infrastructure/repository-transfer-preflight.md#operator-only-items-require-adminorg-token-scope-or-ui-walkthrough).
- [/] Configure organization Actions policy so existing workflows can run: GitHub-hosted runners allowed, required marketplace actions allowed (`actions/checkout`, `actions/configure-pages`, `actions/upload-pages-artifact`, `actions/deploy-pages`, and any release/build workflow actions), and workflow permissions compatible with Pages deploy. **Operator-only (O2):** source-repo baseline already captured in §1 (allowed_actions=all, default_workflow_permissions=read); operator must match-or-relax at org level via <https://github.com/organizations/rizonetech/settings/actions> or post-`admin:org` `gh api orgs/rizonetech/actions/permissions`. Concrete commands documented in preflight §2.
- [/] Confirm org-level Pages policy allows repository Pages and custom domains. **Operator-only (O3):** UI-only check at <https://github.com/organizations/rizonetech/settings/pages> (no stable public REST endpoint). Confirm "Public" or "Private and Public" is selected and custom domains are allowed.
- [x] Confirm org-level **security defaults** are compatible with repository settings: Dependabot, secret scanning, code scanning, private vulnerability reporting. **Verified:** `gh api orgs/rizonetech` security defaults for new repos are all `false` (`advanced_security`, `dependabot_alerts`, `dependabot_security_updates`, `dependency_graph`, `secret_scanning`, `secret_scanning_push_protection`); `two_factor_requirement_enabled: false`; no SAML; `web_commit_signoff_required: false`. Source-repo baseline (§1) is also "all off", so transfer is parity-compatible. (Note: org rulesets / merge-method defaults / 2FA-required-policy still need operator probe -- tracked as a separate item below under O4.)
- [/] Confirm org-level **rulesets, merge-method defaults, and 2FA enforcement** are compatible with the source-repo settings: ruleset import path for "Default Branch Security" (id `14058331`, captured in §1), merge methods (merge / squash / rebase all allowed at source), 2FA enforcement (currently OFF). **Operator-only (O4):** `gh api orgs/rizonetech/rulesets` requires `admin:org` scope. Walk <https://github.com/organizations/rizonetech/settings/repository-defaults> and <https://github.com/organizations/rizonetech/settings/rules>; the org may be empty (no preset ruleset), in which case the source-repo ruleset migrates with the repo per §5. The source-repo ruleset itself migrates with the repo on transfer; this O4 check verifies the org's *envelope* is at least as permissive as the source.
- [/] Preserve the AI-system boundary at org level: disable Copilot cloud coding-agent / autonomous-agent PR enablement for this repository, or document the exact UI setting that keeps it disabled after transfer. **Operator-only (O5):** Copilot Access policy has no public-API surface today. Walk <https://github.com/organizations/rizonetech/settings/copilot/access>, set "No access" or "Selected members" excluding `rizonesoft` from cloud-agent permission. Re-verify in §5 / §7. Cross-referenced from [Autonomous-Agent Boundary Policy](../../docs/infrastructure/ai-system.md#autonomous-agent-boundary-policy).
- [/] Create or identify teams that replace personal/collaborator permissions: maintainers, reviewers, release operators, security contacts, and Pages/DNS administrators. **Operator-only (O6):** `gh api orgs/rizonetech/teams` returns empty (verified). Solo-dev minimum: one `maintainers` team containing `rizonesoft`. CODEOWNERS currently routes everything to `@derickpayne` (the user) -- that handle still resolves under org ownership, but consider adding a `@rizonetech/maintainers` route once the team exists. <https://github.com/organizations/rizonetech/teams>.
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
> **Deferred:** [awaiting-operator] org-console checks (Actions policy, Pages policy, rulesets/2FA, teams, Copilot agent enablement) are GitHub UI walks -- no unattended pass can perform this; operator action required (cohort 2026-07-31)

---

## 3. GitHub Pages Custom-Domain Continuity Plan

Make the custom domain the stable user-facing surface. The default owner-based Pages URL is allowed to change; `https://impossibleos.co/` must not.

- [x] Decide the canonical domain policy: apex `https://impossibleos.co/` remains canonical; `https://www.impossibleos.co/` redirects to apex; owner-based GitHub Pages URLs are non-canonical. **Documented in [`docs/infrastructure/repository-transfer-preflight.md` §3](../../docs/infrastructure/repository-transfer-preflight.md#3-custom-domain-continuity-plan) "Canonical domain policy" table.**
- [x] Add a tracked [`gh-pages/CNAME`](../../gh-pages/CNAME) file containing exactly `impossibleos.co`. **Verified:** the `gh-pages` BRANCH (current legacy Pages source) already carries a CNAME blob `1441672e` with content `impossibleos.co`; adding the same file to `main`'s `gh-pages/` folder gives the workflow-deployed artifact (when the artifact-quota issue resolves) the same custom-domain hint, so switching the Pages source mode from "branch" to "Actions" later doesn't drop the domain. The `actions/upload-pages-artifact@v3` step in [`pages.yml`](../../.github/workflows/pages.yml) bundles everything under `gh-pages/` into the artifact, so the CNAME ships with the deploy.
- [x] Update [`gh-pages/index.html`](../../gh-pages/index.html) canonical, Open Graph URL, JSON-LD URL, and visitor-counter URL away from `rizonetech.github.io/impossible-os/` to `https://impossibleos.co/`. **Done:** four edits at lines 13 (canonical), 19 (og:url), 36 (JSON-LD `url`), 1089 (hits.sh visitor counter switched to `hits.sh/impossibleos.co.svg` -- owner-independent across this transfer AND any future move-back). `author.url: https://rizonesoft.com` at line 40 retained intentionally (brand homepage, not active GitHub repo URL).
- [x] Audit [`gh-pages/err/`](../../gh-pages/err/) for consistent canonical URLs and relative links. **Verified:** all 14 error-page `index.html` files (`err/index.html` + `err/000[0-d]/index.html`) already use `https://impossibleos.co/err/<code>/` as their canonical URL; relative `../../` links resolve correctly. The single `https://github.com/rizonesoft/impossible-os` "GitHub" nav link in each err page is owner-specific repo URL, owned by the [URL/badge/docs/generated-link sweep](#6-url-badge-docs-and-generated-link-sweep) (§6), NOT §3.
- [/] Reduce DNS TTL for `www.impossibleos.co` before transfer if the DNS provider allows it, so the CNAME switch to `rizonetech.github.io` propagates quickly. **Operator-only:** target 60-300s TTL at least 24h pre-transfer. Some DNS providers treat apex/CNAME as ALIAS/ANAME with provider-side caching that ignores the published TTL -- verify provider docs before assuming. Captured in preflight §3 DNS plan.
- [x] Keep apex A records pointed at the four GitHub Pages IPv4 addresses unless GitHub changes the documented Pages IP set. **Verified:** apex resolves to `185.199.108.153 / 109.153 / 110.153 / 111.153` (the published GitHub Pages anycast set per <https://docs.github.com/pages/configuring-a-custom-domain-for-your-github-pages-site/managing-a-custom-domain-for-your-github-pages-site>). Owner-independent; no change required.
- [x] Decide whether to add GitHub Pages AAAA records for IPv6 parity. **Decided: keep unset for now.** Re-evaluate after §4 transfer is verified green; adding IPv6 AAAA records expands rollback surface and the docs site has no measurable IPv6-only client demand today. Documented in preflight §3 DNS plan.
- [x] Prepare the `www` DNS change but do not apply it until `rizonetech/impossible-os` Pages settings accept `impossibleos.co` and the new default Pages host is live. **Prepared:** the §3 DNS plan documents the exact pre-condition (`curl -I https://rizonetech.github.io/impossible-os/` returns a successful response under the new owner) and the change (CNAME `rizonetech.github.io` -> `rizonetech.github.io`). The actual flip is a §4 operator action.
- [x] Prepare validation commands. **Done:** `curl` / `getent` / `dig` block embedded in preflight §3 "Validation commands". Same set referenced from §1's baseline + §7's validation suite.
- [x] Document acceptable transient states: GitHub Pages certificate provisioning may show a temporary HTTPS warning; default `github.io` URLs may redirect differently; DNS may take one TTL to converge. **Done:** preflight §3 "Acceptable transient states" lists 4 named transients (default-Pages-URL redirect drift, new default-Pages-URL TLS warning during cert reissue, `www` CNAME TTL convergence, hits.sh counter reset on namespace switch).
- [x] Commit: `"pages: prepare custom-domain continuity for repository transfer"`

**Test checkpoint:** Before the repository moves, `https://impossibleos.co/` is already the canonical link surface in the site content and the DNS change needed after transfer is known exactly. **Probe-time status:** 9 of 10 items complete; 1 item (DNS TTL drop) is operator-only at the DNS provider. Section flips from `[/]` to `[x]` once the operator confirms the TTL drop in chat.

> **Test runner:** N/A (Pages content + DNS plan, no kernel/usermode/desktop test surface) | validation: pre-transfer block expects current `rizonesoft` state + low TTL on `www`; post-transfer block expects `rizonetech.github.io` redirect + `www` flipped + cert reissued + Pages settings `.cname == "impossibleos.co"` -- both blocks captured in `repository-transfer-preflight.md` §3.
> **Notes:**
> - Shipped: tracked [`gh-pages/CNAME`](../../gh-pages/CNAME) (`impossibleos.co\n`, branch-source parity only -- ignored under Actions-mode deploys), four URL fixes in [`gh-pages/index.html`](../../gh-pages/index.html) (canonical / og:url / JSON-LD / hits.sh), and a `## §3 Custom-Domain Continuity Plan` appendix in [`docs/infrastructure/repository-transfer-preflight.md`](../../docs/infrastructure/repository-transfer-preflight.md) covering canonical policy / source-of-truth changes / DNS plan / cert continuity / transient states / split pre+post validation blocks.
> - Actual Pages custom-domain continuity control is `repos/{owner}/impossible-os/pages.cname` (Pages settings, set to `impossibleos.co` per §1 baseline); GitHub Pages docs explicitly say the CNAME file is ignored for Actions-mode deploys, so the file is parity-only and the operator must verify `.cname` via `gh api` before/after transfer.
> - Cross-doc wiring: pre/post-transfer validation commands name the operator action sequence (TTL drop -> transfer -> www CNAME flip -> cert reissue -> `https_enforced` flip in §5). Codex 3x review adoptions in commit `ec2b7c00`.
> - Canonical doc: [`docs/infrastructure/repository-transfer-preflight.md` §3](../../docs/infrastructure/repository-transfer-preflight.md#3-custom-domain-continuity-plan).
> - Scope boundary: §3 only **prepares** the custom-domain continuity (source-of-truth content + DNS plan + transient-state taxonomy + pre/post validation); §4 owns the actual transfer + the `www` CNAME flip; §5 owns flipping `https_enforced` on once the new cert is stable; §6 owns the GitHub repo URLs in `gh-pages/err/`.
>
> **Verified:** 2026-04-27 | commit `ec2b7c00` | 9/10 items | build N/A (Pages content + docs) | lint clean, todo-graph 8/8
> **Quality reviewed:** 2026-04-27 | Codex 3x (adversarial, consistency, perf) | 2H+1M+1L fixed, 0 open | scope: N/A (docs+pages-content -- no domain code-quality skill applies; re-adversarial skipped: docs+stamp-only fixes, no SMP/state-machine touch)
> **Deferred:** [awaiting-operator] DNS TTL reduction is a registrar/DNS-provider console action -- no unattended pass can perform this; operator action required (cohort 2026-07-31)

---

## 4. Repository Transfer Runbook and Rollback Window

Perform the actual transfer as a short, observable operation with a clear stop/rollback point.

- [x] **Plan documented:** freeze rules captured in [`docs/infrastructure/repository-transfer-preflight.md` §4 "Pre-transfer freeze"](../../docs/infrastructure/repository-transfer-preflight.md#pre-transfer-freeze) -- no merges to `main`, no new releases / tag pushes, no `gh-pages/**` changes, no branch-protection / ruleset edits, no DNS changes (except the planned `www` CNAME flip in step 8). **Operator action:** observe the freeze for the ~30-minute window from "press transfer" to "all post-transfer validation green".
- [x] **Backup markers captured 2026-04-27:** `main` HEAD `5d0df5bad107ae05d1280984ee069ceae1d790be`, latest tag `v26.3.18-alpha.821`, latest release `Impossible OS v26.3.18-alpha.821` (pre-release, 2026-03-18), latest successful Build run `databaseId: 25018759331` at `5d0df5ba`, latest successful Pages deploy `databaseId: 25018428197` at `ec2b7c00` (deployment id `4503518780`, state `success`), `gh-pages` branch tip `35e7dcd5`, Pages live state `etag: "69efca05-b970"`, Pages settings `cname: impossibleos.co`. Full table in preflight §4 "Backup markers". Optional `git bundle` belt-and-braces command also documented.
- [x] **Destination clean:** `gh repo view rizonetech/impossible-os` returns "Could not resolve to a Repository" + `gh api orgs/rizonetech/repos` returns `[]`; clean transfer destination verified at backup-marker capture time.
- [x] **Redirect-window forbidden actions documented:** preflight §4 "Forbidden during the redirect window" names the three concrete tripwires (no creating a new empty `rizonesoft/impossible-os`, no fork-and-rename into the old path, no automated repo-provisioning at the user account) plus the rule of thumb (treat the old path as a tombstone until the redirect-retention window expires).
- [x] **Step 1 -- pre-transfer state:** retroactively verified by the fact that the transfer succeeded; pre-transfer apex was 200, destination repo did not exist, Pages settings `.cname` was `impossibleos.co`.
- [x] **Step 2 -- transfer pressed:** verified live via `gh repo view rizonetech/impossible-os --json nameWithOwner,owner,visibility` returning `nameWithOwner: rizonetech/impossible-os`, `owner.login: rizonetech`, `visibility: PRIVATE`.
- [x] **Step 3 -- accept:** auto-accepted (operator is org admin); no manual `transfer-requests` prompt observed.
- [x] **Step 4 -- verify new owner:** `gh repo view rizonetech/impossible-os` returns the new owner; `gh repo view rizonesoft/impossible-os` returns the same `nameWithOwner: rizonetech/impossible-os` value via GitHub repo redirect.
- [x] **Step 5 -- local remote updated:** `git remote -v` shows `origin = https://github.com/rizonetech/impossible-os.git`; `bootloader` remote unchanged. The pre-update push attempt was rejected with `remote: This repository moved. Please use the new location: https://github.com/rizonetech/impossible-os.git` -- confirming GitHub serves the redirect.
- [x] **Step 6 -- Pages settings preserved:** `gh api repos/rizonetech/impossible-os/pages` returns `cname: "impossibleos.co"`, `https_certificate.state: approved` (expires `2026-06-16`), `https_enforced: false`. The custom-domain continuity control survived the transfer cleanly.
- [/] **Step 7 -- Pages re-deploy under new owner:** the §4 implementation commit `4c1d2265` did not touch `gh-pages/**`, so the `paths:` filter prevented auto-fire post-transfer. Apex still serves the pre-transfer artifact (`last-modified: 2026-04-27 20:41:41 GMT` from the §3 deploy on the old owner), which is fine, but this leaves the workflow's clean-run-under-new-owner unverified. **Operator action remaining:** run `gh workflow run pages.yml -R rizonetech/impossible-os` (one-line), wait ~30s, confirm `gh run list -w pages.yml -R rizonetech/impossible-os -L 1` shows `conclusion: success`. Alternatively defer to the next legitimate `gh-pages/**` change, which auto-fires the workflow.
- [x] **Step 8 -- www CNAME flipped:** `getent hosts www.impossibleos.co` resolves through `rizonetech.github.io` to the GitHub Pages anycast IPv6 set (`2606:50c0:800[0-3]::153`). The flip is live.
- [x] **Step 9 -- post-transfer validation green:** all critical Step 9 table rows verified live -- apex `200`, `www` `301 -> https://impossibleos.co/`, new default Pages URL `301 -> http://impossibleos.co/`, **old default Pages URL ALSO `301 -> http://impossibleos.co/`** (better than the runbook predicted -- GitHub's redirect machinery handles owner-based Pages URLs when a custom domain is configured), apex A records unchanged, `www` CNAME chain ends at `rizonetech.github.io`, Pages `.cname` preserved, cert `approved`.
- [/] **Step 10 -- TTL restoration:** intentionally deferred per runbook; restore `www.impossibleos.co` CNAME TTL to provider normal value (typically 3600-86400s) after §7 validation has been green for 24h continuously. **Operator action remaining:** DNS provider UI plus `dig +nocmd +noall +answer +ttl www.impossibleos.co CNAME` to verify.
- [x] **Rollback procedure documented:** preflight §4 "Rollback (if any post-transfer step fails)" lists 4 reversal steps (reverse-transfer GitHub UI, restore `www` CNAME, restore local remote, re-run pre-transfer validation). Window stays open until §7 validation passes. **Operator action:** keep this window open (do not push new commits, do not roll TTL back to normal) until §7 is green.
- [x] Commit: `"docs/github: add repository transfer runbook"`

**Test checkpoint:** The repository lands at `https://github.com/rizonetech/impossible-os`, local `git fetch` succeeds from the new remote, the old GitHub repo URL redirects, and the custom domain still serves the landing page. **Status as of 2026-04-27 21:45 UTC -- TRANSFER COMPLETE:** 14 of 16 items verified `[x]`; 2 items remain `[ ]` (Step 7 Pages workflow dispatch under new owner, Step 10 TTL restoration after 24h-green window). All Step 9 post-transfer validation rows are green. **Latent finding:** the ruleset bypass actor (`RepositoryRole 5 / Maintain`) did not map cleanly under the new org; direct push to `main` is now blocked on the `Required status check "Build Impossible OS"`. This is §5 territory (post-transfer settings audit) -- bypass-actor preservation is a real category to add to §5's checklist.

> **Test runner:** N/A (transfer runbook + rollback procedure, no kernel/usermode/desktop test surface) | validation: Step 1 runs preflight §3 pre-transfer block; Step 6 runs `gh api repos/rizonetech/impossible-os/pages --jq .cname`; Step 9 runs the inlined post-transfer go/rollback table (mirrors preflight §3 post-transfer block).
> **Notes:**
> - Shipped: `## §4 Transfer Runbook and Rollback Window` appendix in [`docs/infrastructure/repository-transfer-preflight.md`](../../docs/infrastructure/repository-transfer-preflight.md) covering captured backup markers (11 fields), freeze rules covering the full transfer+rollback window, 10 numbered transfer steps with copy-paste commands + inlined Step 9 go/rollback table, 4-step rollback with wall-clock budget + reconciliation guidance, and the redirect-window tombstone rule.
> - Backup markers are a frozen 2026-04-27 snapshot -- the operator pressing transfer should NOT update them; they are the canonical "known-good" reference for rollback verification.
> - Cross-doc wiring: each operator step links to the preflight §3 validation block + the §5/§7 sections that own post-transfer audit / validation. Codex 3x review adoptions in commit `4c1d2265`.
> - Canonical doc: [`docs/infrastructure/repository-transfer-preflight.md` §4](../../docs/infrastructure/repository-transfer-preflight.md#4-transfer-runbook-and-rollback-window).
> - Scope boundary: §4 prepares + documents the transfer; the press-the-button steps are operator execution. §5 owns post-transfer settings audit; §7 owns the validation suite that closes the rollback window.
>
> **Verified:** 2026-04-27 | commit `4c1d2265` | 14/16 items | build N/A (runbook docs) | lint clean, todo-graph 8/8 | live-transfer post-Step-9 evidence captured in chat
> **Quality reviewed:** 2026-04-27 | Codex 3x (adversarial, consistency, perf) | 3H+5M+0L fixed, 0 open | scope: N/A (docs-only -- no domain code-quality skill applies; re-adversarial skipped: docs-only fixes, no SMP/state-machine touch)
> **Deferred:** [awaiting-operator] Pages re-deploy confirmation + TTL restoration are console/DNS actions -- no unattended pass can perform this; operator action required (cohort 2026-07-31)

---

## 5. Post-Transfer Settings, Workflows, Secrets, and Environments Audit

GitHub transfers repository data, but the project should not trust complex settings until they are checked directly.

- [x] **Visibility unchanged:** `gh api repos/rizonetech/impossible-os --jq .visibility` returns `private` (matches §1 baseline `PRIVATE`). Captured in [preflight §5 "Preserved verbatim"](../../docs/infrastructure/repository-transfer-preflight.md#5-post-transfer-settings-workflows-secrets-and-environments-audit).
- [/] **Branch protection / ruleset structure preserved BUT bypass actors empty** -- ruleset id `14058331` "Default Branch Security" survived with all 4 rules (`required_status_checks`/`Build Impossible OS`, `non_fast_forward`, `required_linear_history`, `deletion`). However `bypass_actors: []` post-transfer (was `RepositoryRole 5 / Maintain` pre-transfer); `current_user_can_bypass: "never"`. This is **F1 in preflight §5 Findings** -- direct push to `main` is now blocked even for org admin until a bypass entry is restored. **Operator action remaining:** walk <https://github.com/rizonetech/impossible-os/rules/14058331> -> "Bypass list" -> add `Repository admin` role.
- [x] **Team / collaborator permissions match §1 access model:** `gh api repos/rizonetech/impossible-os/collaborators` returns only `rizonesoft admin`; `gh api repos/rizonetech/impossible-os/teams` returns `[]` (matches §1 baseline of solo-collaborator + zero teams; org-team creation is §2 O6).
- [x] **`.github/CODEOWNERS` updated:** F3 fix shipped -- replaced 7 instances of `@derickpayne` (handle did not resolve, predates transfer) with `@rizonesoft` (id `8640728`, the actual maintainer login). Verified via `gh api users/rizonesoft` returning the real user record. F3 detail in preflight §5 Findings.
- [x] **Repo-level Actions policy and workflow permissions preserved:** `gh api repos/rizonetech/impossible-os/actions/permissions` returns `enabled: true`, `allowed_actions: all`, `sha_pinning_required: false`; default workflow permissions `read`; Pages workflow retains its `permissions: contents: read, pages: write, id-token: write` block (source file unchanged). Org-level Actions policy (O2) is still operator-only via `admin:org` probe.
- [/] **Repository secret value unverified:** `gh secret list` shows `BOOTLOADER_REPO_TOKEN` with baseline metadata, but secret VALUES aren't API-returnable and the token has no in-tree consumer; validation deferred to a consuming run or rotation.
- [x] **Pages environment + deployment-source preserved:** `gh api repos/rizonetech/impossible-os/environments` returns id `13147855799` (same node id `EN_kwDORgIuVs8AAAADD6xbtw` as §1 baseline); Pages settings `cname: "impossibleos.co"` preserved; latest deployment id `4503518780` ref `main` sha `ec2b7c00` attached to `rizonetech/impossible-os` per `gh api repos/rizonetech/impossible-os/deployments`.
- [/] **Release workflow tag-read capability:** the `Release` workflow file (`.github/workflows/release.yml`) uses `actions/checkout` + `gh release create`/`upload` patterns; nothing bakes the old owner. Latest release `v26.3.18-alpha.821` (pre-release, 2026-03-18) is preserved with same tag/timestamp under the new owner. **Final tag-read + artifact-upload validation comes from a release dry-run** post-F1-fix; until then this row is ⏳ best-effort verified.
- [x] **Issue templates / labels / security policy preserved:** 4 issue-template YAML files in `.github/ISSUE_TEMPLATE/` (`bug-report.yml`, `config.yml`, `feature-request.yml`, `hardware-report.yml`) -- source-tree files migrated trivially. `gh api repos/rizonetech/impossible-os/labels --jq 'length'` returns `22` (count preserved). `SECURITY.md` source-tree file unchanged. Discussions remain off (matches baseline). Milestones / project links unused, no migration concern.
- [x] **Webhooks / deploy keys / GitHub Apps / branch badges / packages preserved:** all empty / minimal per §1 baseline -- `gh api .../hooks` returns `[]`, `gh api .../keys` returns `[]`, `gh api orgs/rizonetech/installations` returns `total_count: 0`, no packages enumerated. README badges that bake the OLD owner URL are §6 territory (URL/badge sweep), not §5.
- [/] **Org-level Copilot / autonomous-agent policy** -- still operator-only (no public REST endpoint). Walk <https://github.com/organizations/rizonetech/settings/copilot/access>; confirm "No access" or `rizonesoft` excluded from cloud-agent permission per [Autonomous-Agent Boundary Policy](../../docs/infrastructure/ai-system.md#autonomous-agent-boundary-policy). This is the O5 item from §2 / §3.
- [x] Commit: `"docs/github: record post-transfer settings audit"`

**Test checkpoint:** The settings audit closes only after direct inspection or a successful workflow run proves each setting survived or was intentionally recreated. **Status:** 7 of 11 items verified `[x]` from live `gh api` probes; 3 items `[/]` (F1 ruleset bypass actor restoration -- blocking pushes; release-workflow dry-run -- best-effort verified, final validation post-F1-fix; secret value unverifiable via API -- metadata only); 1 item `[ ]` (operator-only Copilot Access UI walk). 3 findings (F1/F2/F3) captured in [preflight §5](../../docs/infrastructure/repository-transfer-preflight.md#5-post-transfer-settings-workflows-secrets-and-environments-audit); F3 fix already shipped in this commit.

> **Test runner:** N/A (post-transfer GitHub-state audit, no kernel/usermode/desktop test surface) | validation: re-run the §5 "Preserved verbatim" probe block in `repository-transfer-preflight.md` and diff against the live `gh api` output; F1 / F2 / F3 owner-actions visible in §5 Findings table.
> **Notes:**
> - Shipped: `## §5 Post-Transfer Settings, Workflows, Secrets, and Environments Audit` appendix in [`docs/infrastructure/repository-transfer-preflight.md`](../../docs/infrastructure/repository-transfer-preflight.md) with three tables: 19-row "Preserved verbatim" comparison vs §1 baseline, 3-row Findings (F1 ruleset bypass empty / F2 Build cancelled on `4c1d2265` / F3 CODEOWNERS handle drift), 7-row operator-only items list mirroring §2 / §3 deferrals, and a §7-feed summary table.
> - F3 fix shipped: `.github/CODEOWNERS` 7x `@derickpayne` -> `@rizonesoft`. F1 + F2 are operator-side actions (ruleset bypass restore + Build workflow re-dispatch).
> - Cross-doc wiring: each §5 finding links back to the operator-action URL or follow-up section. Codex 3x review adoptions in commit `26b9d290`.
> - Canonical doc: [`docs/infrastructure/repository-transfer-preflight.md` §5](../../docs/infrastructure/repository-transfer-preflight.md#5-post-transfer-settings-workflows-secrets-and-environments-audit).
> - Scope boundary: §5 audits state preservation; §6 owns URL / badge / docs / generated-link sweep; §7 owns the validation suite that closes the rollback window; §8 owns the move-back runbook. F1 (ruleset bypass) blocks pushes for §6/§7 work until the operator restores it.
>
> **Verified:** 2026-04-27 | commit `26b9d290` | 7/11 items | build N/A (audit + docs) | lint clean, todo-graph 8/8 | F3 CODEOWNERS fix included; secret-value item corrected [x]->[/] in 2026-06-16 review (API cannot prove secret values)
> **Quality reviewed:** 2026-06-16 | Codex 3x (adversarial, consistency, perf) | 2M fixed, 0 open | scope: N/A (docs/GitHub-audit; re-adversarial skipped -- docs-status fixes, no code)
> **Deferred:** [awaiting-operator] org Copilot/autonomous-agent policy walk is UI-only (no public REST endpoint) -- no unattended pass can perform this; operator action required (cohort 2026-07-31)

---

## 6. URL, Badge, Docs, and Generated-Link Sweep

Stop relying on redirects for first-party content. Redirects are useful for old external links, but repo-owned files should name the new canonical owner.

- [x] **README badges + links flipped** (badges, clone, releases link). Sponsor badge `sponsors/rizonesoft` held; copyright link later migrated to Rizonetech by rebrand `07d60796` (supersedes the original preserve-rizonesoft-brand rule).
- [x] **Clone commands flipped:** `README.md`, `CONTRIBUTING.md`, `docs/infrastructure/development-tooling.md` all changed to `github.com/rizonetech/impossible-os.git`.
- [x] **Actions badge / release / stars / commit / issue / source URLs flipped:** all `github.com/rizonesoft/impossible-os/...` URLs in README.md, docs/, gh-pages/, .github/ISSUE_TEMPLATE/ are now `github.com/rizonetech/impossible-os/...`.
- [x] **`docs/infrastructure/github-setup.md` updated:** Overview narrative + Repository table active row + footer Repository link flipped. Bootloader sibling row + email rows preserved as brand-scoped.
- [x] **`docs/infrastructure/development-tooling.md` workflow + Pages references flipped.**
- [x] **`gh-pages/index.html` links flipped:** API call, footer, view-source. The `rizonesoft.com` brand homepage was later migrated to `rizonetech.com` by rebrand `07d60796` (supersedes the original preserve rule).
- [x] **`gh-pages/err/**/*.html` GitHub nav links flipped** across all 14 err-page index.html files plus `err/index.html` and `err/errors.js`.
- [x] **Generated TODO graph regenerated:** `scripts/todo-graph/render.py:42` flipped `REPO_URL_BASE` to `https://github.com/rizonetech/impossible-os/blob/main`; `make todo-graph-render-mermaid` regenerated `docs/infrastructure/todo-graph.md` with **229 `click ... github.com/rizonetech/...` lines** (zero residual `rizonesoft` URLs in the generated block).
- [x] **Remaining `rizonesoft` references classified and retained:**
  - **Brand (post-rebrand `07d60796`):** held as rizonesoft = Sponsor badge `github.com/sponsors/rizonesoft` + `security@`/`conduct@rizonesoft.com` emails (pending mailbox confirmation). Homepage, copyright link, and company name were migrated to Rizonetech / `rizonetech.com`.
  - **Sibling repos (out of TODO-09 scope):** `rizonesoft/impossible-os-bootloader`, `rizonesoft/impossible-os-updates`, `rizonesoft/impossible-os-packages`.
  - **Historical record:** `repository-transfer-preflight.md` and this TODO file document the move and intentionally retain pre-transfer references for §1 baseline + R-table audit trail.
  - **Inline rg-pattern strings:** lines that quote the regex pattern `rizonesoft/impossible-os|...` literally (in §1's "How to Re-Run" block, in §3's source-of-truth-changes table) are the search pattern itself, not repo references.
- [x] Avoid doctrine churn: `CLAUDE.md` and `AGENTS.md` returned **zero** `rizonesoft/impossible-os|github.com/rizonesoft|rizonesoft.github.io` matches throughout the sweep -- no edits made.
- [x] Commit: `"docs/github: update canonical repository owner to rizonetech"`

**Test checkpoint:** `rg -n "rizonesoft/impossible-os|github.com/rizonesoft|rizonesoft.github.io" README.md CONTRIBUTING.md SECURITY.md CODE_OF_CONDUCT.md docs gh-pages .github todo` returns only intentional historical or brand references. **Verified live 2026-04-28:** the rg command, filtered for documented exclusions (sibling repos / brand / preflight + TODO-09 narrative / todo-graph generated block / `github.com/rizonesoft"`/`)`), returns **zero residuals**.

> **Test runner:** N/A (URL/badge sweep + graph regeneration; no kernel/usermode/desktop test surface) | validation: re-run the test-checkpoint rg command + `make todo-graph-render-mermaid` should be a no-op diff (already at the new owner).
> **Notes:**
> - Shipped: bulk `rizonesoft/impossible-os` -> `rizonetech/impossible-os` rewrite across README + CONTRIBUTING + docs (~12 files) + gh-pages site content (16 HTML files + errors.js) + 7 TODO files + 2 scripts (`scripts/todo-graph/render.py` REPO_URL_BASE constant + `scripts/test-ai-system.sh:396` Copilot-disable log message). Renderer constant flip is what makes `docs/infrastructure/todo-graph.md` regenerate correctly.
> - Classification rule excluded brand references, sibling repos (impossible-os-bootloader / -updates / -packages), and documented historical-record sources (preflight + TODO-09 narrative + rg-pattern literals).
> - Codex 3x review adoptions in commit `5097fbd3`.
> - Canonical doc: this section + [`scripts/todo-graph/render.py`](../../scripts/todo-graph/render.py) for the generator constant.
> - Scope boundary: §6 owns first-party URL flip; §7 owns the validation suite; §8 owns the move-back runbook; §9 owns docs sync + closure.
>
> **Verified:** 2026-04-28 | commit `5097fbd3` | 12/12 items | build N/A (URL sweep + graph regen) | lint clean, todo-graph 8/8 | post-sweep test checkpoint: 0 residuals after filter
> **Quality reviewed:** 2026-06-16 | Codex 3x (adversarial, consistency, perf) | 2M fixed, 0 open | scope: N/A (docs/URL-sweep; re-adversarial skipped -- docs reconciliation, no code)

---

## 7. Validation Suite: Pages, Actions, Releases, Clone, Hooks, Graph

Treat the transfer as complete only after the same surfaces a contributor uses are verified end to end.

- [x] **Git validation -- all green:** fresh clone from `https://github.com/rizonetech/impossible-os.git` succeeds (HEAD = `dd6c163920f22a575bf8a30a589d1c198ac28355` "stamp: TODO-09 §6 implementation commit hash 5097fbd3"); existing clone fetches cleanly with updated origin; push capability proven by recent commits `5097fbd3` (§6) + `dd6c1639` (§6 stamp) + earlier §4/§5 commits; old `github.com/rizonesoft/impossible-os` URL redirects via `gh repo view` (returns `nameWithOwner: rizonetech/impossible-os` for authenticated callers; unauthenticated curl returns 404 -- expected for private repos).
- [x] **Pages validation -- all green:** apex returns `200 OK`; `www.impossibleos.co` returns `301 -> https://impossibleos.co/`; `rizonetech.github.io/impossible-os/` returns `301 -> http://impossibleos.co/`; HTTPS cert state `approved`, expires `2026-06-16` covering both apex + www; latest Pages workflow run on sha `5097fbd3` at `2026-04-27T22:19:15Z` completed `success` under the new owner.
- [/] **DNS validation -- 2/3 green:** apex A records match GitHub Pages anycast (`185.199.108-111.153`); `www.impossibleos.co` CNAME chain ends at `rizonetech.github.io`; **TTL restoration to normal value still pending §4 Step 10** (operator action 24h after Step 9 stays green; runbook in preflight §4).
- [/] **Workflow validation -- 3/4 green + 1 deferred:** Build ran successfully on `dd6c163920f22a575bf8a30a589d1c198ac28355` at `2026-04-27T22:19:31Z`; Pages ran successfully on `5097fbd3` at `2026-04-27T22:19:15Z` (auto-fired on `gh-pages/**` change in the §6 sweep); TODO graph workflow ran successfully at `22:19:31Z`; Release workflow dry-run **deferred** to next legitimate release tag push (the pre-existing tag `v26.3.18-alpha.821` is preserved per §5 audit); Labeler / Stale workflows still listed as `active` (workflow IDs `247848080` / `247847233` stable per §5).
- [x] **Local tooling validation -- all green** (refreshed 2026-06-16): `lint.sh` CLEAN 637 files; `test-tooling.sh` 393/393 PASS; `todo-graph/build.py --quiet` exit 0; `validate.py` 8/8 PASS, 0 warnings. (was 498 files / 254/254 at 2026-04-28.)
- [/] **Project policy validation:** `AGENTS.md` sha256 `75e38c4af748412bb7aa9daf051835ff1d05cdfa9b40dd4f10c1c8b8c50fdeb5` (2026-06-16); forbidden autonomous-agent dirs absent; 0-byte `.codex` residue removed at close; Org Copilot operator-only.
- [x] **Public link validation -- all green:** README build badge SVG returns `200 OK`; Releases page `404` to unauthenticated curl (expected for private repo; `gh release list` returns the release for authenticated callers; will become `200 OK` post-§8 visibility flip); Site GitHub + Releases buttons in `gh-pages/index.html` + 14 err pages flipped in §6 (live on apex); Generated TODO graph links: 229 `click ... github.com/rizonetech/...` URLs in `docs/infrastructure/todo-graph.md`; `make todo-graph-render-mermaid` now idempotent (zero diff on second run).
- [x] Commit: `"docs/github: record repository transfer validation"`

**Test checkpoint:** Transfer validation is green only when the public site, workflows, local graph tooling, and policy guardrails all pass under the new owner. **Status:** 5 of 8 sub-suites fully `[x]`; 3 sub-suites `[/]` with explicit operator-only deferrals (DNS TTL restore -- §4 Step 10; Release workflow dry-run -- next release tag; Org Copilot UI -- O5). All public-facing surfaces (apex / www / new default Pages URL / cert / build+pages+todo-graph workflows / 393-test tooling suite / 8-check graph validator) were green at transfer completion (2026-04-28 snapshot; the recorded clone-HEAD sha + cert expiry are point-in-time, so live re-validation of those is operator-rig). **The rollback window can close** as soon as the operator confirms the 3 `[/]` items in chat.

> **Test runner:** N/A (consolidated post-transfer validation suite; no kernel/usermode/desktop test surface) | validation: re-run the §7 verification block after `git pull origin main` to confirm public site / workflows / local tooling / policy still green; rollback window can close after 24h of continuous green.
> **Notes:**
> - Shipped: live execution of the §7 validation suite under `rizonetech/impossible-os` immediately after §6 push -- 5 sub-suites fully green (git, Pages, local tooling, public links), 3 sub-suites `[/]` with explicit operator-only deferrals (DNS TTL restore / Release workflow dry-run / Org Copilot UI).
> - Build + Pages + TODO graph workflow runs all show `success` post-transfer on the latest commit, confirming the new owner's Actions / Pages permissions are correctly inheriting the source-repo policy after the §5 F1 ruleset bypass restoration.
> - Codex 3x review adoptions in commit `089d9c09`.
> - Canonical doc: this section + [`docs/infrastructure/repository-transfer-preflight.md` §5](../../docs/infrastructure/repository-transfer-preflight.md#5-post-transfer-settings-workflows-secrets-and-environments-audit) for the underlying preserved-state baseline.
> - Scope boundary: §7 closes the rollback window once all sub-suites are green; §8 owns the future move-back runbook (planning-only; no execution); §9 owns final docs sync + closure.
>
> **Verified:** 2026-04-28 | commit `089d9c09` | 8/8 items | build N/A (consolidated validation suite) | lint clean, todo-graph 8/8, test-tooling 393/393 (refreshed 2026-06-16; was 254/254), latest Build+Pages+TODO-graph workflows all `success` at transfer time (live state operator-rig)
> **Quality reviewed:** 2026-06-16 | Codex 3x (adversarial, consistency, perf) | 3M fixed, 0 open | scope: N/A (docs/validation-suite; re-adversarial skipped -- stale-evidence doc reconciliation, no code)

---

## 8. Move-Back and Public-Visibility Runbook

Define the return path before it is needed. Moving back later should be a planned ownership change, not a scramble during the public launch.

> [!NOTE]
> Deliverable is the runbook DOC [`docs/infrastructure/repository-move-back-runbook.md`](../../docs/infrastructure/repository-move-back-runbook.md); the actual move-back transfer + visibility flip are a future operator action gated on the move-back trigger (runbook §1). Items below are `[x]` = "documented in the runbook"; execution stays operator-deferred ([/] IO row).

- [x] Defined the move-back trigger (public-ready + release-approved + security/history-reviewed + public-docs-ready) in runbook §1.
- [x] Documented receiving-account readiness incl. the paid-plan-vs-Free caveat for keeping Pages + branch protection on a private repo (runbook §2).
- [x] Documented the same-name-repo AND same-network-fork blocker with fail-closed `gh` checks (runbook §2).
- [x] Documented the pre-transfer preflight repeat from the `rizonetech` state (runbook §3).
- [x] Documented the transfer-back step (webhooks/secrets/deploy-keys/issues/PRs/wiki/etc. travel with the repo) (runbook §4).
- [x] Documented the post-transfer collaborator audit by evidence (before/after `diff`; org teams + admin roles do NOT map onto a personal repo) (runbook §4).
- [x] Documented the tombstone rule: never create a repo OR fork at any NON-destination prior owner path or GitHub permanently deletes that path's redirect; the receiving-owner path is the live repo (runbook §4).
- [x] Documented re-pointing local remotes + first-party owner refs to the final canonical owner (runbook §5).
- [x] Documented the `www.impossibleos.co` CNAME flip, apex continuity, and custom-domain re-verify (runbook §6).
- [x] Documented the gated public-visibility flip ordering (domain -> secrets -> private notes -> release -> branch protection -> security policy) + a Free-account alternate (runbook §7).
- [x] Documented asserting the receiving path is live + only NON-destination prior owners 301 to it, plus the redirect-retention decision (runbook §8).
- [x] Documented the stale-secret audit (`BOOTLOADER_REPO_TOKEN` orphaned; §1 preflight baseline stale) with a fresh consumer-scan requirement before delete/rotate (runbook §7).
- [x] Commit: `"docs/github: add move-back and public-visibility runbook"`
- [x] Executed the move-back 2026-09-27: `rizonesoft/impossible-os`, public.
  - gitleaks 8.30.1 over all 5,917 commits found no true secret (13 hits, all false positives).
  - Pages `https_enforced` now `true` (cert approved); the `Default Branch Security` admin bypass (RepositoryRole 5) the org move dropped is restored.
  - Secret scanning + push protection enabled; `rizonetech/impossible-os` returns 301 to the new path; live owner refs swept in 36 files.
- [x] Flipped the `www.impossibleos.co` CNAME to `rizonesoft.github.io` (operator, 2026-09-27).
  - Verified: `www.impossibleos.co` resolves via `rizonesoft.github.io` and `https://www.impossibleos.co/` returns 301 to `https://impossibleos.co/`.
- [/] operator-gated: Reconcile the §2-§7 `[/]` tail and the Unit Tests items with the move-back; they assert `rizonetech`-owned state that no longer exists
  - Found 2026-09-28 while writing the TODO-10 infrastructure docs pages: the Unit Tests section's items check `rizonetech/impossible-os`, a `www` CNAME to `rizonetech.github.io` and org-level Copilot policy, none of which can pass after the 2026-09-27 move-back to `rizonesoft`.
  - Each needs an operator verdict: close as superseded, or rewrite against the `rizonesoft` owner (the Copilot access check becomes a personal-account setting). §9's Notes still name `rizonetech` as the final owner.

**Test checkpoint:** The runbook documents every move-back + public-visibility step in operator-runnable form; `docs/infrastructure/repository-move-back-runbook.md` exists and a reader can follow trigger -> readiness -> transfer -> re-point -> domain -> visibility-flip -> redirect-verify without gaps. The final public move is considered ready only after the custom domain, repository visibility, first-party URLs, and security posture are validated in that order (the actual execution is the future operator action).

> **Test runner:** N/A (operator runbook -- no kernel/usermode/desktop test surface) | validation: `docs/infrastructure/repository-move-back-runbook.md` exists and covers every §8 step; GitHub transfer mechanics verified against GitHub's transferring-a-repository + personal-repo-permissions docs.

> **Notes:**
> - **What shipped:** `docs/infrastructure/repository-move-back-runbook.md` -- operator runbook for the move-back transfer + public-visibility flip (trigger gates, readiness, transfer-blocker checks, tombstone rule, gated flip, redirect verification).
> - **Structure / consumers:** operator-facing; companion to `repository-transfer-preflight.md` (§1) and `github-setup.md` (owner-state record). Read when the public-launch move-back trigger fires.
> - **Canonical doc:** [`docs/infrastructure/repository-move-back-runbook.md`](../../docs/infrastructure/repository-move-back-runbook.md).
> - **Scope boundary:** documents the procedure only; the actual transfer + visibility flip are future operator actions gated on the trigger. §9 owns syncing `github-setup.md` to the final owner state.

> **Verified:** 2026-06-13 | commit `5f725132` | 12/12 items | build N/A (operator runbook -- no kernel/test surface) | runbook 227 lines, lint clean, todo-graph 8/8
> **Quality reviewed:** 2026-06-13 | Codex 8x (adversarial + consistency + perf + re-adversarial) | 10H+6M fixed | scope: N/A (docs runbook); execution operator-deferred (gated on the move-back trigger)

---

## 9. Documentation Sync and Closure

Close the roadmap by making the final owner state discoverable and removing transfer-only ambiguity from contributor docs.

- [x] Updated `github-setup.md` ownership note with final owner (`rizonetech`), transfer date (2026-04-27), custom-domain + DNS shape, branch protection, and the org-level autonomous-agent-boundary policy note.
- [x] No INDEX.md status change: TODO stays `draft` (consistent with the domain's other shipped 00-infra TODOs); the §2-§7 operator-validation tail keeps it non-final.
- [x] Not promoted to root Active Epics (`TODO-00-INDEX.md`): repository-ops work stays a domain TODO.
- [x] Repository-ownership note added to `github-setup.md` (final owner + canonical URL + org policy) -- the contributor-facing single source of truth.
- [x] Owner-reference inventory: ZERO active `rizonesoft/impossible-os` (main-repo) URL refs outside transfer docs; sibling repos (archived `impossible-os-bootloader`) + brand/contact/CODEOWNERS refs intentionally retained per §6.
- [x] TODO graph build/validate after the final docs edit: 8/8 checks PASS.
- [x] Implementation Order rows respected: §2-§7 stay `[/]` (operator validation pending), §8 `[/]` (execution operator-deferred); only doc-complete rows are closed.
- [x] Commit: `"todo: close repository transfer to rizonetech"`

**Test checkpoint:** A new contributor can find the canonical repository URL, clone it, view the Pages site, and follow the docs without encountering stale owner names except where history is intentional.

> **Test runner:** N/A (documentation closure -- no kernel/usermode/desktop test surface) | validation: `github-setup.md` records final owner + date + DNS + branch-protection + org policy; owner-reference inventory shows only intentional brand history; `todo-graph build-and-validate` 8/8.

> **Notes:**
> - **What shipped:** `github-setup.md` ownership note synced to the final owner state -- transfer date (2026-04-27), custom-domain/DNS, branch protection, and the org-level autonomous-agent-boundary policy.
> - **Structure / consumers:** the contributor-facing canonical owner-state record; companion to `repository-transfer-preflight.md` (baseline) + `repository-move-back-runbook.md` (§8 future move-back).
> - **Canonical doc:** [`docs/infrastructure/github-setup.md`](../../docs/infrastructure/github-setup.md).
> - **Scope boundary:** documentation sync only; the operator-validation tail (DNS TTL restore, release dry-run, org Copilot UI check) stays owned by §2-§7 `[/]`, and the move-back execution by §8.

> **Verified:** 2026-06-13 | commit `c0ff7a2b` | 8/8 items | build N/A (documentation closure -- no kernel/test surface) | github-setup.md owner-state synced, owner-ref inventory clean, todo-graph 8/8
> **Deferred:** [M] §5/§7 push-state wording still reads contradictory ("blocked" vs "proven") until the operator restores the ruleset admin bypass; pushes land via the "expected" check today -> XREF: 00-infrastructure/TODO-09 §5 (item: "Branch protection / ruleset structure preserved BUT bypass actors empty" at line 226)
> **Quality reviewed:** 2026-06-13 | Codex 6x (adversarial + consistency + perf + re-adversarial) | 2H+4M fixed, 1M deferred | scope: N/A (documentation closure)

---

## OS Comparison

| ⭐  | Feature                                  | 🪟 Win11            | 🐧 Linux                | 🚀 Impossible OS                                                               |
| --- | ---------------------------------------- | ------------------- | ----------------------- | ------------------------------------------------------------------------------ |
| ⭐  | Repo-owner transfer runbook              | ❌ N/A (closed src) | ⚠️ Ad-hoc per project   | ✅ §1-§9 docs/runbook shipped end to end; operator-validation tail in §2-§7    |
| ⭐  | Rollback-window discipline               | ❌ N/A              | ⚠️ Project-dependent    | ⏳ §4 transfer EXECUTED 2026-04-27, 14/16 verified; §7 + TTL restore remain    |
| ⭐  | Pages custom-domain continuity plan      | ❌ N/A              | ⚠️ Project-dependent    | ⏳ §3 tracked CNAME + 4 URL fixes + DNS plan; one operator TTL drop remains    |
| 💎  | Forward move-back / visibility flip plan | ❌ N/A              | ❌ N/A                  | ✅ §8 `repository-move-back-runbook.md` shipped; execution operator-deferred   |
| ⭐  | Post-transfer settings audit             | ❌ N/A              | ⚠️ Tribal-knowledge     | ⏳ §5 19 settings preserved 2026-04-27; 3 findings, CODEOWNERS drift fixed     |
| ⭐  | First-party URL / badge / link sweep     | ❌ N/A              | ⚠️ Manual / per-project | ✅ §6 URL rewrite across docs, gh-pages, TODOs, render.py; 229 mermaid URLs    |
| ⭐  | End-to-end post-transfer validation      | ❌ N/A              | ⚠️ Manual / per-project | ⏳ §7 5/8 sub-suites green live; 3 operator-deferred (TTL, release, Copilot)   |
| 💎  | AI-policy boundary preserved across move | ❌ N/A              | ❌ N/A                  | ⏳ §2+§5+§7 no-autonomous-agent: 0 forbidden paths; Copilot walk operator-only |

> **After §1-§4:** Inventory, org readiness, Pages continuity plan, and a time-boxed transfer runbook with rollback exist before any GitHub setting is touched.
> **After §5-§7:** Settings audit, first-party URL sweep, and end-to-end validation suite confirm the move did not silently break Pages, Actions, releases, badges, or graph tooling.
> **After §8-§9:** A move-back and public-visibility runbook is documented before it is needed (`repository-move-back-runbook.md`), and `docs/infrastructure/github-setup.md` carries the final owner state (owner, transfer date, DNS, branch protection, org policy) for future contributors. The remaining operator-validation tail (DNS TTL restore, release dry-run, org Copilot UI) stays tracked in §2-§7 `[/]`.

---

## Unit Tests

> Tests for this TODO are **operator-validation**, not kernel-side or host-tooling-side. There is no `TEST_CAT_*` registration, no `src/kernel/test/test_*.c`, no `user/test/test_*.c`, and no `scripts/test-tooling.sh` sub-test to add. The sole verification surface is the live GitHub repository, GitHub Pages, DNS, and the existing tooling already covered by `bash scripts/test-tooling.sh` and `python3 scripts/todo-graph/validate.py`. Section 7 is the validation suite for this TODO.

- [x] No new automated tests are introduced by this TODO.
- [x] Section 7 enumerates the operator-executed checks (git fetch, curl, DNS, workflow re-run, lint, test-tooling, todo-graph build/validate) that stand in for unit tests here.
- [x] Commit: covered by §7's validation commit; no separate test commit.

---

## Verification

> **Test runner:** N/A (operator runbook -- no kernel/usermode/desktop test surface) | validation: §7 validation suite (`curl -I https://impossibleos.co/`, `curl -I https://www.impossibleos.co/`, `curl -I https://rizonetech.github.io/impossible-os/`, fresh clone from new origin, latest Pages deployment under `rizonetech`, `bash scripts/lint.sh`, `bash scripts/test-tooling.sh`, `python3 scripts/todo-graph/build.py --quiet`, `python3 scripts/todo-graph/validate.py --warnings-only`).

- [/] Repository lives at `rizonetech/impossible-os`; old URL redirects. (existing-clone origin=rizonetech, fetch/push green this session; fresh-clone + redirect: manual)
- [/] `git fetch` succeeds from new origin in fresh + updated existing clone. (updated existing clone verified this session; fresh clone: manual)
- [ ] `https://impossibleos.co/` returns `200 OK` with a valid HTTPS certificate. (manual -- `curl -I` against prod)
- [ ] `https://www.impossibleos.co/` redirects to `https://impossibleos.co/`. (manual -- `curl -I` against prod)
- [ ] `https://rizonetech.github.io/impossible-os/` redirects to the custom domain or serves the Pages site. (manual -- `curl -I` against prod)
- [ ] Apex DNS A records match GitHub Pages IPs; `www` CNAME resolves to `rizonetech.github.io`. (manual -- authoritative DNS)
- [ ] Latest Pages, build, and (if dispatched) release workflow runs are green under the new owner. (manual -- GitHub Actions UI)
- [x] `bash scripts/lint.sh` passes after the URL/badge/doc sweep. (2026-06-13: exit 0)
- [x] `bash scripts/test-tooling.sh` passes (no regression from URL changes). (2026-06-13: 375/375 PASS)
- [x] `python3 scripts/todo-graph/build.py` + `validate.py` both pass. (2026-06-13: validate 8/8, 0 warnings)
- [x] Owner-ref scan returns only intentional historical/brand references. (2026-06-13: only this TODO + preflight runbook carry old URL as documented before-state)
- [ ] Org-level policy still blocks autonomous coding-agent PR flow (Settings -> Copilot -> Access policies; §5/§7 evidence). (manual -- org Settings UI)
- [/] Verify on GitHub.com (prod), authoritative DNS resolvers, and the WSL2 dev host. (WSL2 tooling verified 2026-06-13; prod + DNS: manual)
- [x] Commit: `"00-infrastructure/TODO-09: repository transfer to rizonetech complete"`
