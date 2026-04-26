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
> **Current state:** Local remotes point at `https://github.com/rizonesoft/impossible-os.git` and `https://github.com/rizonesoft/impossible-os-bootloader.git`. GitHub Pages is deployed by [`.github/workflows/pages.yml`](../../.github/workflows/pages.yml) from the tracked [`gh-pages/`](../../gh-pages/) folder. The custom domain `https://impossibleos.co/` is live and returns `200 OK` from GitHub Pages. The default Pages URL `https://rizonesoft.github.io/impossible-os/` redirects to the custom domain. DNS for the apex `impossibleos.co` points at GitHub Pages A records, while `www.impossibleos.co` currently CNAMEs to `rizonesoft.github.io`. The landing page still has canonical, Open Graph, JSON-LD, and visitor-counter references to `rizonesoft.github.io/impossible-os/`; the error pages already use `https://impossibleos.co/...`.

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

| Star | Order | Section | Deliverable                                                       | Depends On | Status |
| --- | :---: | :-----: | ----------------------------------------------------------------- | ---------- | :----: |
| Star |   1   |    1    | Preflight inventory and risk register                             | --         |  [ ]   |
| Star |   2   |    2    | `rizonetech` organization readiness and policy parity             | 1          |  [ ]   |
| Star |   3   |    3    | GitHub Pages custom-domain continuity plan                        | 1, 2       |  [ ]   |
| Star |   4   |    4    | Repository transfer runbook and rollback window                   | 1-3        |  [ ]   |
| Star |   5   |    5    | Post-transfer settings, workflows, secrets, environments audit     | 4          |  [ ]   |
| Star |   6   |    6    | URL, badge, docs, and generated-link sweep                        | 4, 5       |  [ ]   |
| Star |   7   |    7    | Validation suite: Pages, Actions, releases, clone, hooks, graph    | 5, 6       |  [ ]   |
| Star |   8   |    8    | Move-back and public-visibility runbook                           | 1-7        |  [ ]   |
| Star |   9   |    9    | Documentation sync and closure                                    | 1-8        |  [ ]   |

> **Order vs section number:** The file is ordered by execution flow. Section 8 is intentionally specified before the first transfer happens so the team can confirm the move is reversible rather than discovering account or Pages constraints after the repository is already in the organization.

---

## 1. Preflight Inventory and Risk Register

Capture the exact current state before changing any GitHub owner setting. The output is the baseline used to prove that the move did not break Pages, CI, release automation, or project policy.

- [ ] Record current repository identity: owner, visibility, default branch, repository ID if available, remote URLs, archived/fork/template flags, topics, description, homepage URL, Discussions state, Issues state, Projects state, Wiki state, Packages state, and security feature toggles.
- [ ] Record current Pages state: source mode, deployment workflow, environment name, custom domain, HTTPS enforcement, last successful deployment SHA, public URL, and whether the repository currently has a `CNAME` file or stores the domain only in Pages settings.
- [ ] Record live domain baseline:
  - `https://impossibleos.co/` returns `200 OK` from GitHub Pages.
  - `https://www.impossibleos.co/` redirects to `https://impossibleos.co/`.
  - `https://rizonesoft.github.io/impossible-os/` redirects to the custom domain.
  - `impossibleos.co` apex DNS has GitHub Pages A records.
  - `www.impossibleos.co` CNAME currently targets `rizonesoft.github.io`.
- [ ] Record current GitHub Actions state: enabled workflows, default token permissions, Actions policy, required workflow approvals, runner groups, caches, artifacts, retention, workflow concurrency, and whether any workflow references the old owner explicitly.
- [ ] Record repository settings that do not reliably travel in a way operators can trust blindly: branch protection, rulesets, environments, environment reviewers, deployment branches, repository secrets, repository variables, Dependabot, code scanning, secret scanning, vulnerability alerts, Pages environment, webhooks, deploy keys, GitHub Apps, and integration installations.
- [ ] Record current access model: collaborators, teams, outside collaborators, CODEOWNERS handles, protected branch bypass users, release managers, and who can administer Pages/DNS.
- [ ] Record all hard-coded owner references with a focused search: `rizonesoft/impossible-os`, `github.com/rizonesoft`, `rizonesoft.github.io`, `hits.sh/rizonesoft.github.io`, badge URLs, generated graph links, clone commands, and release URLs.
- [ ] Record remote consumers that may need notification or manual updates: local clones, CI mirrors, package/release download pages, social/profile links, external docs, and any pinned GitHub App installation.
- [ ] Create a risk register with severity and mitigation for at least: Pages custom-domain detachment, DNS propagation delay, Pages certificate re-provisioning delay, lost repository secrets, broken branch protection, disabled Actions under org policy, lost team access, broken badges, stale GitHub graph links, and redirect shadowing by accidentally recreating the old repository path.
- [ ] Commit: `"todo: document repository-transfer preflight inventory"`

**Test checkpoint:** The preflight document or notes list every setting needed to recreate the repository in `rizonetech`. The current public site and default Pages redirect are checked immediately before transfer, not from stale memory.

---

## 2. `rizonetech` Organization Readiness and Policy Parity

Prepare the receiving organization before transfer so the repository lands into a compatible policy envelope.

- [ ] Confirm the `rizonetech` organization exists, has the intended owners, and the human operator has permission to receive transferred repositories.
- [ ] Confirm the org plan supports the repository's current visibility plus GitHub Pages requirements. If the repo remains private, verify that Pages for private repositories is available under the active plan before transfer.
- [ ] Verify `impossibleos.co` in the receiving organization if GitHub requires account-level domain verification for custom Pages domains.
- [ ] Configure organization Actions policy so existing workflows can run: GitHub-hosted runners allowed, required marketplace actions allowed (`actions/checkout`, `actions/configure-pages`, `actions/upload-pages-artifact`, `actions/deploy-pages`, and any release/build workflow actions), and workflow permissions compatible with Pages deploy.
- [ ] Confirm org-level Pages policy allows repository Pages and custom domains.
- [ ] Confirm org-level security settings are compatible with repository settings: Dependabot, secret scanning, code scanning, private vulnerability reporting, branch protection/rulesets, and allowed merge methods.
- [ ] Preserve the AI-system boundary at org level: disable Copilot cloud coding-agent / autonomous-agent PR enablement for this repository, or document the exact UI setting that keeps it disabled after transfer.
- [ ] Create or identify teams that replace personal/collaborator permissions: maintainers, reviewers, release operators, security contacts, and Pages/DNS administrators.
- [ ] Confirm no existing `rizonetech/impossible-os` repository blocks the transfer. If one exists, stop and decide whether to rename/archive/delete it before proceeding.
- [ ] Commit: `"docs/github: capture rizonetech organization readiness"`

**Test checkpoint:** The receiving organization is ready before the transfer button is pressed. No required setting depends on "we will fix it after it lands" except values that GitHub only exposes after transfer.

---

## 3. GitHub Pages Custom-Domain Continuity Plan

Make the custom domain the stable user-facing surface. The default owner-based Pages URL is allowed to change; `https://impossibleos.co/` must not.

- [ ] Decide the canonical domain policy: apex `https://impossibleos.co/` remains canonical; `https://www.impossibleos.co/` redirects to apex; owner-based GitHub Pages URLs are non-canonical.
- [ ] Add a tracked [`gh-pages/CNAME`](../../gh-pages/CNAME) file containing exactly `impossibleos.co` if GitHub Actions Pages honors it in this repo's deployment mode. If Actions Pages ignores the file for this setup, document that the custom domain is settings-owned and keep the file out.
- [ ] Update [`gh-pages/index.html`](../../gh-pages/index.html) canonical, Open Graph URL, JSON-LD URL, and visitor-counter URL away from `rizonesoft.github.io/impossible-os/` to `https://impossibleos.co/`.
- [ ] Audit [`gh-pages/err/`](../../gh-pages/err/) for consistent canonical URLs and relative links.
- [ ] Reduce DNS TTL for `www.impossibleos.co` before transfer if the DNS provider allows it, so the CNAME switch to `rizonetech.github.io` propagates quickly.
- [ ] Keep apex A records pointed at the four GitHub Pages IPv4 addresses unless GitHub changes the documented Pages IP set.
- [ ] Decide whether to add GitHub Pages AAAA records for IPv6 parity. If added, record them in the DNS baseline and validation checklist.
- [ ] Prepare the `www` DNS change but do not apply it until `rizonetech/impossible-os` Pages settings accept `impossibleos.co` and the new default Pages host is live.
- [ ] Prepare validation commands:
  - `curl -I https://impossibleos.co/`
  - `curl -I https://www.impossibleos.co/`
  - `curl -I https://rizonetech.github.io/impossible-os/`
  - DNS lookup for `impossibleos.co A`
  - DNS lookup for `www.impossibleos.co CNAME`
- [ ] Document acceptable transient states: GitHub Pages certificate provisioning may show a temporary HTTPS warning; default `github.io` URLs may redirect differently; DNS may take one TTL to converge.
- [ ] Commit: `"pages: prepare custom-domain continuity for repository transfer"`

**Test checkpoint:** Before the repository moves, `https://impossibleos.co/` is already the canonical link surface in the site content and the DNS change needed after transfer is known exactly.

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
