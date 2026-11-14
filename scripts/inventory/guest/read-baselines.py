"""Read the six completeness baselines the reference VM holds, inside the VM.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 5.

Run by scripts/inventory/baselines.py through ref-run.ps1 in ref's interactive
session (the window classes are the session's own):

    python C:\\ios\\in\\read-baselines.py --out C:\\ios\\out\\baselines.json

Writes {"build", "complete", "baselines": {name: {"source", "files"?, "rows"}}}
on success; on any failure {"complete": false, "incomplete": reason} and exit 3,
so the host never publishes part of a read. The Microsoft files read here stay in
the VM: only the rows leave it.
"""
import argparse
import ctypes
import glob
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import winreg
from ctypes import wintypes as w

_spec = importlib.util.spec_from_file_location(
    "baseline_core", os.path.join(os.path.dirname(os.path.abspath(__file__)), "baseline_core.py"))
core = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(core)

SETTINGS_GLOB = r"C:\Windows\ImmersiveControlPanel\Settings\AllSystemSettings_*.xml"
NAMESPACE = r"SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\ControlPanel\NameSpace"
FOLDERS = r"SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\FolderDescriptions"
CC6_MANIFEST = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                '<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0"><dependency>'
                '<dependentAssembly><assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" '
                'version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>'
                '</dependentAssembly></dependency></assembly>')
ICC_ALL = 0xFFFF                     # every InitCommonControlsEx class group
USER_ATOMS = range(0xC000, 0x10000)  # registered class names share the user atom table
START_APPS_TIMEOUT = 120             # deadline: Get-StartApps reads the Start menu's app list


def subkeys(root, path):
    with winreg.OpenKey(root, path) as k:
        n = winreg.QueryInfoKey(k)[0]
        return [winreg.EnumKey(k, i) for i in range(n)]


def values(root, path):
    out = {}
    with winreg.OpenKey(root, path) as k:
        for i in range(winreg.QueryInfoKey(k)[1]):
            name, value, _ = winreg.EnumValue(k, i)
            out[name] = value
    return out


def build():
    v = values(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion")
    return f"{v['CurrentBuild']}.{v['UBR']}"


def read_settings():
    files = []
    for path in sorted(glob.glob(SETTINGS_GLOB)):
        with open(path, "rb") as fh:
            data = fh.read()
        files.append((os.path.basename(path), data))
    if not files:
        raise core.SourceError(f"settings: no file matches {SETTINGS_GLOB}")
    return {"source": SETTINGS_GLOB, "rows": core.settings_rows(files),
            "files": [{"path": n, "sha256": hashlib.sha256(d).hexdigest()} for n, d in files]}


def read_control_panel():
    namespace = []
    for key in subkeys(winreg.HKEY_LOCAL_MACHINE, NAMESPACE):
        try:
            name = values(winreg.HKEY_CLASSES_ROOT, f"CLSID\\{key}").get("System.ApplicationName")
        except OSError:
            name = None
        namespace.append((key, name))
    system32 = os.path.join(os.environ["SystemRoot"], "System32")
    cpls = sorted(f for f in os.listdir(system32) if f.lower().endswith(".cpl"))
    return {"source": f"HKLM\\{NAMESPACE}; {system32}\\*.cpl", "rows": core.control_panel_rows(namespace, cpls)}


def read_known_folders():
    folders = [(k, values(winreg.HKEY_LOCAL_MACHINE, f"{FOLDERS}\\{k}")) for k in subkeys(winreg.HKEY_LOCAL_MACHINE, FOLDERS)]
    return {"source": f"HKLM\\{FOLDERS}", "rows": core.known_folder_rows(folders)}, folders


def read_start_apps():
    r = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", core.START_APPS_COMMAND],
                       capture_output=True, text=True, encoding="utf-8", timeout=START_APPS_TIMEOUT)
    return {"source": "Get-StartApps",
            "rows": core.start_app_rows(core.start_apps_from(r.returncode, r.stdout, r.stderr))}


class ACTCTXW(ctypes.Structure):
    _fields_ = [("cbSize", w.ULONG), ("dwFlags", w.DWORD), ("lpSource", w.LPCWSTR), ("wProcessorArchitecture", w.USHORT),
                ("wLangId", w.USHORT), ("lpAssemblyDirectory", w.LPCWSTR), ("lpResourceName", w.LPCWSTR),
                ("lpApplicationName", w.LPCWSTR), ("hModule", w.HMODULE)]


class ICCEX(ctypes.Structure):
    _fields_ = [("dwSize", w.DWORD), ("dwICC", w.DWORD)]


class WNDCLASSEXW(ctypes.Structure):
    _fields_ = [("cbSize", w.UINT), ("style", w.UINT), ("lpfnWndProc", ctypes.c_void_p), ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int), ("hInstance", w.HINSTANCE), ("hIcon", w.HICON), ("hCursor", w.HANDLE),
                ("hbrBackground", w.HBRUSH), ("lpszMenuName", w.LPCWSTR), ("lpszClassName", w.LPCWSTR), ("hIconSm", w.HICON)]


def read_classic_controls():
    """The classes comctl32 v6 registers, found without a list of names to look
    for: load it under a v6 activation context, register every class group,
    then ask, for every name in the user atom table, whether the class resolves
    under the comctl32 module (baseline_core.classic_control_rows keeps its own)."""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    u32 = ctypes.WinDLL("user32", use_last_error=True)
    k32.CreateActCtxW.restype = w.HANDLE
    k32.LoadLibraryW.restype = w.HMODULE
    manifest = os.path.join(os.environ["TEMP"], "ios-cc6.manifest")
    with open(manifest, "w", encoding="utf-8") as fh:
        fh.write(CC6_MANIFEST)
    ctx = ACTCTXW(cbSize=ctypes.sizeof(ACTCTXW), lpSource=manifest)
    handle = k32.CreateActCtxW(ctypes.byref(ctx))
    if handle in (None, w.HANDLE(-1).value):
        raise core.SourceError(f"classic-controls: CreateActCtxW failed ({ctypes.get_last_error()})")
    cookie = ctypes.c_size_t()
    if not k32.ActivateActCtx(w.HANDLE(handle), ctypes.byref(cookie)):
        raise core.SourceError(f"classic-controls: ActivateActCtx failed ({ctypes.get_last_error()})")
    module = k32.LoadLibraryW("comctl32.dll")
    if not module:
        raise core.SourceError(f"classic-controls: comctl32.dll did not load ({ctypes.get_last_error()})")
    path = ctypes.create_unicode_buffer(520)
    k32.GetModuleFileNameW(w.HMODULE(module), path, 520)
    if not ctypes.WinDLL(path.value).InitCommonControlsEx(ctypes.byref(ICCEX(ctypes.sizeof(ICCEX), ICC_ALL))):
        raise core.SourceError("classic-controls: InitCommonControlsEx failed")
    # The classes carry the side-by-side assembly's version, which names the
    # folder the activation context loaded comctl32 from:
    # amd64_microsoft.windows.common-controls_<key>_<version>_none_<hash>.
    folder = os.path.basename(os.path.dirname(path.value))
    parts = folder.split("_")
    if len(parts) < 4 or "common-controls" not in parts[1]:
        raise core.SourceError(f"classic-controls: comctl32 loaded from {path.value}, not a common-controls assembly")
    version = parts[3]
    names, buf = [], ctypes.create_unicode_buffer(256)
    for atom in USER_ATOMS:
        if u32.GetClipboardFormatNameW(atom, buf, 256):
            wc = WNDCLASSEXW(cbSize=ctypes.sizeof(WNDCLASSEXW))
            if u32.GetClassInfoExW(w.HINSTANCE(module), buf.value, ctypes.byref(wc)):
                names.append(buf.value)
    return {"source": f"comctl32 {version} ({folder})",
            "rows": core.classic_control_rows(names, version)}


def read_shell_commands(folders):
    verbs = {}
    for cls in core.VERB_CLASSES:
        try:
            verbs[cls] = subkeys(winreg.HKEY_CLASSES_ROOT, f"{cls}\\shell")
        except FileNotFoundError:
            verbs[cls] = []
    names = sorted({v["Name"] for _, v in folders if v.get("Name")})
    return {"source": "shell: names of the known folders; HKCR\\" + ", HKCR\\".join(f"{c}\\shell" for c in core.VERB_CLASSES),
            "rows": core.shell_command_rows(names, verbs)}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    out = {"complete": False}
    try:
        out["build"] = build()
        known, folders = read_known_folders()
        out["baselines"] = {"settings": read_settings(), "control-panel": read_control_panel(), "known-folders": known,
                            "start-apps": read_start_apps(), "classic-controls": read_classic_controls(),
                            "shell-commands": read_shell_commands(folders)}
        out["complete"] = True
    except (core.SourceError, OSError, ValueError, subprocess.TimeoutExpired) as exc:
        out = {"complete": False, "incomplete": f"{type(exc).__name__}: {exc}"}
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(out, fh, ensure_ascii=False)
    print("read-baselines: " + ("; ".join(f"{k} {len(v['rows'])} rows" for k, v in out["baselines"].items())
                                if out["complete"] else f"incomplete: {out['incomplete']}"))
    return 0 if out["complete"] else 3


if __name__ == "__main__":
    sys.exit(main())
