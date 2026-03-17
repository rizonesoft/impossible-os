---
description: Tag a release, update changelog, build and publish the ISO
---

# Release Workflow

## Versioning Scheme

**Semantic Versioning:** `MAJOR.MINOR.PATCH`

- **MAJOR** — breaking changes or major milestones (e.g. 1.0.0 = boot-to-desktop)
- **MINOR** — new features or subsystems (e.g. 0.3.0 = memory management)
- **PATCH** — bug fixes and polish (e.g. 0.3.1 = heap coalescing fix)

## ISO Naming Convention

```
impossible-os-vX.Y.Z.iso
impossible-os-vX.Y.Z.iso.sha256
```

## Steps

### 1. Update version

Edit `src/kernel/version.c` to set the new version string:
```c
const char *KERNEL_VERSION = "X.Y.Z";
```

### 2. Update CHANGELOG.md (create if it does not exist)

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

### 3. Full build and test

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

### 4. Commit and tag

```bash
git add -A
git commit -m "release: vX.Y.Z"
git tag -a vX.Y.Z -m "Release vX.Y.Z — short description"
```

### 5. Push to GitHub

```bash
git push && git push --tags
```

### 6. Rename ISO for release

```bash
cp build/system-disk.img build/impossible-os-vX.Y.Z.iso
sha256sum build/impossible-os-vX.Y.Z.iso > build/impossible-os-vX.Y.Z.iso.sha256
```

### 7. (Optional) Create GitHub Release

- Go to the repo on GitHub → **Releases → Draft a new release**
- Select the tag `vX.Y.Z`
- Upload `impossible-os-vX.Y.Z.iso` and `.sha256`
- Paste the CHANGELOG entry as release notes

### 8. (Optional) Real hardware test

Follow the `/test-hardware` workflow to validate on real hardware via USB boot.
