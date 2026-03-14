# GitHub Repository Setup

> **Goal:** Establish a multi-repo structure under the `rizonesoft` account that
> separates the public bootloader from the private OS codebase, automates syncing
> via GitHub Actions, covers code signing, and produces automated ISO releases.

> [!CAUTION]
> **Private Key Security:** The MOK private key (`MOK.key`) MUST NEVER be committed
> to any repository. Store it in an encrypted vault or hardware token. If compromised,
> an attacker could sign malware that bypasses Secure Boot on enrolled machines.

---

## 1. Repository Structure ✅

### 1.1 Create Public Bootloader Repo

**Prompt:** Verified (2026-03-14). `https://github.com/rizonesoft/impossible-os-bootloader` is public with `README.md` and `LICENSE` committed as `"chore: initial bootloader public repo"`. Both repos are correctly set up. No further action needed.

- [x] `rizonesoft/impossible-os` — Private — kernel, desktop, drivers, apps (this repo)
- [x] `rizonesoft/impossible-os-bootloader` — Public — bootloader for Secure Boot signing and community audit
- [x] Add `README.md` to bootloader repo explaining its purpose
- [x] Add `LICENSE` file to bootloader repo
- [x] Commit to bootloader repo: `"chore: initial bootloader public repo"`

---

## 2. Bootloader History Extraction ✅

### 2.1 Extract src/boot/ History to Public Repo

**Prompt:** Verified (2026-03-14). `git filter-repo --path src/boot/` was run on a temp clone of the private repo, reducing 504 commits to 17 (only commits touching `src/boot/`). The filtered history (11 files, 17 commits) was force-pushed to `rizonesoft/impossible-os-bootloader`. The temp clone was deleted. Verify by checking `https://github.com/rizonesoft/impossible-os-bootloader` contains only `src/boot/` files with intact commit history.

- [x] Clone private repo to a temp location: `git clone impossible-os /tmp/boot-extract`
- [x] Run: `cd /tmp/boot-extract && git filter-repo --path src/boot/` (504→17 commits)
- [x] Add public repo as remote: `git remote add public https://github.com/rizonesoft/impossible-os-bootloader.git`
- [x] Force push filtered history: `git push public main --force`
- [x] Verify public repo contains only `src/boot/` files with correct history (11 files, 17 commits)
- [x] Delete temp clone after verification: `rm -rf /tmp/boot-extract`

---

## 3. Automated Sync (GitHub Actions) ✅

### 3.1 Sync Bootloader Changes on Push

**Prompt:** Verified (2026-03-14). Workflow at `.github/workflows/sync-bootloader.yml` uses `clone + copy src/boot/ + push` approach. `BOOTLOADER_REPO_TOKEN` secret added to private repo. End-to-end test passed — a change to `src/boot/uefi/bootx64.c` triggered the workflow and the public `impossible-os-bootloader` repo was updated automatically within ~2 minutes. Note: first iteration used `git subtree split` which failed; replaced with clone+copy+push (`532f76a`).

- [x] Create `.github/workflows/sync-bootloader.yml` in private repo
- [x] Trigger: `push` to `main` with paths filter `src/boot/**`
- [x] Action steps:
  - [x] Checkout private repo with full history (`fetch-depth: 0`)
  - [x] Clone public bootloader repo, copy `src/boot/`, commit and push
  - [x] Authenticate using `BOOTLOADER_REPO_TOKEN` secret
- [x] Add `BOOTLOADER_REPO_TOKEN` to private repo secrets
- [x] Test end-to-end: change in `src/boot/uefi/bootx64.c` → public repo updated ✅
- [x] Commit: `"ci: sync bootloader to public repo on push"` (`da1121a`, fixed `532f76a`)

---

## 4. Automated ISO Releases

### 4.1 Build and Publish ISO on Tag

**Prompt:** Create a GitHub Actions workflow that builds the OS ISO and publishes it as a GitHub Release whenever a version tag (e.g., `v0.1.0`) is pushed to the private repo. The ISO should be attached as a release artifact. The workflow must run `bash scripts/build.sh clean` to produce `build/os-build.iso`, then upload it. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, and commit as `"ci: automated release build and ISO publish"`. Test by pushing a `v0.0.1-test` tag and verifying the release appears on GitHub with the ISO attached.

- [ ] Create `.github/workflows/release.yml` in private repo
- [ ] Trigger: `push` with tag matching `v*`
- [ ] Action steps:
  - [ ] Checkout repo
  - [ ] Install build dependencies (cross-compiler, NASM, xorriso, QEMU tools)
  - [ ] Run: `bash scripts/build.sh clean`
  - [ ] Verify `build/os-build.iso` exists and `build/build.log` ends with `=== BUILD OK ===`
  - [ ] Create GitHub Release with tag name and upload ISO
- [ ] Test with `v0.0.1-test` tag
- [ ] Commit: `"ci: automated release build and ISO publish"`

### 4.2 Mirror Tags to Public Bootloader Repo

**Prompt:** When a release tag is pushed to the private repo, also mirror that tag to the public bootloader repo. This keeps release history in sync between repos. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, and commit as `"ci: mirror release tags to bootloader repo"`.

- [ ] Extend `sync-bootloader.yml` or `release.yml` to also push the tag to the public repo
- [ ] Verify public bootloader repo tags match private repo release tags
- [ ] Commit: `"ci: mirror release tags to bootloader repo"`

---

## 5. SDK Repository

### 5.1 Create Public SDK Repo

**Prompt:** Create `rizonesoft/impossible-os-sdk` as a public GitHub repository. This is the outward-facing repo that third-party developers will use to download SDK headers, libraries, and documentation without needing access to the private OS source. Add `README.md` explaining the SDK purpose and `LICENSE` (MIT). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt.

- [ ] Create `rizonesoft/impossible-os-sdk` (public) on GitHub
- [ ] Add `README.md` explaining SDK purpose, contents, and how to use it
- [ ] Add `LICENSE` (MIT)
- [ ] Commit to SDK repo: `"chore: initial SDK public repo"`

### 5.2 Extract sdk/ History to Public SDK Repo

**Prompt:** Extract the `sdk/` directory history from the private repo into the public `impossible-os-sdk` repo using `git filter-repo`, same approach used for the bootloader repo. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt.

- [ ] Clone private repo: `git clone impossible-os /tmp/sdk-extract`
- [ ] Run: `cd /tmp/sdk-extract && git filter-repo --path sdk/`
- [ ] Add public SDK repo as remote and force-push
- [ ] Verify public repo contains only `sdk/` files with correct history
- [ ] Delete temp clone: `rm -rf /tmp/sdk-extract`

### 5.3 Sync SDK Changes on Push

**Prompt:** Create `.github/workflows/sync-sdk.yml` in the private repo to automatically push `sdk/` changes to the public `impossible-os-sdk` repo on every push to `main` that touches `sdk/**`. Mirror the clone+copy+push approach used by `sync-bootloader.yml`. Store auth token as `SDK_REPO_TOKEN` secret. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, and commit as `"ci: sync SDK to public repo on push"`.

- [ ] Create `.github/workflows/sync-sdk.yml`
- [ ] Trigger: `push` to `main` with paths filter `sdk/**`
- [ ] Action: clone public SDK repo → remove old `sdk/` → copy current `sdk/` → commit + push
- [ ] Add `SDK_REPO_TOKEN` to private repo secrets (PAT: `public_repo` write scope)
- [ ] Test end-to-end: change a file in `sdk/`, push, verify public SDK repo updates
- [ ] Commit: `"ci: sync SDK to public repo on push"`

---

## Priority Order

| Priority | Section                            | Reason                                          |
|----------|------------------------------------|-------------------------------------------------|
| ✅ Done   | 1.1 Bootloader repo structure      | Both repos exist                                |
| ✅ Done   | 1.1 README + LICENSE               | Committed to bootloader repo                    |
| ✅ Done   | 2.1 Bootloader history extraction  | 17 commits in public bootloader repo            |
| ✅ Done   | 3.1 Bootloader auto-sync CI        | Tested and working                              |
| 🟡 P2    | 4.1 Automated ISO releases         | Needed when ready to publish builds             |
| 🔵 P4    | 4.2 Mirror release tags            | Polish once releases are automated              |
| 🟠 P1    | 5.1 SDK repo create                | Create before any SDK code is written           |
| 🟠 P1    | 5.2 SDK history extraction         | After sdk/ directory exists in private repo     |
| 🟠 P1    | 5.3 SDK auto-sync CI               | After 5.1 + 5.2                                 |
