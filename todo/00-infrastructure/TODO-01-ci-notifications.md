# TODO-04 — CI Notifications & Build Status

> **Goal:** Never miss a broken build. When CI fails — build error, test failure, boot crash — the developer gets notified immediately. GitHub PR status checks block merge on failure. Email or webhook notifications on `main` branch failures. Build badge in README shows current status. The developer should never have to manually check CI — failures come to them.

> [!IMPORTANT]
> **Current state (2026-04-02):** GitHub Actions builds on push. No build badge in README. No branch protection rules enforcing status checks. CI QEMU tests removed (see 00-infrastructure/TODO-03-kernel-test-framework.md) (unreliable under nested virt) — CI only verifies build, not boot. Local testing via `make test`.

---

## Inputs

- `.github/workflows/build.yml` — existing CI pipeline
- → XREF: `00-infrastructure/TODO-03-kernel-test-framework.md` — `make test` for local verification (CI QEMU tests removed)
- → XREF: `TODO-02-developer-tooling-stack.md

---

## Outcome

- GitHub branch protection: `main` requires passing `Build Impossible OS` check before merge.
- Build badge in README.md shows green/red status.
- Failed builds on `main` trigger a notification (email, webhook, or GitHub notification).
- PR comments show test result summary automatically.
- Nightly scheduled build catches drift even without pushes.

---

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Branch protection rules for `main`              | —          |  [ ]   |
| 💎  |   2   | Build status badge in README.md                 | —          |  [ ]   |
| 💎  |   3   | Workflow failure notification (email/webhook)    | —          |  [ ]   |
| 💎  |   4   | Nightly scheduled build                         | —          |  [ ]   |
| ⭐  |   5   | PR comment bot with build results               | §1         |  [ ]   |
| ⭐  |   6   | Build time trend tracking                       | §4         |  [ ]   |

> 💎 = parity — standard CI/CD practice for any serious project.
> ⭐ = exclusive — automated PR comment with per-suite test table, build time trends.

---

## 1. Branch Protection Rules

Configure GitHub to require passing CI before merge to `main`.

- [ ] Settings → Branches → `main` → Require status checks: `Build Impossible OS`
- [ ] Only the build check is required (CI QEMU tests removed (see 00-infrastructure/TODO-03-kernel-test-framework.md))
- [ ] Require branches to be up to date before merging
- [ ] Document in CONTRIBUTING.md
- [ ] Commit: `"ci: branch protection — require passing CI for merge to main"`

**Test checkpoint:** Open a PR → GitHub shows "Required: Build Impossible OS" status check → PR cannot merge without it passing → direct push to `main` is blocked for non-admins.

---

## 2. Build Status Badge

Add CI status badge to README.md.

- [ ] Add badge markdown: `![Build](https://github.com/rizonesoft/impossible-os/actions/workflows/build.yml/badge.svg)`
- [ ] Place at top of README.md after the project title
- [ ] Commit: `"docs: add CI build status badge to README"`

**Test checkpoint:** Open README.md on GitHub → badge visible at top → shows green after successful build on `main`, red after failed build.

---

## 3. Workflow Failure Notification

Get notified when `main` breaks.

- [ ] Option A: GitHub's built-in email notifications (Settings → Notifications → Actions)
- [ ] Option B: Add a webhook step in build.yml that fires on failure:
  ```yaml
  - name: Notify on failure
    if: failure() && github.ref == 'refs/heads/main'
    uses: actions/github-script@v7
    with:
      script: |
        github.rest.issues.create({
          owner: context.repo.owner,
          repo: context.repo.repo,
          title: `CI Failure on main: ${context.sha.slice(0,7)}`,
          body: `Build failed. [View run](${context.serverUrl}/${context.repo.owner}/${context.repo.repo}/actions/runs/${context.runId})`,
          labels: ['ci-failure']
        })
  ```
- [ ] Commit: `"ci: failure notification — issue created on main branch build failure"`

**Test checkpoint:** Push a deliberate build failure to `main` → GitHub issue created with `ci-failure` label within 5 minutes → issue body contains link to failed Actions run.

---

## 4. Nightly Scheduled Build

Catch drift and flaky tests even without pushes.

- [ ] Add `schedule` trigger to build.yml: `cron: '0 2 * * *'` (2 AM UTC daily)
- [ ] Runs build-only (CI QEMU tests removed — local `make test` covers runtime verification)
- [ ] Upload all artifacts regardless of pass/fail
- [ ] Commit: `"ci: nightly scheduled build with full test suite"`

**Test checkpoint:** GitHub Actions → scheduled workflow visible in Actions tab → next run time shows ~2 AM UTC → after first run, `system-disk.img` artifact uploaded.

---

## 5. PR Comment Bot

Automatically post test results as a PR comment.

- [ ] After test steps: use `actions/github-script` to post a comment with test summary
- [ ] Include: build time, boot time, test pass/fail counts, failed test names
- [ ] Update existing comment on re-push (don't spam with new comments)
- [ ] Commit: `"ci: PR comment bot — automated test result summary on every PR"`

> [!NOTE]
> §5 posts build pass/fail from CI. Runtime test results are local-only (`make test`) since CI QEMU was removed.

**Test checkpoint:** Open PR with code changes → CI runs → PR receives comment with test summary table (suite, passed, failed columns) → re-push updates existing comment, no duplicate.

---

## 6. Build Time Trend Tracking

Track build and boot times per commit to catch performance regressions.

- [ ] Record build wall-clock time in CI step summary: `echo "Build time: ${SECONDS}s" >> $GITHUB_STEP_SUMMARY`
- [ ] Store timing data as workflow artifact: `build/build-timing.json` with `{commit, build_seconds, timestamp}`
- [ ] Script: `scripts/build-timing-report.sh` — reads last N runs, outputs trend table (commit, time, delta)
- [ ] Add trend summary to nightly build output: flag any commit that increased build time by >10%
- [ ] Commit: `"ci: build time trend tracking — catch build performance regressions"`

**Test checkpoint:** After 3+ CI runs → `build-timing-report.sh` outputs table showing commit hash, build time, and delta from previous → >10% increase flagged with warning.

---

## OS Comparison

| ⭐ | Feature                | 🪟 Win11             | 🐧 Linux              | 🚀 Impossible OS |
|----|------------------------|-------------------|--------------------|---------------|
| 💎 | Branch protection      | ✅ Internal gates | ✅ kernel.org rules | ⬜ §1         |
| 💎 | Build badge            | ❌ Internal       | ✅ kernelci badge   | ⬜ §2         |
| 💎 | Failure notifications  | ✅ Internal       | ✅ Email + IRC      | ⬜ §3         |
| 💎 | Nightly builds         | ✅ Internal       | ✅ kernel.org       | ⬜ §4         |
| ⭐ | PR test result comment | ❌ Internal       | ⚠️ Bot comments    | ⬜ §5         |
| ⭐ | Build time trends      | ❌ Internal only  | ❌ Not tracked      | ⬜ §6 🚀      |

---

## Unit Tests

> [!NOTE]
> This TODO produces GitHub Actions workflow configuration and repository settings — no kernel code. There are no kernel-level unit tests to wire into `test_runner_init()`. Acceptance testing is covered by the Verification section below (push broken code → observe notification, badge, protection behavior).

---

## Verification

- [ ] Push broken code → CI fails → notification received → PR blocked from merge.
- [ ] README badge shows green after successful build.
- [ ] Nightly build runs without manual trigger.
- [ ] Commit: `"infra: CI notifications complete — never miss a broken build"`
