# Impossible OS TODO Index

This tree is the live planning scaffold for active work.

## How To Use This Tree

- Use this file for execution order.
- Use each domain folder for ownership and scoped backlog management.
- Give every topic one canonical home. Cross-link instead of duplicating.
- Keep this root file at the domain and epic level only.
- Use local numbering inside each domain for future leaf files, such as `TODO-01-boot-handoff.md` or `TODO-02-vfs.md`.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
- When work is documented elsewhere, move it under a domain's Completed / Doc-converted section instead of keeping a large stale checklist here.

## Domain Order

| Order | Domain                                                      | Purpose                                                                                                                                     |
| ----- | ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| 01    | [00 Infrastructure](./00-infrastructure/INDEX.md)           | Build, tooling, automation, test harnesses, and developer workflows.                                                                        |
| 02    | [01 Boot Platform](./01-boot-platform/INDEX.md)             | UEFI boot, handoff, early platform bring-up, and pre-kernel contracts.                                                                      |
| 03    | [02 Kernel Core](./02-kernel-core/INDEX.md)                 | Kernel bring-up, core runtime behavior, logging, registry, and ABI-level foundations.                                                       |
| 04    | [03 Memory Concurrency](./03-memory-concurrency/INDEX.md)   | Memory management, tasking, scheduling, IPC, and SMP coordination.                                                                          |
| 05    | [04 Drivers Hardware](./04-drivers-hardware/INDEX.md)       | Buses, platform controllers, device drivers, and hardware integration.                                                                      |
| 06    | [05 Storage Filesystems](./05-storage-filesystems/INDEX.md) | Block adapters, partitioning, VFS, filesystem implementations, and Win32 file semantics.                                                    |
| 07    | [06 Desktop Foundation](./06-desktop-foundation/INDEX.md)   | Complete the core desktop: WM, compositor, input system, controls, shell actions.                                                           |
| 08    | [07 Networking](./07-networking/INDEX.md)                   | Kernel networking stack, protocols, sockets, and network-facing integration.                                                                |
| 09    | [08 Graphics UI](./08-graphics-ui/INDEX.md)                 | Themes, animations, advanced widgets, and reusable UI framework pieces.                                                                     |
| 10    | [09 Desktop Shell](./09-desktop-shell/INDEX.md)             | Desktop workflows, file manager, terminal, settings, and shell-level apps.                                                                  |
| 11    | [10 Platform Services](./10-platform-services/INDEX.md)     | Win32 PE/API surface, installer and updates, recovery and accessibility, Linux compat, and other shared platform services above the kernel. |
| 12    | [11 Apps](./11-apps/INDEX.md)                               | Built-in applications, CLI tools, and user-facing product features.                                                                         |
| 13    | [12 User Platform SDK](./12-user-platform-sdk/INDEX.md)     | User-mode ABI, compatibility surface, SDK assets, and developer-facing contracts.                                                           |
| 14    | [13 Tools & Accessories](./13-tools-accessories/INDEX.md)   | System diagnostic tools, namespace browsers, and administrative utilities.                                                                  |
| 15    | [14 Host Tools & SDK](./14-host-tools/INDEX.md)             | SDK tools running on the host OS (Linux) for development and third-party use.                                                               |
| 16    | [15 Installer & Release](./15-installer-release/INDEX.md)   | Installer flow, deployable media, packaging, and release readiness.                                                                         |
| 17    | [16 Architecture Ports](./16-architecture-ports/INDEX.md)   | ARM64 port, arch abstraction layer (HAL), multi-arch build system.                                                                          |
| 18    | [17 Polish & Hardening](./17-polish-hardening/INDEX.md)     | Discovered bugs, regression fixes, performance tuning, UX polish.                                                                           |
| 19    | [18 Future Research](./18-future-research/INDEX.md)         | Long-range research, stretch goals, and ideas not yet ready for active execution.                                                           |

## Active Epics

- [00 Infrastructure] [TODO-01 Developer Tooling Stack](./00-infrastructure/TODO-01-developer-tooling-stack.md) - Define the canonical host-side tooling contract for setup, build, test, run, hooks, GitHub workflows, and a tooling doctor/regression pack.
- [00 Infrastructure] [TODO-02 AI Development System](./00-infrastructure/TODO-02-ai-development-system.md) - Stabilize the repo AI workflow (Claude Code master; Codex GPT-5.5 sole external reviewer -- Copilot CLI retired 2026-04-28) with explicit ownership, hook policy, external-reviewer contract, and regression checks.
- [00 Infrastructure] [TODO-06 TODO Metadata Layer](./00-infrastructure/TODO-06-todo-metadata-layer.md) - Add stable-ID frontmatter and a derived graph cache so cross-TODO dependencies, backlinks, and ready/blocked queries stop depending on file numbering and line numbers. Markdown stays canonical; the cache is a read-only projection.
- [03 Memory Concurrency] [TODO-01 VMM Memory Protection & Diagnostics](./03-memory-concurrency/TODO-01-vmm-memory-protection.md) - Harden the memory model with mprotect, W^X enforcement, demand paging, VirtualAlloc Win32 wrappers, kmalloc lint, heap canaries, leak detector, and PMM statistics.
