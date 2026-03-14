# P0305 — System Tray & Notifications

> **Goal:** System tray icons, notification toasts, and notification center.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 8. System Tray & Notifications *(from Phase 04 §3)*

### 8.1 System Tray Icons *(from Phase 04 §3.1)*

**Prompt:** System tray: volume, network, notification bell icons. Each clickable with popups. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: system tray icons"`. Update `README.md` if it contains stale or incorrect references to the system tray. Add notes, gotchas, and design decisions directly in this TODO section covering the system tray API, icon types, and popup behavior.


- [ ] Create `src/desktop/systray.c`
- [ ] 🔊 Volume icon → volume slider popup
- [ ] 🌐 Network icon → status popup (IP, connected/disconnected)
- [ ] 🔔 Notification bell → open notification center
- [ ] Dynamic icon updates (e.g., muted = different icon)
- [ ] Commit: `"desktop: system tray icons"`

### 8.2 Notification Toasts *(from Phase 02 §9.3 + Phase 04 §3.2)*

**Prompt:** Toast notifications slide in from bottom-right. Auto-dismiss after timeout, stack vertically. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: notification toasts"`. Update `README.md` if it contains stale or incorrect references to notifications. Add notes, gotchas, and design decisions directly in this TODO section covering the notification API, slide animation, auto-dismiss, and stacking.


- [ ] Create `src/desktop/notify.c`
- [ ] `notify_send(title, msg, icon)` → display toast
- [ ] Slide-in animation from right edge (300ms, ease-out-cubic)
- [ ] Auto-dismiss after 5 seconds (configurable)
- [ ] Stack multiple toasts vertically
- [ ] Icon + bold title + message body + [Dismiss] button
- [ ] Commit: `"desktop: notification toasts"`

### 8.3 Notification Center *(from Phase 04 §7)*

**Prompt:** Slide-in panel from right edge with notification history. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: notification center"`. Update `README.md` if it contains stale or incorrect references to notifications. Add notes, gotchas, and design decisions directly in this TODO section covering the notification center panel, history storage, and dismiss actions.


- [ ] Create `src/desktop/notify_center.c`
- [ ] Slide-in panel from right edge
- [ ] Past notifications: grouped by source, icon + title + msg + timestamp
- [ ] Dismiss individual (X) or "Clear all"
- [ ] Store last 100 in Registry `HKLM\SYSTEM\Shell\NotifyHistory`
- [ ] Open: click 🔔 / Close: click outside or Escape
- [ ] Commit: `"desktop: notification center"`

