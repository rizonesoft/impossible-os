"""Show one Win32 dialog for the capture to walk: `python dialog_host.py <kind>`.

Runs in the reference VM as its own process, started by the common dialogs
module (dialogs-common.py). python.exe carries a comctl32 v6 manifest, so every
dialog here is drawn as a themed application draws it (powershell.exe has no
such manifest, 2026-11-20). The process ends when its dialog is dismissed, or
when the capture's close step ends it (its command line names this file).

Every text it passes is neutral sample text; nothing names a person or machine.
"""
import ctypes
import sys
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
comdlg32 = ctypes.WinDLL("comdlg32", use_last_error=True)
shell32 = ctypes.WinDLL("shell32", use_last_error=True)
comctl32 = ctypes.WinDLL("comctl32", use_last_error=True)
ole32 = ctypes.WinDLL("ole32", use_last_error=True)
credui = ctypes.WinDLL("credui", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

CAPTION = "Inventory sample"
SAMPLE = "C:\\Users\\ref\\Documents\\Explorer sample"
TEXT = "This is a sample message."
LONG_TEXT = ("This is a sample message long enough to show how the message box wraps its text. " * 6).strip()
HUNG_S = 900                              # the hung window's sleep; the capture ends it long before

# MessageBox types (WinUser.h): the button sets, the icons, the second default button.
BUTTON_SETS = {"ok": 0x0, "ok-cancel": 0x1, "abort-retry-ignore": 0x2, "yes-no-cancel": 0x3, "yes-no": 0x4,
               "retry-cancel": 0x5, "cancel-try-continue": 0x6}
ICONS = {"error": 0x10, "question": 0x20, "warning": 0x30, "information": 0x40}
MB_DEFBUTTON2 = 0x100

user32.MessageBoxW.argtypes = [wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.UINT]
user32.CreateWindowExW.restype = wintypes.HWND
user32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.HWND, wintypes.HMENU,
                                   wintypes.HINSTANCE, wintypes.LPVOID]
user32.IsDialogMessageW.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.MSG)]
user32.GetMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT]
user32.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
user32.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
user32.IsWindow.argtypes = [wintypes.HWND]
user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
user32.PeekMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT,
                                wintypes.UINT]
user32.RegisterWindowMessageW.argtypes = [wintypes.LPCWSTR]


def message_box(kind):
    """msgbox-<button set>, msgbox-icon-<icon>, msgbox-default-second, msgbox-long."""
    rest = kind[len("msgbox-"):]
    if rest in BUTTON_SETS:
        user32.MessageBoxW(None, TEXT, CAPTION, BUTTON_SETS[rest] | ICONS["information"])
    elif rest.startswith("icon-"):
        user32.MessageBoxW(None, TEXT, CAPTION, ICONS[rest[len("icon-"):]])
    elif rest == "default-second":
        user32.MessageBoxW(None, TEXT, CAPTION, BUTTON_SETS["ok-cancel"] | MB_DEFBUTTON2)
    elif rest == "long":
        user32.MessageBoxW(None, LONG_TEXT, CAPTION, ICONS["information"])
    else:
        raise SystemExit(f"dialog_host: no message box {kind!r}")


# ------------------------------------------------------------ common dialogs --

class OPENFILENAMEW(ctypes.Structure):
    _fields_ = [("lStructSize", wintypes.DWORD), ("hwndOwner", wintypes.HWND), ("hInstance", wintypes.HINSTANCE),
                ("lpstrFilter", wintypes.LPCWSTR), ("lpstrCustomFilter", wintypes.LPWSTR),
                ("nMaxCustFilter", wintypes.DWORD), ("nFilterIndex", wintypes.DWORD), ("lpstrFile", wintypes.LPWSTR),
                ("nMaxFile", wintypes.DWORD), ("lpstrFileTitle", wintypes.LPWSTR), ("nMaxFileTitle", wintypes.DWORD),
                ("lpstrInitialDir", wintypes.LPCWSTR), ("lpstrTitle", wintypes.LPCWSTR), ("Flags", wintypes.DWORD),
                ("nFileOffset", wintypes.WORD), ("nFileExtension", wintypes.WORD), ("lpstrDefExt", wintypes.LPCWSTR),
                ("lCustData", wintypes.LPARAM), ("lpfnHook", ctypes.c_void_p), ("lpTemplateName", wintypes.LPCWSTR),
                ("pvReserved", ctypes.c_void_p), ("dwReserved", wintypes.DWORD), ("FlagsEx", wintypes.DWORD)]


OFN_OVERWRITEPROMPT, OFN_FILEMUSTEXIST, OFN_PATHMUSTEXIST = 0x2, 0x1000, 0x800
OFN_ALLOWMULTISELECT, OFN_EXPLORER = 0x200, 0x80000
FILTER = "Text documents (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0"
FILE_CHARS = 4096


def file_dialog(save=False, multi=False, name=""):
    """GetOpenFileName / GetSaveFileName with no hook or template: Windows shows
    the Common Item Dialog for them, as it does for an application's own."""
    buf = ctypes.create_unicode_buffer(name, FILE_CHARS)
    ofn = OPENFILENAMEW(lStructSize=ctypes.sizeof(OPENFILENAMEW), lpstrFilter=FILTER, nFilterIndex=1,
                        lpstrFile=ctypes.cast(buf, wintypes.LPWSTR), nMaxFile=FILE_CHARS, lpstrInitialDir=SAMPLE,
                        lpstrDefExt="txt")
    if save:
        ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST
        comdlg32.GetSaveFileNameW(ctypes.byref(ofn))
    else:
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | (OFN_ALLOWMULTISELECT if multi else 0)
        comdlg32.GetOpenFileNameW(ctypes.byref(ofn))


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD), ("Data3", wintypes.WORD),
                ("Data4", ctypes.c_ubyte * 8)]

    def __init__(self, text):
        super().__init__()
        ole32.CLSIDFromString(text, ctypes.byref(self))


ole32.CLSIDFromString.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(GUID)]
ole32.CoCreateInstance.argtypes = [ctypes.POINTER(GUID), ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(GUID),
                                   ctypes.POINTER(ctypes.c_void_p)]
CLSID_FILE_OPEN_DIALOG = "{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}"
IID_IFILE_OPEN_DIALOG = "{D57C7288-D4AD-4768-BE02-9D969532D960}"
CLSCTX_INPROC_SERVER = 0x1
FOS_PICKFOLDERS, FOS_FORCEFILESYSTEM = 0x20, 0x40
# IFileOpenDialog's vtable: IUnknown (0-2), IModalWindow::Show (3), then IFileDialog
# SetFileTypes, SetFileTypeIndex, GetFileTypeIndex, Advise, Unadvise, SetOptions (9).
VT_SHOW, VT_SET_OPTIONS, VT_RELEASE = 3, 9, 2


def folder_picker():
    """The Common Item Dialog in its folder-pick form (FOS_PICKFOLDERS)."""
    ole32.CoInitialize(None)
    dialog = ctypes.c_void_p()
    hr = ole32.CoCreateInstance(ctypes.byref(GUID(CLSID_FILE_OPEN_DIALOG)), None, CLSCTX_INPROC_SERVER,
                                ctypes.byref(GUID(IID_IFILE_OPEN_DIALOG)), ctypes.byref(dialog))
    if hr:
        raise SystemExit(f"dialog_host: CoCreateInstance(FileOpenDialog) failed: {hr & 0xFFFFFFFF:#x}")
    vtbl = ctypes.cast(ctypes.cast(dialog, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))

    def method(index, *argtypes):
        return ctypes.WINFUNCTYPE(ctypes.HRESULT, ctypes.c_void_p, *argtypes)(vtbl[index])
    method(VT_SET_OPTIONS, wintypes.DWORD)(dialog, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM)
    try:
        method(VT_SHOW, wintypes.HWND)(dialog, None)
    except OSError:
        pass                              # cancelled: HRESULT_FROM_WIN32(ERROR_CANCELLED)
    ctypes.WINFUNCTYPE(wintypes.ULONG, ctypes.c_void_p)(vtbl[VT_RELEASE])(dialog)


# The Save dialog with an application's own control: IFileDialogCustomize adds a
# labelled Encoding selector, the slot Notepad fills (section 12's extra control).
CLSID_FILE_SAVE_DIALOG = "{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}"
IID_IFILE_SAVE_DIALOG = "{84BCCD23-5FDE-4CDB-AEA4-AF64B83D78AB}"
IID_IFILE_DIALOG_CUSTOMIZE = "{E6FDD21A-163F-4975-9C8C-A69F1BA37034}"
IID_ISHELL_ITEM = "{43826D1E-E718-42EE-BC55-A1E261C37BFE}"
# IFileDialog: SetFileTypes (4), SetFolder (12), SetFileName (15).
VT_SET_FILE_TYPES, VT_SET_FOLDER, VT_SET_FILE_NAME, VT_QUERY_INTERFACE = 4, 12, 15, 0
# IFileDialogCustomize: AddComboBox (6), AddControlItem (19),
# SetSelectedControlItem (25), StartVisualGroup (26), EndVisualGroup (27).
VT_ADD_COMBO, VT_ADD_ITEM, VT_SELECT_ITEM, VT_START_GROUP, VT_END_GROUP = 6, 19, 25, 26, 27
ENCODING_GROUP, ENCODING_COMBO = 1, 2
ENCODING_LABEL = "Encoding:"
ENCODINGS = ("UTF-8", "UTF-8 with BOM", "UTF-16 LE", "UTF-16 BE", "ANSI")


class COMDLG_FILTERSPEC(ctypes.Structure):
    _fields_ = [("pszName", wintypes.LPCWSTR), ("pszSpec", wintypes.LPCWSTR)]


shell32.SHCreateItemFromParsingName.argtypes = [wintypes.LPCWSTR, ctypes.c_void_p, ctypes.POINTER(GUID),
                                                ctypes.POINTER(ctypes.c_void_p)]


def com_method(obj, index, *argtypes, restype=ctypes.HRESULT):
    vtbl = ctypes.cast(ctypes.cast(obj, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    return lambda *args: ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *argtypes)(vtbl[index])(obj, *args)


def save_with_encoding():
    ole32.CoInitialize(None)
    dialog, custom, folder = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_void_p()
    hr = ole32.CoCreateInstance(ctypes.byref(GUID(CLSID_FILE_SAVE_DIALOG)), None, CLSCTX_INPROC_SERVER,
                                ctypes.byref(GUID(IID_IFILE_SAVE_DIALOG)), ctypes.byref(dialog))
    if hr:
        raise SystemExit(f"dialog_host: CoCreateInstance(FileSaveDialog) failed: {hr & 0xFFFFFFFF:#x}")
    com_method(dialog, VT_QUERY_INTERFACE, ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p))(
        ctypes.byref(GUID(IID_IFILE_DIALOG_CUSTOMIZE)), ctypes.byref(custom))
    specs = (COMDLG_FILTERSPEC * 2)(COMDLG_FILTERSPEC("Text documents (*.txt)", "*.txt"),
                                    COMDLG_FILTERSPEC("All files (*.*)", "*.*"))
    com_method(dialog, VT_SET_FILE_TYPES, wintypes.UINT, ctypes.c_void_p)(2, specs)
    shell32.SHCreateItemFromParsingName(SAMPLE, None, ctypes.byref(GUID(IID_ISHELL_ITEM)), ctypes.byref(folder))
    com_method(dialog, VT_SET_FOLDER, ctypes.c_void_p)(folder)
    com_method(dialog, VT_SET_FILE_NAME, wintypes.LPCWSTR)("Notes.txt")
    com_method(custom, VT_START_GROUP, wintypes.DWORD, wintypes.LPCWSTR)(ENCODING_GROUP, ENCODING_LABEL)
    com_method(custom, VT_ADD_COMBO, wintypes.DWORD)(ENCODING_COMBO)
    for n, name in enumerate(ENCODINGS):
        com_method(custom, VT_ADD_ITEM, wintypes.DWORD, wintypes.DWORD, wintypes.LPCWSTR)(ENCODING_COMBO, n, name)
    com_method(custom, VT_SELECT_ITEM, wintypes.DWORD, wintypes.DWORD)(ENCODING_COMBO, 0)
    com_method(custom, VT_END_GROUP)()
    try:
        com_method(dialog, VT_SHOW, wintypes.HWND)(None)
    except OSError:
        pass                              # cancelled: HRESULT_FROM_WIN32(ERROR_CANCELLED)
    for obj in (folder, custom, dialog):
        com_method(obj, VT_RELEASE, restype=wintypes.ULONG)()


class BROWSEINFOW(ctypes.Structure):
    _fields_ = [("hwndOwner", wintypes.HWND), ("pidlRoot", ctypes.c_void_p), ("pszDisplayName", wintypes.LPWSTR),
                ("lpszTitle", wintypes.LPCWSTR), ("ulFlags", wintypes.UINT), ("lpfn", ctypes.c_void_p),
                ("lParam", wintypes.LPARAM), ("iImage", ctypes.c_int)]


BIF_RETURNONLYFSDIRS, BIF_NEWDIALOGSTYLE = 0x1, 0x40
shell32.SHBrowseForFolderW.argtypes = [ctypes.POINTER(BROWSEINFOW)]
shell32.SHBrowseForFolderW.restype = ctypes.c_void_p


def browse_for_folder():
    """The older tree form an application still gets from SHBrowseForFolder."""
    ole32.CoInitialize(None)
    name = ctypes.create_unicode_buffer(260)
    info = BROWSEINFOW(pszDisplayName=ctypes.cast(name, wintypes.LPWSTR), lpszTitle="Choose a folder.",
                       ulFlags=BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE)
    shell32.SHBrowseForFolderW(ctypes.byref(info))


class CHOOSECOLORW(ctypes.Structure):
    _fields_ = [("lStructSize", wintypes.DWORD), ("hwndOwner", wintypes.HWND), ("hInstance", wintypes.HWND),
                ("rgbResult", wintypes.DWORD), ("lpCustColors", ctypes.POINTER(wintypes.DWORD)),
                ("Flags", wintypes.DWORD), ("lCustData", wintypes.LPARAM), ("lpfnHook", ctypes.c_void_p),
                ("lpTemplateName", wintypes.LPCWSTR)]


CC_RGBINIT, CC_FULLOPEN = 0x1, 0x2


def color(full):
    custom = (wintypes.DWORD * 16)(*([0x00FFFFFF] * 16))
    cc = CHOOSECOLORW(lStructSize=ctypes.sizeof(CHOOSECOLORW), lpCustColors=custom,
                      Flags=CC_RGBINIT | (CC_FULLOPEN if full else 0))
    comdlg32.ChooseColorW(ctypes.byref(cc))


class LOGFONTW(ctypes.Structure):
    _fields_ = [("lfHeight", wintypes.LONG), ("lfWidth", wintypes.LONG), ("lfEscapement", wintypes.LONG),
                ("lfOrientation", wintypes.LONG), ("lfWeight", wintypes.LONG), ("lfItalic", wintypes.BYTE),
                ("lfUnderline", wintypes.BYTE), ("lfStrikeOut", wintypes.BYTE), ("lfCharSet", wintypes.BYTE),
                ("lfOutPrecision", wintypes.BYTE), ("lfClipPrecision", wintypes.BYTE), ("lfQuality", wintypes.BYTE),
                ("lfPitchAndFamily", wintypes.BYTE), ("lfFaceName", wintypes.WCHAR * 32)]


class CHOOSEFONTW(ctypes.Structure):
    _fields_ = [("lStructSize", wintypes.DWORD), ("hwndOwner", wintypes.HWND), ("hDC", wintypes.HDC),
                ("lpLogFont", ctypes.POINTER(LOGFONTW)), ("iPointSize", ctypes.c_int), ("Flags", wintypes.DWORD),
                ("rgbColors", wintypes.DWORD), ("lCustData", wintypes.LPARAM), ("lpfnHook", ctypes.c_void_p),
                ("lpTemplateName", wintypes.LPCWSTR), ("hInstance", wintypes.HINSTANCE),
                ("lpszStyle", wintypes.LPWSTR), ("nFontType", wintypes.WORD), ("_pad", wintypes.WORD),
                ("nSizeMin", ctypes.c_int), ("nSizeMax", ctypes.c_int)]


CF_SCREENFONTS, CF_EFFECTS, CF_INITTOLOGFONTSTRUCT = 0x1, 0x100, 0x40


def font():
    lf = LOGFONTW(lfHeight=-15, lfWeight=400, lfFaceName="Segoe UI")
    cf = CHOOSEFONTW(lStructSize=ctypes.sizeof(CHOOSEFONTW), lpLogFont=ctypes.pointer(lf),
                     Flags=CF_SCREENFONTS | CF_EFFECTS | CF_INITTOLOGFONTSTRUCT)
    comdlg32.ChooseFontW(ctypes.byref(cf))


class FINDREPLACEW(ctypes.Structure):
    _fields_ = [("lStructSize", wintypes.DWORD), ("hwndOwner", wintypes.HWND), ("hInstance", wintypes.HINSTANCE),
                ("Flags", wintypes.DWORD), ("lpstrFindWhat", wintypes.LPWSTR), ("lpstrReplaceWith", wintypes.LPWSTR),
                ("wFindWhatLen", wintypes.WORD), ("wReplaceWithLen", wintypes.WORD), ("lCustData", wintypes.LPARAM),
                ("lpfnHook", ctypes.c_void_p), ("lpTemplateName", wintypes.LPCWSTR)]


FR_DOWN = 0x1
FR_DIALOGTERM = 0x40
FIND_CHARS = 256
comdlg32.FindTextW.restype = wintypes.HWND
comdlg32.ReplaceTextW.restype = wintypes.HWND


def owner_window():
    """A hidden window to own a modeless dialog or a dialog that requires an owner."""
    hwnd = user32.CreateWindowExW(0, "STATIC", CAPTION, 0, 0, 0, 0, 0, None, None, None, None)
    if not hwnd:
        raise SystemExit(f"dialog_host: CreateWindowEx failed ({ctypes.get_last_error()})")
    return hwnd


def find_replace(replace):
    """Find or Replace: modeless, so this process pumps its messages until the
    dialog is closed (FINDMSGSTRING with FR_DIALOGTERM)."""
    owner = owner_window()
    what = ctypes.create_unicode_buffer("sample", FIND_CHARS)
    with_ = ctypes.create_unicode_buffer("example", FIND_CHARS)
    fr = FINDREPLACEW(lStructSize=ctypes.sizeof(FINDREPLACEW), hwndOwner=owner, Flags=FR_DOWN,
                      lpstrFindWhat=ctypes.cast(what, wintypes.LPWSTR), wFindWhatLen=FIND_CHARS,
                      lpstrReplaceWith=ctypes.cast(with_, wintypes.LPWSTR), wReplaceWithLen=FIND_CHARS)
    dialog = (comdlg32.ReplaceTextW if replace else comdlg32.FindTextW)(ctypes.byref(fr))
    if not dialog:
        raise SystemExit(f"dialog_host: FindText/ReplaceText failed ({comdlg32.CommDlgExtendedError():#x})")
    msg = wintypes.MSG()
    while user32.IsWindow(dialog) and user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
        if not user32.IsDialogMessageW(dialog, ctypes.byref(msg)):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        if fr.Flags & FR_DIALOGTERM:
            break


class PAGESETUPDLGW(ctypes.Structure):
    _fields_ = [("lStructSize", wintypes.DWORD), ("hwndOwner", wintypes.HWND), ("hDevMode", wintypes.HGLOBAL),
                ("hDevNames", wintypes.HGLOBAL), ("Flags", wintypes.DWORD), ("ptPaperSize", wintypes.POINT),
                ("rtMinMargin", wintypes.RECT), ("rtMargin", wintypes.RECT), ("hInstance", wintypes.HINSTANCE),
                ("lCustData", wintypes.LPARAM), ("lpfnPageSetupHook", ctypes.c_void_p),
                ("lpfnPagePaintHook", ctypes.c_void_p), ("lpPageSetupTemplateName", wintypes.LPCWSTR),
                ("hPageSetupTemplate", wintypes.HGLOBAL)]


def page_setup():
    ps = PAGESETUPDLGW(lStructSize=ctypes.sizeof(PAGESETUPDLGW))
    comdlg32.PageSetupDlgW(ctypes.byref(ps))


class PRINTDLGEXW(ctypes.Structure):
    _fields_ = [("lStructSize", wintypes.DWORD), ("hwndOwner", wintypes.HWND), ("hDevMode", wintypes.HGLOBAL),
                ("hDevNames", wintypes.HGLOBAL), ("hDC", wintypes.HDC), ("Flags", wintypes.DWORD),
                ("Flags2", wintypes.DWORD), ("ExclusionFlags", wintypes.DWORD), ("nPageRanges", wintypes.DWORD),
                ("nMaxPageRanges", wintypes.DWORD), ("lpPageRanges", ctypes.c_void_p), ("nMinPage", wintypes.DWORD),
                ("nMaxPage", wintypes.DWORD), ("nCopies", wintypes.DWORD), ("hInstance", wintypes.HINSTANCE),
                ("lpPrintTemplateName", wintypes.LPCWSTR), ("lpCallback", ctypes.c_void_p),
                ("nPropertyPages", wintypes.DWORD), ("lphPropertyPages", ctypes.c_void_p),
                ("nStartPage", wintypes.DWORD), ("dwResultAction", wintypes.DWORD)]


class PRINTPAGERANGE(ctypes.Structure):
    _fields_ = [("nFromPage", wintypes.DWORD), ("nToPage", wintypes.DWORD)]


START_PAGE_GENERAL = 0xFFFFFFFF
PD_ALLPAGES, PD_PAGENUMS = 0x0, 0x2


def print_dialog():
    """PrintDlgEx: the property-sheet Print dialog with its printer list (it needs an owner)."""
    ranges = (PRINTPAGERANGE * 4)()
    pd = PRINTDLGEXW(lStructSize=ctypes.sizeof(PRINTDLGEXW), hwndOwner=owner_window(), Flags=PD_ALLPAGES,
                     nMaxPageRanges=4, lpPageRanges=ctypes.cast(ranges, ctypes.c_void_p), nMinPage=1, nMaxPage=10,
                     nCopies=1, nStartPage=START_PAGE_GENERAL)
    hr = comdlg32.PrintDlgExW(ctypes.byref(pd))
    if hr:
        raise SystemExit(f"dialog_host: PrintDlgEx failed: {hr & 0xFFFFFFFF:#x}")


# --------------------------------------------------------------- task dialog --

class TASKDIALOG_BUTTON(ctypes.Structure):
    _pack_ = 1                            # CommCtrl.h declares the task dialog structures packed
    _fields_ = [("nButtonID", ctypes.c_int), ("pszButtonText", wintypes.LPCWSTR)]


class TASKDIALOGCONFIG(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("cbSize", wintypes.UINT), ("hwndParent", wintypes.HWND), ("hInstance", wintypes.HINSTANCE),
                ("dwFlags", ctypes.c_int), ("dwCommonButtons", ctypes.c_int), ("pszWindowTitle", wintypes.LPCWSTR),
                ("pszMainIcon", ctypes.c_void_p), ("pszMainInstruction", wintypes.LPCWSTR),
                ("pszContent", wintypes.LPCWSTR), ("cButtons", wintypes.UINT),
                ("pButtons", ctypes.POINTER(TASKDIALOG_BUTTON)), ("nDefaultButton", ctypes.c_int),
                ("cRadioButtons", wintypes.UINT), ("pRadioButtons", ctypes.POINTER(TASKDIALOG_BUTTON)),
                ("nDefaultRadioButton", ctypes.c_int), ("pszVerificationText", wintypes.LPCWSTR),
                ("pszExpandedInformation", wintypes.LPCWSTR), ("pszExpandedControlText", wintypes.LPCWSTR),
                ("pszCollapsedControlText", wintypes.LPCWSTR), ("pszFooterIcon", ctypes.c_void_p),
                ("pszFooter", wintypes.LPCWSTR), ("pfCallback", ctypes.c_void_p), ("lpCallbackData", ctypes.c_ssize_t),
                ("cxWidth", wintypes.UINT)]


TDF_ENABLE_HYPERLINKS, TDF_USE_COMMAND_LINKS, TDF_EXPANDED_BY_DEFAULT = 0x1, 0x10, 0x80
TDF_SHOW_PROGRESS_BAR, TDF_ALLOW_DIALOG_CANCELLATION = 0x200, 0x8
TDCBF_CANCEL_BUTTON = 0x8
TD_INFORMATION_ICON = 0xFFFD              # MAKEINTRESOURCEW(-3)


def task_dialog():
    """Every part at once: main instruction, content, two command links, two radio
    buttons, the verification check box, the expander (open), the footer and the
    progress bar."""
    links = (TASKDIALOG_BUTTON * 2)(TASKDIALOG_BUTTON(101, "First choice\nWhat the first choice does."),
                                    TASKDIALOG_BUTTON(102, "Second choice\nWhat the second choice does."))
    radios = (TASKDIALOG_BUTTON * 2)(TASKDIALOG_BUTTON(201, "First option"), TASKDIALOG_BUTTON(202, "Second option"))
    cfg = TASKDIALOGCONFIG(
        cbSize=ctypes.sizeof(TASKDIALOGCONFIG),
        dwFlags=TDF_USE_COMMAND_LINKS | TDF_EXPANDED_BY_DEFAULT | TDF_SHOW_PROGRESS_BAR | TDF_ALLOW_DIALOG_CANCELLATION,
        dwCommonButtons=TDCBF_CANCEL_BUTTON, pszWindowTitle=CAPTION, pszMainIcon=TD_INFORMATION_ICON,
        pszMainInstruction="Choose how to go on", pszContent=TEXT, cButtons=2, pButtons=links,
        cRadioButtons=2, pRadioButtons=radios, nDefaultRadioButton=201, pszVerificationText="Do not ask again",
        pszExpandedInformation="More about the choices.", pszExpandedControlText="Hide details",
        pszCollapsedControlText="Show details", pszFooterIcon=TD_INFORMATION_ICON, pszFooter="A footer note.")
    button, radio, checked = ctypes.c_int(), ctypes.c_int(), wintypes.BOOL()
    hr = comctl32.TaskDialogIndirect(ctypes.byref(cfg), ctypes.byref(button), ctypes.byref(radio),
                                     ctypes.byref(checked))
    if hr:
        raise SystemExit(f"dialog_host: TaskDialogIndirect failed: {hr & 0xFFFFFFFF:#x}")


# ------------------------------------------------------------- shell dialogs --

class CREDUI_INFOW(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("hwndParent", wintypes.HWND), ("pszMessageText", wintypes.LPCWSTR),
                ("pszCaptionText", wintypes.LPCWSTR), ("hbmBanner", wintypes.HBITMAP)]


CREDUIWIN_GENERIC = 0x1


def credential():
    """The Windows Security credential prompt an application asks for (cancelled by the capture)."""
    info = CREDUI_INFOW(cbSize=ctypes.sizeof(CREDUI_INFOW), pszMessageText=TEXT, pszCaptionText=CAPTION)
    package, out, out_size, save = wintypes.ULONG(), ctypes.c_void_p(), wintypes.ULONG(), wintypes.BOOL()
    credui.CredUIPromptForWindowsCredentialsW(ctypes.byref(info), 0, ctypes.byref(package), None, 0,
                                              ctypes.byref(out), ctypes.byref(out_size), ctypes.byref(save),
                                              CREDUIWIN_GENERIC)


def about():
    """ShellAbout: the About frame an application gets from the shell."""
    shell32.ShellAboutW(None, CAPTION + "#" + CAPTION, TEXT, None)


def hung():
    """A window that stops answering: Windows ghosts it after five seconds, and
    closing the ghost asks whether to end the program."""
    hwnd = user32.CreateWindowExW(0, "STATIC", CAPTION, 0x10CF0000, 200, 200, 480, 320, None, None, None, None)
    user32.ShowWindow(hwnd, 5)
    msg = wintypes.MSG()
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:       # answer briefly, so the window is drawn
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        time.sleep(0.05)
    time.sleep(HUNG_S)


def crash():
    """A fatal fault reported to Windows Error Reporting, what it shows for a crash.
    Not a null read: ctypes turns that into a Python OSError."""
    kernel32.SetErrorMode(0)
    kernel32.RaiseFailFastException(None, None, 0)


KINDS = {
    "open": lambda: file_dialog(), "open-multi": lambda: file_dialog(multi=True),
    "save": lambda: file_dialog(save=True, name="Notes.txt"), "save-encoding": save_with_encoding,
    "folder-pick": folder_picker,
    "browse-folder": browse_for_folder, "color": lambda: color(False), "color-full": lambda: color(True),
    "font": font, "find": lambda: find_replace(False), "replace": lambda: find_replace(True),
    "page-setup": page_setup, "print": print_dialog, "task-dialog": task_dialog, "credential": credential,
    "about": about, "hung": hung, "crash": crash,
}


def main(argv):
    if len(argv) != 2:
        raise SystemExit("usage: dialog_host.py <kind>")
    kind = argv[1]
    if kind.startswith("msgbox-"):
        message_box(kind)
    elif kind in KINDS:
        KINDS[kind]()
    else:
        raise SystemExit(f"dialog_host: no dialog {kind!r} (there are: msgbox-*, {', '.join(sorted(KINDS))})")


if __name__ == "__main__":
    main(sys.argv)
