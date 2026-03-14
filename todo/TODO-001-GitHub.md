# TODO-001 — GitHub Repository Setup

> **Goal:** Establish a clean multi-repo GitHub structure that separates the public
> bootloader from the private OS codebase, with automated syncing via CI.

---

## 1. Repository Structure

| Repo                               | Visibility   | Purpose                                                    |
|------------------------------------|--------------|------------------------------------------------------------|
| `rizonesoft/impossible-os`         | 🔒 Private   | Main OS — kernel, desktop, drivers, apps (current repo)    |
| `rizonesoft/impossible-os-bootloader` | 🌐 Public | Bootloader only — required for Secure Boot signing/audit   |

> **No GitHub Organization needed.** Both repos live under the `rizonesoft`
> personal account. Organizations add complexity only needed for a multi-person team.

---

## 2. Bootloader Repo Setup

- [ ] Create `rizonesoft/impossible-os-bootloader` (public) on GitHub
- [ ] Extract `src/boot/` history using `git filter-repo --path src/boot/`
- [ ] Push extracted history to the public repo
- [ ] Add `README.md` explaining: "Bootloader component of Impossible OS"
- [ ] Add `LICENSE` file to public repo
- [ ] Commit: `"chore: initial bootloader public repo"`

---

## 3. Automated Sync (GitHub Actions)

When `src/boot/` changes in the private repo, automatically push to the public repo.

- [ ] Create `.github/workflows/sync-bootloader.yml` in private repo
- [ ] Trigger: `push` to `main` with changes in `src/boot/**`
- [ ] Action: use `git subtree push` or `git filter-repo` + force push to public repo
- [ ] Store `BOOTLOADER_REPO_TOKEN` as a GitHub secret (PAT with public repo write access)
- [ ] Test: make a change to `src/boot/`, verify it appears in public repo within minutes
- [ ] Commit: `"ci: sync bootloader to public repo on push"`

---

## 4. Secure Boot Signing (Future)

- [ ] Generate a key pair for Secure Boot signing:
  - [ ] `openssl req -new -x509 -newkey rsa:2048 -keyout MOK.key -out MOK.crt`
- [ ] Sign bootloader EFI binary: `sbsign --key MOK.key --cert MOK.crt --output bootx64.efi bootx64.efi`
- [ ] Document MOK enrollment process for users (UEFI → Enroll MOK → select `MOK.crt`)
- [ ] *(Stretch)* Submit for Microsoft shim signing (requires stable, public, audited code)
- [ ] Commit: `"boot: Secure Boot signing via MOK"`

---

## 5. Release Workflow

- [ ] Tag releases in private repo: `git tag v0.1.0`
- [ ] GitHub Actions: on tag → build ISO → upload as GitHub Release artifact
- [ ] Public bootloader repo: mirror the release tags
- [ ] Signed bootloader EFI attached to each release
- [ ] Commit: `"ci: automated release build and publish"`

---

## Priority Order

| Priority | Section              | Reason                                      |
|----------|----------------------|---------------------------------------------|
| 🔴 P0   | §1 Repo Structure    | Do this before bootloader work begins       |
| 🔴 P0   | §2 Bootloader Repo   | Required for Secure Boot signing path       |
| 🟠 P1   | §3 Automated Sync    | Keep repos in sync without manual effort    |
| 🟡 P2   | §5 Release Workflow  | Needed when ready to publish builds         |
| 🔵 P4   | §4 Secure Boot       | After OS is stable and bootloader is mature |
