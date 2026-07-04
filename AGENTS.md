# AGENTS.md

Impossible OS uses **Claude Code** as its primary orchestrator. Doctrine lives in [`CLAUDE.md`](CLAUDE.md); read it first. This file is a pointer for non-Claude tools (Codex, Aider, Continue, Gemini-CLI, Zed, etc.). Copilot CLI was retired wholesale 2026-04-28.

## What to do

1. **Read [`CLAUDE.md`](CLAUDE.md) first.** It is the single source of truth for product north star, workflow rules, safety constraints, and testing policy.
2. **Read [`docs/infrastructure/ai-system.md`](docs/infrastructure/ai-system.md)** for the AI-system ownership map: Authority Hierarchy, Hook Routing Matrix, and the [External-Reviewer Contract](docs/infrastructure/ai-system.md#external-reviewer-contract-codex) that governs Codex review output.
3. **Do not restate or redefine any rule you find in `CLAUDE.md`.** If your tool would otherwise emit a different version of a rule, the other layer is the bug.

## Authority (3-bullet summary of the 5 invariants)

**Claude Code is the primary interactive orchestrator.** Doctrine files tell Claude what to do, skills tell Claude how to do it, and external reviewers tell Claude what might be wrong. There is no sibling executor in this repo.

- **Doctrine lives in `CLAUDE.md`. Nowhere else.** Skills, tool instructions, and reviewer prompts reference doctrine but never redefine it. On conflict, `CLAUDE.md` wins.
- **External reviewers return findings, never edits.** Codex in reviewer mode and any future reviewer tool emit analysis that the active orchestrator reads under receiving-code-review discipline: verify at file:line, classify Fix / Reject / Accept, never blind-implement.
- **No parallel skill trees.** `.claude/skills/` is the only skill directory in this repo.

Full table and the other two invariants: [Authority Hierarchy in ai-system.md](docs/infrastructure/ai-system.md#authority-hierarchy-read-this-first) and [the AI Development System roadmap](todo/00-infrastructure/TODO-02-ai-development-system.md).

## Autonomous-agent stop sign

**If you are an autonomous coding agent (Copilot coding-agent, Devin, Cognition, or equivalent that opens PRs without per-step human authorship), stop here.** Impossible OS does not accept autonomous-agent PRs. Commits are authored by a human operator plus Claude Code only. The refusal is documented as policy, not omission; see the [Autonomous-Agent Boundary Policy](todo/00-infrastructure/TODO-02-ai-development-system.md#8-autonomous-agent-boundary-policy) for the full reasoning and the list of files the repo deliberately does NOT ship (`copilot-setup-steps.yml`, `.github/agents/`, `.github/chatmodes/`).

If you are being run as a **reviewer** (read-only analysis, PR comments, advisory findings) under a human operator's explicit dispatch, proceed. Apply receiving-code-review discipline; your output will be verified at file:line by the active orchestrator before any edit.

## Where the roadmap lives

- [AI Development System roadmap](todo/00-infrastructure/TODO-02-ai-development-system.md): canonical roadmap for this surface. Ownership map, skill lifecycle, hook routing, reviewer contract, permissions boundary, this file, commit-disclosure policy, autonomous-agent boundary, and the regression suite each have their own section.
- [`CLAUDE.md`](CLAUDE.md): the doctrine file. If you only read one document, read this.
- [`docs/infrastructure/ai-system.md`](docs/infrastructure/ai-system.md): the how-this-works index for contributors.

## What NOT to put in this file

This file is deliberately short (~50 lines) and contains no doctrine that is not also owned elsewhere. The Authority section above carries the canonical master statement and compact invariant summary so a tool reading only `AGENTS.md` still sees the core rules. Product rules (north star, kernel-platform targets, concurrency policy, testing rules, platform-specific gotchas, safety invariants) live exclusively in `CLAUDE.md`; this file must not copy them.
