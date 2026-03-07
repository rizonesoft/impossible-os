# Clock & Time Sync — Research

## Overview

Implement **real wall-clock time** in Impossible OS with:
1. **CMOS RTC** — read initial date/time from hardware at boot
2. **NTP** — sync with internet time servers for accuracy
3. **Kernel time API** — maintain current time, timezone, formatting
4. **Taskbar clock** — display time in the system tray area

---

## Current State

| Feature | Status |
|---------|--------|
| PIT timer (100Hz ticks) | ✅ `pit_get_ticks()` — monotonic counter |
| Uptime | ✅ `uptime()` — seconds since boot |
| SYS_UPTIME syscall | ✅ Returns seconds since boot |
| CMOS RTC read | ❌ Not implemented |
| Wall-clock time | ❌ Not implemented |
| NTP client | ❌ Not implemented |
| Taskbar clock | ❌ Not implemented (shows uptime only) |
| UDP stack | ✅ Working (used by DHCP) |

---

## Part 1: CMOS RTC — Boot-Time Clock

The **CMOS Real-Time Clock** is a hardware chip on every x86 PC that keeps time even when powered off (battery-backed). Read it at boot to get the initial date/time.

### Reading CMOS RTC

```c
/* rtc.h */

struct rtc_time {
    uint8_t  second;    /* 0-59 */
    uint8_t  minute;    /* 0-59 */
    uint8_t  hour;      /* 0-23 */
    uint8_t  day;       /* 1-31 */
    uint8_t  month;     /* 1-12 */
    uint16_t year;      /* e.g. 2026 */
    uint8_t  weekday;   /* 1=Sunday, 7=Saturday */
};

void rtc_init(void);
struct rtc_time rtc_read(void);
```

### How CMOS RTC works

```c
#define CMOS_ADDR  0x70   /* I/O port: register select */
#define CMOS_DATA  0x71   /* I/O port: read/write data */

/* RTC register addresses */
#define RTC_SECONDS  0x00
#define RTC_MINUTES  0x02
#define RTC_HOURS    0x04
#define RTC_DAY      0x07
#define RTC_MONTH    0x08
#define RTC_YEAR     0x09
#define RTC_CENTURY  0x32   /* may not exist on all hardware */
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

struct rtc_time rtc_read(void) {
    struct rtc_time t;

    /* Wait for update-in-progress to finish */
    while (cmos_read(RTC_STATUS_A) & 0x80);

    t.second = cmos_read(RTC_SECONDS);
    t.minute = cmos_read(RTC_MINUTES);
    t.hour   = cmos_read(RTC_HOURS);
    t.day    = cmos_read(RTC_DAY);
    t.month  = cmos_read(RTC_MONTH);
    t.year   = cmos_read(RTC_YEAR);

    /* Check if BCD mode (bit 2 of Status B = 0 means BCD) */
    uint8_t status_b = cmos_read(RTC_STATUS_B);
    if (!(status_b & 0x04)) {
        t.second = bcd_to_bin(t.second);
        t.minute = bcd_to_bin(t.minute);
        t.hour   = bcd_to_bin(t.hour);
        t.day    = bcd_to_bin(t.day);
        t.month  = bcd_to_bin(t.month);
        t.year   = bcd_to_bin(t.year);
    }

    /* CMOS year is 2-digit; assume 2000s */
    t.year += 2000;

    return t;
}
```

**~50 lines of C** — straightforward I/O port reads.

---

## Part 2: Kernel Time System

Maintain wall-clock time by combining RTC (boot-time) + PIT ticks (elapsed):

```c
/* time.h */

/* Unix timestamp (seconds since 1970-01-01 00:00:00 UTC) */
typedef int64_t time_t;

struct datetime {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    int16_t  tz_offset_min;  /* timezone offset in minutes from UTC */
};

/* Initialize time system — reads RTC, starts tracking */
void time_init(void);

/* Get current Unix timestamp */
time_t time_now(void);

/* Get current date/time in local timezone */
struct datetime time_now_local(void);

/* Convert Unix timestamp to datetime */
struct datetime time_to_datetime(time_t ts, int16_t tz_offset_min);

/* Convert datetime to Unix timestamp */
time_t datetime_to_time(const struct datetime *dt);

/* Format time as string */
void time_format(const struct datetime *dt, char *buf, uint32_t size,
                 const char *fmt);
/* fmt: "%H:%M"       → "14:30"
 *      "%H:%M:%S"    → "14:30:45"
 *      "%Y-%m-%d"    → "2026-03-07"
 *      "%d/%m/%Y"    → "07/03/2026"
 *      "%I:%M %p"    → "2:30 PM"
 */

/* Set timezone offset (stored in Codex) */
void time_set_timezone(int16_t offset_minutes);

/* Called by NTP to adjust clock */
void time_set(time_t new_time);

/* New syscall */
#define SYS_TIME     17   /* sys_time() → Unix timestamp */
```

### How it tracks time

```
Boot:
  1. rtc_read() → get date/time from CMOS
  2. Convert to Unix timestamp → boot_timestamp
  3. Record boot_ticks = pit_get_ticks()

Any time later:
  elapsed_seconds = (pit_get_ticks() - boot_ticks) / PIT_TARGET_FREQ
  current_time = boot_timestamp + elapsed_seconds + ntp_offset
```

---

## Part 3: NTP Client

**NTP** (Network Time Protocol) syncs the clock with internet time servers via UDP port 123.

### NTP Packet (simplified — SNTPv4)

```c
struct ntp_packet {
    uint8_t  flags;          /* LI(2) | VN(3) | Mode(3) = 0x1B for client */
    uint8_t  stratum;
    uint8_t  poll;
    int8_t   precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_timestamp[2];
    uint32_t orig_timestamp[2];
    uint32_t rx_timestamp[2];
    uint32_t tx_timestamp[2];   /* Server's transmit time — this is what we want */
} __attribute__((packed));     /* Total: 48 bytes */
```

### NTP sync process

```
1. Build NTP request packet (flags = 0x1B = client mode v3)
2. Send via UDP to time server IP, port 123
3. Receive response
4. Extract tx_timestamp (server's transmit time)
5. Convert NTP epoch (1900-01-01) to Unix epoch (1970-01-01)
   → subtract 2208988800 seconds
6. Call time_set() to update kernel clock
```

### Implementation

```c
#define NTP_PORT        123
#define NTP_EPOCH_DIFF  2208988800UL  /* seconds between 1900 and 1970 */

/* Public NTP servers (resolved by IP since we may not have DNS) */
static const uint32_t ntp_servers[] = {
    0xD8EF2300,  /* 216.239.35.0  — time.google.com */
    0xD8EF2304,  /* 216.239.35.4  — time.google.com */
    0xD8EF2308,  /* 216.239.35.8  — time.google.com */
    0x81FA0140,  /* 129.250.1.64  — ntp.api.bz */
};

void ntp_sync(void) {
    struct ntp_packet pkt;
    memzero(&pkt, sizeof(pkt));
    pkt.flags = 0x1B;  /* LI=0, VN=3, Mode=3 (client) */

    /* Send to first available NTP server */
    udp_send(ntp_servers[0], NTP_PORT, NTP_PORT,
             (const uint8_t *)&pkt, sizeof(pkt));

    /* Response is handled in udp_receive():
     * when src_port == 123, call ntp_handle_response() */
}

void ntp_handle_response(const uint8_t *data, uint32_t len) {
    struct ntp_packet *resp = (struct ntp_packet *)data;

    /* Extract server transmit time (seconds since 1900-01-01) */
    uint32_t ntp_seconds = ntohl(resp->tx_timestamp[0]);

    /* Convert to Unix timestamp */
    time_t unix_time = (time_t)ntp_seconds - NTP_EPOCH_DIFF;

    /* Update kernel clock */
    time_set(unix_time);

    printk("[NTP] Clock synced: %u\n", (uint32_t)unix_time);
}
```

### Sync schedule

| When | Action |
|------|--------|
| Boot (after DHCP) | First NTP sync |
| Every 60 minutes | Periodic re-sync |
| User triggers manually | Settings Panel → Date & Time → "Sync Now" |

---

## Part 4: Taskbar Clock

### Display

```
┌──────────────────────────────────────────────────┐
│  ⊞  │                              │  2:30 PM   │
│     │                              │  3/7/2026  │
└──────────────────────────────────────────────────┘
       start button                    clock area
```

### Implementation in `desktop.c`

```c
/* Draw clock in taskbar — right-aligned */
static void draw_taskbar_clock(void) {
    struct datetime now = time_now_local();
    char time_str[16];
    char date_str[16];

    time_format(&now, time_str, 16, "%I:%M %p");   /* "2:30 PM" */
    time_format(&now, date_str, 16, "%m/%d/%Y");    /* "3/7/2026" */

    uint32_t clock_x = fb_get_width() - 100;
    uint32_t clock_y_time = TASKBAR_Y + 8;
    uint32_t clock_y_date = TASKBAR_Y + 26;

    draw_text(clock_x, clock_y_time, time_str, 0xFFFFFF);
    draw_text(clock_x, clock_y_date, date_str, 0xAAAAAA);
}
```

### Click behavior

Clicking the clock area opens a **calendar popup** (future feature) or the Date & Time settings panel.

---

## Timezone Handling

Stored in Codex at `System\DateTime`:

```ini
[DateTime]
TimezoneOffset = 120        ; UTC+2 (South Africa = +120 minutes)
TimezoneName = "SAST"       ; South Africa Standard Time
Use24Hour = 0               ; 0 = 12h with AM/PM, 1 = 24h
DateFormat = "mm/dd/yyyy"   ; or "dd/mm/yyyy", "yyyy-mm-dd"
NTPEnabled = 1
NTPServer = "216.239.35.0"  ; time.google.com
```

---

## Files

| Action | File | Lines (est.) | Purpose |
|--------|------|-------------|---------|
| **[NEW]** | `include/rtc.h` | ~20 | RTC struct, init, read |
| **[NEW]** | `src/kernel/drivers/rtc.c` | ~60 | CMOS RTC reading |
| **[NEW]** | `include/time.h` | ~40 | Time types, API declarations |
| **[NEW]** | `src/kernel/time.c` | ~200 | Time tracking, formatting, timezone |
| **[NEW]** | `src/kernel/net/ntp.c` | ~80 | NTP client (SNTP) |
| **[MODIFY]** | `src/kernel/net/udp.c` | +5 | Route port 123 to `ntp_handle_response()` |
| **[MODIFY]** | `src/desktop/desktop.c` | +30 | Taskbar clock rendering |
| **[MODIFY]** | `src/kernel/main.c` | +5 | Call `rtc_init()`, `time_init()`, trigger NTP sync |
| **[MODIFY]** | `include/syscall.h` | +1 | Add `SYS_TIME` |

---

## Implementation Order

### Phase 1: CMOS RTC + kernel time (2-3 days)
- [ ] `rtc.c` — read CMOS date/time at boot
- [ ] `time.c` — convert to Unix timestamp, track with PIT ticks
- [ ] `time_now()`, `time_now_local()`, `time_format()`
- [ ] Add `SYS_TIME` syscall
- [ ] Store timezone in Codex defaults

### Phase 2: Taskbar clock (1 day)
- [ ] Draw time + date on right side of taskbar
- [ ] Update every second (based on PIT tick comparison)
- [ ] Read format preferences from Codex

### Phase 3: NTP sync (1-2 days)
- [ ] `ntp.c` — send SNTP request, parse response
- [ ] Hook into UDP receive for port 123
- [ ] Auto-sync after DHCP completes
- [ ] Periodic re-sync (hourly)

### Phase 4: Settings integration (1 day)
- [ ] Date & Time settings in `datetime.spl`
- [ ] Timezone selector
- [ ] 12h/24h toggle
- [ ] Manual "Sync Now" button
