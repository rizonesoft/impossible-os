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

## 2. Bootloader History Extraction

### 2.1 Extract src/boot/ History to Public Repo

**Prompt:** Extract the `src/boot/` directory history from the private `impossible-os` repo into the public `impossible-os-bootloader` repo using `git filter-repo`. This preserves full commit history for the bootloader component while keeping the rest of the OS private. Install `git filter-repo` if not already installed (`pip3 install git-filter-repo`). Work in a temporary clone to avoid modifying the working repo. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, and commit the result to the public repo as the initial history push. Update `README.md` in the bootloader repo with accurate source path information.

- [ ] Clone private repo to a temp location: `git clone impossible-os /tmp/boot-extract`
- [ ] Run: `cd /tmp/boot-extract && git filter-repo --path src/boot/`
- [ ] Add public repo as remote: `git remote add public https://github.com/rizonesoft/impossible-os-bootloader.git`
- [ ] Force push filtered history: `git push public main --force`
- [ ] Verify public repo contains only `src/boot/` files with correct history
- [ ] Delete temp clone after verification: `rm -rf /tmp/boot-extract`

---

## 3. Automated Sync (GitHub Actions)

### 3.1 Sync Bootloader Changes on Push

**Prompt:** Create a GitHub Actions workflow in the private `impossible-os` repo that automatically pushes changes to `src/boot/` to the public `impossible-os-bootloader` repo whenever a commit touches that path. Use `git subtree` to push a clean copy with no private history leaking. Store the GitHub Personal Access Token (with public repo write scope) as a repository secret named `BOOTLOADER_REPO_TOKEN`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, and commit as `"ci: sync bootloader to public repo on push"`. Test by making a small change to `src/boot/` and confirming it appears in the public repo within 2 minutes.

- [ ] Create `.github/workflows/sync-bootloader.yml` in private repo
- [ ] Trigger: `push` to `main` with paths filter `src/boot/**`
- [ ] Action steps:
  - [ ] Checkout private repo with full history
  - [ ] Use `git subtree push` (or `git filter-repo` + force push) to public repo
  - [ ] Authenticate using `BOOTLOADER_REPO_TOKEN` secret
- [ ] Add `BOOTLOADER_REPO_TOKEN` to private repo secrets (PAT: public repo write scope)
- [ ] Test end-to-end: change a file in `src/boot/`, push, verify public repo updates
- [ ] Commit: `"ci: sync bootloader to public repo on push"`

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

## Priority Order

| Priority | Section                            | Reason                                          |
|----------|------------------------------------|-------------------------------------------------|
| ✅ Done   | 1.1 Repository structure           | Both repos exist                                |
| 🔴 P0    | 1.1 README + LICENSE               | Public repo needs basic info before sharing     |
| 🔴 P0    | 2.1 Bootloader history extraction  | Populate the bootloader repo with real content  |
| 🟠 P1    | 3.1 Automated sync via CI          | Keep repos in sync without manual effort        |
| 🟡 P2    | 4.1 Automated ISO releases         | Needed when ready to publish builds             |
| 🔵 P4    | 4.2 Mirror release tags            | Nice-to-have polish once releases are automated |
