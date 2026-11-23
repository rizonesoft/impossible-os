<!-- docs: covers=todo/10-platform-services/TODO-13-program-needs.md sources=scripts/imports.py,todo/imports.json reviewed=2026-11-23 order=13 -->
# What Programs Need

## What is it?

Programs for Impossible OS are built beside it, on Windows first. This roadmap holds what they need of the OS: the Windows functions of [Impossible SDK](https://github.com/rizonesoft/ImpossibleSDK)'s tiers, and any engine or service a program declares.

## How does it work?

**Today.** `scripts/imports.py --needs` compares what the programs declare with the roadmap, and `--file-needs` writes a section for what is missing. No tier has been shown complete on the OS yet.

**Planned design.** Each tier has a section: console, window, documents, media, network, desktop. Each is done when Impossible SDK's own tests give the same results here and on Windows.

```mermaid
flowchart LR
    P[Program declares a need] --> N[imports.py names it]
    N --> S[Section in this roadmap]
    S --> B[Built in the OS]
```

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `scripts/imports.py --needs`, `--file-needs` | Shipped |
| The functions of each tier | Planned |

## How do I use it?

When Check 42 names a need, run `python3 scripts/imports.py --file-needs` and commit what it staged.

## Who owns what?

- [Win32 API surface](win32-api-surface.md) and the [PE loader](win32-pe-loader.md) build most functions; this roadmap shows that each is there.

## What is not implemented yet?

- Every section of [the roadmap](../../todo/10-platform-services/TODO-13-program-needs.md).

## How does it compare with Windows 11 and Linux?

Windows has the functions; Wine provides them on Linux. Neither keeps a list of what its programs need that the system itself checks.

## See also

- [What programs need roadmap](../../todo/10-platform-services/TODO-13-program-needs.md)
- [Imported programs and parts](../apps/imported-programs.md)
