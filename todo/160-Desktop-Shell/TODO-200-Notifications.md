# P0305 — System Tray & Notifications

> **Goal:** System tray icons, notification toasts, and notification center.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 8. System Tray & Notifications *(from Phase 04 §3)*

### 8.1 System Tray Icons *(from Phase 04 §3.1)*

**Prompt:** System tray: volume, network, notification bell icons. Each clickable with popups. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: system tray icons"`. Add notes, gotchas, and design decisions directly in this TODO section covering the system tray API, icon types, and popup behavior.
- [ ] Create `src/desktop/systray.c`
- [ ] 🔊 Volume icon → volume slider popup
- [ ] 🌐 Network icon → status popup (IP, connected/disconnected)
- [ ] 🔔 Notification bell → open notification center
- [ ] Dynamic icon updates (e.g., muted = different icon)
- [ ] Commit: `"desktop: system tray icons"`

### 8.2 Notification Toasts *(from Phase 02 §9.3 + Phase 04 §3.2)*

**Prompt:** Toast notifications slide in from bottom-right. Auto-dismiss after timeout, stack vertically. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: notification toasts"`. Add notes, gotchas, and design decisions directly in this TODO section covering the notification API, slide animation, auto-dismiss, and stacking.
- [ ] Create `src/desktop/notify.c`
- [ ] `notify_send(title, msg, icon)` → display toast
- [ ] Slide-in animation from right edge (300ms, ease-out-cubic)
- [ ] Auto-dismiss after 5 seconds (configurable)
- [ ] Stack multiple toasts vertically
- [ ] Icon + bold title + message body + [Dismiss] button
- [ ] Commit: `"desktop: notification toasts"`

### 8.3 Notification Center *(from Phase 04 §7)*

**Prompt:** Slide-in panel from right edge with notification history. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: notification center"`. Add notes, gotchas, and design decisions directly in this TODO section covering the notification center panel, history storage, and dismiss actions.
- [ ] Create `src/desktop/notify_center.c`
- [ ] Slide-in panel from right edge
- [ ] Past notifications: grouped by source, icon + title + msg + timestamp
- [ ] Dismiss individual (X) or "Clear all"
- [ ] Store last 100 in Registry `HKLM\SYSTEM\Shell\NotifyHistory`
- [ ] Open: click 🔔 / Close: click outside or Escape
  - [ ] Commit: `"desktop: notification center"`

### 8.4 Do Not Disturb Integration

**Prompt:** When Do Not Disturb mode is active (from TODO-170 §9), notifications are silently queued — no toast slides in. The notification bell in the system tray shows a badge with the count of missed notifications. When DND ends, a summary toast appears: "You missed N notifications — see Notification Center". After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: DND notification integration"`.

- [ ] Check DND state before showing toast: if DND active → queue silently
- [ ] Notification bell tray icon: badge count of missed notifications during DND
- [ ] On DND disable: toast summary "You missed N notifications"
- [ ] Click badge → open notification center
- [ ] Commit: `"desktop: DND notification integration"`

### 8.5 System Event Notifications

**Prompt:** Key system events should generate notifications automatically. USB device connected/disconnected, low disk space, low battery, network connected/disconnected, new update available. Each calls `notify_send()` with appropriate title, message, and icon. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: system event notifications"`.

- [ ] USB device inserted → `notify_send("USB Device Connected", name, ICON_USB)`
- [ ] Low disk space (< 10% free) → `notify_send("Low Disk Space", "C:\\ drive is almost full", ICON_WARNING)`
- [ ] Low battery (< 20%) → `notify_send("Low Battery", "XX% remaining", ICON_BATTERY_LOW)`
- [ ] Network connected → `notify_send("Connected", "IP: X.X.X.X", ICON_NETWORK)`
- [ ] DHCP lease failed → `notify_send("Network Error", "Could not obtain IP", ICON_WARNING)`
- [ ] Crash dump detected on boot → `notify_send("PC restarted after a problem", ..., ICON_ERROR)`
- [ ] Commit: `"kernel: system event notifications"`

---

## Priority Order

| Priority | Section                     | Reason                                              |
|----------|-----------------------------|-----------------------------------------------------|
| 🔴 P0    | §8.1 System Tray Icons      | Volume, network in tray — core desktop chrome       |
| 🔴 P0    | §8.2 Notification Toasts    | Apps need to surface events to user                 |
| 🟠 P1    | §8.3 Notification Center    | History of past notifications                       |
| 🟠 P1    | §8.5 System Event Notifs    | USB, network, battery events are high-value         |
| 🟡 P2    | §8.4 DND Integration        | Requires TODO-170 §9 focus mode first               |

---

## Key Files

| File                          | Purpose                                      |
|-------------------------------|----------------------------------------------|
| `src/desktop/systray.c`       | [NEW] System tray icon management            |
| `src/desktop/notify.c`        | [NEW] Toast notification queue               |
| `src/desktop/notify_center.c` | [NEW] Notification center panel              |
| `include/desktop/notify.h`    | [NEW] Notification API header                |

---

## OS Comparison

| Feature                         | 🪟 Windows 11 (Action Center)        | 🐧 Linux (GNOME Shell / Dunst)     | 🚀 Impossible OS                       |
| ------------------------------- | ----------------------------------- | --------------------------------- | ------------------------------------- |
| System tray                     | ✅ Shell_NotifyIcon / NOTIFYICONDATA | ✅ AppIndicator / StatusIcon       | ⬜ §8.1 P0                             |
| Volume/network tray icons       | ✅ Built-in                          | ✅ GNOME built-in indicators       | ⬜ §8.1 P0                             |
| Toast notifications             | ✅ WinRT ToastNotification (COM)     | ✅ libnotify + notification daemon | ⬜ §8.2 P0 — **in-kernel, no daemon**  |
| Notification center / history   | ✅ Action Center                     | ✅ GNOME notification tray         | ⬜ §8.3 P1                             |
| Notification stacking           | ✅ Groups by app                     | ✅ Dunst groups                    | ⬜ §8.2 — stack vertically             |
| DND / Focus Assist              | ✅ Focus Assist                      | ✅ GNOME DND                       | ⬜ §8.4 P2 (requires TODO-170 §9)      |
| Badge count on tray icon        | ✅ Overlay icons                     | ✅ libunity badge                  | ⬜ §8.4 — missed count badge           |
| System event auto-notifications | ✅ (Windows generates many events)   | ✅ udev rules + libnotify          | ⬜ §8.5 P1 — USB, battery, network     |
| **No COM (in-kernel toasts)**   | ❌ COM + WinRT required              | ❌ Separate notify daemon          | ✅ **`notify_send()` — zero IPC**      |
| **Works before login session**  | ❌ Toasts require user session       | ❌ libnotify requires session      | ✅ **Kernel global queue — always on** |
