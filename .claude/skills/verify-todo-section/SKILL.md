---
name: verify-todo-section
description: Verify whether a TODO section marked done or in progress matches actual code, build, and runtime evidence, then correct the section state conservatively. Use when auditing completed TODO work, reconciling stale checklist state, or verifying a claimed implementation.
---

# Verify TODO Section

Read and follow the canonical workflow: `.impossible/workflows/verify-todo-section.md`

## Claude Code-Specific Notes

- Use Grep and Glob tools for finding code evidence — no Srclight MCP.
- Use `bash scripts/build.sh` for build verification.
- Use `bash scripts/build.sh run` for QEMU runtime evidence.
- Use `llvm-addr2line-19 -e build/kernel.exe -f <RIP>` for crash analysis.
