# Production Testing (Hyper-V)

> **Deferred** — run after all features are implemented and the ISO installer is complete.

## 15.1 Hyper-V VM Setup

- [ ] Enable Hyper-V on Windows host (if not already)
- [ ] Open Hyper-V Manager → **New → Virtual Machine**
- [ ] Choose **Generation 2** VM
- [ ] Allocate ≥ 2 GB RAM, ≥ 1 vCPU
- [ ] Create a virtual hard disk (≥ 20 GB, VHDX)
- [ ] **Disable Secure Boot** (VM Settings → Security → uncheck)
- [ ] Attach ISO: `\\wsl.localhost\Ubuntu\home\<user>\impossible-os\build\os-build.iso`

## 15.2 Hyper-V Test Runs

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

- [ ] Run the OS for 30+ minutes without crash
- [ ] Stress-test memory allocator (allocate/free in a loop)
- [ ] Stress-test process creation (fork-bomb protection)
- [ ] Verify no memory leaks via serial log inspection
- [ ] Commit: `"test: Hyper-V validation pass"`
