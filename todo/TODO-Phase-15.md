# Production Testing (Hyper-V)

> **Deferred** — run after all features are implemented and the ISO installer is complete.

## 15.1 Hyper-V VM Setup

**Prompt:** Set up a Generation 2 Hyper-V VM on the Windows host with 2+ GB RAM, 1+ vCPU, 20+ GB VHDX disk, Secure Boot disabled. Attach the ISO from `build/os-build.iso` via the WSL path. This tests the OS on real Microsoft hardware virtualization (different from QEMU's KVM). After completing all items, mark every item as `[x]` and document results.


- [ ] Enable Hyper-V on Windows host (if not already)
- [ ] Open Hyper-V Manager → **New → Virtual Machine**
- [ ] Choose **Generation 2** VM
- [ ] Allocate ≥ 2 GB RAM, ≥ 1 vCPU
- [ ] Create a virtual hard disk (≥ 20 GB, VHDX)
- [ ] **Disable Secure Boot** (VM Settings → Security → uncheck)
- [ ] Attach ISO: `\\wsl.localhost\Ubuntu\home\<user>\impossible-os\build\os-build.iso`

## 15.2 Hyper-V Test Runs

**Prompt:** Execute 8 validation tests in sequence: ISO boots to installer, installer partitions and formats, installer copies files and installs bootloader, VM reboots from disk → kernel loads → desktop, keyboard/mouse work, filesystem CRUD works, window manager renders correctly at Hyper-V resolution, graceful ACPI shutdown/reboot. Document any Hyper-V-specific issues. After all tests, mark every item as `[x]`.


- [ ] **Test 1:** ISO boots to installer without errors
- [ ] **Test 2:** Installer partitions and formats the virtual disk
- [ ] **Test 3:** Installer copies OS files and installs bootloader
- [ ] **Test 4:** VM reboots from disk → kernel loads → shell or desktop appears
- [ ] **Test 5:** Keyboard and mouse work inside Hyper-V
- [ ] **Test 6:** Filesystem operations work (create, read, delete files)
- [ ] **Test 7:** Window manager renders correctly at Hyper-V's resolution
- [ ] **Test 8:** Graceful shutdown/reboot via ACPI
- [ ] Document any Hyper-V-specific issues and fixes

## 15.3 Performance & Stability

**Prompt:** Run the OS for 30+ minutes without crash. Stress-test memory allocator (alloc/free loops), stress-test process creation (fork-bomb protection), verify no memory leaks via serial log. Document results. After all items, mark every item as `[x]`, and commit as `"test: Hyper-V validation pass"`.


- [ ] Run the OS for 30+ minutes without crash
- [ ] Stress-test memory allocator (allocate/free in a loop)
- [ ] Stress-test process creation (fork-bomb protection)
- [ ] Verify no memory leaks via serial log inspection
- [ ] Commit: `"test: Hyper-V validation pass"`
