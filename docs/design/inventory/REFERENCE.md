<!-- docs: sources=docs/design/inventory/reference.json,scripts/inventory/ref-vm.ps1,scripts/inventory/ref-run.ps1,scripts/inventory/ref-common.ps1,scripts/inventory/guest reviewed=2026-11-14 -->
# Windows 11 Reference Machine

Every record in the inventory is read from one machine: a clean Hyper-V virtual machine running one pinned Windows 11 build under one capture profile. A position or a size only means something against that machine, so this page names it, and a capture from anything else is refused until it is recorded here first. What a capture may take from it, facts and never Microsoft's text, images or files, is the rule in [the inventory format](index.md#what-does-it-never-hold).

## Which build is the reference?

| Field          | Value                                                                                                |
| -------------- | ---------------------------------------------------------------------------------------------------- |
| Edition        | Windows 11 Pro (`Professional`, image index 6 of the ISO)                                            |
| Version        | 26H2                                                                                                 |
| Build          | 26300.9711 (`CurrentBuild`.`UBR`)                                                                    |
| Read from      | the VM's Registry, `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion`, inside the signed-in session |
| Read on        | 2026-11-14                                                                                           |
| Installed from | `R:\11\win11.iso`, en-US, `install.esd`                                                              |

The machine-readable copy is [`reference.json`](reference.json). `ref-vm.ps1` refuses to take or verify `clean`, and `ref-run.ps1` refuses to capture (exit 5), when the guest's edition, version or build differs from it. The values come from `ref-vm.ps1 -Verify`, which prints `ref-vm: ios-win11-ref ok, profile fhd-100 (Windows 11 Professional 26H2 build 26300.9711)` when the machine matches.

## What is the rule for a new build?

Every captured record carries the build it was read from in `capturedFrom`, as `26300.9711`. A capture from any other build is a new reference: build the machine from the new image (`-Rebuild` installs it and stops before `clean`, because the build differs), record the new build in `reference.json` and on this page, run `-Reclean`, and only then capture. Records from two builds never share a surface file, and a record is never "updated" to a new build without being captured again.

## What is the capture profile?

`fhd-100` is the profile every canonical record is captured under:

| Setting                | Value                                                                       |
| ---------------------- | --------------------------------------------------------------------------- |
| Resolution             | 1920 by 1080, one monitor                                                   |
| Scaling                | 100 percent (96 DPI)                                                        |
| Display language       | en-US                                                                       |
| Regional format        | en-US                                                                       |
| Theme                  | light for apps and system (the Windows default)                             |
| Accent                 | the default, `FFD47800` (DWM `AccentColor`), not shown on Start and taskbar |
| Taskbar                | the default, centred                                                        |
| Focus at capture start | the bare desktop: no Start, Search or app window open                       |
| Session                | `ref`'s interactive console session, never session 0                        |

`ref-vm.ps1 -Verify` reads every value in this table from inside that session (resolution, monitor count and DPI through a per-monitor DPI-aware process) and fails on any difference; `clean` is only taken when the same check passes.

## Which other profiles exist?

Every profile is an entry of `profiles.json`, the one place its values live; this table describes them. A profile lists every probed value, so a capture under it checks all of them.

| Profile                                                                 | Differs from `fhd-100` by                             | Kind    |
| ----------------------------------------------------------------------- | ----------------------------------------------------- | ------- |
| `fhd-125`, `fhd-150`, `fhd-175`                                         | scaling 125, 150 or 175 percent (120, 144 or 168 DPI) | live    |
| `fhd-100-dark`                                                          | dark mode for apps and system                         | live    |
| `fhd-100-aquatic`, `fhd-100-desert`, `fhd-100-dusk`, `fhd-100-nightsky` | the contrast theme of that Settings name              | live    |
| `fhd-100-text150`                                                       | text size 150 percent                                 | live    |
| `qhd-200`                                                               | 2560 by 1440 at 200 percent                           | machine |
| `fhd-100-rtl`                                                           | display language `ar-SA`, right to left               | machine |
| `wide-100`, `tall-100`                                                  | a 2560 by 1080 or 1080 by 1920 display                | machine |
| `dual-100-150`, `dual-150-100`                                          | two displays, the second at the other scale           | machine |

**Live** profiles are applied in `ref`'s session from `clean` by `guest/set-profile.ps1`, each setting through the path Windows itself uses: scaling through the logical DPI override (Hyper-V's display refuses the per-display scale call), a contrast theme through its theme file, light and dark through the Personalize values, and text size through the Settings slider and its Apply button (the registry value alone changes no font). A contrast theme sets the light values itself, so those profiles do not list them. `ref-vm.ps1 -Verify -Profile live` applies and probes every live profile in turn; `capture.py --profile` applies its profile before it checks it.

**Machine** profiles need a changed VM (another resolution, a second display or another display language) and are built as derived machines by [Derived Reference Machines](../../graphics/win11-parity-inventory.md); `-Verify` refuses one by name on the `fhd-100` machine. 200 percent is a machine profile because Windows offers at most 175 percent at 1920 by 1080 (measured 2026-11-16).

Windows stores a contrast theme under its pre-Win11 name (Aquatic is `High Contrast #1`, Desert `High Contrast #2`, Dusk `High Contrast Black`, Night sky `High Contrast White`); the probe reports the Settings name.

An observation captured under a profile other than `fhd-100` goes in a `<surface>/<file>.<profile>.json` file, as the [inventory format](index.md) describes.

## Which fixtures prepare a capture?

A fixture is a guest script in `scripts/inventory/fixtures/` that puts made-up content where a surface shows it, so a list's rows, a notification's card and their overflow are recorded while every payload is fictional. `capture.py --prepare <name>:<variant>` runs it in `ref`'s session before the walk, and an `expected.json` row names the fixture it needs. Each script removes everything it owns before it adds a variant's entries, so running it twice leaves the same state; `fixtures/run.py --twice all` proves that on a restored `clean` for every variant.

| Fixture | Variants | What it adds |
| --- | --- | --- |
| `lists` | `empty`, `populated` (4), `overflowing` (40) | files in `Documents\Sample folder`, per-user uninstall entries (Installed apps), printers on the inbox PDF driver each on its own port (Printers & scanners), per-user VPN connections, local users in Users (Other users) |
| `notifications` | `groups` (3 a sender), `overflow` (12 a sender) | toasts from three registered sample senders, with fixed tags and a fixed `displayTimestamp`, posted straight to the notification centre |

`empty` removes only the fixture's own entries and keeps `Sample folder`, empty, for File Explorer to open; a list Windows fills itself (the inbox apps, Microsoft Print to PDF) still shows those. The clean checkpoint has Do not disturb on, which the notification centre shows at its top. The setup flow needs no fixture: the First-Run Setup capture takes it from the `pre-oobe` checkpoint, and the service states (update and network) are [Service-State Fixtures](../../graphics/win11-parity-inventory.md)' work.

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

## How are the hardware pages captured?

Hyper-V shows no battery, lid, backlight or touch screen, so the Settings pages for them are captured on a stand-in machine: the `clean` disk booted under QEMU on the WSL host with a laptop's devices described in ACPI. No physical laptop is used.

| Part | Value |
| --- | --- |
| Disk | `clean` merged into one VHDX by `standin/derive.ps1`, converted to qcow2 at `~/.local/share/ios-standin/standin.qcow2`; each run writes to a throwaway overlay |
| Machine | QEMU q35 under KVM, OVMF without Secure Boot, AHCI, xHCI with a USB keyboard and tablet, the VGA display at 1920 by 1080 with EDID |
| Devices | `standin/standin.asl`: an AC adapter, a 50 Wh battery, a lid, and backlight control on the display (`_BCL`, `_BCM`, `_BQC`); Windows binds its inbox drivers |
| Variants | `standin/variants.json`: `charging` (60 percent on AC), `discharging` (60 percent on battery), `saver` (15 percent, below the 30 percent Energy saver threshold) |
| Jobs | a logon task in `ref`'s session runs `ios-job\job.ps1` from a removable FAT volume labelled IOSJOB, writes its exit code, and shuts the guest down |

```bash
powershell.exe -File scripts\inventory\standin\derive.ps1 -Out R:\11\standin\standin.vhdx
qemu-img convert -f vhdx -O qcow2 /mnt/r/11/standin/standin.vhdx ~/.local/share/ios-standin/standin.qcow2
python3 scripts/inventory/standin/prove.py                        # proves every stand-in, records each proof
python3 scripts/inventory/capture.py --surface settings --file battery --standin saver
```

`docs/design/inventory/stand-ins.json` lists each stand-in with its variant, the page that shows it and what the guest must report. `prove.py` checks that report (power status, a working device, or a monitor brightness reading) and keeps the page's screenshot in `R:\11\ref-shots\stand-ins\` with its SHA-256; a `stand-in` row of `expected.json` names its stand-in, and `inventory-report.py --campaign --check` refuses one without a proof. `capture.py --standin VARIANT` runs a capture on the stand-in machine exactly as `ref-run` runs it on the VM.

Touch, pen, touchpad, rotation lock and tablet posture have no stand-in: QEMU's virtio multitouch device does not start under the virtio-win 0.1.302 input driver (code 10, `0xC000009A`), and no emulated pen digitizer, precision touchpad, orientation sensor or slate-mode indicator has a Windows driver. Those rows, with Wi-Fi, fingerprint and face enrolment, are `absent`: each names its reason and the captured sibling page it is designed from.

## Which records are not built as captured?

The [inventory format](index.md#how-are-decisions-recorded) defines the four dispositions. These rules say which one a record gets.

- **Cloud-tied records are `equivalent`.** Sign-in with a Microsoft account, the Store, cloud file sync, the assistant, the news feed, and the advertising and telemetry consent pages: Impossible OS builds its own version, or `excluded` with the reason where it has none.
- **Absent hardware is `equivalent`.** A surface that no stand-in in the virtual machine and no host can show is written by hand from the nearest captured sibling, with the component library's controls. The sibling is named where the absence is documented.
- **Names and places stay.** What opens a `replaced` flow keeps its captured name, place and order as a `build` record: the Control Panel name and its commands, and the four sign-in method entries.
- **Nothing is replaced on judgment.** A record is `replaced` only under a ledger entry that names two captured records which disagree.

## Which baseline rows are absent?

A row of a completeness baseline that the reference build lists but no surface shows (a setting gated off in this edition, a verb whose handler is not installed) is documented here, with the build the absence was checked on and the reason, instead of being matched by a captured record. `check-inventory.py` refuses a row listed here that a record also matches, a row that names nothing, and a row without a build or a reason. An absence checked on another build than the reference build counts nothing until it is checked again and its build updated.

| Baseline | Row | Build | Reason |
| -------- | --- | ----- | ------ |
