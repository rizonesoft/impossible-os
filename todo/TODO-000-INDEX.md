# Impossible OS — Implementation Order

> Work through this list sequentially, top to bottom.
> If a section references another TODO, complete that dependency first.
>
> **Structure:** TODOs are organised into subfolders by layer.
> The folder prefix matches the first TODO number in that group.
> **Naming:** `TODO-NNN-Name.md` — gaps of 10 allow inserting up to 9 files between any two entries.

---

## Infrastructure  `000-Infrastructure/`

| #   | TODO                                                                    | Status |
|-----|-------------------------------------------------------------------------|--------|
| 001 | [GitHub Setup](000-Infrastructure/TODO-001-GitHub.md)                   |        |
| 002 | [Development Tooling](000-Infrastructure/TODO-002-Development.md)      |        |
| 008 | [Hyper-V Gen 2 Boot](000-Infrastructure/TODO-008-Hyper-V-Runner.md)   |        |
| 009 | [Debug & Logging System](000-Infrastructure/TODO-009-Debug.md)          |        |

---

## Layer 1: Kernel Foundations  `010-Kernel-Foundations/`

| #   | TODO                                                                                                    | Status         |
|-----|---------------------------------------------------------------------------------------------------------|----------------|
| 010 | [Bootloader](010-Kernel-Foundations/TODO-010-Bootloader.md)                                             | 🔄 In progress |
| 020 | [Threading & Synchronization](010-Kernel-Foundations/TODO-020-Threading-Synchronization.md)             | 🔄 In progress |
| 021 | [Kernel Scheduler — CFS, RT, priorities](010-Kernel-Foundations/TODO-021-Scheduler.md)                  |                |
| 022 | [IPC — Pipes, Signals, Shared Memory](010-Kernel-Foundations/TODO-022-IPC.md)                           | ✅ All done    |
| 023 | [Virtual Memory — Swap & mmap](010-Kernel-Foundations/TODO-023-Virtual-Memory.md)                       | 🔄 In progress |
| 024 | [System Logging — klog, ring buffer, disk](010-Kernel-Foundations/TODO-024-System-Logging.md)           | 🔄 In progress |
| 025 | [Environment Variables — env_get/set, PATH](010-Kernel-Foundations/TODO-025-Environment-Variables.md)   |                |
| 026 | [ELF Dynamic Linker & Kernel Modules](010-Kernel-Foundations/TODO-026-ELF-Shared-Libraries.md)          |                |
| 027 | [Kernel Libraries — miniz, monocypher, cJSON](010-Kernel-Foundations/TODO-027-Kernel-Libraries.md)      |                |
| 028 | [Process Model — FD table, CWD, timers](010-Kernel-Foundations/TODO-028-Process-Model.md)               |                |
| 029 | [Memory Guardrails & Audit](010-Kernel-Foundations/TODO-029-Memory-Guardrails.md)                       |                |
| 030 | [Memory Advanced](010-Kernel-Foundations/TODO-030-Memory-Advanced.md)                                   |                |
| 040 | [Filesystem](010-Kernel-Foundations/TODO-040-Filesystem.md)                                             |                |
| 050 | [Registry](010-Kernel-Foundations/TODO-050-Registry.md)                                                 |                |


---

## Layer 2: Hardware & Drivers  `060-Hardware-Drivers/`

| #   | TODO                                                                              | Status |
|-----|-----------------------------------------------------------------------------------|--------|
| 060 | [Keyboard](060-Hardware-Drivers/TODO-060-Keyboard.md)                             |        |
| 070 | [Mouse](060-Hardware-Drivers/TODO-070-Mouse.md)                                   |        |
| 080 | [Drivers](060-Hardware-Drivers/TODO-080-Drivers.md)                               |        |
| 090 | [Guest Additions](060-Hardware-Drivers/TODO-090-Guest-Additions.md)               |        |
| 100 | [Power Management](060-Hardware-Drivers/TODO-100-Power-Management.md)             |        |

---

## Layer 3: GFX & UI Framework  `110-GFX-UI-Framework/`

| #   | TODO                                                              | Status |
|-----|-------------------------------------------------------------------|--------|
| 110 | [UI Framework](110-GFX-UI-Framework/TODO-110-UI-Framework.md)    |        |
| 120 | [Theme](110-GFX-UI-Framework/TODO-120-Theme.md)                  |        |
| 130 | [Controls](110-GFX-UI-Framework/TODO-130-Controls.md)            |        |
| 140 | [Animation](110-GFX-UI-Framework/TODO-140-Animation.md)          |        |
| 150 | [Message Box](110-GFX-UI-Framework/TODO-150-Message-Box.md)      |        |

---

## Layer 4: Desktop Shell  `160-Desktop-Shell/`

| #   | TODO                                                                | Status |
|-----|---------------------------------------------------------------------|--------|
| 160 | [Boot Splash](160-Desktop-Shell/TODO-160-Boot-Splash.md)            |        |
| 170 | [Desktop Shell](160-Desktop-Shell/TODO-170-Desktop-Shell.md)        |        |
| 180 | [Taskbar](160-Desktop-Shell/TODO-180-Taskbar.md)                    |        |
| 190 | [Start Menu](160-Desktop-Shell/TODO-190-Start-Menu.md)              |        |
| 200 | [Notifications](160-Desktop-Shell/TODO-200-Notifications.md)        |        |
| 210 | [Clock](160-Desktop-Shell/TODO-210-Clock.md)                        |        |
| 220 | [BSOD](160-Desktop-Shell/TODO-220-BSOD.md)                          |        |

---

## Layer 5: Core Services  `230-Core-Services/`

| #   | TODO                                                                          | Status |
|-----|-------------------------------------------------------------------------------|--------|
| 230 | [Clipboard](230-Core-Services/TODO-230-Clipboard.md)                          |        |
| 240 | [Resources](230-Core-Services/TODO-240-Resources.md)                          |        |
| 250 | [System Services](230-Core-Services/TODO-250-System-Services.md)              |        |
| 260 | [Search](230-Core-Services/TODO-260-Search.md)                                |        |
| 270 | [Recycle Bin](230-Core-Services/TODO-270-Recycle-Bin.md)                      |        |
| 280 | [ZIP](230-Core-Services/TODO-280-ZIP.md)                                      |        |
| 290 | [Scheduler](230-Core-Services/TODO-290-Scheduler.md)                          |        |
| 300 | [Security & Accounts](230-Core-Services/TODO-300-Security-Accounts.md)        |        |

---

## Layer 6: Core Apps  `310-Core-Apps/`

| #   | TODO                                                              | Status |
|-----|-------------------------------------------------------------------|--------|
| 310 | [Terminal](310-Core-Apps/TODO-310-Terminal.md)                    |        |
| 320 | [File Manager](310-Core-Apps/TODO-320-File-Manager.md)            |        |
| 330 | [Notepad](310-Core-Apps/TODO-330-Notepad.md)                      |        |
| 340 | [Calculator](310-Core-Apps/TODO-340-Calculator.md)                |        |
| 350 | [Settings Panel](310-Core-Apps/TODO-350-Settings-Panel.md)        |        |
| 360 | [Task Manager](310-Core-Apps/TODO-360-Task-Manager.md)            |        |
| 370 | [Utility Apps](310-Core-Apps/TODO-370-Utility-Apps.md)            |        |

---

## Layer 7: Multimedia  `380-Multimedia/`

| #   | TODO                                            | Status |
|-----|-------------------------------------------------|--------|
| 380 | [Audio](380-Multimedia/TODO-380-Audio.md)       |        |
| 390 | [Paint](380-Multimedia/TODO-390-Paint.md)       |        |

---

## Layer 8: Networking & Internet Apps  `400-Networking-Internet-Apps/`

| #   | TODO                                                                              | Status |
|-----|-----------------------------------------------------------------------------------|--------|
| 400 | [Networking](400-Networking-Internet-Apps/TODO-400-Networking.md)                 |        |
| 410 | [Browser](400-Networking-Internet-Apps/TODO-410-Browser.md)                       |        |
| 420 | [FTP](400-Networking-Internet-Apps/TODO-420-FTP.md)                               |        |
| 430 | [SSH](400-Networking-Internet-Apps/TODO-430-SSH.md)                               |        |
| 440 | [Email](400-Networking-Internet-Apps/TODO-440-Email.md)                           |        |
| 450 | [PDF Viewer](400-Networking-Internet-Apps/TODO-450-PDF-Viewer.md)                 |        |

---

## Layer 9: Polish & Extras  `460-Polish-Extras/`

| #   | TODO                                                                          | Status |
|-----|-------------------------------------------------------------------------------|--------|
| 460 | [DPI Scaling](460-Polish-Extras/TODO-460-DPI.md)                              |        |
| 470 | [Screensaver & Lock Screen](460-Polish-Extras/TODO-470-Screensaver.md)        |        |
| 480 | [Desktop Widgets](460-Polish-Extras/TODO-480-Desktop-Widgets.md)              |        |
| 490 | [Display & Resolution](460-Polish-Extras/TODO-490-Display.md)                 |        |
| 500 | [System Maintenance](460-Polish-Extras/TODO-500-System-Maintenance.md)        |        |

---

## Layer 10: Future Releases  `510-Long-Term-Stretch/`

| #   | TODO                                                                          | Status |
|-----|-------------------------------------------------------------------------------|--------|
| 510 | [Native Win32](510-Long-Term-Stretch/TODO-510-Native-Win32.md)                |        |
| 530 | [Compiler](510-Long-Term-Stretch/TODO-530-Compiler.md)                        |        |
| 535 | [SDK](510-Long-Term-Stretch/TODO-535-SDK.md)                                  |        |
| 540 | [Linux Compat](510-Long-Term-Stretch/TODO-540-Linux.md)                       |        |
| 550 | [Installer & ISO](510-Long-Term-Stretch/TODO-550-Installer-ISO.md)            |        |
| 560 | [Long-Term Features](510-Long-Term-Stretch/TODO-560-Long-Term.md)             |        |
