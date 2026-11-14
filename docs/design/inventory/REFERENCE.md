<!-- docs: sources=docs/design/inventory/reference.json,scripts/inventory/ref-vm.ps1,scripts/inventory/ref-run.ps1,scripts/inventory/ref-common.ps1,scripts/inventory/guest reviewed=2026-11-14 -->
# Windows 11 Reference Machine

Every record in the inventory is read from one machine: a clean Hyper-V virtual machine running one pinned Windows 11 build under one capture profile. A position or a size only means something against that machine, so this page names it, and a capture from anything else is refused until it is recorded here first. What a capture may take from it, facts and never Microsoft's text, images or files, is the rule in [the inventory format](index.md#what-does-it-never-hold).

## Which build is the reference?

| Field | Value |
| --- | --- |
| Edition | Windows 11 Pro (`Professional`, image index 6 of the ISO) |
| Version | 26H2 |
| Build | 26300.9711 (`CurrentBuild`.`UBR`) |
| Read from | the VM's Registry, `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion`, inside the signed-in session |
| Read on | 2026-11-14 |
| Installed from | `R:\11\win11.iso`, en-US, `install.esd` |

The machine-readable copy is [`reference.json`](reference.json). `ref-vm.ps1` refuses to take or verify `clean`, and `ref-run.ps1` refuses to capture (exit 5), when the guest's edition, version or build differs from it. The values come from `ref-vm.ps1 -Verify`, which prints `ref-vm: ios-win11-ref ok, profile fhd-100 (Windows 11 Professional 26H2 build 26300.9711)` when the machine matches.

## What is the rule for a new build?

Every captured record carries the build it was read from in `capturedFrom`, as `26300.9711`. A capture from any other build is a new reference: build the machine from the new image (`-Rebuild` installs it and stops before `clean`, because the build differs), record the new build in `reference.json` and on this page, run `-Reclean`, and only then capture. Records from two builds never share a surface file, and a record is never "updated" to a new build without being captured again.

## What is the capture profile?

`fhd-100` is the profile every canonical record is captured under:

| Setting | Value |
| --- | --- |
| Resolution | 1920 by 1080, one monitor |
| Scaling | 100 percent (96 DPI) |
| Display language | en-US |
| Regional format | en-US |
| Theme | light for apps and system (the Windows default) |
| Accent | the default, `FFD47800` (DWM `AccentColor`), not shown on Start and taskbar |
| Taskbar | the default, centred |
| Focus at capture start | the bare desktop: no Start, Search or app window open |
| Session | `ref`'s interactive console session, never session 0 |

`ref-vm.ps1 -Verify` reads every value in this table from inside that session (resolution, monitor count and DPI through a per-monitor DPI-aware process) and fails on any difference; `clean` is only taken when the same check passes.

## Which other profiles exist?

The further profiles are entries of the profile matrix that [Capture Matrix, Fixtures and Device Stand-ins](../../graphics/win11-parity-inventory.md) owns; their values and their `-Verify` checks are defined there, not here. The catalogues name two of them now:

- `fhd-100-dark`: `fhd-100` with dark mode for apps and system.
- `fhd-150`: `fhd-100` at 150 percent scaling (144 DPI).

An observation captured under one of them goes in a `<surface>/<file>.<profile>.json` file, as the [inventory format](index.md) describes.

## What is the machine?

| Part | Value |
| --- | --- |
| VM | `ios-win11-ref`, Hyper-V Generation 2, Secure Boot (Microsoft Windows template), virtual TPM |
| Processors and memory | 4 vCPU, static 4 GiB |
| Disk | 80 GiB dynamic, `R:\11\ios-win11-ref\disk.vhdx` |
| Network | none: no adapter, so nothing updates the build or changes content between captures |
| Account | one local administrator, `ref`, with a random password kept only as DPAPI `Export-Clixml` beside the VM |
| Checkpoints | `pre-oobe` (the first setup page) and `clean` (the signed-in desktop under `fhd-100`) |

Install and setup run unattended. The setup pages are reached with no keyboard input: a watcher inside the guest reports them through Hyper-V key-value exchange, and the host asks the watcher to finish setup when it is time.

## How is it built, checked and used?

```powershell
powershell.exe -File scripts\inventory\ref-vm.ps1 -Rebuild    # install from the ISO: about 12 minutes
powershell.exe -File scripts\inventory\ref-vm.ps1 -Reclean    # clean again from pre-oobe, no reinstall
powershell.exe -File scripts\inventory\ref-vm.ps1 -Verify     # exits 0 when the machine matches this page
powershell.exe -File scripts\inventory\ref-run.ps1 -Proof -Out R:\11\out
```

`ref-run.ps1` takes a new, empty `-Out` folder for each run, holds the VM for itself, restores `clean`, checks the build, copies its inputs to `C:\ios\in` as one zip, runs one command in `ref`'s desktop (an interactive scheduled task under a headless console, so it draws no window of its own), waits for its exit code, copies `C:\ios\out` back and turns the VM off. Each capture therefore starts from the same desktop. `-Proof` runs the end-to-end check: it starts Notepad, finds its window through UI Automation and saves a screenshot. `pre-oobe` exists so first-run setup can be captured again at any time.

The machine and the tools are kept after the inventory is captured, so anything missed can be captured later from the same build.

## Which baseline rows are absent?

A row of a completeness baseline that the reference build lists but no surface shows (a setting gated off in this edition, a verb whose handler is not installed) is documented here, with the build the absence was checked on and the reason, instead of being matched by a captured record. `check-inventory.py` refuses a row listed here that a record also matches, a row that names nothing, and a row without a build or a reason. An absence checked on another build than the reference build counts nothing until it is checked again and its build updated.

| Baseline | Row | Build | Reason |
| --- | --- | --- | --- |
