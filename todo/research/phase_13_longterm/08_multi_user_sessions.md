# 96 — Multi-User Sessions

## Overview
Multiple users logged in simultaneously (like Windows Fast User Switching). Each user has their own desktop, apps, and state.

## How it works
1. User A is logged in → full desktop running
2. User A clicks Start → Switch User
3. Lock screen appears (User A's apps stay running in background)
4. User B logs in → gets their own desktop, separate window list
5. User B switches back → User A's desktop restored instantly

## Architecture
```c
struct user_session {
    uint32_t       uid;
    char           username[32];
    gfx_surface_t *desktop_surface;  /* each session has its own compositor */
    wm_window_t   *window_list;      /* separate window list */
    process_t     *processes;         /* processes owned by this user */
    uint8_t        active;           /* currently displayed session */
};
```

## Memory: each session keeps its compositor state → expensive (multiply RAM by sessions)
## Depends on: `33_user_accounts_login.md`
## Files: `src/kernel/session.c` (~400 lines) | 2-4 weeks
## Priority: 🔴 Long-term
