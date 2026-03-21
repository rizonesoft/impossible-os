# Impossible OS Documentation

> Complete documentation wiki for Impossible OS — architecture, infrastructure, guides, and reference specs.

## Categories

| Category                                 | Description                                             |
| ---------------------------------------- | ------------------------------------------------------- |
| [Architecture](architecture/Index.md)    | OS internals: kernel, drivers, filesystem, boot, desktop |
| [Infrastructure](infrastructure/Index.md) | Build system, CI/CD, tooling, development environment   |
| [Getting Started](getting-started/Index.md) | Setup guides, emulator configuration                  |
| [Specs](specs/Index.md)                  | External reference specifications                       |

## Topic Ownership Rules

Each concept is explained in **exactly one** canonical document. Other docs link to it instead of re-explaining.

### How to Find the Right Doc

1. Check the **Index.md** in the relevant category folder
2. Look at the **Owned Topics** column — each topic belongs to one doc
3. If your topic is already owned, **link to it** — don't duplicate the explanation
4. If no doc owns the topic, create a new doc and register it in the Index.md

### What Counts as Duplication

- ❌ Explaining how the build system works in both `development-tooling.md` and `github-setup.md`
- ✅ `github-setup.md` saying "Build CI runs `build.sh` — see [Development Tooling](infrastructure/development-tooling.md#build-script) for details"
- ❌ Describing AHCI register layouts in both the driver doc and the spec
- ✅ Driver doc linking to `[AHCI 1.3.1 Spec](../specs/storage/ahci-1.3.1.md#register-layout)` for register details
