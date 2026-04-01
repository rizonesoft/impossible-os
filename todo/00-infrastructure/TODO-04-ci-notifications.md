# TODO-04 — CI Notifications & Build Status

> **Goal:** Never miss a broken build. When CI fails — build error, test failure, boot crash — the developer gets notified immediately. GitHub PR status checks block merge on failure. Email or webhook notifications on `main` branch failures. Build badge in README shows current status. The developer should never have to manually check CI — failures come to them.

> [!IMPORTANT]
> **Current state:** GitHub Actions builds on push but the only notification is the default GitHub email (which many developers disable). No build badge in README. Smoke tests commented out. No Slack/Discord/webhook integration. No branch protection rules enforcing status checks.

---

## Inputs

- `.github/workflows/build.yml` — existing CI pipeline
- → XREF: `TODO-03-kernel-test-framework.md §4–§6` — test steps that need status check enforcement
- → XREF: `TODO-02-developer-tooling-stack.md §5` — GitHub sync workstream

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
| 💎  |   4   | Nightly scheduled build                         | T03 §4     |  [ ]   |
| ⭐  |   5   | PR comment bot with test results                | T03 §7     |  [ ]   |

> 💎 = parity — standard CI/CD practice for any serious project.
> ⭐ = exclusive — automated PR comment with per-suite test table.

---

## 1. Branch Protection Rules

Configure GitHub to require passing CI before merge to `main`.

- [ ] Settings → Branches → `main` → Require status checks: `Build Impossible OS`
- [ ] When TODO-03 §4–§6 land: add smoke test, unit test, FS test as required checks
- [ ] Require branches to be up to date before merging
- [ ] Document in CONTRIBUTING.md
- [ ] Commit: `"ci: branch protection — require passing CI for merge to main"`

---

## 2. Build Status Badge

Add CI status badge to README.md.

- [ ] Add badge markdown: `![Build](https://github.com/rizonesoft/impossible-os/actions/workflows/build.yml/badge.svg)`
- [ ] Place at top of README.md after the project title
- [ ] Commit: `"docs: add CI build status badge to README"`

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

---

## 4. Nightly Scheduled Build

Catch drift and flaky tests even without pushes.

- [ ] Add `schedule` trigger to build.yml: `cron: '0 2 * * *'` (2 AM UTC daily)
- [ ] Run full test suite (smoke + unit + FS) on nightly
- [ ] Upload all artifacts regardless of pass/fail
- [ ] Commit: `"ci: nightly scheduled build with full test suite"`

---

## 5. PR Comment Bot

Automatically post test results as a PR comment.

- [ ] After test steps: use `actions/github-script` to post a comment with test summary
- [ ] Include: build time, boot time, test pass/fail counts, failed test names
- [ ] Update existing comment on re-push (don't spam with new comments)
- [ ] Commit: `"ci: PR comment bot — automated test result summary on every PR"`

---

## OS Comparison

| ⭐ | Feature                  | Win11                | Linux               | Impossible OS          |
|----|--------------------------|----------------------|----------------------|------------------------|
| 💎 | Branch protection        | ✅ Internal gates    | ✅ kernel.org rules  | ⬜ §1                  |
| 💎 | Build badge              | ❌ Internal          | ✅ kernelci badge    | ⬜ §2                  |
| 💎 | Failure notifications    | ✅ Internal          | ✅ Email + IRC       | ⬜ §3                  |
| 💎 | Nightly builds           | ✅ Internal          | ✅ kernel.org nightly | ⬜ §4                 |
| ⭐ | PR test result comment   | ❌ Internal          | ⚠️ Bot comments      | ⬜ §5 🚀               |

---

## Verification

- [ ] Push broken code → CI fails → notification received → PR blocked from merge.
- [ ] README badge shows green after successful build.
- [ ] Nightly build runs without manual trigger.
- [ ] Commit: `"infra: CI notifications complete — never miss a broken build"`
