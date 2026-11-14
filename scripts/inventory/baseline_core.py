"""Completeness baselines: the rows, built from what the reference machine holds.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 5.

Shared by the guest reader (guest/read-baselines.py, which gathers the raw facts
inside the reference VM) and the host (baselines.py, which publishes them), so
the rule that turns a source into rows is written once and tested on the host.
Standard library only: the guest runs it under the capture runtime.

A baseline is a JSON Lines file, docs/design/inventory/<name>/baseline.json:
line 1 is its header ({"baseline", "owner", "source", "build" or "upstream",
"files"?}) and every other line one row ({"id", facts...}). A row holds facts
only (identifiers, names, paths, categories), never a description, keyword or
help string.
"""
import json
import re
import xml.etree.ElementTree as ET

# Each baseline and the surface whose capture must account for its rows.
OWNERS = {
    "settings": "settings",
    "control-panel": "control-panel",
    "known-folders": "explorer",
    "start-apps": "start",
    "controls": "system",
    "classic-controls": "system",
    "shell-commands": "explorer",
}
VM_BASELINES = ("settings", "control-panel", "known-folders", "start-apps", "classic-controls", "shell-commands")

# The WinUI Gallery control list, pinned: an import is reproducible, and a new
# Gallery release is taken by changing these on purpose.
GALLERY = {
    "repo": "microsoft/WinUI-Gallery",
    "commit": "6de91feccadfbb0a796b71caedead5a2284f730e",
    "path": "WinUIGallery/SampleSupport/Data/ControlInfoData.json",
    "license": "MIT",
}

# HKCR keys whose shell verbs the shell-commands baseline lists.
VERB_CLASSES = ("*", "Directory", "Directory\\Background", "Drive")

_GUID = re.compile(r"^\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}$")


class SourceError(ValueError):
    """A source did not have the shape the baseline reads; nothing is published."""


def _unique(rows, name):
    """The rows sorted by id; a repeated id is a SourceError (a key must be unique)."""
    seen = {}
    for r in rows:
        if not isinstance(r.get("id"), str) or not r["id"]:
            raise SourceError(f"{name}: a row has no id: {r!r}")
        if r["id"] in seen:
            raise SourceError(f"{name}: id {r['id']!r} appears twice")
        seen[r["id"]] = r
    if not seen:
        raise SourceError(f"{name}: the source yielded no rows")
    return [seen[k] for k in sorted(seen)]


def settings_rows(files):
    """Rows of the Settings search index.

    files: [(name, bytes)] of the AllSystemSettings_*.xml files. A build ships
    several; they are read largest first and the first file that names a
    setting supplies its row, so a setting is listed once (on 26300 the largest
    file holds every name of the others and 146 more). One row per
    SearchableContent: its Filename (the setting id), the Settings pages it
    sits on, its DeepLink when it has one, the condition and feature flag that
    gate it. Descriptions and keywords are resource references to Microsoft's
    text and are never read.
    """
    rows = {}
    for name, data in sorted(files, key=lambda f: (-len(f[1]), f[0])):
        try:
            root = ET.fromstring(data)
        except ET.ParseError as exc:
            raise SourceError(f"settings: {name} is not XML ({exc})") from None
        if root.tag != "PCSettings":
            raise SourceError(f"settings: {name}: root is {root.tag}, expected PCSettings")
        # Every file is read whole: a child that is not a setting, or a file with
        # no settings, is a shape this reader does not know, never an empty list.
        other = sorted({c.tag for c in root if c.tag != "SearchableContent"})
        if other:
            raise SourceError(f"settings: {name}: unexpected element(s) under PCSettings: {', '.join(other)}")
        if root.find("SearchableContent") is None:
            raise SourceError(f"settings: {name} holds no SearchableContent")
        for sc in root.findall("SearchableContent"):
            sid = (sc.findtext("Filename") or "").strip()
            if not sid:
                raise SourceError(f"settings: {name}: a SearchableContent has no Filename")
            if sid in rows:
                continue
            pages = []
            for node in sc.iter("Node"):
                if node.get("Type") == "Page" and node.text and node.text.strip() not in pages:
                    pages.append(node.text.strip())
            if not pages and sc.findtext("SettingIdentity/PageID"):
                pages.append(sc.findtext("SettingIdentity/PageID").strip())
            row = {"id": sid, "pages": pages}
            for key, at in (("deepLink", "ApplicationInformation/DeepLink"),
                            ("condition", "SettingIdentity/Condition")):
                text = (sc.findtext(at) or "").strip()
                if text:
                    row[key] = text
            for attr in ("IncludeWithFeature", "ExcludeWithFeature"):
                if sc.get(attr):
                    row[attr[0].lower() + attr[1:]] = sc.get(attr)
            rows[sid] = row
    return _unique(rows.values(), "settings")


def control_panel_rows(namespace, cpls):
    """Rows of the Control Panel: each NameSpace entry by CLSID with its
    canonical name (System.ApplicationName), and each .cpl file of System32.

    namespace: [(subkey name, canonical name or None)]; a subkey that is not a
    CLSID (DelegateFolders) is not an applet. cpls: file names.
    """
    rows = []
    for key, canonical in namespace:
        if not _GUID.match(key):
            continue
        row = {"id": f"clsid:{key.lower()}", "kind": "namespace"}
        if canonical:
            row["name"] = canonical
        rows.append(row)
    for f in cpls:
        if not f.lower().endswith(".cpl"):
            raise SourceError(f"control-panel: {f} is not a .cpl file")
        rows.append({"id": f"cpl:{f.lower()}", "kind": "cpl", "file": f})
    return _unique(rows, "control-panel")


def known_folder_rows(folders):
    """Rows of the known folders: one per FolderDescriptions subkey.

    folders: [(subkey CLSID, {value name: value})]. The row keeps the folder's
    canonical Name (its shell: name), Category (1 virtual, 2 fixed, 3 common,
    4 per user), RelativePath and ParentFolder; LocalizedName is a resource
    reference to Microsoft's text and is left out.
    """
    rows = []
    for key, values in folders:
        if not _GUID.match(key):
            raise SourceError(f"known-folders: subkey {key} is not a folder id")
        if not values.get("Name"):
            raise SourceError(f"known-folders: {key} has no Name")
        row = {"id": key.lower(), "name": values["Name"]}
        if "Category" in values:
            row["category"] = int(values["Category"])
        for field, value in (("relativePath", "RelativePath"), ("parent", "ParentFolder")):
            if values.get(value):
                row[field] = values[value].lower() if field == "parent" else values[value]
        rows.append(row)
    return _unique(rows, "known-folders")


# Any error stops the pipeline and fails the process, so a partial list never
# reaches ConvertTo-Json; the @() keeps a single app an array.
START_APPS_COMMAND = ("$ErrorActionPreference = 'Stop'; "
                      "ConvertTo-Json -Compress -InputObject @(Get-StartApps | Select-Object Name, AppID)")


def start_apps_from(returncode, stdout, stderr):
    """The app list Get-StartApps printed, or SourceError: a non-zero exit, any
    error text or output that is not a JSON list is a failed read, never a
    short list."""
    if returncode != 0 or stderr.strip():
        raise SourceError(f"start-apps: Get-StartApps failed (exit {returncode}): {stderr.strip()[:300]}")
    try:
        apps = json.loads(stdout)
    except ValueError as exc:
        raise SourceError(f"start-apps: Get-StartApps printed no JSON ({exc})") from None
    if not isinstance(apps, list) or not all(isinstance(a, dict) for a in apps):
        raise SourceError("start-apps: Get-StartApps did not print a list of apps")
    return apps


def start_app_rows(apps):
    """Rows of the Start menu's app list (Get-StartApps): AppID, name and kind.

    A packaged app's id carries a ! before its application id; anything else
    (a path, a known-folder relative path, an AutoGenerated id) is a desktop app.
    The same AppID listed twice under two names is one app, kept at its first.
    """
    rows, seen = [], set()
    for app in apps:
        aid, name = app.get("AppID"), app.get("Name")
        if not aid or not name:
            raise SourceError(f"start-apps: an entry lacks AppID or Name: {app!r}")
        if aid in seen:
            continue
        seen.add(aid)
        rows.append({"id": aid, "name": name, "kind": "packaged" if "!" in aid else "desktop"})
    return _unique(rows, "start-apps")


def control_rows(gallery):
    """Rows of the WinUI Gallery control list: each page of a control group,
    by UniqueId, with its Title and group. The special sections (Fundamentals,
    Design, Accessibility) teach concepts rather than show controls and are
    left out."""
    groups = gallery.get("Groups") if isinstance(gallery, dict) else None
    if not isinstance(groups, list):
        raise SourceError("controls: the Gallery data has no Groups list")
    rows = []
    for g in groups:
        if not isinstance(g, dict) or not g.get("UniqueId"):
            raise SourceError("controls: a group has no UniqueId")
        if g.get("IsSpecialSection"):
            continue
        items = g.get("Items")
        if not isinstance(items, list) or not items or not all(isinstance(i, dict) for i in items):
            raise SourceError(f"controls: group {g['UniqueId']} has no Items list")
        for item in items:
            if not item.get("UniqueId") or not item.get("Title"):
                raise SourceError(f"controls: an item of {g.get('UniqueId')} lacks UniqueId or Title")
            rows.append({"id": item["UniqueId"], "name": item["Title"], "category": g["UniqueId"]})
    return _unique(rows, "controls")


# What the two lookups of the class walk answer when there is nothing to find
# (measured on 26300.9711: 16,008 unused atoms, 318 names that are not classes,
# and no other code). Any other failure is a failed read, never "no class".
NO_SUCH_ATOM = 6      # ERROR_INVALID_HANDLE from GetClipboardFormatNameW
NO_SUCH_CLASS = 1411  # ERROR_CLASS_DOES_NOT_EXIST from GetClassInfoExW


def registered_classes(atoms, name_of, is_class):
    """The names in the user atom table that are window classes.

    name_of(atom) -> (name or None, last error); is_class(name) -> (bool, last
    error). A lookup that fails for any reason but "nothing there" raises
    SourceError, so a shortened list is never taken for the whole one."""
    names = []
    for atom in atoms:
        name, err = name_of(atom)
        if name is None:
            if err != NO_SUCH_ATOM:
                raise SourceError(f"classic-controls: reading atom {atom:#x} failed (error {err})")
            continue
        found, err = is_class(name)
        if found:
            names.append(name)
        elif err != NO_SUCH_CLASS:
            raise SourceError(f"classic-controls: looking up class {name!r} failed (error {err})")
    return names


def classic_control_rows(class_names, version):
    """Rows of the window classes comctl32 version 6 registers.

    class_names: the names found in the session's user atom table that
    GetClassInfoExW resolves under the comctl32 v6 module. User32's own classes
    resolve under any module too, so only the names comctl32 registers under its
    version prefix ("6.0.26100.9711!Button") are its own; the row is the name
    without the prefix.
    """
    prefix = f"{version}!"
    rows = [{"id": n[len(prefix):]} for n in class_names if n.startswith(prefix) and len(n) > len(prefix)]
    return _unique(rows, "classic-controls")


def shell_command_rows(folder_names, verbs):
    """Rows of the shell commands: every shell: folder name (the known folders'
    canonical names) and every verb registered under the VERB_CLASSES keys.

    verbs: {class key: [verb key names]}.
    """
    rows = [{"id": f"folder:{n}", "kind": "folder", "name": n} for n in folder_names]
    for cls in VERB_CLASSES:
        if cls not in verbs:
            raise SourceError(f"shell-commands: no verb list for HKCR\\{cls}")
        rows += [{"id": f"verb:{cls}/{v}", "kind": "verb", "class": cls, "verb": v} for v in verbs[cls]]
    return _unique(rows, "shell-commands")
