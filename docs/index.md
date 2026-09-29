# Impossible OS Documentation

Welcome to the Impossible OS documentation -- a 64-bit operating system built from scratch for modern x86-64 hardware.

## 📖 Categories

| Category                                          | Description                                                |
| ------------------------------------------------- | ---------------------------------------------------------- |
| [🥾 Boot Platform](boot/index.md)                 | UEFI loader, boot_info handoff, boot entries, CPU bringup  |
| [🧠 Kernel](kernel/index.md)                     | Boot chain, memory management, scheduler, IPC              |
| [🧮 Memory and Concurrency](memory/index.md)       | Virtual memory, heap, paging, scheduler, SMP, sync, IPC     |
| [💾 Storage](storage/index.md)                    | Storage controllers, partitioning, filesystems             |
| [🌐 Networking](networking/index.md)              | Network drivers and protocols                              |
| [🖥️ Desktop](desktop/index.md)                   | Window manager, compositor, input, controls, desktop shell |
| [🎨 Graphics](graphics/index.md)                  | 2D drawing, text, themes, animation, widgets, accessibility |
| [🧩 Platform Services](services/index.md)         | Win32 and Linux programs, SDK, audio, updates, recovery, installer |
| [📦 Applications](apps/index.md)                  | Browser, mail, viewers, Notepad, Calculator, accessories, system tools |
| [🛠️ SDK and User Platform](sdk/index.md)          | Libraries, environment ABI, modules, ntdll, Win32 subsystem, SDK, compatibility |
| [🚢 Release and Installation](release/index.md)     | Release artifacts, unattended install, updates, release QA, community launch |
| [⚙️ Hardware](hardware/index.md)                 | CPU architecture, bus protocols, firmware, interrupts       |
| [🔧 Infrastructure](infrastructure/index.md)      | Build system, CI/CD, tooling                               |
| [🚀 Getting Started](getting-started/index.md)    | Setup guides and emulator configuration                    |
| [✍️ Contributing to the Docs](contributing/index.md) | Page contract, template and folder map for docs pages     |
| [📋 Specs](../specs/index.md)                        | External reference specifications                          |

## 🗺️ Quick Links

| I want to...                    | Go to                                                                         |
| ------------------------------- | ----------------------------------------------------------------------------- |
| Build and run the OS            | [Getting Started → QEMU](getting-started/qemu.md)                            |
| Understand the build system     | [Infrastructure → Development Tooling](infrastructure/development-tooling.md) |
| Set up CI/CD                    | [Infrastructure → GitHub Setup](infrastructure/github-setup.md)               |
| Read a hardware spec            | [Specs](../specs/index.md)                                                       |
| Contribute to the project       | [CONTRIBUTING.md](https://github.com/rizonesoft/impossible-os/blob/main/CONTRIBUTING.md) |
| Write a docs page               | [Documentation Page Contract](contributing/docs-page-contract.md)              |
| Find work items                 | [TODO Index](https://github.com/rizonesoft/impossible-os/blob/main/todo/TODO-00-INDEX.md) |
