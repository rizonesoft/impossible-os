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

Edit the `VERSION` file in the repo root:
```bash
echo "X.Y.Z" > VERSION
```

### 2. Update CHANGELOG.md

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
make clean && make all && make iso
```

```bash
make run
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
cp build/os-build.iso build/impossible-os-vX.Y.Z.iso
sha256sum build/impossible-os-vX.Y.Z.iso > build/impossible-os-vX.Y.Z.iso.sha256
```

### 7. (Optional) Create GitHub Release

- Go to the repo on GitHub → **Releases → Draft a new release**
- Select the tag `vX.Y.Z`
- Upload `impossible-os-vX.Y.Z.iso` and `.sha256`
- Paste the CHANGELOG entry as release notes

### 8. Hyper-V production test

Follow the `/test-hyperv` workflow to validate in Hyper-V.
