# 85 — Clipboard History

## Overview
Remember last 25 copied items. Press Win+V to see history and paste any previous clip. Like Windows 11 Clipboard History.

## Layout
```
         ┌───────────────────────┐
         │ Clipboard History     │
         │ ┌───────────────────┐ │
         │ │ Hello world       │ │  ← most recent
         │ └───────────────────┘ │
         │ ┌───────────────────┐ │
         │ │ https://example.. │ │
         │ └───────────────────┘ │
         │ ┌───────────────────┐ │
         │ │ 🖼 [image thumb]  │ │  ← copied image
         │ └───────────────────┘ │
         │        [Clear all]    │
         └───────────────────────┘
```

## Data: ring buffer of last 25 clipboard entries
## Each entry stores: format (text/image), data, timestamp, source app
## Codex: `System\Clipboard\HistoryEnabled = 1`, `System\Clipboard\MaxItems = 25`
## Extends: `03_clipboard.md`

## Files: `src/desktop/clip_history.c` (~150 lines) | 2-3 days
