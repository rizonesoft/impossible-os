# Applications and Accessories

The apps that ship with the OS and the diagnostic tools beside them: a web browser, file transfer and SSH clients, mail, PDF and video viewers, chat and remote desktop clients, Notepad, Calculator, WordPad, Photos, a screenshot tool and archive manager, small accessories such as Sticky Notes and a colour picker, and two system tools, an object namespace browser and an event log viewer. None of these apps has been built yet, so each page says plainly what runs today (often a kernel call, a vendored library or a shell command the app will build on), which other roadmap owns the engine or protocol underneath, and which roadmap section owns each gap.

## Roadmap Overviews

One page per apps or tools roadmap file, each following the [page contract](../contributing/docs-page-contract.md).

| Document | Topics |
| --- | --- |
| [Web Browser App](web-browser.md) | Image, font and window calls it builds on; planned text browser, DOM, layout, chrome and settings |
| [FTP, wget, curl and Wi-Fi Tools](ftp-wget-wifi.md) | The shipped `ping` and IP stack; planned FTP client and commands, `wget`, `curl`, dual-pane GUI |
| [SSH Client](ssh-client.md) | Shipped Monocypher, Ed25519, SHA-256 and terminal calls; planned `ssh.exe`, host trust, `scp`, `sftp` |
| [Email Client App](email-client.md) | Shipped codecs and Registry; planned mail window, compose, encrypted credentials, spam filter |
| [PDF Viewer App](pdf-viewer.md) | Shipped image and inflate decoders; planned viewer window, search, file association, engine owner |
| [Video Player](video-player.md) | Drawing, timing and SSE2 build support; planned pl_mpeg player, A/V sync, playlist, subtitles |
| [Collaboration and Network Client Apps](collaboration-apps.md) | Shipped ARP and ICMP; planned VNC client, IRC, feed reader, `arp` and `route` commands |
| [Notepad App](notepad.md) | Text and file calls; planned line-ending aware editor, associations, recent files |
| [Calculator](calculator.md) | Shipped `kmath` functions; planned Standard, Scientific and Programmer modes with history |
| [WordPad](wordpad.md) | Text calls; planned rich text model, RTF reader and writer, ruler, toolbar, print to PDF |
| [Photos](photos.md) | Shipped image load, scale and save, wallpaper keys; planned viewer, slideshow, EXIF, editing |
| [Screenshot Tool and Archive Manager](screenshot-archive.md) | Frame buffer and PNG writer; planned capture hotkeys, snipping tool, ZIP manager |
| [Calendar, Sticky Notes and Utility Apps](calendar-utilities.md) | The shipped `sysinfo.exe`, hardware queries and key injection; planned accessories and shared dialogs |
| [ObBrowse: Object Namespace Browser](obbrowse.md) | Shipped directory-object syscalls and wrappers; planned console and GUI namespace browser |
| [Event Viewer](event-viewer.md) | The shipped `events.jsonl` log; planned console and GUI viewer with live tail |
