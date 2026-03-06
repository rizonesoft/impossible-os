---
description: Test the OS ISO in Hyper-V for production validation
---

# Hyper-V Testing Workflow

> This workflow involves steps on the **Windows host**. The agent builds the ISO inside WSL; the user performs Hyper-V steps manually.

## Prerequisites

- Hyper-V enabled on the Windows host
- A Generation 2 VM created in Hyper-V Manager with:
  - ≥ 2 GB RAM, ≥ 1 vCPU
  - Secure Boot **disabled**
  - A virtual hard disk (≥ 20 GB VHDX)

## Steps

### 1. Build the ISO (Agent — in WSL terminal)

// turbo-all

```bash
make clean && make all && make iso
```

### 2. Verify in QEMU first (Agent — in WSL terminal)

```bash
make run
```

Confirm the OS boots correctly before handing off to Hyper-V.

### 3. Attach ISO in Hyper-V (User — on Windows host)

1. Open **Hyper-V Manager**
2. Right-click the VM → **Settings**
3. Under **SCSI Controller → DVD Drive**, click **Browse**
4. In the file picker address bar, type:
   ```
   \\wsl.localhost\Ubuntu\home\derickpayne\impossible-os\build\os-build.iso
   ```
5. Select `os-build.iso` and click **OK**

### 4. Boot the VM (User — on Windows host)

1. Right-click the VM → **Start**
2. Right-click the VM → **Connect** to open the console

### 5. Validate (User — on Windows host)

Run through this checklist:

- [ ] ISO boots without errors
- [ ] Kernel loads and displays output
- [ ] Keyboard input works
- [ ] Mouse input works (if GUI phase)
- [ ] Filesystem operations work
- [ ] Graceful shutdown/reboot via ACPI

### 6. Document Results

Report any Hyper-V-specific issues (resolution, input, timing) back to the agent for fixes.
