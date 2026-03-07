# 45 — App Installer & Package Manager

## Overview

Install/uninstall apps, manage installed programs, and optionally download from an app repository.

---

## Installer format (`.ipkg` — Impossible Package)

```
myapp.ipkg (ZIP archive containing):
├── manifest.ini         ← app name, version, author, icon
├── files/
│   ├── myapp.exe        ← main executable
│   ├── myapp.dll        ← optional libraries
│   └── data/            ← app data files
└── install.ini          ← install instructions
```

### manifest.ini
```ini
[Package]
Name = "My Application"
Version = "1.0.0"
Author = "Developer Name"
Icon = "icon.raw"
Description = "A useful application"
InstallPath = "C:\Programs\MyApp\"
StartMenu = 1
Desktop = 1
```

### install.ini
```ini
[Files]
myapp.exe = "C:\Programs\MyApp\myapp.exe"
myapp.dll = "C:\Programs\MyApp\myapp.dll"

[Registry]
System\Apps\MyApp\Version = "1.0.0"
System\Apps\MyApp\InstallPath = "C:\Programs\MyApp\"

[Shortcuts]
StartMenu = "My Application"
Desktop = "My Application"
```

## Uninstall: reverse the install.ini (delete files, remove Codex entries, remove shortcuts)
## Add/Remove Programs: list in Settings Panel `apps.spl` (see `17_settings_panel.md`)

## Files: `src/apps/installer/installer.c` (~400 lines), `tools/ipkg_create.c` (build tool)
## Implementation: 1-2 weeks
