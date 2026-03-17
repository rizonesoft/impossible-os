# P0508 — Task Manager

> **Goal:** Process list and performance monitor accessible via Ctrl+Shift+Esc.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Task Manager

### 1.1 Task Manager App

**Prompt:** The Task Manager opens via Ctrl+Shift+Esc (registered as a system-wide hotkey in Phase 04 §15.1). It has two tabs: Processes (table listing all threads from the scheduler with Name, CPU%, RAM, PID, Status columns — read from the scheduler's task list from Phase 01 §1) and Performance (CPU usage as a rolling 60-second line chart, RAM usage bar from PMM stats). The "End Task" button kills the selected process. Auto-update the display every 1 second. Status bar shows total process count, overall CPU%, and RAM used/total. After completing all items,sh clean`, and commit as `"apps: Task Manager"`.


- [ ] Create `src/apps/taskmgr/taskmgr.c`
- [ ] Open via Ctrl+Shift+Esc system-wide shortcut
- [ ] **Processes tab**: table listing all processes
  - [ ] Columns: Name, CPU%, RAM, PID, Status
  - [ ] Read from scheduler task list
  - [ ] Select process → [End Task] button → kill process
  - [ ] Auto-update every 1 second
- [ ] **Performance tab**:
  - [ ] CPU usage: rolling line chart (last 60 seconds)
  - [ ] RAM usage: bar showing used / total MB (from PMM stats)
- [ ] Status bar: total processes, overall CPU%, RAM used/total
- [ ] Commit: `"apps: Task Manager"`

---

## 2. Device Manager

### 2.1 Device Manager App

**Prompt:** The Device Manager displays all hardware in a tree view (from §1.4 Tree View widget): categories as parent nodes (Display adapters, Network adapters, Storage controllers, Input devices, System devices), individual devices as children. Device info comes from PCI enumeration results (vendor/device IDs, BARs, IRQs) and registered driver names. Status indicators: green check for working devices, yellow warning for no-driver, red cross for errors. Click a device to see properties (PCI address, vendor ID, device ID, IRQ, driver name). After completing all items,sh clean`, and commit as `"apps: Device Manager"`.


- [ ] Create `src/apps/devmgr/devmgr.c`
- [ ] Define `struct device_info` (name, driver, category, vendor/device IDs, bus/slot/func, IRQ, status)
- [ ] Tree view: categories → individual devices
  - [ ] Display adapters → "VGA Compatible (Multiboot2 FB)"
  - [ ] Network adapters → "Realtek RTL8139 (PCI)"
  - [ ] Storage controllers → "VirtIO Block Device"
  - [ ] Input devices → "PS/2 Keyboard (IRQ 1)", "VirtIO Tablet (PCI)"
  - [ ] System devices → "PIT Timer", "PCI Bus", "ACPI"
- [ ] Data source: PCI enumeration + registered driver list
- [ ] Device status indicators: OK (✓), no driver (⚠), error (✗)
- [ ] *(Stretch)* Click device → properties: vendor ID, device ID, IRQ, driver name
- [ ] Commit: `"apps: Device Manager"`

