# TODO-001 — GitHub Repository Setup ✅

**Status:** Completed — 2026-03-21
**Documentation:** [github-setup.md](../../docs/architecture/infrastructure/github-setup.md)
**Commits:** See git log — spans initial repo setup through CI/CD, templates, branch protection, and GitHub Pages.

## Gotchas

- MOK private key (`MOK.key`) must NEVER be committed — store in encrypted vault or hardware token
- LLVM-19 not in `ubuntu-latest` — CI installs from `apt.llvm.org` snapshot repo; cache key `llvm-19-Linux-v1` (bump suffix to invalidate)
- `llvm-objcopy` cannot produce EFI binaries — release workflow uses GNU `objcopy`
- Smoke test in `build.yml` is commented out — uncomment when `test-smoke.sh` CI integration is ready
- License changed from MIT → GPL-3.0 (commit `474abac`) — all badges/docs must reflect GPL-3.0
- `actions/labeler@v5` uses `changed-files` syntax (not legacy v4 array format)
- Release workflow needs `permissions: contents: write` for `softprops/action-gh-release@v2`

## Cross-References

- Depends on: TODO-002 §1.4 (build version metadata), TODO-002 §5.3 (smoke test script)
- Depended on by: (none — foundational infrastructure)
- Related: [Development Tooling](../../docs/architecture/infrastructure/development-tooling.md)
