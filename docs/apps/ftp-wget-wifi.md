<!-- docs: covers=todo/11-apps/TODO-02-ftp-wget-wifi.md sources=include/kernel/net/net.h,user/cmd.c,include/desktop/controls.h reviewed=2026-09-29 order=2 -->
# FTP, wget, curl and Wi-Fi Tools

## What is it?

This roadmap plans the file transfer and download toolbox: an FTP client with interactive shell commands, `wget` and `curl` commands with progress bars, a dual-pane graphical FTP client, and a stretch Wi-Fi framework. None of it exists yet. The network stack today stops below TCP, so every tool here waits on the socket and HTTP work in the networking roadmaps.

## How does it work?

**Today.** The kernel network stack ships Ethernet, ARP, IPv4, ICMP, UDP and DHCP ([`net.h`](../../include/kernel/net/net.h)), described in [TCP and Network Infrastructure](../networking/tcp-network-infrastructure.md). There is no TCP, no socket API and no HTTP client. The shell ([`cmd.c`](../../user/cmd.c)) has `ping` and `ifconfig`, but no `ftp`, `wget` or `curl`.

**Planned design.**

1. **FTP protocol core**: connect, log in (anonymous by default), passive mode, list, download and upload in 64 KiB chunks straight to the file system, plus `CWD`, `PWD`, `MKD`, `RMD`, `SIZE` and `DELE`.
2. **FTP shell commands**: `ftp <host>` with `ls`, `cd`, `get`, `put`, `pwd`, `mkdir`, `rm` and `bye`, `wget ftp://` for anonymous downloads, and one shared `progress_bar_print()` helper that shows KB/s and time remaining on a single updating line.
3. **`wget`**: `-O`, `-q`, `-c` (resume with an HTTP `Range` request), `-r` (a two-level link crawl) and `--no-check-certificate`.
4. **`curl`**: `-X`, `-d`, `-d @file`, `-H`, `-s`, `-I`, `-L`, `-u`, `-v` and `-k`, with JSON pretty-printing.
5. **`ftpgui.exe`** (a stretch): local and remote panes, upload and download buttons with a progress bar, refresh and rename, and `ftp://` paths opened from the File Manager.
6. **Wi-Fi** (a stretch): scanning, WPA2 connection and a Wi-Fi tab in the network settings.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `ping`, `ifconfig` shell commands; ICMP echo | Shipped |
| Kernel sockets (`kern_socket()`, `kern_connect()` and friends) | Planned in the [DNS and Sockets](../networking/dns-sockets.md) roadmap |
| `http_get()`, `https_get()` | Planned in the [HTTP and TLS](../networking/http-tls.md) roadmap |
| `ftp_connect()`, `ftp_list()`, `ftp_download()`, `ftp_upload()` | Planned in this roadmap and in the networking FTP roadmap (see below) |
| `progress_bar_print()` | Planned in this roadmap, section 2 |
| List view and progress bar controls | Planned in the [widget library](../graphics/widget-library.md); today's control set is in [`controls.h`](../../include/desktop/controls.h) |

## How do I use it?

None of these commands exist yet. The nearest thing that runs today is `ping <ip>` in the shell, which sends four ICMP echo requests through the shipped IP stack.

## How does this relate to the other network roadmaps?

Three other roadmaps specify overlapping work, and the overlap has not been settled:

- **FTP protocol and shell commands** are also specified by the networking [SSH, FTP and SMB Clients](../networking/ssh-ftp.md) roadmap, which assigns only the graphical client to this file. The two disagree on header paths, function names and chunk sizes.
- **`wget` and `curl`** are also specified by the [HTTP and TLS](../networking/http-tls.md) roadmap, which writes the output flag as `-o` where this file writes `-O`, and names a different progress helper.
- **Wi-Fi** has its own driver roadmap, [Wi-Fi Drivers](../hardware/wifi-drivers.md), which owns the device interface, the 802.11 frames, the WPA2 supplicant and the settings tab. This file's section 6 predates it.

These are filed in [section 1](../../todo/11-apps/TODO-02-ftp-wget-wifi.md#1-ftp-protocol-core-sonnet) and [section 6](../../todo/11-apps/TODO-02-ftp-wget-wifi.md#6-wifi-framework-stretch-opus) so that each piece ends up with one owner. The unique content here is the flag set, the shared progress helper and the dual-pane GUI.

## What is not implemented yet?

Nothing in this roadmap has started:

- [FTP Protocol Core](../../todo/11-apps/TODO-02-ftp-wget-wifi.md#1-ftp-protocol-core-sonnet), which needs TCP sockets
- [FTP Shell Commands and `wget ftp://`](../../todo/11-apps/TODO-02-ftp-wget-wifi.md#2-ftp-shell-commands--wget-ftp-sonnet)
- [`wget`](../../todo/11-apps/TODO-02-ftp-wget-wifi.md#3-wget-command-httphttps--progress-sonnet) and [`curl`](../../todo/11-apps/TODO-02-ftp-wget-wifi.md#4-curl-command-full-flag-set-sonnet), which need the HTTP and TLS client
- [FTP GUI Client](../../todo/11-apps/TODO-02-ftp-wget-wifi.md#5-ftp-gui-client-dual-pane-stretch-sonnet), which needs the list view and progress bar controls
- [Wi-Fi Framework](../../todo/11-apps/TODO-02-ftp-wget-wifi.md#6-wifi-framework-stretch-opus), which belongs with the Wi-Fi driver roadmap

## How does it compare with Windows 11 and Linux?

Windows 11 includes `ftp.exe` and, since Windows 10, `curl`; it has no inbox `wget`, and users install FileZilla for a graphical FTP client. Wi-Fi is handled by WLAN AutoConfig. Linux distributions ship `wget` and `curl`, offer `ftp` or `lftp` and FileZilla, and manage Wi-Fi with NetworkManager and `wpa_supplicant`. The Impossible OS plan puts `ftp`, `wget` and `curl` in the base shell with one shared progress display, and adds its own dual-pane client. It does not exist yet.

## See also

- [FTP, wget, curl and Wi-Fi roadmap](../../todo/11-apps/TODO-02-ftp-wget-wifi.md)
- [SSH, FTP and SMB Clients](../networking/ssh-ftp.md)
- [HTTP and TLS](../networking/http-tls.md)
- [Wi-Fi Drivers](../hardware/wifi-drivers.md)
- [Terminal](../desktop/terminal.md)
- [File Manager](../desktop/file-manager.md)
