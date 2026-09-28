# Infrastructure

Build system, CI/CD pipelines, tooling, and development environment.

## Roadmap Overviews

One page per infrastructure roadmap file, each following the [page contract](../contributing/docs-page-contract.md).

| Document                                                      | Topics                                                                |
| ------------------------------------------------------------- | --------------------------------------------------------------------- |
| [Developer Tooling Stack](developer-tooling-stack.md)         | Setup, build, test, lint and run wrappers, hooks, tooling doctor      |
| [AI Development System](ai-development-system.md)             | Doctrine, skills, hooks, reviewer contract, AI regression pack        |
| [Kernel Test Harness](kernel-test-harness.md)                 | In-kernel unit tests, fault injection, race barrier, leak detection   |
| [User-Mode Test Framework](usermode-test-framework.md)        | `test_*.exe` binaries, launcher, TAP, JUnit XML and JSON output       |
| [Desktop and UI Test Framework](desktop-ui-test-framework.md) | Screen capture, input injection, visual regression, frame timing      |
| [TODO Metadata Layer and Graph](todo-metadata-layer.md)       | Roadmap frontmatter, derived cache, queries, MCP server               |
| [LSP to MCP Bridge](lsp-mcp-bridge.md)                        | Language servers as read-only MCP tools for AI agents                 |
| [Automation Hardening](automation-hardening.md)               | Commit and review gates, hook audits, Codex policy                    |
| [Repository Ownership and Transfers](repository-transfer.md)  | Where the repository lives, transfer runbooks, owner-reference checks |

## Reference Documents

| Document                                        | Topics                                                                                                |
| ----------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| [Development Tooling](development-tooling.md)   | Build system, Makefile, Clang toolchain, asset pipeline, test framework                               |
| [AI Development System](ai-system.md)           | Claude Code + Codex ownership map, Authority Hierarchy, edit-here-not-there rules                     |
| [Skill Authoring Lifecycle](skill-authoring.md) | How to add/edit/retire a Claude Code skill, canonical SKILL.md template, catalog hygiene sync rules   |
| [TODO Frontmatter Spec](todo-metadata.md)       | Canonical YAML frontmatter for todo/**/*.md: required + optional + auto-derived fields, parsing rules |
| [GitHub Setup](github-setup.md)                 | CI/CD workflows, GitHub Actions, issue templates, labels, branch protection                           |

## See Also

- [Getting Started](../getting-started/index.md) -- QEMU and VirtualBox setup guides
