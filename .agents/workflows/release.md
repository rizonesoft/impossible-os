---
description: Tag a release, update changelog, build and publish the ISO
---

> **Reference document.** `.cursor/rules/` and `.cursor/skills/` are the single source of truth for AI guidance.
> This file is kept as a supplementary reference for the release process specifically.

# Release Workflow

## Versioning Scheme

**CalVer:** `YY.M.D` (auto-generated from build date, e.g., `26.3.21`)

Build number auto-increments from `.build_number`. Version is set automatically by the build system — no manual version editing required.

## ISO Naming Convention

```
impossible-os-vYY.M.D.iso
impossible-os-vYY.M.D.iso.sha256
```

## Steps

### 1. Update CHANGELOG.md (create if it does not exist)

Add a new section at the top:
```markdown
## [X.Y.Z] — YYYY-MM-DD

### Added
- Feature description

### Fixed
- Bug fix description

### Changed
- Change description
```

### 2. Full build and test

// turbo-all

```bash
bash scripts/build.sh clean
```

```bash
tail -1 build/build.log
```
Expected: `=== BUILD OK ===`

```bash
bash scripts/build.sh run
```

Confirm QEMU boots the new version correctly.

### 3. Commit and tag

```bash
git add -A
git commit -m "release: vYY.M.D"
git tag -a vYY.M.D -m "Release vYY.M.D — short description"
```

### 4. Push to GitHub

```bash
git push && git push --tags
```

### 5. Rename ISO for release

```bash
cp build/system-disk.img build/impossible-os-vYY.M.D.iso
sha256sum build/impossible-os-vYY.M.D.iso > build/impossible-os-vYY.M.D.iso.sha256
```

### 6. (Optional) Create GitHub Release

- Go to the repo on GitHub → **Releases → Draft a new release**
- Select the tag `vYY.M.D`
- Upload `impossible-os-vYY.M.D.iso` and `.sha256`
- Paste the CHANGELOG entry as release notes

### 7. (Optional) Real hardware test

Follow the `/test-hardware` workflow to validate on real hardware via USB boot.
