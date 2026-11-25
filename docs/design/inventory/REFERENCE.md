<!-- docs: sources=docs/design/inventory/reference.json,scripts/inventory/update-iso.ps1,scripts/inventory/ref-vm.ps1,scripts/inventory/ref-run.ps1,scripts/inventory/ref-host.ps1,scripts/inventory/host-capture.py,scripts/inventory/operator-capture.py,scripts/inventory/operator-host.ps1,scripts/inventory/window-capture.cs,scripts/inventory/host,scripts/inventory/ref-common.ps1,scripts/inventory/guest reviewed=2026-11-25T18:00 -->
# Windows 11 Reference Machine

Every record in the inventory is read from one machine: a clean Hyper-V virtual machine running one pinned Windows 11 build under one capture profile. A position or a size only means something against that machine, so this page names it, and a capture from anything else is refused until it is recorded here first. What a capture may take from it, facts and never Microsoft's text, images or files, is the rule in [the inventory format](index.md#what-does-it-never-hold).

## Which build is the reference?

| Field          | Value                                                                                                        |
| -------------- | ------------------------------------------------------------------------------------------------------------ |
| Edition        | Windows 11 Pro (`Professional`, image index 6 of `win11.iso`, the only image of the updated ISO)             |
| Version        | 26H2                                                                                                         |
| Build          | 26300.9974 (`CurrentBuild`.`UBR`), the operator's host build                                                 |
| Read from      | the VM's Registry, `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion`, inside the signed-in session         |
| Read on        | 2026-11-16                                                                                                   |
| Installed from | `D:\11\win11-26300.9974.iso`: `D:\11\win11.iso` (en-US, 26300.9711) with KB5132061 added by `update-iso.ps1` |

The machine-readable copy is [`reference.json`](reference.json). `ref-vm.ps1` refuses to take or verify `clean`, and `ref-run.ps1` refuses to capture (exit 5), when the guest's edition, version or build differs from it. The values come from `ref-vm.ps1 -Verify`, which prints `ref-vm: ios-win11-ref ok, profile fhd-100 (Windows 11 Professional 26H2 build 26300.9974)` when the machine matches.

## What is the rule for a new build?

Every captured record carries the build it was read from in `capturedFrom`, as `26300.9974`. A capture from any other build is a new reference. The reference follows the operator's host build, because the host captures (Bluetooth, camera, graphics, Wi-Fi) must share it with the VM's.

A new build goes into the install image, never into a running machine, so `pre-oobe` and every setup page are the new build too:

1. Download the build's cumulative update from the Microsoft Update Catalog; the file name carries its SHA-1. Only the cumulative update: the catalog also lists the 24H2 checkpoint update (KB5043080), which a 26300 image refuses with `0x80070228`.
2. `update-iso.ps1 -Package <msu> -Out D:\11\win11-<build>.iso` services image 6 inside the reference VM (DISM needs elevation, which the guest has and the host shell does not), splits it into `install.swm` parts and builds the ISO with `xorriso` in WSL. About two and a half hours, most of it DISM. The serviced parts stay in `D:\11\update-iso\parts` with `parts.json` naming their inputs, so a run that fails while building the ISO is run again in about 15 minutes, without servicing again; one run at a time per work folder and VM (a second exits 4).
3. Record the new build in `reference.json` and on this page, then build the machine from the new ISO with `-ImageIndex 1`. To keep the old machine until the new one verifies, build it beside it (`-Name ios-win11-ref-next -VmPath D:\11\ios-win11-ref-next`), then remove the old VM, `Rename-VM` the new one, `Move-VMStorage` it to `D:\11\ios-win11-ref` and move `ref.clixml`, `answer-install.iso`, `pre-oobe.png` and `ref-vm.log` after it by hand. `Move-VMStorage` carries only Hyper-V's own files, and `ref.clixml` is the only copy of the `ref` password: a store move on 2026-11-19 left it behind and the machine had to be rebuilt. Moving the whole store is the same: move every file in the folder, not only the VM.
4. `-Derive -Profile machine`, rebuild the stand-in disk (`standin/derive.ps1`), regenerate `ms-hashes.txt` (`make-ms-hashes.ps1`) and read the baselines again (`baselines.py`); only then capture.

Records from two builds never share a surface file, and a record is never "updated" to a new build without being captured again.

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

| Profile                                                                 | Differs from `fhd-100` by                             | Kind     |
| ----------------------------------------------------------------------- | ----------------------------------------------------- | -------- |
| `fhd-125`, `fhd-150`, `fhd-175`                                         | scaling 125, 150 or 175 percent (120, 144 or 168 DPI) | live     |
| `fhd-100-dark`                                                          | dark mode for apps and system                         | live     |
| `fhd-100-aquatic`, `fhd-100-desert`, `fhd-100-dusk`, `fhd-100-nightsky` | the contrast theme of that Settings name              | live     |
| `fhd-100-text150`                                                       | text size 150 percent                                 | live     |
| `qhd-200`                                                               | 2560 by 1440 at 200 percent                           | machine  |
| `fhd-100-rtl`                                                           | display language `ar-SA`, right to left               | machine  |
| `wide-100`, `tall-100`                                                  | a 2560 by 1080 or 1080 by 1920 display                | machine  |
| `dual-100-150`, `dual-150-100`                                          | two displays at 100 and 150 percent, then swapped     | stand-in |

**Live** profiles are applied in `ref`'s session from `clean` by `guest/set-profile.ps1`, each setting through the path Windows itself uses: scaling through the logical DPI override (Hyper-V's display refuses the per-display scale call), a contrast theme through its theme file, light and dark through the Personalize values, and text size through the Settings slider and its Apply button (the registry value alone changes no font). A contrast theme sets the light values itself, so those profiles do not list them. `ref-vm.ps1 -Verify -Profile live` applies and probes every live profile in turn; `capture.py --profile` applies its profile before it checks it.

**Machine** profiles need a changed VM (another resolution or another display language). Each is a checkpoint `profile-<id>` derived from `clean` by `ref-vm.ps1 -Derive -Profile <id>` (or `-Profile machine` for all of them), and a capture under it restores that checkpoint instead of `clean`. A resolution is the VM's video mode (`Set-VMVideo`, set with the guest off). The Arabic display language is the `ar-SA` language pack, installed through a network adapter on the Default Switch that exists only while it installs, with automatic and Store updates held off by policy for that time; the build is checked after, so a derived machine is the same build as `clean`. Each derived machine then gets its live values from `set-profile.ps1`, and the whole profile is probed before the checkpoint is taken. `clean` has OneDrive not installed with its first-run setup pending; captures from `clean` never sign in again, but a derived machine restarts, so the pending entry is removed first and the derived machine keeps `clean`'s software. After that cold sign-in no window has focus, so Win+D gives it to the desktop, the state `clean` starts in. 200 percent is a machine profile because Windows offers at most 175 percent at 1920 by 1080 (measured 2026-11-16).

**Stand-in** profiles need two displays, and Hyper-V gives a VM one, so they run on the stand-in machine's `dual` variant (below): two virtio GPU heads side by side. `dual-100-150` has the main display at 100 percent with the second one, at 150 percent, on its right. `dual-150-100` is the same pair after Windows' "Make this my main display" on the right-hand one: the main display is now at 150 percent and the old main display sits on its left. `set-profile.ps1` applies both values in the session. It first extends the desktop over both displays, as Win+P "Extend" does, because on a first boot Windows may show the desktop on the first one only (it reports as an integrated panel). Then `secondSide` moves the main display with `SetDisplayConfig`, and each display's scale (`dpi` for the main one, `secondDpi` for the other) is set through the per-display scale call, as Settings sets each display. The virtio display driver accepts that call where Hyper-V's refuses it, and the logical DPI override the live profiles use would move both displays at once. `capture.py` picks the variant from the profile's display count.

`ref-vm.ps1 -Verify -Profile all` checks every profile: the live ones in one session from `clean`, each machine profile on its own checkpoint, and each stand-in profile through `standin/probe.py`, which applies the profile on the stand-in machine and returns the probe's line for the same comparison.

Every captured record and observation names its `session`: `console`, the machine's own console session, which is where every capture runs (the reference VM's or the stand-in machine's). A capture in a remote session is refused, because inside the guest an enhanced session cannot be told apart from a remote one.

Windows stores a contrast theme under its pre-Win11 name (Aquatic is `High Contrast #1`, Desert `High Contrast #2`, Dusk `High Contrast Black`, Night sky `High Contrast White`); the probe reports the Settings name.

An observation captured under a profile other than `fhd-100` goes in a `<surface>/<file>.<profile>.json` file, as the [inventory format](index.md) describes.

## Which fixtures prepare a capture?

A fixture is a guest script in `scripts/inventory/fixtures/` that puts made-up content where a surface shows it, so a list's rows, a notification's card and their overflow are recorded while every payload is fictional. `capture.py --prepare <name>:<variant>` runs it in `ref`'s session before the walk, and an `expected.json` row names the fixture it needs. Each script removes everything it owns before it adds a variant's entries, so running it twice leaves the same state; `fixtures/run.py --twice all` proves that on a restored `clean` for every variant.

| Fixture | Variants | What it adds |
| --- | --- | --- |
| `lists` | `empty`, `populated` (4), `overflowing` (40) | files in `Documents\Sample folder`, per-user uninstall entries (Installed apps), printers on the inbox PDF driver each on its own port (Printers & scanners), per-user VPN connections, local users in Users (Other users) |
| `notifications` | `groups` (3 a sender), `overflow` (12 a sender) | toasts from three registered sample senders, with fixed tags and a fixed `displayTimestamp`, posted straight to the notification centre |
| `services` | `update-checking`, `update-failed` | Windows Update pointed at a local port for one scan: one that holds the connection open and never answers, or one nothing listens on; the policy is removed after the scan, so the page shows no managed-by line, and no update is ever delivered |

`empty` removes only the fixture's own entries and keeps `Sample folder`, empty, for File Explorer to open; a list Windows fills itself (the inbox apps, Microsoft Print to PDF) still shows those. The clean checkpoint has Do not disturb on, which the notification centre shows at its top. The setup flow needs no fixture: the First-Run Setup capture takes it from the `pre-oobe` checkpoint, the update states that need a delivered update and the network and page states are the parity inventory's sections 25 and 26 ([the roadmap page](../../graphics/win11-parity-inventory.md)).

## What is the machine?

| Part | Value |
| --- | --- |
| VM | `ios-win11-ref`, Hyper-V Generation 2, Secure Boot (Microsoft Windows template), virtual TPM |
| Processors and memory | 4 vCPU, static 4 GiB |
| Disk | 80 GiB dynamic, `D:\11\ios-win11-ref\Virtual Hard Disks\disk.vhdx` (a moved VM keeps Hyper-V's folder layout) |
| Network | none: no adapter, so nothing updates the build or changes content between captures |
| Account | one local administrator, `ref`, with a random password kept only as DPAPI `Export-Clixml` beside the VM |
| Checkpoints | `pre-oobe` (the first setup page) and `clean` (the signed-in desktop under `fhd-100`) |

Install and setup run unattended. The setup pages are reached with no keyboard input: a watcher inside the guest reports them through Hyper-V key-value exchange, and the host asks the watcher to finish setup when it is time.

## How is it built, checked and used?

```powershell
powershell.exe -File scripts\inventory\ref-vm.ps1 -Rebuild    # install from the ISO: about 12 minutes
powershell.exe -File scripts\inventory\ref-vm.ps1 -Reclean    # clean again from pre-oobe, no reinstall
powershell.exe -File scripts\inventory\ref-vm.ps1 -Verify     # exits 0 when the machine matches this page
powershell.exe -File scripts\inventory\ref-run.ps1 -Proof -Out D:\11\out
```

`ref-run.ps1` takes a new, empty `-Out` folder for each run, holds the VM for itself, restores `clean`, checks the build, copies its inputs to `C:\ios\in` as one zip, runs one command in `ref`'s desktop (an interactive scheduled task under a headless console, so it draws no window of its own), waits for its exit code, copies `C:\ios\out` back and turns the VM off. Each capture therefore starts from the same desktop. `-Proof` runs the end-to-end check: it starts Notepad, finds its window through UI Automation and saves a screenshot. `pre-oobe` exists so first-run setup can be captured again at any time.

The machine and the tools are kept after the inventory is captured, so anything missed can be captured later from the same build.

## How does the reference machine share the host with the SDK's test machine?

Inventory takes turns. The host is short of memory, and Impossible SDK has a 4 GB machine of its own, `ui-test-win11`, which shares no disk, checkpoint, lock or password with this one. Before any script here starts the VM it takes the one place of Bosun's `Bosun.VM` queue, the place the SDK's jobs take too (`Enter-RefTurn` in `ref-common.ps1`, which runs `bosun run --ns Bosun.VM --slots 1 --max-wait 360`), and frees it once the VM is off. A run that is killed frees it as well.

- A run waits up to 6 minutes, then starts anyway: inventory is never refused. `capture.py` and `host-capture.py` allow for the wait.
- Without Bosun, or when Bosun fails or does not answer within 20 seconds more, the run goes on without a place and says so. Bosun `8d5f318` or later is needed.
- The VM's own lock comes first and is as it was: a second run on this VM exits 4 at once. `ref-run.ps1 -Stop` never waits.
- A job a run starts under the same queue name passes straight through.
- A script given `ui-test-win11` as `-Name`, or a path inside a folder of that name, refuses (exit 2) and changes nothing. So does a `-Name` that is a pattern.
- A VM that a killed run left on is turned off before the next run waits.

## How are the screens UI Automation cannot see captured?

The lock and sign-in screens, the secure desktop (security options and the elevation prompt), and the boot, setup and recovery screens are captured from the host instead. `ref-host.ps1` restores a checkpoint and drives the VM through Hyper-V's own keyboard and mouse (keys, chords, text, Ctrl+Alt+Delete, absolute clicks), runs guest commands where a session exists, and shoots the framebuffer with `GetVirtualSystemThumbnailImage` at the video head's current mode, which is pixel for pixel. Each shot in its `shots.json` records the mode read just before and just after it; a shot whose mode changed under it is not exact. It can make fixture accounts whose random passwords exist only in its memory, and types `ref`'s password from the DPAPI-protected credential beside the VM (`ref.clixml`) without writing it anywhere.

```bash
python3 scripts/inventory/host-capture.py --scenario session-secure            # capture, measure, check, publish
python3 scripts/inventory/host-capture.py --scenario session-secure --dry-run  # capture and check only
```

A scenario in `scripts/inventory/host/` lists the steps and the shots it publishes, and measures its records from those shots: it looks for the pieces of ink it expects in a region and refuses when it finds a different number, so a screen that changed is never published with stale geometry. Every record is a `hand:` record. `host-capture.py` refuses a run with a failed step, a missing shot or one that is not exact, then checks and publishes exactly as `capture.py` does, under the same lock and into the same store and manifest.

First-run setup is captured the same way from the `pre-oobe` checkpoint (`--scenario session-setup`). No account exists there, so the scenario sets `SESSION = False` and `ref-host.ps1 -NoSession` drives the screen with keys and clicks only. The VM has no network adapter, so the walk opens the local account form through Shift+F10 and `start ms-cxh:localonly`; the scenario's `PAGES` names the local-account path, and the capture refuses shots that show another page order. A checkpoint taken while the VM was built in its `-next` folder still names that folder's media in its saved state, so every restore attaches the media again from the VM's own folder.

How do we know a host shot is the guest's screen pixel for pixel? The boot scenario (`--scenario session-boot`) begins with a proof: the guest takes its own screenshot of the desktop, the host shoots the same desktop, `ref-host.ps1`'s `fetch` step copies the guest's file out, and `host-capture.py` refuses the run unless at least 99.9% of pixels agree within 8 on every channel, the rounding of the host path's 16-bit colour. The first proof, on 2026-11-18, failed and found a real fault: Hyper-V puts 4 bytes before the pixels, and reading from byte 0 had shifted every host shot 2 pixels right. With those bytes skipped, every pixel agrees within 7. The same 4 bytes come before the pixels at 1024 by 768 and 640 by 360, and `Save-Thumbnail` refuses any other length. Each run's `shots.json` names the revision of that reading (`thumbnail: 2`), and `host-capture.py` refuses shots from any other, so a `--from` replay of a run from before the fix cannot republish shifted geometry. A run folder also holds `build.json`, the reference build its checkpoint was, and a replay of another build's shots is refused. A walk with a session checks the guest's build and records it in the VM's notes (`ios-build=<build>`); a walk without one (the setup pages from `pre-oobe`) checks those notes instead, so a VM not yet verified as the inventory's build is refused, and a rebuilt VM starts with none. The lock, sign-in and setup files were captured again after the fix. Each published host shot's manifest entry records its output size (`width`, `height`), the mode it was taken from (`sourceWidth`, `sourceHeight`) and its `depth`, and whether it is `exact`. Only the numbered frames of a burst across a restart may be inexact, and no geometry is measured from them.

The boot walk restores `clean`, adds a Safe Mode boot entry so the boot manager shows its menu, sets a small memory dump with a stop error on NMI, and restarts. It walks the boot logo, the menu and its options page, the recovery environment (Choose an option, Troubleshoot, Reset this PC, Advanced options), Startup Settings and its numbered menu, Safe mode by its number, a stop error raised with `Debug-VM -InjectNonMaskableInterrupt`, and, from the menu the crash restarts into, a Startup Repair that fails without a network and opens More recovery options. Every host run ends with its checkpoint restored and the VM not running, on a failed step too, so what a walk changed does not outlive it; a run killed at its deadline or by Ctrl+C is restored the same way by `ref-run.ps1 -Stop -Checkpoint <name>`, with the checkpoint the run started from. Both visits to the boot menu end their burst at the first frame with the menu's blue (`until`), since its 30-second countdown is the walk's only window. On the first visit that exact frame is the boot-manager shot (`as`), so the first key goes in at once. A burst that ended without one waits for the blue with `awaitcolour` and shoots the menu then. The Startup Settings menu is waited for the same way, by its blue in the firmware's mode (the page before it is blue at the display's), and the repair failure page as the first screen that stays still after the repair's spinner, inside one burst that keeps frames until then; that still exact frame is the failure page's shot. Choose other options opens recovery without a restart, so the walk waits for that page to stop changing. The boot and stop bursts watch for a set time, each shot starting at least a second after the one before, so the host's shot speed does not change how long they watch; a slower host keeps fewer frames, never more than the burst's count. No host call starts after its deadline: a burst's frame still waiting for the VM at the burst's end is dropped and the burst ends, a shot that gets no frame within 15 s fails, and a colour wait's or settle's poll stops at the wait's timeout. Safe mode is reached by waiting for its light taskbar at the basic driver's 1024x768, since the black restart before it holds still for longer than a settle allows. The walk drives recovery with keys only (arrows, Enter and Backspace), and each record credits only the keys the walk sends on its own page, read from the walk's steps. The firmware logo, Startup Settings, Safe mode and the stop error draw in the firmware's or the basic display driver's mode (1024 by 768 here), which no profile governs, so their records give positions as fractions of that screen and carry no `fhd-100` bounds.

## How are the hardware pages captured?

Hyper-V shows no battery, lid, backlight, touch screen or second display, so the Settings pages for them are captured on a stand-in machine: the `clean` disk booted under QEMU on the WSL host with a laptop's devices described in ACPI, or with two displays. No physical laptop or monitor is used.

| Part | Value |
| --- | --- |
| Disk | `clean` merged into one VHDX by `standin/derive.ps1`, converted to qcow2 at `~/.local/share/ios-standin/standin.qcow2`; each run writes to a throwaway overlay. The derive also stages the virtio GPU display driver (`viogpudo`) in the driver store, from the virtio-win 0.1.302 ISO pinned by SHA-256 in `standin/drivers.json` (BSD-3-Clause, fetched, not redistributed) |
| Machine | QEMU q35 under KVM, OVMF without Secure Boot, AHCI, xHCI with a USB keyboard and tablet, the VGA display at 1920 by 1080 with EDID; the `dual` variant has two virtio GPU heads at 1920 by 1080 instead, which `viogpudo` drives as two displays |
| Devices | `standin/standin.asl`: an AC adapter, a 50 Wh battery, a lid, and backlight control on the display (`_BCL`, `_BCM`, `_BQC`); Windows binds its inbox drivers |
| Variants | `standin/variants.json`: `charging` (60 percent on AC), `discharging` (60 percent on battery), `saver` (15 percent, below the 30 percent Energy saver threshold), and `dual` (two displays, no laptop devices, so its taskbar is the reference VM's) |
| Jobs | a logon task in `ref`'s session gives the desktop focus (Win+D, the state `clean` starts in), runs `ios-job\job.ps1` from a removable FAT volume labelled IOSJOB, writes its exit code, and shuts the guest down |

```bash
powershell.exe -File scripts\inventory\standin\derive.ps1 -Out D:\11\standin\standin.vhdx
qemu-img convert -f vhdx -O qcow2 /mnt/d/11/standin/standin.vhdx ~/.local/share/ios-standin/standin.qcow2
python3 scripts/inventory/standin/prove.py                        # proves every stand-in, records each proof
python3 scripts/inventory/standin/probe.py dual-100-150           # applies a two-display profile there, prints the probe
python3 scripts/inventory/capture.py --surface settings --file battery --standin saver   # once the Settings walker exists
```

`docs/design/inventory/stand-ins.json` lists each stand-in with its variant, the page that shows it, a marker (the name of an element only that page shows) and what the guest must report. `prove.py` opens each page once, takes its screenshot when the marker shows, checks that report (power status, a working device, or a monitor brightness reading) and keeps the page's screenshot in `D:\11\ref-shots\stand-ins\` with its SHA-256; a `stand-in` row of `expected.json` names its stand-in, and `inventory-report.py --campaign --check` refuses one without a proof. `capture.py --standin VARIANT` runs a capture on the stand-in machine exactly as `ref-run` runs it on the VM; the Settings walker it needs is written by the [Settings inventory](../../../todo/08-graphics-ui/TODO-17-win11-parity-inventory.md) (section 10), so today only `prove.py` runs there.

Touch, pen, touchpad, rotation lock and tablet posture have no stand-in: QEMU's virtio multitouch device does not start under the virtio-win 0.1.302 input driver (code 10, `0xC000009A`), and no emulated pen digitizer, precision touchpad, orientation sensor or slate-mode indicator has a Windows driver. Those rows, with fingerprint and face enrolment, are `absent`: each names its reason and its sibling: the page, captured by the Settings sections, that it is designed from.

The Settings pages that need real hardware the VM lacks (Bluetooth, Cameras, Graphics, Wi-Fi) are read on the operator's own machine at the reference build: `python3 scripts/inventory/operator-capture.py` runs `operator-host.ps1` in the operator's session, which gives the session the profile's light theme and blue accent for the capture only and puts the operator's own back afterwards (a capture cut off by a kill or a restart leaves a pending-restore file that the next run replays first). The records are `hand:<build>` in `settings/host.json` with one shot per page. What names the operator never leaves the private run folder: the account header, the machine name, the user's profile path, saved Wi-Fi network names, the taskbar and each list entry's own texts (devices, apps, adapters) are blanked in the shots, a list keeps only its first entry, labelled `Entry`, as its template, and a record naming any of them is refused. Each shot is the Settings window's own pixels, taken through Windows Graphics Capture (`scripts/inventory/window-capture.cs`, compiled on the host at the first run), so a notification or any other window over Settings cannot be in it. `--dry-run` captures and checks without publishing, and `--from <run folder>` publishes a run read at the reference build.

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
| `known-folders` | `{31c0dd25-9439-4f12-bf41-7ff4eda38722}` | 26300.9974 | 3D Objects: The shell namespace resolves `shell:3D Objects` to no folder on the reference account's profile. |
| `known-folders` | `{b2c5e279-7add-439f-b28c-c41fe1bbf672}` | 26300.9974 | AppDataDesktop: The shell namespace resolves `shell:AppDataDesktop` to no folder on the reference account's profile. |
| `known-folders` | `{7be16610-1f7f-44ac-bff0-83e15f2ffca1}` | 26300.9974 | AppDataDocuments: The shell namespace resolves `shell:AppDataDocuments` to no folder on the reference account's profile. |
| `known-folders` | `{7cfbefbc-de1f-45aa-b843-a542ac536cc9}` | 26300.9974 | AppDataFavorites: The shell namespace resolves `shell:AppDataFavorites` to no folder on the reference account's profile. |
| `known-folders` | `{559d40a3-a036-40fa-af61-84cb430a4d34}` | 26300.9974 | AppDataProgramData: The shell namespace resolves `shell:AppDataProgramData` to no folder on the reference account's profile. |
| `known-folders` | `{7ad67899-66af-43ba-9156-6aad42e6c596}` | 26300.9974 | AppMods: The shell namespace resolves `shell:AppMods` to no folder on the reference account's profile. |
| `known-folders` | `{ab5fb87b-7ce2-4f83-915d-550846c9537b}` | 26300.9974 | Camera Roll: The shell namespace resolves `shell:Camera Roll` to no folder on the reference account's profile. |
| `known-folders` | `{2b20df75-1eda-4039-8097-38798227d5b7}` | 26300.9974 | CameraRollLibrary: The shell namespace resolves `shell:CameraRollLibrary` to no folder on the reference account's profile. |
| `known-folders` | `{edc0fe71-98d8-4f4a-b920-c8dc133cb165}` | 26300.9974 | Captures: The shell namespace resolves `shell:Captures` to no folder on the reference account's profile. |
| `known-folders` | `{a440879f-87a0-4f7d-b700-0207b966194a}` | 26300.9974 | Common Start Menu Places: The shell namespace resolves `shell:Common Start Menu Places` to no folder on the reference account's profile. |
| `known-folders` | `{dbe8e08e-3053-4bbc-b183-2a7b2b191e59}` | 26300.9974 | Development Files: The shell namespace resolves `shell:Development Files` to no folder on the reference account's profile. |
| `known-folders` | `{2a00375e-224c-49de-b8d1-440df7ef3ddc}` | 26300.9974 | LocalizedResourcesDir: The shell namespace resolves `shell:LocalizedResourcesDir` to no folder on the reference account's profile. |
| `known-folders` | `{98ec0e18-2098-4d44-8644-66979315a281}` | 26300.9974 | MAPIFolder: The shell namespace resolves `shell:MAPIFolder` to no folder on the reference account's profile. |
| `known-folders` | `{c1bae2d0-10df-4334-bedd-7aa20b227a9d}` | 26300.9974 | OEM Links: The shell namespace resolves `shell:OEM Links` to no folder on the reference account's profile. |
| `known-folders` | `{767e6811-49cb-4273-87c2-20f355e1085b}` | 26300.9974 | OneDriveCameraRoll: The shell namespace resolves `shell:OneDriveCameraRoll` to no folder on the reference account's profile. |
| `known-folders` | `{24d89e24-2f19-4534-9dde-6a6671fbb8fe}` | 26300.9974 | OneDriveDocuments: The shell namespace resolves `shell:OneDriveDocuments` to no folder on the reference account's profile. |
| `known-folders` | `{c3f2459e-80d6-45dc-bfef-1f769f2be730}` | 26300.9974 | OneDriveMusic: The shell namespace resolves `shell:OneDriveMusic` to no folder on the reference account's profile. |
| `known-folders` | `{339719b5-8c47-4894-94c2-d8f77add44a6}` | 26300.9974 | OneDrivePictures: The shell namespace resolves `shell:OneDrivePictures` to no folder on the reference account's profile. |
| `known-folders` | `{2c36c0aa-5812-4b87-bfd0-4cd0dfb19b39}` | 26300.9974 | Original Images: The shell namespace resolves `shell:Original Images` to no folder on the reference account's profile. |
| `known-folders` | `{69d2cf90-fc33-4fb7-9a0c-ebb0f0fcb43c}` | 26300.9974 | PhotoAlbums: The shell namespace resolves `shell:PhotoAlbums` to no folder on the reference account's profile. |
| `known-folders` | `{de92c1c7-837f-4f69-a3bb-86e631204a23}` | 26300.9974 | Playlists: The shell namespace resolves `shell:Playlists` to no folder on the reference account's profile. |
| `known-folders` | `{2f8b40c2-83ed-48ee-b383-a1f157ec6f9a}` | 26300.9974 | Recorded Calls: The shell namespace resolves `shell:Recorded Calls` to no folder on the reference account's profile. |
| `known-folders` | `{1a6fdba2-f42d-4358-a798-b74d745926c5}` | 26300.9974 | RecordedTVLibrary: The shell namespace resolves `shell:RecordedTVLibrary` to no folder on the reference account's profile. |
| `known-folders` | `{aaa8d5a5-f1d6-4259-baa8-78e7ef60835e}` | 26300.9974 | Roamed Tile Images: The shell namespace resolves `shell:Roamed Tile Images` to no folder on the reference account's profile. |
| `known-folders` | `{3b193882-d3ad-4eab-965a-69829d1fb59f}` | 26300.9974 | SavedPictures: The shell namespace resolves `shell:SavedPictures` to no folder on the reference account's profile. |
| `known-folders` | `{e25b5812-be88-4bd9-94b0-29233477b6c3}` | 26300.9974 | SavedPicturesLibrary: The shell namespace resolves `shell:SavedPicturesLibrary` to no folder on the reference account's profile. |
| `known-folders` | `{b7bede81-df94-4682-a7d8-57a52620b86f}` | 26300.9974 | Screenshots: The shell namespace resolves `shell:Screenshots` to no folder on the reference account's profile. |
| `known-folders` | `{0d4c3db6-03a3-462f-a0e6-08924c41b5d4}` | 26300.9974 | SearchHistoryFolder: The shell namespace resolves `shell:SearchHistoryFolder` to no folder on the reference account's profile. |
| `known-folders` | `{7e636bfe-dfa9-4d5e-b456-d7b39851d8a9}` | 26300.9974 | SearchTemplatesFolder: The shell namespace resolves `shell:SearchTemplatesFolder` to no folder on the reference account's profile. |
| `known-folders` | `{5cd7aee2-2219-4a67-b85d-6c9ce15660cb}` | 26300.9974 | UserProgramFiles: The shell namespace resolves `shell:UserProgramFiles` to no folder on the reference account's profile. |
| `known-folders` | `{bcbd3057-ca5c-4622-b42d-bc56db0ae516}` | 26300.9974 | UserProgramFilesCommon: The shell namespace resolves `shell:UserProgramFilesCommon` to no folder on the reference account's profile. |
| `shell-commands` | `folder:3D Objects` | 26300.9974 | The shell namespace resolves `shell:3D Objects` to no folder on the reference account's profile. |
| `shell-commands` | `folder:AppDataDesktop` | 26300.9974 | The shell namespace resolves `shell:AppDataDesktop` to no folder on the reference account's profile. |
| `shell-commands` | `folder:AppDataDocuments` | 26300.9974 | The shell namespace resolves `shell:AppDataDocuments` to no folder on the reference account's profile. |
| `shell-commands` | `folder:AppDataFavorites` | 26300.9974 | The shell namespace resolves `shell:AppDataFavorites` to no folder on the reference account's profile. |
| `shell-commands` | `folder:AppDataProgramData` | 26300.9974 | The shell namespace resolves `shell:AppDataProgramData` to no folder on the reference account's profile. |
| `shell-commands` | `folder:AppMods` | 26300.9974 | The shell namespace resolves `shell:AppMods` to no folder on the reference account's profile. |
| `shell-commands` | `folder:Camera Roll` | 26300.9974 | The shell namespace resolves `shell:Camera Roll` to no folder on the reference account's profile. |
| `shell-commands` | `folder:CameraRollLibrary` | 26300.9974 | The shell namespace resolves `shell:CameraRollLibrary` to no folder on the reference account's profile. |
| `shell-commands` | `folder:Captures` | 26300.9974 | The shell namespace resolves `shell:Captures` to no folder on the reference account's profile. |
| `shell-commands` | `folder:Common Start Menu Places` | 26300.9974 | The shell namespace resolves `shell:Common Start Menu Places` to no folder on the reference account's profile. |
| `shell-commands` | `folder:Development Files` | 26300.9974 | The shell namespace resolves `shell:Development Files` to no folder on the reference account's profile. |
| `shell-commands` | `folder:LocalizedResourcesDir` | 26300.9974 | The shell namespace resolves `shell:LocalizedResourcesDir` to no folder on the reference account's profile. |
| `shell-commands` | `folder:MAPIFolder` | 26300.9974 | The shell namespace resolves `shell:MAPIFolder` to no folder on the reference account's profile. |
| `shell-commands` | `folder:OEM Links` | 26300.9974 | The shell namespace resolves `shell:OEM Links` to no folder on the reference account's profile. |
| `shell-commands` | `folder:OneDriveCameraRoll` | 26300.9974 | The shell namespace resolves `shell:OneDriveCameraRoll` to no folder on the reference account's profile. |
| `shell-commands` | `folder:OneDriveDocuments` | 26300.9974 | The shell namespace resolves `shell:OneDriveDocuments` to no folder on the reference account's profile. |
| `shell-commands` | `folder:OneDriveMusic` | 26300.9974 | The shell namespace resolves `shell:OneDriveMusic` to no folder on the reference account's profile. |
| `shell-commands` | `folder:OneDrivePictures` | 26300.9974 | The shell namespace resolves `shell:OneDrivePictures` to no folder on the reference account's profile. |
| `shell-commands` | `folder:Original Images` | 26300.9974 | The shell namespace resolves `shell:Original Images` to no folder on the reference account's profile. |
| `shell-commands` | `folder:PhotoAlbums` | 26300.9974 | The shell namespace resolves `shell:PhotoAlbums` to no folder on the reference account's profile. |
| `shell-commands` | `folder:Playlists` | 26300.9974 | The shell namespace resolves `shell:Playlists` to no folder on the reference account's profile. |
| `shell-commands` | `folder:Recorded Calls` | 26300.9974 | The shell namespace resolves `shell:Recorded Calls` to no folder on the reference account's profile. |
| `shell-commands` | `folder:RecordedTVLibrary` | 26300.9974 | The shell namespace resolves `shell:RecordedTVLibrary` to no folder on the reference account's profile. |
| `shell-commands` | `folder:Roamed Tile Images` | 26300.9974 | The shell namespace resolves `shell:Roamed Tile Images` to no folder on the reference account's profile. |
| `shell-commands` | `folder:SavedPictures` | 26300.9974 | The shell namespace resolves `shell:SavedPictures` to no folder on the reference account's profile. |
| `shell-commands` | `folder:SavedPicturesLibrary` | 26300.9974 | The shell namespace resolves `shell:SavedPicturesLibrary` to no folder on the reference account's profile. |
| `shell-commands` | `folder:Screenshots` | 26300.9974 | The shell namespace resolves `shell:Screenshots` to no folder on the reference account's profile. |
| `shell-commands` | `folder:SearchHistoryFolder` | 26300.9974 | The shell namespace resolves `shell:SearchHistoryFolder` to no folder on the reference account's profile. |
| `shell-commands` | `folder:SearchTemplatesFolder` | 26300.9974 | The shell namespace resolves `shell:SearchTemplatesFolder` to no folder on the reference account's profile. |
| `shell-commands` | `folder:UserProgramFiles` | 26300.9974 | The shell namespace resolves `shell:UserProgramFiles` to no folder on the reference account's profile. |
| `shell-commands` | `folder:UserProgramFilesCommon` | 26300.9974 | The shell namespace resolves `shell:UserProgramFilesCommon` to no folder on the reference account's profile. |
