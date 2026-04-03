# 17 Polish & Hardening

This domain is a staging area for discovered issues, regression fixes, performance tuning, UX polish, and final hardening work that doesn't fit cleanly into a feature-focused domain.

## Belongs Here

- Bugs discovered during integration testing or bare-metal verification that span multiple subsystems.
- Performance regressions, boot time optimization, memory usage reduction.
- UX polish: better error messages, smoother animations, cleaner serial output formatting.
- Security hardening follow-ups: audit findings, fuzzer results, code review issues.
- Cross-cutting cleanup: dead code removal, naming consistency, header reorganization.

## Does Not Belong Here

- New features -- those belong in the domain that owns the subsystem.
- Architecture-specific issues -- those go in [16 Architecture Ports](../16-architecture-ports/INDEX.md).
- Speculative or research work -- that goes in [18 Future Research](../18-future-research/INDEX.md).

## Likely Source Areas

- Any file in the repository. This domain is cross-cutting by nature.

## Epics

- None yet.

## Active TODOs

- None yet. TODOs are created here as issues are discovered.

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-short-description.md` as the filename style.
- Prefer specific, scoped TODOs over catch-all "fix everything" files.
