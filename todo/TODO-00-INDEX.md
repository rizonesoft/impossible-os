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

| Order | Domain | Purpose |
| --- | --- | --- |
| 01 | [00 Infrastructure](./00-infrastructure/INDEX.md) | Build, tooling, automation, test harnesses, and developer workflows. |
| 02 | [01 Boot Platform](./01-boot-platform/INDEX.md) | UEFI boot, handoff, early platform bring-up, and pre-kernel contracts. |
| 03 | [02 Kernel Core](./02-kernel-core/INDEX.md) | Kernel bring-up, core runtime behavior, logging, registry, and ABI-level foundations. |
| 04 | [03 Memory Concurrency](./03-memory-concurrency/INDEX.md) | Memory management, tasking, scheduling, IPC, and SMP coordination. |
| 05 | [04 Drivers Hardware](./04-drivers-hardware/INDEX.md) | Buses, platform controllers, device drivers, and hardware integration. |
| 06 | [05 Storage Filesystems](./05-storage-filesystems/INDEX.md) | Block adapters, partitioning, VFS, filesystem implementations, and Win32 file semantics. |
| 07 | [06 Networking](./06-networking/INDEX.md) | Kernel networking stack, protocols, sockets, and network-facing integration. |
| 08 | [07 Graphics UI](./07-graphics-ui/INDEX.md) | Rendering primitives, fonts, controls, themes, and reusable UI framework pieces. |
| 09 | [08 Desktop Shell](./08-desktop-shell/INDEX.md) | Compositor, window management, desktop UX, and shell-level experiences. |
| 10 | [09 Services Security](./09-services-security/INDEX.md) | Shared system services, security policy, accounts, crypto, and orchestration. |
| 11 | [10 Apps](./10-apps/INDEX.md) | Built-in applications, CLI tools, and user-facing product features. |
| 12 | [11 User Platform SDK](./11-user-platform-sdk/INDEX.md) | User-mode ABI, compatibility surface, SDK assets, and developer-facing contracts. |
| 13 | [12 Installer Release](./12-installer-release/INDEX.md) | Installer flow, deployable media, packaging, and release readiness. |
| 14 | [13 Future Research](./13-future-research/INDEX.md) | Long-range research, stretch goals, and ideas not yet ready for active execution. |
| 15 | [14 SDK Tools](./14-host-tools/INDEX.md) | SDK tools running on the host OS (Windows/Linux) for development and third-party use. |

## Active Epics

- [00 Infrastructure] [TODO-01 AI Development System](./00-infrastructure/TODO-01-ai-development-system.md) - Establish Cursor as the canonical agent layer, define MCP policy, and replace prompt-heavy TODO procedure with reusable skills.
- [00 Infrastructure] [TODO-02 Developer Tooling Stack](./00-infrastructure/TODO-02-developer-tooling-stack.md) - Define the parent roadmap for the git-tracked developer tooling contract, with build, run, host utilities, and GitHub sync kept aligned.
- [03 Memory Concurrency] [TODO-01 VMM Memory Protection & Diagnostics](./03-memory-concurrency/TODO-01-vmm-memory-protection.md) - Harden the memory model with mprotect, W^X enforcement, demand paging, VirtualAlloc Win32 wrappers, kmalloc lint, heap canaries, leak detector, and PMM statistics.

## Current State

- The scaffold is in place and the first two infrastructure epics have been added.
- No legacy TODOs have been migrated into the new tree yet.
- Historical reference remains in [todo-old](../todo-old/).
