# 15 Installer & Release

This domain tracks how the OS is installed, packaged, delivered, and prepared for release.

## Belongs Here

- Installer flow, disk image deployment, first-boot setup, and release media creation.
- Packaging, shipping, and release-readiness work specific to distribution of the OS.
- Install or deployment behavior that should not be mixed with generic build tooling.

## Does Not Belong Here

- General build scripts and repo-wide tooling. Put that in [00 Infrastructure](../00-infrastructure/INDEX.md).
- Runtime feature work that belongs to a specific subsystem domain.

## Likely Source Areas

- [installer](../installer/)
- [scripts](../scripts/)
- [resources](../resources/)
- [build](../build/)

## Epics

- None yet.

## Active TODOs

- [`TODO-05-github-release-community.md`](TODO-05-github-release-community.md) -- GitHub Releases & Community Launch: §1 GitHub release workflow (`create-release.sh`: annotated tag, `gh release create`, delegate to `upload-release.sh`+`release-sdk.sh`, Discord webhook, SDK version assert; GHA on `v*` tag); §4 changelog discipline (`CHANGELOG.md` Keep-a-Changelog overhaul; PR requirement for `[Unreleased]` entry; `gen-changelog.sh` draft flow; `lint-changelog.py` CI check); §2 `CONTRIBUTING.md` overhaul (WSL2 setup, build commands, code style, conventional commits, PR process, DCO sign-off, issue labels, good-first-issue criteria); §3 YAML issue templates + PR template (structured `bug_report.yml`/`feature_request.yml`; 7-item PR checklist; `config.yml` blank-issues off); §5 README overhaul (desktop screenshot, quick-start 3 commands, dynamic download table, CI badge, architecture ASCII, `update-readme-links.sh`, `sync-readme-status.sh`); §6 project website (`docs/website/` HTML/CSS; GitHub Pages → `impossible-os.dev` via Cloudflare; dynamic version CTA from update API; blog; GHA deploy workflow); §7 community channels (GitHub Discussions 4 categories; Discord server + roles + `#good-first-issues` bot; release webhook; `CODE_OF_CONDUCT.md` Contributor Covenant 2.1); §8 roadmap + GitHub Milestones sync (`docs/roadmap.md` v1.0/v1.1/v2.0 plan; domain→milestone mapping; `sync-milestones.sh` idempotent; `sync-issues.sh` P0/P1 dry-run-safe; compat progress bar)
- [`TODO-04-release-qa.md`](TODO-04-release-qa.md) -- Release QA & Platform Certification: §2 automated regression test suite (`run-tests.sh`: 11-test matrix -- kernel init, PMM/heap stress, FS CRUD, registry, network DHCP/DNS/HTTP, compositor FPS, process spawn, syscall smoke; `TEST PASS|FAIL` serial parsing; GHA CI); §4 QEMU validation (`qemu-test.sh`: 8 scenarios -- cold boot, warm reboot, ACPI shutdown, memory pressure, disk stress, network loopback, hibernate stretch, 30-min idle soak); §3 Hyper-V certification (`hyperv-test.ps1`: Gen2 VM unattended install; 8 required tests: OOBE, HV detection, TSC enlightenment, regression suite, PS/2 keyboard, mouse integration, screen resize, ACPI shutdown; `docs/guides/hyperv-known-issues.md`); §6 VirtualBox certification (`vbox-test.sh`: EFI VM; OVA import test; guest additions detection; shared clipboard; `virtualbox-known-issues.md`); §5 real hardware checklist (`hardware-test-checklist.md`: 17-row structured checklist; ≥ 3 distinct machines; results archived by version); §8 performance benchmarks (`benchmark.sh` → `benchmark-{version}.json`; 9 metrics: boot time, PMM throughput, heap throughput, FS R/W, network loopback, compositor FPS, process spawn, syscall roundtrip; 10%/25% regression thresholds); §1 release readiness checklist (`release-checklist.md` PR template: 14-item gated list; automated gates + VM cert + human sign-offs; merge blocked until all `[x]`); §7 crash analytics review (QA build `KASAN=1 LOCKDEP=1` + PMM sentinel pages; crash dump triage from `CrashDumps\`; `KERNEL_ASSERT` per resolved root cause; 30-min soak pass = 0 panics + 0 sentinel events)
- [`TODO-03-update-server.md`](TODO-03-update-server.md) -- Update Server Infrastructure: §1 version manifest JSON API (static GitHub Pages; channels stable/beta/dev; `delta_url` field; CF Worker query-string routing; client INI→JSON parse upgrade note for `TODO-03 §1`); §9 CDN & hosting (`upload-release.sh` → GitHub Releases for images/ISO + Cloudflare R2 for IPKG; GHA workflow on `git tag v*`); §5 release promotion pipeline (`promote-release.sh dev→beta→stable`, `rollback-release.sh`, sign-off checklist, `gen-changelog.sh`); §6 IPKG package repository (`index.json` on R2; `ipkg search/install/list/update/upgrade` commands; index cache + `RepoURL` Registry override); §3 package submission (GitHub PR CI: `ipkg-sign`/`ipkg-verify` Ed25519, manifest validation, ClamAV scan stub, QEMU install+uninstall test, auto `index.json` rebuild on approval); §4 update delta packages (`make-delta.sh` BLAKE2b-160 file diff; `delta.ini` manifest; client delta-first with full fallback; `update_apply_delta()`); §8 telemetry pipeline (CF Worker + D1 receiver; GDPR EU-anon; Grafana dashboard at `stats.impossible-os.dev`; `privacy-policy.md`); §10 status page (GHA cron every 5 min; auto-incident GitHub Issue + webhook; 90-day uptime history at `status.impossible-os.dev`)
- [`TODO-02-unattended-install.md`](TODO-02-unattended-install.md) -- Unattended Installation & Deployment: §1 answer file INI format + parser (`answer_parse`, discovery via `boot_info->cmdline` / media root / Registry); §2 unattended install mode (auto-proceed through `TODO-11` installer pipeline -- disk/format/copy/user/network/OEM/packages -- with `setup.log`); §5 `sysprep.exe /generalize` (CSPRNG 96-bit machine SID, hostname/GUID/credential/log clear, `/oobe`/`/audit` flags, `sysprep.cpl`); §8 OEM customization (`$OEM$\Drivers\*.kmod` auto-load, `$OEM$\$1\registry.reg` injection, wallpaper + branding + `ipkg` pre-install); §9 `imagex.exe` WIM/`.iim` capture with BLAKE2b-160 single-instancing (`/capture`, `/apply`, `/info`); §1 PXE netboot (TFTP `RFC 1350` server on UDP/69, DHCP proxy for PXEClient option, `pxesrv start/stop/status` shell command); §2 VM provisioning templates (`provision-qemu.sh` QCOW2 + `provision-hyperv.ps1` VHDX + `ci-provision.sh` CI wrapper); §4 enterprise deployment guide (`docs/guides/enterprise-deployment.md` -- answer file reference, sysprep/WIM/PXE walkthroughs, QEMU farm + Hyper-V cluster examples)
- [`TODO-01-release-artifacts.md`](TODO-01-release-artifacts.md) -- Disk Image, USB & Release Artifacts: §1 versioning (`OS_VERSION_STRING` macro, `set-version.sh`/`increment-build.sh`, Registry baking at boot, `winver.exe`); §2 GPT disk image release (`release-image.sh`: integrity checks for `BOOTX64.EFI`+`kernel.exe`, `zstd -T0 -9` compress, SHA-256, `make release-image` target); §3 USB-bootable (`make-usb.sh` removable-only guard + `dd` + verify; `usb_creator.exe` Win32 GUI with UAC, `SetupDi` drive list, `DeviceIoControl` write, progress bar); §4 Bootable ISO Joliet+Rock Ridge (extends `TODO-11 §6`: `-R -J --joliet-long`, versioned filename, `README.txt` at root, SHA-256); §9 code signing (`sign-release.sh`: `CODESIGN_PRIV_KEY` GitHub secret → `tools/codesign_host` on `kernel.exe`+`BOOTX64.EFI`; bootloader-side Ed25519 verify with `Enforce` toggle in UEFI var; GPG-sign `.iso`+`.img.zst`; `bake_pubkey.sh`); §1 artifact manifest (`make-manifest.sh` → `release-{ver}.json` with filename/size/sha256/git_commit/download_url per artifact; consumed by `TODO-03` update check); §8 VM image variants (`qemu-img convert` → VMDK+VHD+VHDX with SHA-256; `.ovf`+`.ova` VirtualBox import package; `make vm-images`); §10 reproducible builds (`SOURCE_DATE_EPOCH` from `git log -1 --format=%ct`, `llvm-ar rcsD`, replace `__TIME__`/`__DATE__`, `make verify-reproducible`, `docs/infrastructure/reproducible-builds.md`)

## Completed / Doc-converted

- None yet.

## Local Naming

- Use `TODO-01-installer-flow.md` as the filename style for new leaf TODOs in this folder.
- Create a parent TODO only when one topic needs multiple leaf files or shared verification.
