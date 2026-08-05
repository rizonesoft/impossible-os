---
schema_version: 1
id: win32k-shadow-ssdt-master-table
domain: 08-graphics-ui
status: active
title: "Win32k Shadow SSDT Master Table"
---

# Win32k Shadow SSDT Master Table

> **SSDT Table 1.** Service numbers in the `0x1000+` range dispatch to Win32k (`NtGdi*` / `NtUser*`), separate from the main native SSDT (Table 0) in [`../02-kernel-core/TODO-A-SSDT-Master-Table.md`](../02-kernel-core/TODO-A-SSDT-Master-Table.md). Each range has headroom for future additions. The `§` column references [`TODO-15-win32k-shadow-ssdt.md`](TODO-15-win32k-shadow-ssdt.md). Routing, NTSTATUS contract, and filter/audit hooks for Table 1 are in [`TODO-16-win32k-shadow-native-api.md`](TODO-16-win32k-shadow-native-api.md).


**0x1000–0x100F: GDI Device Context and Object Management**

| Index  | Function                | §   | Owner | Done |
| ------ | ----------------------- | --- | ----- | ---- |
| 0x1000 | NtGdiCreateCompatibleDC | §2  | T12   | [ ]  |
| 0x1001 | NtGdiDeleteObjectApp    | §2  | T12   | [ ]  |
| 0x1002 | NtGdiSelectObject       | §2  | T12   | [ ]  |
| 0x1003 | NtGdiGetDC              | §2  | T12   | [ ]  |
| 0x1004 | NtGdiReleaseDC          | §2  | T12   | [ ]  |
| 0x1005 | NtGdiSaveDC             | §2  | T12   | [ ]  |
| 0x1006 | NtGdiRestoreDC          | §2  | T12   | [ ]  |

**0x1010–0x101F: GDI Drawing**

| Index  | Function          | §   | Owner | Done |
| ------ | ----------------- | --- | ----- | ---- |
| 0x1010 | NtGdiSetPixel     | §3  | T12   | [ ]  |
| 0x1011 | NtGdiGetPixel     | §3  | T12   | [ ]  |
| 0x1012 | NtGdiMoveTo       | §3  | T12   | [ ]  |
| 0x1013 | NtGdiLineTo       | §3  | T12   | [ ]  |
| 0x1014 | NtGdiRectangle    | §3  | T12   | [ ]  |
| 0x1015 | NtGdiFillRect     | §3  | T12   | [ ]  |
| 0x1016 | NtGdiEllipse      | §3  | T12   | [ ]  |
| 0x1017 | NtGdiBitBlt       | §3  | T12   | [ ]  |
| 0x1018 | NtGdiStretchBlt   | §3  | T12   | [ ]  |
| 0x1019 | NtGdiPatBlt       | §3  | T12   | [ ]  |
| 0x101A | NtGdiPolyline     | §3  | T12   | [ ]  |
| 0x101B | NtGdiPolygon      | §3  | T12   | [ ]  |
| 0x101C | NtGdiSetBkColor   | §3  | T12   | [ ]  |
| 0x101D | NtGdiSetTextColor | §3  | T12   | [ ]  |
| 0x101E | NtGdiSetBkMode    | §3  | T12   | [ ]  |

**0x1020–0x102F: GDI Text and Font**

| Index  | Function                | §   | Owner | Done |
| ------ | ----------------------- | --- | ----- | ---- |
| 0x1020 | NtGdiCreateFont         | §4  | T12   | [ ]  |
| 0x1021 | NtGdiTextOut            | §4  | T12   | [ ]  |
| 0x1022 | NtGdiExtTextOut         | §4  | T12   | [ ]  |
| 0x1023 | NtGdiDrawText           | §4  | T12   | [ ]  |
| 0x1024 | NtGdiGetTextMetrics     | §4  | T12   | [ ]  |
| 0x1025 | NtGdiGetTextExtentPoint | §4  | T12   | [ ]  |
| 0x1026 | NtGdiGetTextFace        | §4  | T12   | [ ]  |
| 0x1027 | NtGdiSetTextAlign       | §4  | T12   | [ ]  |

**0x1030–0x103F: GDI Bitmap and DIB**

| Index  | Function                    | §   | Owner | Done |
| ------ | --------------------------- | --- | ----- | ---- |
| 0x1030 | NtGdiCreateCompatibleBitmap | §5  | T12   | [ ]  |
| 0x1031 | NtGdiCreateDIBSection       | §5  | T12   | [ ]  |
| 0x1032 | NtGdiGetDIBits              | §5  | T12   | [ ]  |
| 0x1033 | NtGdiSetDIBits              | §5  | T12   | [ ]  |
| 0x1034 | NtGdiSetDIBitsToDevice      | §5  | T12   | [ ]  |
| 0x1035 | NtGdiStretchDIBits          | §5  | T12   | [ ]  |
| 0x1036 | NtGdiGetObject              | §5  | T12   | [ ]  |

**0x1040–0x104F: GDI Pen, Brush, and Region**

| Index  | Function                | §   | Owner | Done |
| ------ | ----------------------- | --- | ----- | ---- |
| 0x1040 | NtGdiCreatePen          | §6  | T12   | [ ]  |
| 0x1041 | NtGdiCreateSolidBrush   | §6  | T12   | [ ]  |
| 0x1042 | NtGdiCreateHatchBrush   | §6  | T12   | [ ]  |
| 0x1043 | NtGdiCreatePatternBrush | §6  | T12   | [ ]  |
| 0x1044 | NtGdiGetStockObject     | §6  | T12   | [ ]  |
| 0x1045 | NtGdiCreateRectRgn      | §6  | T12   | [ ]  |
| 0x1046 | NtGdiCombineRgn         | §6  | T12   | [ ]  |
| 0x1047 | NtGdiSelectClipRgn      | §6  | T12   | [ ]  |
| 0x1048 | NtGdiOffsetRgn          | §6  | T12   | [ ]  |
| 0x1049 | NtGdiPtInRegion         | §6  | T12   | [ ]  |

**0x1050–0x105F: USER Window Management**

| Index  | Function              | §   | Owner | Done |
| ------ | --------------------- | --- | ----- | ---- |
| 0x1050 | NtUserCreateWindowEx  | §7  | T12   | [ ]  |
| 0x1051 | NtUserDestroyWindow   | §7  | T12   | [ ]  |
| 0x1052 | NtUserShowWindow      | §7  | T12   | [ ]  |
| 0x1053 | NtUserMoveWindow      | §7  | T12   | [ ]  |
| 0x1054 | NtUserSetWindowPos    | §7  | T12   | [ ]  |
| 0x1055 | NtUserGetClientRect   | §7  | T12   | [ ]  |
| 0x1056 | NtUserGetWindowRect   | §7  | T12   | [ ]  |
| 0x1057 | NtUserSetWindowText   | §7  | T12   | [ ]  |
| 0x1058 | NtUserGetWindowText   | §7  | T12   | [ ]  |
| 0x1059 | NtUserInvalidateRect  | §7  | T12   | [ ]  |
| 0x105A | NtUserBeginPaint      | §7  | T12   | [ ]  |
| 0x105B | NtUserEndPaint        | §7  | T12   | [ ]  |
| 0x105C | NtUserSetFocus        | §7  | T12   | [ ]  |
| 0x105D | NtUserGetFocus        | §7  | T12   | [ ]  |
| 0x105E | NtUserIsWindow        | §7  | T12   | [ ]  |
| 0x105F | NtUserIsWindowVisible | §7  | T12   | [ ]  |

**0x1060–0x106F: USER Message Queue**

| Index  | Function               | §   | Owner                      | Done |
| ------ | ---------------------- | --- | -------------------------- | ---- |
| 0x1060 | NtUserGetMessage       | §8  | T12 (SYS_GETMESSAGE=74)    | [ ]  |
| 0x1061 | NtUserPeekMessage      | §8  | T12                        | [ ]  |
| 0x1062 | NtUserTranslateMessage | §8  | T12                        | [ ]  |
| 0x1063 | NtUserDispatchMessage  | §8  | T12                        | [ ]  |
| 0x1064 | NtUserPostMessage      | §8  | T12                        | [ ]  |
| 0x1065 | NtUserSendMessage      | §8  | T12                        | [ ]  |
| 0x1066 | NtUserWaitMessage      | §8  | T12 (SYS_WAIT_MESSAGE=75)  | [ ]  |
| 0x1067 | NtUserRegisterClassEx  | §8  | T12 (SYS_REGISTERCLASS=76) | [ ]  |
| 0x1068 | NtUserFindWindow       | §8  | T12 (SYS_FINDWINDOW=77)    | [ ]  |
| 0x1069 | NtUserPostQuitMessage  | §8  | T12                        | [ ]  |
| 0x106A | NtUserDefWindowProc    | §8  | T12                        | [ ]  |
| 0x106B | NtUserSetTimer         | §8  | T12                        | [ ]  |
| 0x106C | NtUserKillTimer        | §8  | T12                        | [ ]  |

**0x1070–0x107F: USER Input and Cursor**

| Index  | Function               | §   | Owner | Done |
| ------ | ---------------------- | --- | ----- | ---- |
| 0x1070 | NtUserGetKeyState      | §9  | T12   | [ ]  |
| 0x1071 | NtUserGetAsyncKeyState | §9  | T12   | [ ]  |
| 0x1072 | NtUserGetCursorPos     | §9  | T12   | [ ]  |
| 0x1073 | NtUserSetCursorPos     | §9  | T12   | [ ]  |
| 0x1074 | NtUserSetCursor        | §9  | T12   | [ ]  |
| 0x1075 | NtUserLoadCursor       | §9  | T12   | [ ]  |
| 0x1076 | NtUserShowCursor       | §9  | T12   | [ ]  |
| 0x1077 | NtUserGetCapture       | §9  | T12   | [ ]  |
| 0x1078 | NtUserSetCapture       | §9  | T12   | [ ]  |
| 0x1079 | NtUserReleaseCapture   | §9  | T12   | [ ]  |
| 0x107A | NtUserLoadIcon         | §9  | T12   | [ ]  |
| 0x107B | NtUserGetSystemMetrics | §9  | T12   | [ ]  |

**0x1080–0x108F: USER Menu and Accelerator**

| Index  | Function                      | §   | Owner | Done |
| ------ | ----------------------------- | --- | ----- | ---- |
| 0x1080 | NtUserCreateMenu              | §10 | T12   | [ ]  |
| 0x1081 | NtUserCreatePopupMenu         | §10 | T12   | [ ]  |
| 0x1082 | NtUserAppendMenu              | §10 | T12   | [ ]  |
| 0x1083 | NtUserInsertMenuItem          | §10 | T12   | [ ]  |
| 0x1084 | NtUserSetMenu                 | §10 | T12   | [ ]  |
| 0x1085 | NtUserDestroyMenu             | §10 | T12   | [ ]  |
| 0x1086 | NtUserTrackPopupMenu          | §10 | T12   | [ ]  |
| 0x1087 | NtUserCreateAcceleratorTable  | §10 | T12   | [ ]  |
| 0x1088 | NtUserTranslateAccelerator    | §10 | T12   | [ ]  |
| 0x1089 | NtUserDestroyAcceleratorTable | §10 | T12   | [ ]  |

**0x1090–0x109F: USER Clipboard**

| Index  | Function                         | §   | Owner | Done |
| ------ | -------------------------------- | --- | ----- | ---- |
| 0x1090 | NtUserOpenClipboard              | §11 | T12   | [ ]  |
| 0x1091 | NtUserCloseClipboard             | §11 | T12   | [ ]  |
| 0x1092 | NtUserEmptyClipboard             | §11 | T12   | [ ]  |
| 0x1093 | NtUserSetClipboardData           | §11 | T12   | [ ]  |
| 0x1094 | NtUserGetClipboardData           | §11 | T12   | [ ]  |
| 0x1095 | NtUserIsClipboardFormatAvailable | §11 | T12   | [ ]  |
| 0x1096 | NtUserCountClipboardFormats      | §11 | T12   | [ ]  |
| 0x1097 | NtUserEnumClipboardFormats       | §11 | T12   | [ ]  |
| 0x1098 | NtUserRegisterClipboardFormat    | §11 | T12   | [ ]  |
| 0x1099 | NtUserAddClipboardFormatListener | §11 | T12   | [ ]  |

**0x10A0–0x10CF: GDI Path and Curve Operations**

| Index  | Function               | §   | Owner | Done |
| ------ | ---------------------- | --- | ----- | ---- |
| 0x10A0 | NtGdiBeginPath         | §13 | T12   | [ ]  |
| 0x10A1 | NtGdiEndPath           | §13 | T12   | [ ]  |
| 0x10A2 | NtGdiStrokePath        | §13 | T12   | [ ]  |
| 0x10A3 | NtGdiFillPath          | §13 | T12   | [ ]  |
| 0x10A4 | NtGdiStrokeAndFillPath | §13 | T12   | [ ]  |
| 0x10A5 | NtGdiAbortPath         | §13 | T12   | [ ]  |
| 0x10A6 | NtGdiCloseFigure       | §13 | T12   | [ ]  |
| 0x10A7 | NtGdiFlattenPath       | §13 | T12   | [ ]  |
| 0x10A8 | NtGdiWidenPath         | §13 | T12   | [ ]  |
| 0x10A9 | NtGdiSelectClipPath    | §13 | T12   | [ ]  |
| 0x10AA | NtGdiPathToRegion      | §13 | T12   | [ ]  |
| 0x10AB | NtGdiGetPath           | §13 | T12   | [ ]  |
| 0x10AC | NtGdiArc               | §13 | T12   | [ ]  |
| 0x10AD | NtGdiArcTo             | §13 | T12   | [ ]  |
| 0x10AE | NtGdiAngleArc          | §13 | T12   | [ ]  |
| 0x10AF | NtGdiChord             | §13 | T12   | [ ]  |
| 0x10B0 | NtGdiPie               | §13 | T12   | [ ]  |
| 0x10B1 | NtGdiPolyBezier        | §13 | T12   | [ ]  |
| 0x10B2 | NtGdiPolyBezierTo      | §13 | T12   | [ ]  |
| 0x10B3 | NtGdiPolyDraw          | §13 | T12   | [ ]  |
| 0x10B4 | NtGdiPolyPolyline      | §13 | T12   | [ ]  |
| 0x10B5 | NtGdiPolyPolygon       | §13 | T12   | [ ]  |
| 0x10B6 | NtGdiPolyTextOut       | §13 | T12   | [ ]  |
| 0x10B7 | NtGdiRoundRect         | §13 | T12   | [ ]  |
| 0x10B8 | NtGdiSetMiterLimit     | §13 | T12   | [ ]  |
| 0x10B9 | NtGdiGetMiterLimit     | §13 | T12   | [ ]  |
| 0x10BA | NtGdiSetArcDirection   | §13 | T12   | [ ]  |
| 0x10BB | NtGdiGetArcDirection   | §13 | T12   | [ ]  |
| 0x10BC | NtGdiSetPolyFillMode   | §13 | T12   | [ ]  |
| 0x10BD | NtGdiGetPolyFillMode   | §13 | T12   | [ ]  |
| 0x10BE | NtGdiSetROP2           | §13 | T12   | [ ]  |
| 0x10BF | NtGdiGetROP2           | §13 | T12   | [ ]  |
| 0x10C0 | NtGdiGradientFill      | §13 | T12   | [ ]  |
| 0x10C1 | NtGdiAlphaBlend        | §13 | T12   | [ ]  |
| 0x10C2 | NtGdiTransparentBlt    | §13 | T12   | [ ]  |
| 0x10C3 | NtGdiPlgBlt            | §13 | T12   | [ ]  |
| 0x10C4 | NtGdiMaskBlt           | §13 | T12   | [ ]  |
| 0x10C5 | NtGdiFloodFill         | §13 | T12   | [ ]  |
| 0x10C6 | NtGdiExtFloodFill      | §13 | T12   | [ ]  |
| 0x10C7 | NtGdiDrawEdge          | §13 | T12   | [ ]  |
| 0x10C8 | NtGdiDrawFrameControl  | §13 | T12   | [ ]  |
| 0x10C9 | NtGdiFrameRgn          | §13 | T12   | [ ]  |
| 0x10CA | NtGdiFillRgn           | §13 | T12   | [ ]  |
| 0x10CB | NtGdiInvertRgn         | §13 | T12   | [ ]  |
| 0x10CC | NtGdiPaintRgn          | §13 | T12   | [ ]  |
| 0x10CD | NtGdiSetPixelFormat    | §13 | T12   | [ ]  |
| 0x10CE | NtGdiGetPixelFormat    | §13 | T12   | [ ]  |
| 0x10CF | NtGdiSwapBuffers       | §13 | T12   | [ ]  |

**0x10D0–0x10FF: GDI Transform and Coordinate**

| Index  | Function                     | §   | Owner | Done |
| ------ | ---------------------------- | --- | ----- | ---- |
| 0x10D0 | NtGdiSetMapMode              | §14 | T12   | [ ]  |
| 0x10D1 | NtGdiGetMapMode              | §14 | T12   | [ ]  |
| 0x10D2 | NtGdiSetViewportOrgEx        | §14 | T12   | [ ]  |
| 0x10D3 | NtGdiGetViewportOrgEx        | §14 | T12   | [ ]  |
| 0x10D4 | NtGdiSetViewportExtEx        | §14 | T12   | [ ]  |
| 0x10D5 | NtGdiGetViewportExtEx        | §14 | T12   | [ ]  |
| 0x10D6 | NtGdiSetWindowOrgEx          | §14 | T12   | [ ]  |
| 0x10D7 | NtGdiGetWindowOrgEx          | §14 | T12   | [ ]  |
| 0x10D8 | NtGdiSetWindowExtEx          | §14 | T12   | [ ]  |
| 0x10D9 | NtGdiGetWindowExtEx          | §14 | T12   | [ ]  |
| 0x10DA | NtGdiSetWorldTransform       | §14 | T12   | [ ]  |
| 0x10DB | NtGdiGetWorldTransform       | §14 | T12   | [ ]  |
| 0x10DC | NtGdiModifyWorldTransform    | §14 | T12   | [ ]  |
| 0x10DD | NtGdiCombineTransform        | §14 | T12   | [ ]  |
| 0x10DE | NtGdiSetGraphicsMode         | §14 | T12   | [ ]  |
| 0x10DF | NtGdiGetGraphicsMode         | §14 | T12   | [ ]  |
| 0x10E0 | NtGdiDPtoLP                  | §14 | T12   | [ ]  |
| 0x10E1 | NtGdiLPtoDP                  | §14 | T12   | [ ]  |
| 0x10E2 | NtGdiOffsetViewportOrgEx     | §14 | T12   | [ ]  |
| 0x10E3 | NtGdiOffsetWindowOrgEx       | §14 | T12   | [ ]  |
| 0x10E4 | NtGdiScaleViewportExtEx      | §14 | T12   | [ ]  |
| 0x10E5 | NtGdiScaleWindowExtEx        | §14 | T12   | [ ]  |
| 0x10E6 | NtGdiCreatePalette           | §14 | T12   | [ ]  |
| 0x10E7 | NtGdiSelectPalette           | §14 | T12   | [ ]  |
| 0x10E8 | NtGdiRealizePalette          | §14 | T12   | [ ]  |
| 0x10E9 | NtGdiGetNearestColor         | §14 | T12   | [ ]  |
| 0x10EA | NtGdiGetNearestPaletteIndex  | §14 | T12   | [ ]  |
| 0x10EB | NtGdiGetPaletteEntries       | §14 | T12   | [ ]  |
| 0x10EC | NtGdiSetPaletteEntries       | §14 | T12   | [ ]  |
| 0x10ED | NtGdiAnimatePalette          | §14 | T12   | [ ]  |
| 0x10EE | NtGdiResizePalette           | §14 | T12   | [ ]  |
| 0x10EF | NtGdiGetSystemPaletteEntries | §14 | T12   | [ ]  |
| 0x10F0 | NtGdiGetSystemPaletteUse     | §14 | T12   | [ ]  |
| 0x10F1 | NtGdiSetSystemPaletteUse     | §14 | T12   | [ ]  |
| 0x10F2 | NtGdiUpdateColors            | §14 | T12   | [ ]  |
| 0x10F3 | NtGdiGetColorSpace           | §14 | T12   | [ ]  |
| 0x10F4 | NtGdiSetColorSpace           | §14 | T12   | [ ]  |
| 0x10F5 | NtGdiGetICMProfile           | §14 | T12   | [ ]  |
| 0x10F6 | NtGdiSetICMProfile           | §14 | T12   | [ ]  |
| 0x10F7 | NtGdiSetICMMode              | §14 | T12   | [ ]  |
| 0x10F8 | NtGdiCheckColorsInGamut      | §14 | T12   | [ ]  |
| 0x10F9 | NtGdiColorMatchToTarget      | §14 | T12   | [ ]  |
| 0x10FA | NtGdiGetColorAdjustment      | §14 | T12   | [ ]  |
| 0x10FB | NtGdiSetColorAdjustment      | §14 | T12   | [ ]  |
| 0x10FC | NtGdiGetBoundsRect           | §14 | T12   | [ ]  |
| 0x10FD | NtGdiSetBoundsRect           | §14 | T12   | [ ]  |
| 0x10FE | NtGdiGetLayout               | §14 | T12   | [ ]  |
| 0x10FF | NtGdiSetLayout               | §14 | T12   | [ ]  |

**0x1100–0x112F: GDI Print and Metafile**

| Index  | Function                          | §   | Owner | Done |
| ------ | --------------------------------- | --- | ----- | ---- |
| 0x1100 | NtGdiStartDoc                     | §15 | T12   | [ ]  |
| 0x1101 | NtGdiEndDoc                       | §15 | T12   | [ ]  |
| 0x1102 | NtGdiStartPage                    | §15 | T12   | [ ]  |
| 0x1103 | NtGdiEndPage                      | §15 | T12   | [ ]  |
| 0x1104 | NtGdiAbortDoc                     | §15 | T12   | [ ]  |
| 0x1105 | NtGdiSetAbortProc                 | §15 | T12   | [ ]  |
| 0x1106 | NtGdiCreateDC                     | §15 | T12   | [ ]  |
| 0x1107 | NtGdiCreateIC                     | §15 | T12   | [ ]  |
| 0x1108 | NtGdiResetDC                      | §15 | T12   | [ ]  |
| 0x1109 | NtGdiGetDeviceCaps                | §15 | T12   | [ ]  |
| 0x110A | NtGdiEscape                       | §15 | T12   | [ ]  |
| 0x110B | NtGdiExtEscape                    | §15 | T12   | [ ]  |
| 0x110C | NtGdiCreateEnhMetaFile            | §15 | T12   | [ ]  |
| 0x110D | NtGdiCloseEnhMetaFile             | §15 | T12   | [ ]  |
| 0x110E | NtGdiPlayEnhMetaFile              | §15 | T12   | [ ]  |
| 0x110F | NtGdiGetEnhMetaFile               | §15 | T12   | [ ]  |
| 0x1110 | NtGdiDeleteEnhMetaFile            | §15 | T12   | [ ]  |
| 0x1111 | NtGdiGetEnhMetaFileHeader         | §15 | T12   | [ ]  |
| 0x1112 | NtGdiGetEnhMetaFileDescription    | §15 | T12   | [ ]  |
| 0x1113 | NtGdiGetEnhMetaFilePaletteEntries | §15 | T12   | [ ]  |
| 0x1114 | NtGdiEnumEnhMetaFile              | §15 | T12   | [ ]  |
| 0x1115 | NtGdiGetEnhMetaFilePixelFormat    | §15 | T12   | [ ]  |
| 0x1116 | NtGdiCopyEnhMetaFile              | §15 | T12   | [ ]  |
| 0x1117 | NtGdiGetWinMetaFileBits           | §15 | T12   | [ ]  |
| 0x1118 | NtGdiSetWinMetaFileBits           | §15 | T12   | [ ]  |
| 0x1119 | NtGdiGdiComment                   | §15 | T12   | [ ]  |

**0x1130–0x116F: GDI Font Advanced**

| Index  | Function                        | §   | Owner | Done |
| ------ | ------------------------------- | --- | ----- | ---- |
| 0x1130 | NtGdiGetFontData                | §16 | T12   | [ ]  |
| 0x1131 | NtGdiGetGlyphOutline            | §16 | T12   | [ ]  |
| 0x1132 | NtGdiGetGlyphIndices            | §16 | T12   | [ ]  |
| 0x1133 | NtGdiGetCharWidth               | §16 | T12   | [ ]  |
| 0x1134 | NtGdiGetCharWidth32             | §16 | T12   | [ ]  |
| 0x1135 | NtGdiGetCharWidthFloat          | §16 | T12   | [ ]  |
| 0x1136 | NtGdiGetCharABCWidths           | §16 | T12   | [ ]  |
| 0x1137 | NtGdiGetCharABCWidthsFloat      | §16 | T12   | [ ]  |
| 0x1138 | NtGdiGetKerningPairs            | §16 | T12   | [ ]  |
| 0x1139 | NtGdiGetOutlineTextMetrics      | §16 | T12   | [ ]  |
| 0x113A | NtGdiGetTextCharsetInfo         | §16 | T12   | [ ]  |
| 0x113B | NtGdiGetFontLanguageInfo        | §16 | T12   | [ ]  |
| 0x113C | NtGdiGetFontUnicodeRanges       | §16 | T12   | [ ]  |
| 0x113D | NtGdiEnumFontFamilies           | §16 | T12   | [ ]  |
| 0x113E | NtGdiEnumFontFamiliesEx         | §16 | T12   | [ ]  |
| 0x113F | NtGdiEnumFonts                  | §16 | T12   | [ ]  |
| 0x1140 | NtGdiAddFontResource            | §16 | T12   | [ ]  |
| 0x1141 | NtGdiAddFontResourceEx          | §16 | T12   | [ ]  |
| 0x1142 | NtGdiRemoveFontResource         | §16 | T12   | [ ]  |
| 0x1143 | NtGdiRemoveFontResourceEx       | §16 | T12   | [ ]  |
| 0x1144 | NtGdiAddFontMemResourceEx       | §16 | T12   | [ ]  |
| 0x1145 | NtGdiRemoveFontMemResourceEx    | §16 | T12   | [ ]  |
| 0x1146 | NtGdiCreateScalableFontResource | §16 | T12   | [ ]  |
| 0x1147 | NtGdiGetFontResourceInfo        | §16 | T12   | [ ]  |
| 0x1148 | NtGdiCreateFontIndirect         | §16 | T12   | [ ]  |
| 0x1149 | NtGdiCreateFontIndirectEx       | §16 | T12   | [ ]  |
| 0x114A | NtGdiGetCharacterPlacement      | §16 | T12   | [ ]  |
| 0x114B | NtGdiGetTextExtentExPoint       | §16 | T12   | [ ]  |
| 0x114C | NtGdiGetTextExtentPoint32       | §16 | T12   | [ ]  |
| 0x114D | NtGdiSetTextCharacterExtra      | §16 | T12   | [ ]  |
| 0x114E | NtGdiGetTextCharacterExtra      | §16 | T12   | [ ]  |
| 0x114F | NtGdiSetTextJustification       | §16 | T12   | [ ]  |
| 0x1150 | NtGdiTabbedTextOut              | §16 | T12   | [ ]  |
| 0x1151 | NtGdiGetTabbedTextExtent        | §16 | T12   | [ ]  |

**0x1160–0x117F: GDI Extended Object Management**

| Index  | Function                     | §   | Owner | Done |
| ------ | ---------------------------- | --- | ----- | ---- |
| 0x1160 | NtGdiExtCreatePen            | §17 | T12   | [ ]  |
| 0x1161 | NtGdiCreateBrushIndirect     | §17 | T12   | [ ]  |
| 0x1162 | NtGdiCreateDIBPatternBrush   | §17 | T12   | [ ]  |
| 0x1163 | NtGdiCreateDIBPatternBrushPt | §17 | T12   | [ ]  |
| 0x1164 | NtGdiCreateHalftonePalette   | §17 | T12   | [ ]  |
| 0x1165 | NtGdiGetObjectType           | §17 | T12   | [ ]  |
| 0x1166 | NtGdiGetCurrentObject        | §17 | T12   | [ ]  |
| 0x1167 | NtGdiGetCurrentPositionEx    | §17 | T12   | [ ]  |
| 0x1168 | NtGdiGetDCOrgEx              | §17 | T12   | [ ]  |
| 0x1169 | NtGdiGetDCBrushColor         | §17 | T12   | [ ]  |
| 0x116A | NtGdiSetDCBrushColor         | §17 | T12   | [ ]  |
| 0x116B | NtGdiGetDCPenColor           | §17 | T12   | [ ]  |
| 0x116C | NtGdiSetDCPenColor           | §17 | T12   | [ ]  |
| 0x116D | NtGdiGetStockBrush           | §17 | T12   | [ ]  |
| 0x116E | NtGdiGetStockPen             | §17 | T12   | [ ]  |
| 0x116F | NtGdiGetStockFont            | §17 | T12   | [ ]  |
| 0x1170 | NtGdiEnumObjects             | §17 | T12   | [ ]  |
| 0x1171 | NtGdiUnrealizeObject         | §17 | T12   | [ ]  |
| 0x1172 | NtGdiGetBitmapBits           | §17 | T12   | [ ]  |
| 0x1173 | NtGdiSetBitmapBits           | §17 | T12   | [ ]  |
| 0x1174 | NtGdiGetBitmapDimensionEx    | §17 | T12   | [ ]  |
| 0x1175 | NtGdiSetBitmapDimensionEx    | §17 | T12   | [ ]  |
| 0x1176 | NtGdiCreateBitmap            | §17 | T12   | [ ]  |
| 0x1177 | NtGdiCreateBitmapIndirect    | §17 | T12   | [ ]  |
| 0x1178 | NtGdiGetRegionData           | §17 | T12   | [ ]  |
| 0x1179 | NtGdiSetRectRgn              | §17 | T12   | [ ]  |
| 0x117A | NtGdiCreateRoundRectRgn      | §17 | T12   | [ ]  |
| 0x117B | NtGdiCreateEllipticRgn       | §17 | T12   | [ ]  |
| 0x117C | NtGdiCreatePolygonRgn        | §17 | T12   | [ ]  |
| 0x117D | NtGdiCreatePolyPolygonRgn    | §17 | T12   | [ ]  |
| 0x117E | NtGdiEqualRgn                | §17 | T12   | [ ]  |
| 0x117F | NtGdiRectInRegion            | §17 | T12   | [ ]  |

**0x1180–0x11CF: USER Window Properties, Styles, and Enumeration**

| Index  | Function                         | §   | Owner | Done |
| ------ | -------------------------------- | --- | ----- | ---- |
| 0x1180 | NtUserGetWindowLong              | §18 | T12   | [ ]  |
| 0x1181 | NtUserSetWindowLong              | §18 | T12   | [ ]  |
| 0x1182 | NtUserGetWindowLongPtr           | §18 | T12   | [ ]  |
| 0x1183 | NtUserSetWindowLongPtr           | §18 | T12   | [ ]  |
| 0x1184 | NtUserGetClassLong               | §18 | T12   | [ ]  |
| 0x1185 | NtUserSetClassLong               | §18 | T12   | [ ]  |
| 0x1186 | NtUserGetClassLongPtr            | §18 | T12   | [ ]  |
| 0x1187 | NtUserSetClassLongPtr            | §18 | T12   | [ ]  |
| 0x1188 | NtUserGetClassName               | §18 | T12   | [ ]  |
| 0x1189 | NtUserGetProp                    | §18 | T12   | [ ]  |
| 0x118A | NtUserSetProp                    | §18 | T12   | [ ]  |
| 0x118B | NtUserRemoveProp                 | §18 | T12   | [ ]  |
| 0x118C | NtUserEnumPropsEx                | §18 | T12   | [ ]  |
| 0x118D | NtUserGetWindowInfo              | §18 | T12   | [ ]  |
| 0x118E | NtUserGetWindowPlacement         | §18 | T12   | [ ]  |
| 0x118F | NtUserSetWindowPlacement         | §18 | T12   | [ ]  |
| 0x1190 | NtUserGetAncestor                | §18 | T12   | [ ]  |
| 0x1191 | NtUserGetParent                  | §18 | T12   | [ ]  |
| 0x1192 | NtUserSetParent                  | §18 | T12   | [ ]  |
| 0x1193 | NtUserGetWindow                  | §18 | T12   | [ ]  |
| 0x1194 | NtUserGetTopWindow               | §18 | T12   | [ ]  |
| 0x1195 | NtUserGetDesktopWindow           | §18 | T12   | [ ]  |
| 0x1196 | NtUserGetForegroundWindow        | §18 | T12   | [ ]  |
| 0x1197 | NtUserSetForegroundWindow        | §18 | T12   | [ ]  |
| 0x1198 | NtUserBringWindowToTop           | §18 | T12   | [ ]  |
| 0x1199 | NtUserEnumWindows                | §18 | T12   | [ ]  |
| 0x119A | NtUserEnumChildWindows           | §18 | T12   | [ ]  |
| 0x119B | NtUserEnumThreadWindows          | §18 | T12   | [ ]  |
| 0x119C | NtUserFindWindowEx               | §18 | T12   | [ ]  |
| 0x119D | NtUserGetLastActivePopup         | §18 | T12   | [ ]  |
| 0x119E | NtUserGetWindowThreadProcessId   | §18 | T12   | [ ]  |
| 0x119F | NtUserIsWindowUnicode            | §18 | T12   | [ ]  |
| 0x11A0 | NtUserIsChild                    | §18 | T12   | [ ]  |
| 0x11A1 | NtUserIsIconic                   | §18 | T12   | [ ]  |
| 0x11A2 | NtUserIsZoomed                   | §18 | T12   | [ ]  |
| 0x11A3 | NtUserIsHungAppWindow            | §18 | T12   | [ ]  |
| 0x11A4 | NtUserRealGetWindowClass         | §18 | T12   | [ ]  |
| 0x11A5 | NtUserGetWindowDC                | §18 | T12   | [ ]  |
| 0x11A6 | NtUserWindowFromPoint            | §18 | T12   | [ ]  |
| 0x11A7 | NtUserChildWindowFromPoint       | §18 | T12   | [ ]  |
| 0x11A8 | NtUserChildWindowFromPointEx     | §18 | T12   | [ ]  |
| 0x11A9 | NtUserRealChildWindowFromPoint   | §18 | T12   | [ ]  |
| 0x11AA | NtUserMapWindowPoints            | §18 | T12   | [ ]  |
| 0x11AB | NtUserScreenToClient             | §18 | T12   | [ ]  |
| 0x11AC | NtUserClientToScreen             | §18 | T12   | [ ]  |
| 0x11AD | NtUserLogicalToPhysicalPoint     | §18 | T12   | [ ]  |
| 0x11AE | NtUserPhysicalToLogicalPoint     | §18 | T12   | [ ]  |
| 0x11AF | NtUserSetWindowRgn               | §18 | T12   | [ ]  |
| 0x11B0 | NtUserGetWindowRgn               | §18 | T12   | [ ]  |
| 0x11B1 | NtUserGetUpdateRect              | §18 | T12   | [ ]  |
| 0x11B2 | NtUserGetUpdateRgn               | §18 | T12   | [ ]  |
| 0x11B3 | NtUserValidateRect               | §18 | T12   | [ ]  |
| 0x11B4 | NtUserValidateRgn                | §18 | T12   | [ ]  |
| 0x11B5 | NtUserInvalidateRgn              | §18 | T12   | [ ]  |
| 0x11B6 | NtUserRedrawWindow               | §18 | T12   | [ ]  |
| 0x11B7 | NtUserLockWindowUpdate           | §18 | T12   | [ ]  |
| 0x11B8 | NtUserScrollWindow               | §18 | T12   | [ ]  |
| 0x11B9 | NtUserScrollWindowEx             | §18 | T12   | [ ]  |
| 0x11BA | NtUserScrollDC                   | §18 | T12   | [ ]  |
| 0x11BB | NtUserSetLayeredWindowAttributes | §18 | T12   | [ ]  |
| 0x11BC | NtUserGetLayeredWindowAttributes | §18 | T12   | [ ]  |
| 0x11BD | NtUserUpdateLayeredWindow        | §18 | T12   | [ ]  |
| 0x11BE | NtUserPrintWindow                | §18 | T12   | [ ]  |
| 0x11BF | NtUserFlashWindowEx              | §18 | T12   | [ ]  |
| 0x11C0 | NtUserAnimateWindow              | §18 | T12   | [ ]  |
| 0x11C1 | NtUserSetWindowCompositionAttr   | §18 | T12   | [ ]  |
| 0x11C2 | NtUserGetWindowCompositionAttr   | §18 | T12   | [ ]  |
| 0x11C3 | NtUserSetWindowDisplayAffinity   | §18 | T12   | [ ]  |
| 0x11C4 | NtUserGetWindowDisplayAffinity   | §18 | T12   | [ ]  |
| 0x11C5 | NtUserEnableWindow               | §18 | T12   | [ ]  |
| 0x11C6 | NtUserIsWindowEnabled            | §18 | T12   | [ ]  |
| 0x11C7 | NtUserArrangeIconicWindows       | §18 | T12   | [ ]  |
| 0x11C8 | NtUserCascadeWindows             | §18 | T12   | [ ]  |
| 0x11C9 | NtUserTileWindows                | §18 | T12   | [ ]  |
| 0x11CA | NtUserCloseWindow                | §18 | T12   | [ ]  |
| 0x11CB | NtUserOpenIcon                   | §18 | T12   | [ ]  |
| 0x11CC | NtUserGetMinMaxInfo              | §18 | T12   | [ ]  |
| 0x11CD | NtUserSwitchToThisWindow         | §18 | T12   | [ ]  |

**0x11D0–0x120F: USER Dialog and MessageBox**

| Index  | Function                        | §   | Owner | Done |
| ------ | ------------------------------- | --- | ----- | ---- |
| 0x11D0 | NtUserCreateDialogParam         | §19 | T12   | [ ]  |
| 0x11D1 | NtUserCreateDialogIndirectParam | §19 | T12   | [ ]  |
| 0x11D2 | NtUserDialogBox                 | §19 | T12   | [ ]  |
| 0x11D3 | NtUserDialogBoxParam            | §19 | T12   | [ ]  |
| 0x11D4 | NtUserDialogBoxIndirectParam    | §19 | T12   | [ ]  |
| 0x11D5 | NtUserEndDialog                 | §19 | T12   | [ ]  |
| 0x11D6 | NtUserGetDlgItem                | §19 | T12   | [ ]  |
| 0x11D7 | NtUserSetDlgItemText            | §19 | T12   | [ ]  |
| 0x11D8 | NtUserGetDlgItemText            | §19 | T12   | [ ]  |
| 0x11D9 | NtUserSetDlgItemInt             | §19 | T12   | [ ]  |
| 0x11DA | NtUserGetDlgItemInt             | §19 | T12   | [ ]  |
| 0x11DB | NtUserGetDlgCtrlID              | §19 | T12   | [ ]  |
| 0x11DC | NtUserGetNextDlgTabItem         | §19 | T12   | [ ]  |
| 0x11DD | NtUserGetNextDlgGroupItem       | §19 | T12   | [ ]  |
| 0x11DE | NtUserIsDialogMessage           | §19 | T12   | [ ]  |
| 0x11DF | NtUserSendDlgItemMessage        | §19 | T12   | [ ]  |
| 0x11E0 | NtUserMapDialogRect             | §19 | T12   | [ ]  |
| 0x11E1 | NtUserCheckDlgButton            | §19 | T12   | [ ]  |
| 0x11E2 | NtUserCheckRadioButton          | §19 | T12   | [ ]  |
| 0x11E3 | NtUserIsDlgButtonChecked        | §19 | T12   | [ ]  |
| 0x11E4 | NtUserMessageBox                | §19 | T12   | [ ]  |
| 0x11E5 | NtUserMessageBoxEx              | §19 | T12   | [ ]  |
| 0x11E6 | NtUserMessageBoxIndirect        | §19 | T12   | [ ]  |
| 0x11E7 | NtUserCreateCaret               | §19 | T12   | [ ]  |
| 0x11E8 | NtUserDestroyCaret              | §19 | T12   | [ ]  |
| 0x11E9 | NtUserShowCaret                 | §19 | T12   | [ ]  |
| 0x11EA | NtUserHideCaret                 | §19 | T12   | [ ]  |
| 0x11EB | NtUserSetCaretPos               | §19 | T12   | [ ]  |
| 0x11EC | NtUserGetCaretPos               | §19 | T12   | [ ]  |
| 0x11ED | NtUserSetCaretBlinkTime         | §19 | T12   | [ ]  |
| 0x11EE | NtUserGetCaretBlinkTime         | §19 | T12   | [ ]  |
| 0x11EF | NtUserDrawText                  | §19 | T12   | [ ]  |
| 0x11F0 | NtUserDrawTextEx                | §19 | T12   | [ ]  |
| 0x11F1 | NtUserGrayString                | §19 | T12   | [ ]  |
| 0x11F2 | NtUserDrawIcon                  | §19 | T12   | [ ]  |
| 0x11F3 | NtUserDrawIconEx                | §19 | T12   | [ ]  |
| 0x11F4 | NtUserDrawCaption               | §19 | T12   | [ ]  |
| 0x11F5 | NtUserDrawMenuBar               | §19 | T12   | [ ]  |
| 0x11F6 | NtUserDrawEdge                  | §19 | T12   | [ ]  |
| 0x11F7 | NtUserDrawFrameControl          | §19 | T12   | [ ]  |
| 0x11F8 | NtUserDrawFocusRect             | §19 | T12   | [ ]  |
| 0x11F9 | NtUserFillRect                  | §19 | T12   | [ ]  |
| 0x11FA | NtUserFrameRect                 | §19 | T12   | [ ]  |
| 0x11FB | NtUserInvertRect                | §19 | T12   | [ ]  |
| 0x11FC | NtUserExcludeUpdateRgn          | §19 | T12   | [ ]  |
| 0x11FD | NtUserGetDCEx                   | §19 | T12   | [ ]  |
| 0x11FE | NtUserSetScrollInfo             | §20 | T12   | [ ]  |
| 0x11FF | NtUserGetScrollInfo             | §20 | T12   | [ ]  |
| 0x1200 | NtUserShowScrollBar             | §20 | T12   | [ ]  |
| 0x1201 | NtUserEnableScrollBar           | §20 | T12   | [ ]  |
| 0x1202 | NtUserSetScrollPos              | §20 | T12   | [ ]  |
| 0x1203 | NtUserGetScrollPos              | §20 | T12   | [ ]  |
| 0x1204 | NtUserSetScrollRange            | §20 | T12   | [ ]  |
| 0x1205 | NtUserGetScrollRange            | §20 | T12   | [ ]  |
| 0x1206 | NtUserGetScrollBarInfo          | §20 | T12   | [ ]  |
| 0x1207 | NtUserSBGetParms                | §20 | T12   | [ ]  |

**0x1210–0x126F: USER Keyboard, IME, and Hook**

| Index  | Function                      | §   | Owner | Done |
| ------ | ----------------------------- | --- | ----- | ---- |
| 0x1210 | NtUserGetKeyboardState        | §21 | T12   | [ ]  |
| 0x1211 | NtUserSetKeyboardState        | §21 | T12   | [ ]  |
| 0x1212 | NtUserGetKeyboardLayout       | §21 | T12   | [ ]  |
| 0x1213 | NtUserGetKeyboardLayoutList   | §21 | T12   | [ ]  |
| 0x1214 | NtUserActivateKeyboardLayout  | §21 | T12   | [ ]  |
| 0x1215 | NtUserLoadKeyboardLayout      | §21 | T12   | [ ]  |
| 0x1216 | NtUserUnloadKeyboardLayout    | §21 | T12   | [ ]  |
| 0x1217 | NtUserMapVirtualKey           | §21 | T12   | [ ]  |
| 0x1218 | NtUserMapVirtualKeyEx         | §21 | T12   | [ ]  |
| 0x1219 | NtUserToUnicode               | §21 | T12   | [ ]  |
| 0x121A | NtUserToUnicodeEx             | §21 | T12   | [ ]  |
| 0x121B | NtUserVkKeyScan               | §21 | T12   | [ ]  |
| 0x121C | NtUserVkKeyScanEx             | §21 | T12   | [ ]  |
| 0x121D | NtUserGetKeyNameText          | §21 | T12   | [ ]  |
| 0x121E | NtUserOemKeyScan              | §21 | T12   | [ ]  |
| 0x121F | NtUserSendInput               | §21 | T12   | [ ]  |
| 0x1220 | NtUserKeybd_event             | §21 | T12   | [ ]  |
| 0x1221 | NtUserMouse_event             | §21 | T12   | [ ]  |
| 0x1222 | NtUserBlockInput              | §21 | T12   | [ ]  |
| 0x1223 | NtUserSetWindowsHookEx        | §21 | T12   | [ ]  |
| 0x1224 | NtUserUnhookWindowsHookEx     | §21 | T12   | [ ]  |
| 0x1225 | NtUserCallNextHookEx          | §21 | T12   | [ ]  |
| 0x1226 | NtUserSetWindowsHook          | §21 | T12   | [ ]  |
| 0x1227 | NtUserUnhookWindowsHook       | §21 | T12   | [ ]  |
| 0x1228 | NtUserCallMsgFilter           | §21 | T12   | [ ]  |
| 0x1229 | NtUserRegisterHotKey          | §21 | T12   | [ ]  |
| 0x122A | NtUserUnregisterHotKey        | §21 | T12   | [ ]  |
| 0x122B | NtUserGetHotKey               | §21 | T12   | [ ]  |
| 0x122C | NtUserImmGetContext           | §21 | T12   | [ ]  |
| 0x122D | NtUserImmReleaseContext       | §21 | T12   | [ ]  |
| 0x122E | NtUserImmGetCompositionString | §21 | T12   | [ ]  |
| 0x122F | NtUserImmSetCompositionString | §21 | T12   | [ ]  |
| 0x1230 | NtUserImmGetCompositionWindow | §21 | T12   | [ ]  |
| 0x1231 | NtUserImmSetCompositionWindow | §21 | T12   | [ ]  |
| 0x1232 | NtUserImmGetCandidateList     | §21 | T12   | [ ]  |
| 0x1233 | NtUserImmNotifyIME            | §21 | T12   | [ ]  |
| 0x1234 | NtUserImmGetConversionStatus  | §21 | T12   | [ ]  |
| 0x1235 | NtUserImmSetConversionStatus  | §21 | T12   | [ ]  |
| 0x1236 | NtUserImmGetOpenStatus        | §21 | T12   | [ ]  |
| 0x1237 | NtUserImmSetOpenStatus        | §21 | T12   | [ ]  |
| 0x1238 | NtUserImmGetDefaultIMEWnd     | §21 | T12   | [ ]  |
| 0x1239 | NtUserImmAssociateContext     | §21 | T12   | [ ]  |
| 0x123A | NtUserImmAssociateContextEx   | §21 | T12   | [ ]  |
| 0x123B | NtUserImmCreateContext        | §21 | T12   | [ ]  |
| 0x123C | NtUserImmDestroyContext       | §21 | T12   | [ ]  |
| 0x123D | NtUserImmDisableIME           | §21 | T12   | [ ]  |

**0x1240–0x127F: USER DPI, Accessibility, and SystemParametersInfo**

| Index  | Function                          | §   | Owner | Done |
| ------ | --------------------------------- | --- | ----- | ---- |
| 0x1240 | NtUserSystemParametersInfo        | §22 | T12   | [ ]  |
| 0x1241 | NtUserGetDpiForWindow             | §22 | T12   | [ ]  |
| 0x1242 | NtUserGetDpiForSystem             | §22 | T12   | [ ]  |
| 0x1243 | NtUserGetSystemDpiForProcess      | §22 | T12   | [ ]  |
| 0x1244 | NtUserSetProcessDpiAwareness      | §22 | T12   | [ ]  |
| 0x1245 | NtUserGetProcessDpiAwareness      | §22 | T12   | [ ]  |
| 0x1246 | NtUserSetProcessDpiAwarenessCtx   | §22 | T12   | [ ]  |
| 0x1247 | NtUserGetThreadDpiAwarenessCtx    | §22 | T12   | [ ]  |
| 0x1248 | NtUserSetThreadDpiAwarenessCtx    | §22 | T12   | [ ]  |
| 0x1249 | NtUserAreDpiAwarenessCtxEqual     | §22 | T12   | [ ]  |
| 0x124A | NtUserGetDpiFromDpiAwarenessCtx   | §22 | T12   | [ ]  |
| 0x124B | NtUserAdjustWindowRectExForDpi    | §22 | T12   | [ ]  |
| 0x124C | NtUserGetSystemMetricsForDpi      | §22 | T12   | [ ]  |
| 0x124D | NtUserEnableNonClientDpiScaling   | §22 | T12   | [ ]  |
| 0x124E | NtUserSetDialogDpiChangeBehavior  | §22 | T12   | [ ]  |
| 0x124F | NtUserGetDialogDpiChangeBehavior  | §22 | T12   | [ ]  |
| 0x1250 | NtUserGetDoubleClickTime          | §22 | T12   | [ ]  |
| 0x1251 | NtUserSetDoubleClickTime          | §22 | T12   | [ ]  |
| 0x1252 | NtUserSwapMouseButton             | §22 | T12   | [ ]  |
| 0x1253 | NtUserGetCursorInfo               | §22 | T12   | [ ]  |
| 0x1254 | NtUserClipCursor                  | §22 | T12   | [ ]  |
| 0x1255 | NtUserGetClipCursor               | §22 | T12   | [ ]  |
| 0x1256 | NtUserGetCaretBlink               | §22 | T12   | [ ]  |
| 0x1257 | NtUserGetSysColor                 | §22 | T12   | [ ]  |
| 0x1258 | NtUserSetSysColors                | §22 | T12   | [ ]  |
| 0x1259 | NtUserGetSysColorBrush            | §22 | T12   | [ ]  |
| 0x125A | NtUserMessageBeep                 | §22 | T12   | [ ]  |
| 0x125B | NtUserGetInputState               | §22 | T12   | [ ]  |
| 0x125C | NtUserGetQueueStatus              | §22 | T12   | [ ]  |
| 0x125D | NtUserAttachThreadInput           | §22 | T12   | [ ]  |
| 0x125E | NtUserInSendMessage               | §22 | T12   | [ ]  |
| 0x125F | NtUserInSendMessageEx             | §22 | T12   | [ ]  |
| 0x1260 | NtUserReplyMessage                | §22 | T12   | [ ]  |
| 0x1261 | NtUserWaitForInputIdle            | §22 | T12   | [ ]  |
| 0x1262 | NtUserMsgWaitForMultipleObjects   | §22 | T12   | [ ]  |
| 0x1263 | NtUserMsgWaitForMultipleObjectsEx | §22 | T12   | [ ]  |
| 0x1264 | NtUserSetMessageExtraInfo         | §22 | T12   | [ ]  |
| 0x1265 | NtUserGetMessageExtraInfo         | §22 | T12   | [ ]  |
| 0x1266 | NtUserGetMessagePos               | §22 | T12   | [ ]  |
| 0x1267 | NtUserGetMessageTime              | §22 | T12   | [ ]  |

**0x1270–0x129F: USER Raw Input, Touch, and Gesture**

| Index  | Function                         | §   | Owner | Done |
| ------ | -------------------------------- | --- | ----- | ---- |
| 0x1270 | NtUserRegisterRawInputDevices    | §23 | T12   | [ ]  |
| 0x1271 | NtUserGetRawInputData            | §23 | T12   | [ ]  |
| 0x1272 | NtUserGetRawInputBuffer          | §23 | T12   | [ ]  |
| 0x1273 | NtUserGetRawInputDeviceInfo      | §23 | T12   | [ ]  |
| 0x1274 | NtUserGetRawInputDeviceList      | §23 | T12   | [ ]  |
| 0x1275 | NtUserGetRegisteredRawInputDev   | §23 | T12   | [ ]  |
| 0x1276 | NtUserDefRawInputProc            | §23 | T12   | [ ]  |
| 0x1277 | NtUserRegisterTouchWindow        | §23 | T12   | [ ]  |
| 0x1278 | NtUserUnregisterTouchWindow      | §23 | T12   | [ ]  |
| 0x1279 | NtUserIsTouchWindow              | §23 | T12   | [ ]  |
| 0x127A | NtUserGetTouchInputInfo          | §23 | T12   | [ ]  |
| 0x127B | NtUserCloseTouchInputHandle      | §23 | T12   | [ ]  |
| 0x127C | NtUserRegisterPointerInputTarget | §23 | T12   | [ ]  |
| 0x127D | NtUserGetPointerInfo             | §23 | T12   | [ ]  |
| 0x127E | NtUserGetPointerFrameInfo        | §23 | T12   | [ ]  |
| 0x127F | NtUserGetPointerType             | §23 | T12   | [ ]  |
| 0x1280 | NtUserGetPointerDevices          | §23 | T12   | [ ]  |
| 0x1281 | NtUserGetPointerDevice           | §23 | T12   | [ ]  |
| 0x1282 | NtUserGetPointerDeviceRects      | §23 | T12   | [ ]  |
| 0x1283 | NtUserSetGestureConfig           | §23 | T12   | [ ]  |
| 0x1284 | NtUserGetGestureConfig           | §23 | T12   | [ ]  |
| 0x1285 | NtUserGetGestureInfo             | §23 | T12   | [ ]  |
| 0x1286 | NtUserGetGestureExtraArgs        | §23 | T12   | [ ]  |
| 0x1287 | NtUserCloseGestureInfoHandle     | §23 | T12   | [ ]  |
| 0x1288 | NtUserBeginDeferWindowPos        | §23 | T12   | [ ]  |
| 0x1289 | NtUserDeferWindowPos             | §23 | T12   | [ ]  |
| 0x128A | NtUserEndDeferWindowPos          | §23 | T12   | [ ]  |

**0x1290–0x12CF: USER Multi-Monitor and Display**

| Index  | Function                         | §   | Owner | Done |
| ------ | -------------------------------- | --- | ----- | ---- |
| 0x1290 | NtUserEnumDisplayMonitors        | §24 | T12   | [ ]  |
| 0x1291 | NtUserGetMonitorInfo             | §24 | T12   | [ ]  |
| 0x1292 | NtUserMonitorFromWindow          | §24 | T12   | [ ]  |
| 0x1293 | NtUserMonitorFromPoint           | §24 | T12   | [ ]  |
| 0x1294 | NtUserMonitorFromRect            | §24 | T12   | [ ]  |
| 0x1295 | NtUserEnumDisplayDevices         | §24 | T12   | [ ]  |
| 0x1296 | NtUserEnumDisplaySettings        | §24 | T12   | [ ]  |
| 0x1297 | NtUserEnumDisplaySettingsEx      | §24 | T12   | [ ]  |
| 0x1298 | NtUserChangeDisplaySettings      | §24 | T12   | [ ]  |
| 0x1299 | NtUserChangeDisplaySettingsEx    | §24 | T12   | [ ]  |
| 0x129A | NtGdiGetDeviceCapsAll            | §24 | T12   | [ ]  |
| 0x129B | NtGdiGetDeviceGammaRamp          | §24 | T12   | [ ]  |
| 0x129C | NtGdiSetDeviceGammaRamp          | §24 | T12   | [ ]  |
| 0x129D | NtUserSetDisplayConfig           | §24 | T12   | [ ]  |
| 0x129E | NtUserQueryDisplayConfig         | §24 | T12   | [ ]  |
| 0x129F | NtUserGetDisplayConfigBufferSize | §24 | T12   | [ ]  |

**0x12A0–0x12CF: USER Shell Integration**

| Index  | Function                         | §   | Owner | Done |
| ------ | -------------------------------- | --- | ----- | ---- |
| 0x12A0 | NtUserRegisterShellHookWindow    | §25 | T12   | [ ]  |
| 0x12A1 | NtUserDeregisterShellHookWindow  | §25 | T12   | [ ]  |
| 0x12A2 | NtUserSetShellWindow             | §25 | T12   | [ ]  |
| 0x12A3 | NtUserGetShellWindow             | §25 | T12   | [ ]  |
| 0x12A4 | NtUserSetTaskmanWindow           | §25 | T12   | [ ]  |
| 0x12A5 | NtUserGetTaskmanWindow           | §25 | T12   | [ ]  |
| 0x12A6 | NtUserSetProgmanWindow           | §25 | T12   | [ ]  |
| 0x12A7 | NtUserGetProgmanWindow           | §25 | T12   | [ ]  |
| 0x12A8 | NtUserRegisterWindowMessage      | §25 | T12   | [ ]  |
| 0x12A9 | NtUserBroadcastSystemMessage     | §25 | T12   | [ ]  |
| 0x12AA | NtUserBroadcastSystemMessageEx   | §25 | T12   | [ ]  |
| 0x12AB | NtUserSendMessageTimeout         | §25 | T12   | [ ]  |
| 0x12AC | NtUserSendNotifyMessage          | §25 | T12   | [ ]  |
| 0x12AD | NtUserSendMessageCallback        | §25 | T12   | [ ]  |
| 0x12AE | NtUserPostThreadMessage          | §25 | T12   | [ ]  |
| 0x12AF | NtUserSetCoalescableTimer        | §25 | T12   | [ ]  |
| 0x12B0 | NtUserRegisterPowerSettingNotif  | §25 | T12   | [ ]  |
| 0x12B1 | NtUserUnregPowerSettingNotif     | §25 | T12   | [ ]  |
| 0x12B2 | NtUserRegisterSuspendResumeNotif | §25 | T12   | [ ]  |
| 0x12B3 | NtUserUnregSuspendResumeNotif    | §25 | T12   | [ ]  |
| 0x12B4 | NtUserChangeWindowMessageFilter  | §25 | T12   | [ ]  |
| 0x12B5 | NtUserChangeWindowMsgFilterEx    | §25 | T12   | [ ]  |

**0x12C0–0x131F: GDI/USER DirectX and OpenGL Integration**

| Index  | Function                               | §   | Owner         | Done |
| ------ | -------------------------------------- | --- | ------------- | ---- |
| 0x12C0 | NtGdiDdDDICreateDevice                 | §26 | T12 / T08-gfx | [ ]  |
| 0x12C1 | NtGdiDdDDIDestroyDevice                | §26 | T12 / T08-gfx | [ ]  |
| 0x12C2 | NtGdiDdDDICreateAllocation             | §26 | T12 / T08-gfx | [ ]  |
| 0x12C3 | NtGdiDdDDIDestroyAllocation            | §26 | T12 / T08-gfx | [ ]  |
| 0x12C4 | NtGdiDdDDIPresent                      | §26 | T12 / T08-gfx | [ ]  |
| 0x12C5 | NtGdiDdDDIRender                       | §26 | T12 / T08-gfx | [ ]  |
| 0x12C6 | NtGdiDdDDIOpenAdapterFromHdc           | §26 | T12 / T08-gfx | [ ]  |
| 0x12C7 | NtGdiDdDDICloseAdapter                 | §26 | T12 / T08-gfx | [ ]  |
| 0x12C8 | NtGdiDdDDIQueryAdapterInfo             | §26 | T12 / T08-gfx | [ ]  |
| 0x12C9 | NtGdiDdDDICreateContext                | §26 | T12 / T08-gfx | [ ]  |
| 0x12CA | NtGdiDdDDIDestroyContext               | §26 | T12 / T08-gfx | [ ]  |
| 0x12CB | NtGdiDdDDICreateSynchronizationObject  | §26 | T12 / T08-gfx | [ ]  |
| 0x12CC | NtGdiDdDDIWaitForSynchronizationObject | §26 | T12 / T08-gfx | [ ]  |
| 0x12CD | NtGdiDdDDISignalSynchronizationObject  | §26 | T12 / T08-gfx | [ ]  |
| 0x12CE | NtGdiDdDDILock                         | §26 | T12 / T08-gfx | [ ]  |
| 0x12CF | NtGdiDdDDIUnlock                       | §26 | T12 / T08-gfx | [ ]  |
| 0x12D0 | NtGdiDdDDISetVidPnSourceOwner          | §26 | T12 / T08-gfx | [ ]  |
| 0x12D1 | NtGdiDdDDIGetMultisampleMethod         | §26 | T12 / T08-gfx | [ ]  |
| 0x12D2 | NtGdiDdDDIGetRuntimeData               | §26 | T12 / T08-gfx | [ ]  |
| 0x12D3 | NtGdiDdDDIQueryStatistics              | §26 | T12 / T08-gfx | [ ]  |
| 0x12D4 | NtGdiDdDDISetDisplayMode               | §26 | T12 / T08-gfx | [ ]  |
| 0x12D5 | NtGdiDdDDIGetDisplayModeList           | §26 | T12 / T08-gfx | [ ]  |
| 0x12D6 | NtGdiDdDDISetQueuedLimit               | §26 | T12 / T08-gfx | [ ]  |
| 0x12D7 | NtGdiDdDDIPollDisplayChildren          | §26 | T12 / T08-gfx | [ ]  |
| 0x12D8 | NtGdiDdDDIInvalidateActiveVidPn        | §26 | T12 / T08-gfx | [ ]  |
| 0x12D9 | NtGdiDdDDICheckOcclusion               | §26 | T12 / T08-gfx | [ ]  |
| 0x12DA | NtGdiDdDDIWaitForIdle                  | §26 | T12 / T08-gfx | [ ]  |
| 0x12DB | NtGdiDdDDICheckMonitorPowerState       | §26 | T12 / T08-gfx | [ ]  |
| 0x12DC | NtGdiDdDDICheckExclusiveOwnership      | §26 | T12 / T08-gfx | [ ]  |
| 0x12DD | NtGdiDdDDISetGammaRamp                 | §26 | T12 / T08-gfx | [ ]  |
| 0x12DE | NtGdiDdDDIGetScanLine                  | §26 | T12 / T08-gfx | [ ]  |
| 0x12DF | NtGdiDdDDISetHwProtectionTeardown      | §26 | T12 / T08-gfx | [ ]  |
| 0x12E0 | NtGdiDdDDICreateOverlay                | §26 | T12 / T08-gfx | [ ]  |
| 0x12E1 | NtGdiDdDDIDestroyOverlay               | §26 | T12 / T08-gfx | [ ]  |
| 0x12E2 | NtGdiDdDDIFlipOverlay                  | §26 | T12 / T08-gfx | [ ]  |
| 0x12E3 | NtGdiDdDDIUpdateOverlay                | §26 | T12 / T08-gfx | [ ]  |
| 0x12E4 | NtGdiDdDDICreateSwapChain              | §26 | T12 / T08-gfx | [ ]  |
| 0x12E5 | NtGdiDdDDIShareObjects                 | §26 | T12 / T08-gfx | [ ]  |
| 0x12E6 | NtGdiDdDDIOpenKeyedMutex               | §26 | T12 / T08-gfx | [ ]  |
| 0x12E7 | NtGdiDdDDIAcquireKeyedMutex            | §26 | T12 / T08-gfx | [ ]  |
| 0x12E8 | NtGdiDdDDIReleaseKeyedMutex            | §26 | T12 / T08-gfx | [ ]  |
| 0x12E9 | NtGdiDdDDIOutputDuplPresent            | §26 | T12 / T08-gfx | [ ]  |
| 0x12EA | NtGdiDdDDIOutputDuplGetFrameInfo       | §26 | T12 / T08-gfx | [ ]  |
| 0x12EB | NtGdiDdDDIOutputDuplReleaseFrame       | §26 | T12 / T08-gfx | [ ]  |
| 0x12EC | NtGdiDdDDIEnumAdapters                 | §26 | T12 / T08-gfx | [ ]  |
| 0x12ED | NtGdiDdDDIEnumAdapters2                | §26 | T12 / T08-gfx | [ ]  |
| 0x12EE | NtGdiDdDDIOpenAdapterFromLuid          | §26 | T12 / T08-gfx | [ ]  |
| 0x12EF | NtGdiDdDDIGetSharedPrimaryHandle       | §26 | T12 / T08-gfx | [ ]  |
| 0x12F0 | NtGdiDdDDIEscape                       | §26 | T12 / T08-gfx | [ ]  |
| 0x12F1 | NtGdiDdDDIGetContextSchedulingPriority | §26 | T12 / T08-gfx | [ ]  |
| 0x12F2 | NtGdiDdDDISetContextSchedulingPriority | §26 | T12 / T08-gfx | [ ]  |
| 0x12F3 | NtGdiDdDDIGetProcessSchedulingPriority | §26 | T12 / T08-gfx | [ ]  |
| 0x12F4 | NtGdiDdDDISetProcessSchedulingPriority | §26 | T12 / T08-gfx | [ ]  |

**0x1300–0x135F: Impossible OS Exclusive Graphics Extensions**

| Index  | Function                       | §   | Owner | Done |
| ------ | ------------------------------ | --- | ----- | ---- |
| 0x1300 | NtGdiQueryCompositorStats      | §27 | T12   | [ ]  |
| 0x1301 | NtGdiQueryFrameRate            | §27 | T12   | [ ]  |
| 0x1302 | NtGdiQueryDirtyRectCount       | §27 | T12   | [ ]  |
| 0x1303 | NtGdiQuerySurfaceMemoryUsage   | §27 | T12   | [ ]  |
| 0x1304 | NtGdiSetCompositorMode         | §27 | T12   | [ ]  |
| 0x1305 | NtGdiForceCompositorRedraw     | §27 | T12   | [ ]  |
| 0x1306 | NtGdiQueryAcrylicSupport       | §27 | T12   | [ ]  |
| 0x1307 | NtGdiSetAcrylicBlur            | §27 | T12   | [ ]  |
| 0x1308 | NtGdiSetMicaEffect             | §27 | T12   | [ ]  |
| 0x1309 | NtGdiQueryGpuInfo              | §27 | T12   | [ ]  |
| 0x130A | NtGdiQueryFramebufferInfo      | §27 | T12   | [ ]  |
| 0x130B | NtGdiSetNightLightStrength     | §27 | T12   | [ ]  |
| 0x130C | NtGdiGetNightLightStrength     | §27 | T12   | [ ]  |
| 0x130D | NtGdiQueryHardwareAcceleration | §27 | T12   | [ ]  |
| 0x130E | NtGdiSetHardwareAcceleration   | §27 | T12   | [ ]  |
| 0x130F | NtUserQueryDesktopInfo         | §27 | T12   | [ ]  |
| 0x1310 | NtUserQueryVirtualDesktopList  | §27 | T12   | [ ]  |
| 0x1311 | NtUserSwitchVirtualDesktop     | §27 | T12   | [ ]  |
| 0x1312 | NtUserCreateVirtualDesktop     | §27 | T12   | [ ]  |
| 0x1313 | NtUserRemoveVirtualDesktop     | §27 | T12   | [ ]  |
| 0x1314 | NtUserMoveWindowToDesktop      | §27 | T12   | [ ]  |
| 0x1315 | NtUserQuerySnapLayout          | §27 | T12   | [ ]  |
| 0x1316 | NtUserSetSnapLayout            | §27 | T12   | [ ]  |
| 0x1317 | NtUserQueryTaskbarState        | §27 | T12   | [ ]  |
| 0x1318 | NtUserSetTaskbarProgress       | §27 | T12   | [ ]  |
| 0x1319 | NtUserSetTaskbarOverlayIcon    | §27 | T12   | [ ]  |
| 0x131A | NtUserFlashTaskbarEntry        | §27 | T12   | [ ]  |
| 0x131B | NtUserQueryWindowThumbnail     | §27 | T12   | [ ]  |
| 0x131C | NtUserRegisterThumbnail        | §27 | T12   | [ ]  |
| 0x131D | NtUserUnregisterThumbnail      | §27 | T12   | [ ]  |
| 0x131E | NtUserQueryNotificationState   | §27 | T12   | [ ]  |
| 0x131F | NtUserSendToast                | §27 | T12   | [ ]  |
| 0x1320 | NtUserDismissToast             | §27 | T12   | [ ]  |
| 0x1321 | NtUserQueryToastHistory        | §27 | T12   | [ ]  |
| 0x1322 | NtUserQueryThemeInfo           | §27 | T12   | [ ]  |
| 0x1323 | NtUserSetThemeMode             | §27 | T12   | [ ]  |
| 0x1324 | NtUserSetAccentColor           | §27 | T12   | [ ]  |
| 0x1325 | NtUserQueryIconCache           | §27 | T12   | [ ]  |
| 0x1326 | NtUserInvalidateIconCache      | §27 | T12   | [ ]  |
| 0x1327 | NtGdiQueryFontCache            | §27 | T12   | [ ]  |
| 0x1328 | NtGdiPreloadFont               | §27 | T12   | [ ]  |
| 0x1329 | NtGdiFlushFontCache            | §27 | T12   | [ ]  |
| 0x132A | NtUserQueryAnimationState      | §27 | T12   | [ ]  |
| 0x132B | NtUserSetAnimationSpeed        | §27 | T12   | [ ]  |
| 0x132C | NtUserSetReduceMotion          | §27 | T12   | [ ]  |
| 0x132D | NtUserQueryWallpaperInfo       | §27 | T12   | [ ]  |
| 0x132E | NtUserSetWallpaper             | §27 | T12   | [ ]  |
| 0x132F | NtGdiScreenCapture             | §27 | T12   | [ ]  |
| 0x1330 | NtGdiWindowCapture             | §27 | T12   | [ ]  |
| 0x1331 | NtUserQueryStartMenuPins       | §27 | T12   | [ ]  |
| 0x1332 | NtUserSetStartMenuPin          | §27 | T12   | [ ]  |
| 0x1333 | NtUserRemoveStartMenuPin       | §27 | T12   | [ ]  |
| 0x1334 | NtUserQueryJumpList            | §27 | T12   | [ ]  |
| 0x1335 | NtUserAddJumpListItem          | §27 | T12   | [ ]  |
| 0x1336 | NtUserClearJumpList            | §27 | T12   | [ ]  |

**0x1340–0x13FF: GDI Internal Operations**

| Index  | Function                          | §   | Owner | Done |
| ------ | --------------------------------- | --- | ----- | ---- |
| 0x1340 | NtGdiGetCharSet                   | §16 | T12   | [ ]  |
| 0x1341 | NtGdiGetTextAlign                 | §4  | T12   | [ ]  |
| 0x1342 | NtGdiGetBkColor                   | §3  | T12   | [ ]  |
| 0x1343 | NtGdiGetTextColor                 | §3  | T12   | [ ]  |
| 0x1344 | NtGdiGetBkMode                    | §3  | T12   | [ ]  |
| 0x1345 | NtGdiGetStretchBltMode            | §14 | T12   | [ ]  |
| 0x1346 | NtGdiSetStretchBltMode            | §14 | T12   | [ ]  |
| 0x1347 | NtGdiGetClipBox                   | §6  | T12   | [ ]  |
| 0x1348 | NtGdiIntersectClipRect            | §6  | T12   | [ ]  |
| 0x1349 | NtGdiExcludeClipRect              | §6  | T12   | [ ]  |
| 0x134A | NtGdiOffsetClipRgn                | §6  | T12   | [ ]  |
| 0x134B | NtGdiGetClipRgn                   | §6  | T12   | [ ]  |
| 0x134C | NtGdiExtSelectClipRgn             | §6  | T12   | [ ]  |
| 0x134D | NtGdiGetRandomRgn                 | §17 | T12   | [ ]  |
| 0x134E | NtGdiCreateRectRgnIndirect        | §17 | T12   | [ ]  |
| 0x134F | NtGdiGetRgnBox                    | §17 | T12   | [ ]  |
| 0x1350 | NtGdiGetDIBColorTable             | §5  | T12   | [ ]  |
| 0x1351 | NtGdiSetDIBColorTable             | §5  | T12   | [ ]  |
| 0x1352 | NtGdiGdiAlphaBlend                | §13 | T12   | [ ]  |
| 0x1353 | NtGdiGdiGradientFill              | §13 | T12   | [ ]  |
| 0x1354 | NtGdiGdiTransparentBlt            | §13 | T12   | [ ]  |
| 0x1355 | NtGdiGdiSetBatchLimit             | §17 | T12   | [ ]  |
| 0x1356 | NtGdiGdiGetBatchLimit             | §17 | T12   | [ ]  |
| 0x1357 | NtGdiGdiFlush                     | §17 | T12   | [ ]  |
| 0x1358 | NtGdiSetRectRgnIndirect           | §17 | T12   | [ ]  |
| 0x1359 | NtGdiD3dContextCreate             | §26 | T12   | [ ]  |
| 0x135A | NtGdiD3dContextDestroy            | §26 | T12   | [ ]  |
| 0x135B | NtGdiD3dDrawPrimitives2           | §26 | T12   | [ ]  |
| 0x135C | NtGdiD3dValidateTextureStageState | §26 | T12   | [ ]  |
| 0x135D | NtGdiDdGetDriverState             | §26 | T12   | [ ]  |
| 0x135E | NtGdiDdCreateSurface              | §26 | T12   | [ ]  |
| 0x135F | NtGdiDdDestroySurface             | §26 | T12   | [ ]  |
| 0x1360 | NtGdiDdLock                       | §26 | T12   | [ ]  |
| 0x1361 | NtGdiDdUnlock                     | §26 | T12   | [ ]  |
| 0x1362 | NtGdiDdBlt                        | §26 | T12   | [ ]  |
| 0x1363 | NtGdiDdFlip                       | §26 | T12   | [ ]  |
| 0x1364 | NtGdiDdGetBltStatus               | §26 | T12   | [ ]  |
| 0x1365 | NtGdiDdGetFlipStatus              | §26 | T12   | [ ]  |
| 0x1366 | NtGdiDdWaitForVerticalBlank       | §26 | T12   | [ ]  |
| 0x1367 | NtGdiDdCanCreateSurface           | §26 | T12   | [ ]  |
| 0x1368 | NtGdiDdGetScanLine                | §26 | T12   | [ ]  |
| 0x1369 | NtGdiDdSetColorKey                | §26 | T12   | [ ]  |
| 0x136A | NtGdiDdCreateSurfaceEx            | §26 | T12   | [ ]  |
| 0x136B | NtGdiDdSetExclusiveMode           | §26 | T12   | [ ]  |
| 0x136C | NtGdiDdFlipToGDISurface           | §26 | T12   | [ ]  |
| 0x136D | NtGdiDdGetAvailDriverMemory       | §26 | T12   | [ ]  |
| 0x136E | NtGdiDdGetDC                      | §26 | T12   | [ ]  |
| 0x136F | NtGdiDdReleaseDC                  | §26 | T12   | [ ]  |
| 0x1370 | NtGdiDdReenableDirectDrawObject   | §26 | T12   | [ ]  |
| 0x1371 | NtGdiDdAttachSurface              | §26 | T12   | [ ]  |
| 0x1372 | NtGdiDdUnattachSurface            | §26 | T12   | [ ]  |
| 0x1373 | NtGdiDdQueryDirectDrawObject      | §26 | T12   | [ ]  |
| 0x1374 | NtGdiDdCreateDirectDrawObject     | §26 | T12   | [ ]  |
| 0x1375 | NtGdiDdDeleteDirectDrawObject     | §26 | T12   | [ ]  |
| 0x1376 | NtGdiDdGetDriverInfo              | §26 | T12   | [ ]  |
| 0x1377 | NtGdiDdSetOverlayPosition         | §26 | T12   | [ ]  |
| 0x1378 | NtGdiDdUpdateOverlay              | §26 | T12   | [ ]  |
| 0x1379 | NtGdiDdDestroyOverlay             | §26 | T12   | [ ]  |
| 0x137A | NtGdiDdCreateSurfaceObject        | §26 | T12   | [ ]  |
| 0x137B | NtGdiDdDeleteSurfaceObject        | §26 | T12   | [ ]  |
| 0x137C | NtGdiDdCreateD3DBuffer            | §26 | T12   | [ ]  |
| 0x137D | NtGdiDdDestroyD3DBuffer           | §26 | T12   | [ ]  |
| 0x137E | NtGdiDdLockD3D                    | §26 | T12   | [ ]  |
| 0x137F | NtGdiDdUnlockD3D                  | §26 | T12   | [ ]  |

**0x1380–0x13FF: DXGK Extended Operations**

| Index  | Function                                 | §   | Owner       | Done |
| ------ | ---------------------------------------- | --- | ----------- | ---- |
| 0x1380 | NtGdiDdDDICreatePagingQueue              | §26 | T12/T08-gfx | [ ]  |
| 0x1381 | NtGdiDdDDIDestroyPagingQueue             | §26 | T12/T08-gfx | [ ]  |
| 0x1382 | NtGdiDdDDIMakeResident                   | §26 | T12/T08-gfx | [ ]  |
| 0x1383 | NtGdiDdDDIEvict                          | §26 | T12/T08-gfx | [ ]  |
| 0x1384 | NtGdiDdDDISubmitCommand                  | §26 | T12/T08-gfx | [ ]  |
| 0x1385 | NtGdiDdDDICreateHwQueue                  | §26 | T12/T08-gfx | [ ]  |
| 0x1386 | NtGdiDdDDIDestroyHwQueue                 | §26 | T12/T08-gfx | [ ]  |
| 0x1387 | NtGdiDdDDISubmitCommandToHwQueue         | §26 | T12/T08-gfx | [ ]  |
| 0x1388 | NtGdiDdDDISubmitSignalSyncObjectToHwQueue | §26 | T12/T08-gfx | [ ]  |
| 0x1389 | NtGdiDdDDISubmitWaitForSyncObjectToHwQueue | §26 | T12/T08-gfx | [ ]  |
| 0x138A | NtGdiDdDDIMapGpuVirtualAddress           | §26 | T12/T08-gfx | [ ]  |
| 0x138B | NtGdiDdDDIReserveGpuVirtualAddress       | §26 | T12/T08-gfx | [ ]  |
| 0x138C | NtGdiDdDDIFreeGpuVirtualAddress          | §26 | T12/T08-gfx | [ ]  |
| 0x138D | NtGdiDdDDIUpdateGpuVirtualAddress        | §26 | T12/T08-gfx | [ ]  |
| 0x138E | NtGdiDdDDICreateContextVirtual           | §26 | T12/T08-gfx | [ ]  |
| 0x138F | NtGdiDdDDIGetContextInProcessSchedulingPriority | §26 | T12/T08-gfx | [ ]  |
| 0x1390 | NtGdiDdDDISetContextInProcessSchedulingPriority | §26 | T12/T08-gfx | [ ]  |
| 0x1391 | NtGdiDdDDIOfferAllocations               | §26 | T12/T08-gfx | [ ]  |
| 0x1392 | NtGdiDdDDIReclaimAllocations             | §26 | T12/T08-gfx | [ ]  |
| 0x1393 | NtGdiDdDDIReclaimAllocations2            | §26 | T12/T08-gfx | [ ]  |
| 0x1394 | NtGdiDdDDIOutputDuplGetMetaData          | §26 | T12/T08-gfx | [ ]  |
| 0x1395 | NtGdiDdDDIOutputDuplGetPointerShapeData  | §26 | T12/T08-gfx | [ ]  |
| 0x1396 | NtGdiDdDDIQueryVideoMemoryInfo           | §26 | T12/T08-gfx | [ ]  |
| 0x1397 | NtGdiDdDDIChangeVideoMemoryReservation   | §26 | T12/T08-gfx | [ ]  |
| 0x1398 | NtGdiDdDDISetStablePowerState            | §26 | T12/T08-gfx | [ ]  |
| 0x1399 | NtGdiDdDDIQueryClockCalibration          | §26 | T12/T08-gfx | [ ]  |
| 0x139A | NtGdiDdDDIGetMultiPlaneOverlayCaps       | §26 | T12/T08-gfx | [ ]  |
| 0x139B | NtGdiDdDDIGetPostCompositionCaps         | §26 | T12/T08-gfx | [ ]  |
| 0x139C | NtGdiDdDDISetVidPnSourceHwProtection     | §26 | T12/T08-gfx | [ ]  |
| 0x139D | NtGdiDdDDIMarkDeviceAsError              | §26 | T12/T08-gfx | [ ]  |
| 0x139E | NtGdiDdDDIFlushHeapTransitions           | §26 | T12/T08-gfx | [ ]  |
| 0x139F | NtGdiDdDDISetHwProtectionTeardown        | §26 | T12/T08-gfx | [ ]  |

**0x13A0–0x143F: USER Extended Message Processing and Misc**

| Index  | Function                                | §   | Owner | Done |
| ------ | --------------------------------------- | --- | ----- | ---- |
| 0x13A0 | NtUserCallWindowProc                    | §8  | T12   | [ ]  |
| 0x13A1 | NtUserCallNextHookProc                  | §21 | T12   | [ ]  |
| 0x13A2 | NtUserGetWindowMinimizeRect             | §18 | T12   | [ ]  |
| 0x13A3 | NtUserGetInternalWindowPos              | §18 | T12   | [ ]  |
| 0x13A4 | NtUserSetInternalWindowPos              | §18 | T12   | [ ]  |
| 0x13A5 | NtUserCalculatePopupWindowPosition      | §10 | T12   | [ ]  |
| 0x13A6 | NtUserMNDragLeave                       | §10 | T12   | [ ]  |
| 0x13A7 | NtUserMNDragOver                        | §10 | T12   | [ ]  |
| 0x13A8 | NtUserGetMenuItemRect                   | §10 | T12   | [ ]  |
| 0x13A9 | NtUserGetMenuItemInfo                   | §10 | T12   | [ ]  |
| 0x13AA | NtUserSetMenuItemInfo                   | §10 | T12   | [ ]  |
| 0x13AB | NtUserMenuItemFromPoint                 | §10 | T12   | [ ]  |
| 0x13AC | NtUserGetMenuState                      | §10 | T12   | [ ]  |
| 0x13AD | NtUserGetMenuItemCount                  | §10 | T12   | [ ]  |
| 0x13AE | NtUserGetMenuItemID                     | §10 | T12   | [ ]  |
| 0x13AF | NtUserHiliteMenuItem                    | §10 | T12   | [ ]  |
| 0x13B0 | NtUserGetMenuString                     | §10 | T12   | [ ]  |
| 0x13B1 | NtUserCheckMenuItem                     | §10 | T12   | [ ]  |
| 0x13B2 | NtUserEnableMenuItem                    | §10 | T12   | [ ]  |
| 0x13B3 | NtUserRemoveMenu                        | §10 | T12   | [ ]  |
| 0x13B4 | NtUserDeleteMenu                        | §10 | T12   | [ ]  |
| 0x13B5 | NtUserModifyMenu                        | §10 | T12   | [ ]  |
| 0x13B6 | NtUserGetSubMenu                        | §10 | T12   | [ ]  |
| 0x13B7 | NtUserSetMenuDefaultItem                | §10 | T12   | [ ]  |
| 0x13B8 | NtUserGetMenuDefaultItem                | §10 | T12   | [ ]  |
| 0x13B9 | NtUserGetMenuCheckMarkDimensions        | §10 | T12   | [ ]  |
| 0x13BA | NtUserSetMenuInfo                       | §10 | T12   | [ ]  |
| 0x13BB | NtUserGetMenuInfo                       | §10 | T12   | [ ]  |
| 0x13BC | NtUserGetMenuBarInfo                    | §10 | T12   | [ ]  |
| 0x13BD | NtUserEndMenu                           | §10 | T12   | [ ]  |
| 0x13BE | NtUserTrackPopupMenuEx                  | §10 | T12   | [ ]  |
| 0x13BF | NtUserSetSystemMenu                     | §10 | T12   | [ ]  |
| 0x13C0 | NtUserGetSystemMenu                     | §10 | T12   | [ ]  |
| 0x13C1 | NtUserInsertMenu                        | §10 | T12   | [ ]  |
| 0x13C2 | NtUserSetMenuContextHelpId              | §10 | T12   | [ ]  |
| 0x13C3 | NtUserGetMenuContextHelpId              | §10 | T12   | [ ]  |
| 0x13C4 | NtUserCreateIconFromResource            | §9  | T12   | [ ]  |
| 0x13C5 | NtUserCreateIconIndirect                | §9  | T12   | [ ]  |
| 0x13C6 | NtUserCopyIcon                          | §9  | T12   | [ ]  |
| 0x13C7 | NtUserDestroyIcon                       | §9  | T12   | [ ]  |
| 0x13C8 | NtUserGetIconInfo                       | §9  | T12   | [ ]  |
| 0x13C9 | NtUserGetIconInfoEx                     | §9  | T12   | [ ]  |
| 0x13CA | NtUserDrawIconEx2                       | §9  | T12   | [ ]  |
| 0x13CB | NtUserLookupIconIdFromDirectory         | §9  | T12   | [ ]  |
| 0x13CC | NtUserLookupIconIdFromDirectoryEx       | §9  | T12   | [ ]  |
| 0x13CD | NtUserCreateCursorFromResource          | §9  | T12   | [ ]  |
| 0x13CE | NtUserDestroyCursor                     | §9  | T12   | [ ]  |
| 0x13CF | NtUserGetCursorFrameInfo                | §9  | T12   | [ ]  |
| 0x13D0 | NtUserSetSystemCursor                   | §9  | T12   | [ ]  |
| 0x13D1 | NtUserGetCursorInfo2                    | §9  | T12   | [ ]  |
| 0x13D2 | NtUserConfineRect                       | §9  | T12   | [ ]  |
| 0x13D3 | NtUserCountClipboardFormats2            | §11 | T12   | [ ]  |
| 0x13D4 | NtUserGetClipboardOwner                 | §11 | T12   | [ ]  |
| 0x13D5 | NtUserGetClipboardViewer                | §11 | T12   | [ ]  |
| 0x13D6 | NtUserSetClipboardViewer                | §11 | T12   | [ ]  |
| 0x13D7 | NtUserChangeClipboardChain              | §11 | T12   | [ ]  |
| 0x13D8 | NtUserGetClipboardSequenceNumber        | §11 | T12   | [ ]  |
| 0x13D9 | NtUserGetPriorityClipboardFormat        | §11 | T12   | [ ]  |
| 0x13DA | NtUserGetClipboardFormatName            | §11 | T12   | [ ]  |
| 0x13DB | NtUserGetOpenClipboardWindow            | §11 | T12   | [ ]  |
| 0x13DC | NtUserRemoveClipboardFormatListener     | §11 | T12   | [ ]  |
| 0x13DD | NtUserDragDetect                        | §9  | T12   | [ ]  |
| 0x13DE | NtUserDragObject                        | §9  | T12   | [ ]  |
| 0x13DF | NtUserSetForegroundWindowForApp         | §18 | T12   | [ ]  |
| 0x13E0 | NtUserAllowSetForegroundWindow          | §18 | T12   | [ ]  |
| 0x13E1 | NtUserLockSetForegroundWindow           | §18 | T12   | [ ]  |
| 0x13E2 | NtUserGetProcessWindowStation           | §25 | T12   | [ ]  |
| 0x13E3 | NtUserSetProcessWindowStation           | §25 | T12   | [ ]  |
| 0x13E4 | NtUserOpenWindowStation                 | §25 | T12   | [ ]  |
| 0x13E5 | NtUserCloseWindowStation                | §25 | T12   | [ ]  |
| 0x13E6 | NtUserCreateWindowStation               | §25 | T12   | [ ]  |
| 0x13E7 | NtUserEnumWindowStations                | §25 | T12   | [ ]  |
| 0x13E8 | NtUserOpenDesktop                       | §25 | T12   | [ ]  |
| 0x13E9 | NtUserCloseDesktop                      | §25 | T12   | [ ]  |
| 0x13EA | NtUserCreateDesktop                     | §25 | T12   | [ ]  |
| 0x13EB | NtUserEnumDesktops                      | §25 | T12   | [ ]  |
| 0x13EC | NtUserSwitchDesktop                     | §25 | T12   | [ ]  |
| 0x13ED | NtUserGetThreadDesktop                  | §25 | T12   | [ ]  |
| 0x13EE | NtUserSetThreadDesktop                  | §25 | T12   | [ ]  |
| 0x13EF | NtUserGetObjectInformation              | §25 | T12   | [ ]  |
| 0x13F0 | NtUserSetObjectInformation              | §25 | T12   | [ ]  |
| 0x13F1 | NtUserGetGuiResources                   | §25 | T12   | [ ]  |
| 0x13F2 | NtUserSetWinEventHook                   | §21 | T12   | [ ]  |
| 0x13F3 | NtUserUnhookWinEvent                    | §21 | T12   | [ ]  |
| 0x13F4 | NtUserNotifyWinEvent                    | §21 | T12   | [ ]  |
| 0x13F5 | NtUserIsWinEventHookInstalled           | §21 | T12   | [ ]  |
| 0x13F6 | NtUserGetGUIThreadInfo                  | §22 | T12   | [ ]  |
| 0x13F7 | NtUserGetWindowFeedbackSetting          | §22 | T12   | [ ]  |
| 0x13F8 | NtUserSetWindowFeedbackSetting          | §22 | T12   | [ ]  |
| 0x13F9 | NtUserChangeFilterWindow                | §22 | T12   | [ ]  |
| 0x13FA | NtUserGetTitleBarInfo                   | §18 | T12   | [ ]  |
| 0x13FB | NtUserGetComboBoxInfo                   | §19 | T12   | [ ]  |
| 0x13FC | NtUserGetListBoxInfo                    | §19 | T12   | [ ]  |
| 0x13FD | NtUserGetAltTabInfo                     | §18 | T12   | [ ]  |
| 0x13FE | NtUserGetLastInputInfo                  | §22 | T12   | [ ]  |
| 0x13FF | NtUserRealInternalGetMessage            | §8  | T12   | [ ]  |
| 0x1400 | NtUserWaitMessage2                      | §8  | T12   | [ ]  |
| 0x1401 | NtUserTranslateAccelerator2             | §10 | T12   | [ ]  |
| 0x1402 | NtUserGetClassInfo                      | §18 | T12   | [ ]  |
| 0x1403 | NtUserGetClassInfoEx                    | §18 | T12   | [ ]  |
| 0x1404 | NtUserUnregisterClass                   | §18 | T12   | [ ]  |
| 0x1405 | NtUserSetCursor2                        | §9  | T12   | [ ]  |
| 0x1406 | NtUserGetClassWord                      | §18 | T12   | [ ]  |
| 0x1407 | NtUserSetClassWord                      | §18 | T12   | [ ]  |
| 0x1408 | NtUserGetWindowWord                     | §18 | T12   | [ ]  |
| 0x1409 | NtUserSetWindowWord                     | §18 | T12   | [ ]  |
| 0x140A | NtUserGetProcessDefaultLayout           | §14 | T12   | [ ]  |
| 0x140B | NtUserSetProcessDefaultLayout           | §14 | T12   | [ ]  |
| 0x140C | NtUserPaintDesktop                      | §27 | T12   | [ ]  |
| 0x140D | NtUserSetSysColorsTemp                  | §22 | T12   | [ ]  |
| 0x140E | NtUserUserHandleGrantAccess             | §25 | T12   | [ ]  |
| 0x140F | NtUserGetAppCompatFlags                 | §22 | T12   | [ ]  |
| 0x1410 | NtUserGetAppCompatFlags2                | §22 | T12   | [ ]  |
| 0x1411 | NtUserLoadKeyboardLayoutEx              | §21 | T12   | [ ]  |
| 0x1412 | NtUserGetKeyboardLayoutNameW            | §21 | T12   | [ ]  |
| 0x1413 | NtUserMapVirtualKeyExW                  | §21 | T12   | [ ]  |
| 0x1414 | NtUserGetKeyboardType                   | §21 | T12   | [ ]  |
| 0x1415 | NtUserToAscii                           | §21 | T12   | [ ]  |
| 0x1416 | NtUserToAsciiEx                         | §21 | T12   | [ ]  |
| 0x1417 | NtUserGetQueueStatusReadonly            | §22 | T12   | [ ]  |
| 0x1418 | NtUserSetMenuBarInfo                    | §10 | T12   | [ ]  |
| 0x1419 | NtUserSetWindowStationUser              | §25 | T12   | [ ]  |
| 0x141A | NtUserSetThreadLayoutHandles            | §21 | T12   | [ ]  |
| 0x141B | NtUserGetMouseMovePointsEx              | §9  | T12   | [ ]  |
| 0x141C | NtUserTrackMouseEvent                   | §9  | T12   | [ ]  |
| 0x141D | NtUserAddClipboardFormatListener2       | §11 | T12   | [ ]  |
| 0x141E | NtUserGetUpdatedClipboardFormats        | §11 | T12   | [ ]  |
| 0x141F | NtUserSetWinEventHookEx                 | §21 | T12   | [ ]  |
| 0x1420 | NtUserAssociateInputContext             | §21 | T12   | [ ]  |
| 0x1421 | NtUserBuildPropList                     | §18 | T12   | [ ]  |
| 0x1422 | NtUserCallHwnd                          | §8  | T12   | [ ]  |
| 0x1423 | NtUserCallHwndLock                      | §8  | T12   | [ ]  |
| 0x1424 | NtUserCallHwndOpt                       | §8  | T12   | [ ]  |
| 0x1425 | NtUserCallHwndParam                     | §8  | T12   | [ ]  |
| 0x1426 | NtUserCallHwndParamLock                 | §8  | T12   | [ ]  |
| 0x1427 | NtUserCallMsgFilter2                    | §21 | T12   | [ ]  |
| 0x1428 | NtUserCallNoParam                       | §8  | T12   | [ ]  |
| 0x1429 | NtUserCallOneParam                      | §8  | T12   | [ ]  |
| 0x142A | NtUserCallTwoParam                      | §8  | T12   | [ ]  |
| 0x142B | NtUserCheckAccessForIntegrityLevel      | §22 | T12   | [ ]  |
| 0x142C | NtUserCheckDesktopByThreadId            | §25 | T12   | [ ]  |
| 0x142D | NtUserCheckProcessForClipboardAccess    | §11 | T12   | [ ]  |
| 0x142E | NtUserConsoleControl                    | §25 | T12   | [ ]  |
| 0x142F | NtUserCreateInputContext                | §21 | T12   | [ ]  |
| 0x1430 | NtUserDestroyInputContext               | §21 | T12   | [ ]  |
| 0x1431 | NtUserDisableThreadIme                  | §21 | T12   | [ ]  |
| 0x1432 | NtUserDispatchMessage2                  | §8  | T12   | [ ]  |
| 0x1433 | NtUserDoSoundConnect                    | §22 | T12   | [ ]  |
| 0x1434 | NtUserDoSoundDisconnect                 | §22 | T12   | [ ]  |
| 0x1435 | NtUserDwmGetRemoteSessionOcclusionEvent | §27 | T12   | [ ]  |
| 0x1436 | NtUserDwmGetRemoteSessionOcclusionState | §27 | T12   | [ ]  |
| 0x1437 | NtUserDwmValidateWindow                 | §27 | T12   | [ ]  |
| 0x1438 | NtUserDwmKernelShutdown                 | §27 | T12   | [ ]  |
| 0x1439 | NtUserDwmKernelStartup                  | §27 | T12   | [ ]  |
| 0x143A | NtUserEnableMouseInPointer              | §23 | T12   | [ ]  |
| 0x143B | NtUserEnableMouseInPointerForThread     | §23 | T12   | [ ]  |
| 0x143C | NtUserFrostCrashedWindow                | §18 | T12   | [ ]  |
| 0x143D | NtUserGetAutoRotationState              | §24 | T12   | [ ]  |
| 0x143E | NtUserGetCIMSSM                         | §22 | T12   | [ ]  |
| 0x143F | NtUserGetClipboardAccessToken           | §11 | T12   | [ ]  |

**0x1440–0x14FF: GDI DDI Thunks and Internal Dispatch**

| Index  | Function                      | §   | Owner | Done |
| ------ | ----------------------------- | --- | ----- | ---- |
| 0x1440 | NtGdiEngCreateBitmap          | §17 | T12   | [ ]  |
| 0x1441 | NtGdiEngDeleteSurface         | §17 | T12   | [ ]  |
| 0x1442 | NtGdiEngLockSurface           | §17 | T12   | [ ]  |
| 0x1443 | NtGdiEngUnlockSurface         | §17 | T12   | [ ]  |
| 0x1444 | NtGdiEngMarkBandingSurface    | §17 | T12   | [ ]  |
| 0x1445 | NtGdiEngAssociateSurface      | §17 | T12   | [ ]  |
| 0x1446 | NtGdiEngCreateDeviceSurface   | §17 | T12   | [ ]  |
| 0x1447 | NtGdiEngCreateDeviceBitmap    | §17 | T12   | [ ]  |
| 0x1448 | NtGdiEngCopyBits              | §17 | T12   | [ ]  |
| 0x1449 | NtGdiEngStretchBlt            | §17 | T12   | [ ]  |
| 0x144A | NtGdiEngBitBlt                | §17 | T12   | [ ]  |
| 0x144B | NtGdiEngPlgBlt                | §17 | T12   | [ ]  |
| 0x144C | NtGdiEngAlphaBlend            | §17 | T12   | [ ]  |
| 0x144D | NtGdiEngGradientFill          | §17 | T12   | [ ]  |
| 0x144E | NtGdiEngTransparentBlt        | §17 | T12   | [ ]  |
| 0x144F | NtGdiEngTextOut               | §17 | T12   | [ ]  |
| 0x1450 | NtGdiEngStrokePath            | §17 | T12   | [ ]  |
| 0x1451 | NtGdiEngFillPath              | §17 | T12   | [ ]  |
| 0x1452 | NtGdiEngStrokeAndFillPath     | §17 | T12   | [ ]  |
| 0x1453 | NtGdiEngLineTo                | §17 | T12   | [ ]  |
| 0x1454 | NtGdiEngPaint                 | §17 | T12   | [ ]  |
| 0x1455 | NtGdiEngCreateClip            | §17 | T12   | [ ]  |
| 0x1456 | NtGdiEngDeleteClip            | §17 | T12   | [ ]  |
| 0x1457 | NtGdiEngCreatePalette         | §17 | T12   | [ ]  |
| 0x1458 | NtGdiEngDeletePalette         | §17 | T12   | [ ]  |
| 0x1459 | NtGdiDrvCopyBits              | §26 | T12   | [ ]  |
| 0x145A | NtGdiDrvBitBlt                | §26 | T12   | [ ]  |
| 0x145B | NtGdiDrvStretchBlt            | §26 | T12   | [ ]  |
| 0x145C | NtGdiDrvTextOut               | §26 | T12   | [ ]  |
| 0x145D | NtGdiDrvLineTo                | §26 | T12   | [ ]  |
| 0x145E | NtGdiDrvStrokePath            | §26 | T12   | [ ]  |
| 0x145F | NtGdiDrvFillPath              | §26 | T12   | [ ]  |
| 0x1460 | NtGdiDrvGradientFill          | §26 | T12   | [ ]  |
| 0x1461 | NtGdiDrvAlphaBlend            | §26 | T12   | [ ]  |
| 0x1462 | NtGdiDrvTransparentBlt        | §26 | T12   | [ ]  |
| 0x1463 | NtGdiDrvPlgBlt                | §26 | T12   | [ ]  |
| 0x1464 | NtGdiDrvEscape                | §26 | T12   | [ ]  |
| 0x1465 | NtGdiDrvDrawEscape            | §26 | T12   | [ ]  |
| 0x1466 | NtGdiDrvQueryFont             | §26 | T12   | [ ]  |
| 0x1467 | NtGdiDrvQueryFontTree         | §26 | T12   | [ ]  |
| 0x1468 | NtGdiDrvQueryFontData         | §26 | T12   | [ ]  |
| 0x1469 | NtGdiDrvQueryFontFile         | §26 | T12   | [ ]  |
| 0x146A | NtGdiDrvQueryAdvanceWidths    | §26 | T12   | [ ]  |
| 0x146B | NtGdiDrvGetGlyphMode          | §26 | T12   | [ ]  |
| 0x146C | NtGdiDrvFontManagement        | §26 | T12   | [ ]  |
| 0x146D | NtGdiDrvQueryTrueTypeOutline  | §26 | T12   | [ ]  |
| 0x146E | NtGdiDrvQueryTrueTypeTable    | §26 | T12   | [ ]  |
| 0x146F | NtGdiDrvCompletePDEV          | §26 | T12   | [ ]  |
| 0x1470 | NtGdiDrvEnablePDEV            | §26 | T12   | [ ]  |
| 0x1471 | NtGdiDrvDisablePDEV           | §26 | T12   | [ ]  |
| 0x1472 | NtGdiDrvEnableSurface         | §26 | T12   | [ ]  |
| 0x1473 | NtGdiDrvDisableSurface        | §26 | T12   | [ ]  |
| 0x1474 | NtGdiDrvAssertMode            | §26 | T12   | [ ]  |
| 0x1475 | NtGdiDrvResetPDEV             | §26 | T12   | [ ]  |
| 0x1476 | NtGdiDrvGetModes              | §26 | T12   | [ ]  |
| 0x1477 | NtGdiDrvDisableDriver         | §26 | T12   | [ ]  |
| 0x1478 | NtGdiDrvEnableDriver          | §26 | T12   | [ ]  |
| 0x1479 | NtGdiGetDhpdev                | §26 | T12   | [ ]  |
| 0x147A | NtGdiSetUMPDSandboxState      | §15 | T12   | [ ]  |
| 0x147B | NtGdiAddFontResourceW         | §16 | T12   | [ ]  |
| 0x147C | NtGdiRemoveFontResourceW      | §16 | T12   | [ ]  |
| 0x147D | NtGdiAddRemoteFontToDC        | §16 | T12   | [ ]  |
| 0x147E | NtGdiAddFontMemResourceEx2    | §16 | T12   | [ ]  |
| 0x147F | NtGdiRemoveFontMemResourceEx2 | §16 | T12   | [ ]  |
| 0x1480 | NtGdiGetFontFileData          | §16 | T12   | [ ]  |
| 0x1481 | NtGdiGetFontFileInfo          | §16 | T12   | [ ]  |
| 0x1482 | NtGdiQueryFontAssocInfo       | §16 | T12   | [ ]  |
| 0x1483 | NtGdiGetRealizationInfo       | §16 | T12   | [ ]  |
| 0x1484 | NtGdiGetCharWidthInfo         | §16 | T12   | [ ]  |
| 0x1485 | NtGdiGetLinkedUFIs            | §16 | T12   | [ ]  |
| 0x1486 | NtGdiGetEmbedFonts            | §16 | T12   | [ ]  |
| 0x1487 | NtGdiChangeGhostFont          | §16 | T12   | [ ]  |
| 0x1488 | NtGdiGetSpoolMessage          | §15 | T12   | [ ]  |
| 0x1489 | NtGdiInitSpool                | §15 | T12   | [ ]  |
| 0x148A | NtGdiGetEudcTimeStampEx       | §16 | T12   | [ ]  |
| 0x148B | NtGdiQueryFonts               | §16 | T12   | [ ]  |
| 0x148C | NtGdiGetCharWidthI            | §16 | T12   | [ ]  |
| 0x148D | NtGdiGetCharABCWidthsI        | §16 | T12   | [ ]  |
| 0x148E | NtGdiMirrorWindowOrg          | §14 | T12   | [ ]  |
| 0x148F | NtGdiGetServerMetaFileBits    | §15 | T12   | [ ]  |
| 0x1490 | NtGdiGetColorSpaceforBitmap   | §14 | T12   | [ ]  |
| 0x1491 | NtGdiGetETM                   | §16 | T12   | [ ]  |
| 0x1492 | NtGdiGetEudcTimeStamp         | §16 | T12   | [ ]  |
| 0x1493 | NtGdiHT_Get8BPPFormatPalette  | §14 | T12   | [ ]  |
| 0x1494 | NtGdiHT_Get8BPPMaskPalette    | §14 | T12   | [ ]  |
| 0x1495 | NtGdiMakeInfoDC               | §15 | T12   | [ ]  |
| 0x1496 | NtGdiMakeObjectUnXferable     | §17 | T12   | [ ]  |
| 0x1497 | NtGdiMakeObjectXferable       | §17 | T12   | [ ]  |
| 0x1498 | NtGdiMonoBitmap               | §5  | T12   | [ ]  |
| 0x1499 | NtGdiMoveTo2                  | §3  | T12   | [ ]  |
| 0x149A | NtGdiOpenDCW                  | §15 | T12   | [ ]  |
| 0x149B | NtGdiPatBlt2                  | §3  | T12   | [ ]  |
| 0x149C | NtGdiPolyPatBlt               | §3  | T12   | [ ]  |
| 0x149D | NtGdiQueryFontResourceInfo    | §16 | T12   | [ ]  |
| 0x149E | NtGdiRectVisible              | §6  | T12   | [ ]  |
| 0x149F | NtGdiRemoveMergeFont          | §16 | T12   | [ ]  |

**0x14A0–0x155F: USER Internal Callbacks and Session Support**

| Index  | Function                                 | §   | Owner | Done |
| ------ | ---------------------------------------- | --- | ----- | ---- |
| 0x14A0 | NtUserActivateKeyboardLayout2            | §21 | T12   | [ ]  |
| 0x14A1 | NtUserAddVisualIdentifier                | §27 | T12   | [ ]  |
| 0x14A2 | NtUserAlterWindowStyle                   | §18 | T12   | [ ]  |
| 0x14A3 | NtUserBuildHimcList                      | §21 | T12   | [ ]  |
| 0x14A4 | NtUserBuildHwndList                      | §18 | T12   | [ ]  |
| 0x14A5 | NtUserBuildNameList                      | §25 | T12   | [ ]  |
| 0x14A6 | NtUserCalcMenuBar                        | §10 | T12   | [ ]  |
| 0x14A7 | NtUserCallHwndLockSafe                   | §8  | T12   | [ ]  |
| 0x14A8 | NtUserCheckImeHotKey                     | §21 | T12   | [ ]  |
| 0x14A9 | NtUserCheckWindowThreadDesktop           | §25 | T12   | [ ]  |
| 0x14AA | NtUserChildWindowFromPointEx2            | §18 | T12   | [ ]  |
| 0x14AB | NtUserClearForeground                    | §18 | T12   | [ ]  |
| 0x14AC | NtUserConfigureActivationObject          | §22 | T12   | [ ]  |
| 0x14AD | NtUserCreateActivationObject             | §22 | T12   | [ ]  |
| 0x14AE | NtUserCtxDisplayIOCtl                    | §25 | T12   | [ ]  |
| 0x14AF | NtUserDbgWin32HeapFail                   | §27 | T12   | [ ]  |
| 0x14B0 | NtUserDbgWin32HeapStat                   | §27 | T12   | [ ]  |
| 0x14B1 | NtUserDestroyActivationObject            | §22 | T12   | [ ]  |
| 0x14B2 | NtUserDeviceEventWorker                  | §25 | T12   | [ ]  |
| 0x14B3 | NtUserDisableProcessWindowFiltering      | §22 | T12   | [ ]  |
| 0x14B4 | NtUserDisplayConfigGetDeviceInfo         | §24 | T12   | [ ]  |
| 0x14B5 | NtUserDisplayConfigSetDeviceInfo         | §24 | T12   | [ ]  |
| 0x14B6 | NtUserEndDeferWindowPosEx                | §23 | T12   | [ ]  |
| 0x14B7 | NtUserEndTouchOperation                  | §23 | T12   | [ ]  |
| 0x14B8 | NtUserEvent                              | §8  | T12   | [ ]  |
| 0x14B9 | NtUserExcludeUpdateRgn2                  | §19 | T12   | [ ]  |
| 0x14BA | NtUserFillWindow                         | §19 | T12   | [ ]  |
| 0x14BB | NtUserFindExistingCursorIcon             | §9  | T12   | [ ]  |
| 0x14BC | NtUserFlashWindow                        | §18 | T12   | [ ]  |
| 0x14BD | NtUserGetAncestor2                       | §18 | T12   | [ ]  |
| 0x14BE | NtUserGetAtomName                        | §25 | T12   | [ ]  |
| 0x14BF | NtUserGetCaretBlinkTime2                 | §19 | T12   | [ ]  |
| 0x14C0 | NtUserGetClassInfoEx2                    | §18 | T12   | [ ]  |
| 0x14C1 | NtUserGetClientRect2                     | §18 | T12   | [ ]  |
| 0x14C2 | NtUserGetComboBoxInfo2                   | §19 | T12   | [ ]  |
| 0x14C3 | NtUserGetControlBrush                    | §19 | T12   | [ ]  |
| 0x14C4 | NtUserGetControlColor                    | §19 | T12   | [ ]  |
| 0x14C5 | NtUserGetCPD                             | §8  | T12   | [ ]  |
| 0x14C6 | NtUserGetCurrentInputMessageSource       | §22 | T12   | [ ]  |
| 0x14C7 | NtUserGetCursorPos2                      | §9  | T12   | [ ]  |
| 0x14C8 | NtUserGetDC2                             | §2  | T12   | [ ]  |
| 0x14C9 | NtUserGetDCEx2                           | §19 | T12   | [ ]  |
| 0x14CA | NtUserGetDesktopID                       | §25 | T12   | [ ]  |
| 0x14CB | NtUserGetDisplayAutoRotationPreferences  | §24 | T12   | [ ]  |
| 0x14CC | NtUserGetDManipHookInitFunction          | §23 | T12   | [ ]  |
| 0x14CD | NtUserGetDpiForCurrentProcess            | §22 | T12   | [ ]  |
| 0x14CE | NtUserGetDpiForMonitor                   | §22 | T12   | [ ]  |
| 0x14CF | NtUserGetExtendedPointerDeviceProperty   | §23 | T12   | [ ]  |
| 0x14D0 | NtUserGetForegroundWindow2               | §18 | T12   | [ ]  |
| 0x14D1 | NtUserGetImeInfoEx                       | §21 | T12   | [ ]  |
| 0x14D2 | NtUserGetInputDesktop                    | §25 | T12   | [ ]  |
| 0x14D3 | NtUserGetInputLocaleInfo                 | §21 | T12   | [ ]  |
| 0x14D4 | NtUserGetInteractiveControlDeviceInfo    | §23 | T12   | [ ]  |
| 0x14D5 | NtUserGetInteractiveCtrlSupportedWaveforms | §23 | T12   | [ ]  |
| 0x14D6 | NtUserGetInternalPaintCount              | §18 | T12   | [ ]  |
| 0x14D7 | NtUserGetKeyNameText2                    | §21 | T12   | [ ]  |
| 0x14D8 | NtUserGetLayeredWindowAttributes2        | §18 | T12   | [ ]  |
| 0x14D9 | NtUserGetListBoxInfo2                    | §19 | T12   | [ ]  |
| 0x14DA | NtUserGetMenuIndex                       | §10 | T12   | [ ]  |
| 0x14DB | NtUserGetMessage2                        | §8  | T12   | [ ]  |
| 0x14DC | NtUserGetMessagePos2                     | §22 | T12   | [ ]  |
| 0x14DD | NtUserGetMouseMoveCountFromHidData       | §23 | T12   | [ ]  |
| 0x14DE | NtUserGetObjectInformation2              | §25 | T12   | [ ]  |
| 0x14DF | NtUserGetOwnerTransformedMonitorRect     | §24 | T12   | [ ]  |
| 0x14E0 | NtUserGetPhysicalDeviceRect              | §24 | T12   | [ ]  |
| 0x14E1 | NtUserGetPointerCursorId                 | §23 | T12   | [ ]  |
| 0x14E2 | NtUserGetPointerDeviceCursors            | §23 | T12   | [ ]  |
| 0x14E3 | NtUserGetPointerDeviceProperties         | §23 | T12   | [ ]  |
| 0x14E4 | NtUserGetPointerDeviceInputSpace         | §23 | T12   | [ ]  |
| 0x14E5 | NtUserGetPointerFrameArrivalTimes        | §23 | T12   | [ ]  |
| 0x14E6 | NtUserGetPointerFrameInfoHistory         | §23 | T12   | [ ]  |
| 0x14E7 | NtUserGetPointerFramePenInfo             | §23 | T12   | [ ]  |
| 0x14E8 | NtUserGetPointerFramePenInfoHistory      | §23 | T12   | [ ]  |
| 0x14E9 | NtUserGetPointerFrameTouchInfo           | §23 | T12   | [ ]  |
| 0x14EA | NtUserGetPointerFrameTouchInfoHistory    | §23 | T12   | [ ]  |
| 0x14EB | NtUserGetPointerInfoHistory              | §23 | T12   | [ ]  |
| 0x14EC | NtUserGetPointerInfoList                 | §23 | T12   | [ ]  |
| 0x14ED | NtUserGetPointerInputTransform           | §23 | T12   | [ ]  |
| 0x14EE | NtUserGetPointerPenInfo                  | §23 | T12   | [ ]  |
| 0x14EF | NtUserGetPointerPenInfoHistory           | §23 | T12   | [ ]  |
| 0x14F0 | NtUserGetPointerTouchInfo                | §23 | T12   | [ ]  |
| 0x14F1 | NtUserGetPointerTouchInfoHistory         | §23 | T12   | [ ]  |
| 0x14F2 | NtUserGetProcessDpiAwarenessContext      | §22 | T12   | [ ]  |
| 0x14F3 | NtUserGetProcessUIAccess                 | §22 | T12   | [ ]  |
| 0x14F4 | NtUserGetProp2                           | §18 | T12   | [ ]  |
| 0x14F5 | NtUserGetRawInputDeviceInfo2             | §23 | T12   | [ ]  |
| 0x14F6 | NtUserGetRegisteredRawInputDevices2      | §23 | T12   | [ ]  |
| 0x14F7 | NtUserGetScrollBarInfo2                  | §20 | T12   | [ ]  |
| 0x14F8 | NtUserGetSendMessageReceiver             | §8  | T12   | [ ]  |
| 0x14F9 | NtUserGetSharedWindowData                | §18 | T12   | [ ]  |
| 0x14FA | NtUserGetSystemMetrics2                  | §22 | T12   | [ ]  |
| 0x14FB | NtUserGetThreadInfo                      | §22 | T12   | [ ]  |
| 0x14FC | NtUserGetTitleBarInfo2                   | §18 | T12   | [ ]  |
| 0x14FD | NtUserGetTopLevelWindow                  | §18 | T12   | [ ]  |
| 0x14FE | NtUserGetTouchInputInfo2                 | §23 | T12   | [ ]  |
| 0x14FF | NtUserGetTouchPadRect                    | §23 | T12   | [ ]  |
| 0x1500 | NtUserGetTouchValidationStatus           | §23 | T12   | [ ]  |
| 0x1501 | NtUserGetUniformSpaceMapping             | §22 | T12   | [ ]  |
| 0x1502 | NtUserGetUpdateRgn2                      | §18 | T12   | [ ]  |
| 0x1503 | NtUserGetWindowBand                      | §18 | T12   | [ ]  |
| 0x1504 | NtUserGetWindowCompositionAttribute2     | §18 | T12   | [ ]  |
| 0x1505 | NtUserGetWindowDC2                       | §18 | T12   | [ ]  |
| 0x1506 | NtUserGetWindowFeedbackSetting2          | §22 | T12   | [ ]  |
| 0x1507 | NtUserGetWindowMinimizeRect2             | §18 | T12   | [ ]  |
| 0x1508 | NtUserGetWindowPlacement2                | §18 | T12   | [ ]  |
| 0x1509 | NtUserGetWindowProcessHandle             | §18 | T12   | [ ]  |
| 0x150A | NtUserGetWindowRect2                     | §18 | T12   | [ ]  |
| 0x150B | NtUserGetWindowRgn2                      | §18 | T12   | [ ]  |
| 0x150C | NtUserGetWindowTextLength                | §18 | T12   | [ ]  |
| 0x150D | NtUserGetWindowTrackInfoAsync            | §18 | T12   | [ ]  |
| 0x150E | NtUserGhostWindowFromHungWindow          | §18 | T12   | [ ]  |
| 0x150F | NtUserHandleDelegatedInput               | §23 | T12   | [ ]  |
| 0x1510 | NtUserHardErrorControl                   | §22 | T12   | [ ]  |
| 0x1511 | NtUserHiliteMenuItem2                    | §10 | T12   | [ ]  |
| 0x1512 | NtUserHungWindowFromGhostWindow          | §18 | T12   | [ ]  |
| 0x1513 | NtUserImpersonateDdeClientWindow         | §25 | T12   | [ ]  |
| 0x1514 | NtUserInheritWindowMonitor               | §24 | T12   | [ ]  |
| 0x1515 | NtUserInitialize                         | §1  | T12   | [ ]  |
| 0x1516 | NtUserInitializeClientPfnArrays          | §1  | T12   | [ ]  |
| 0x1517 | NtUserInitializeGenericHidInjection      | §23 | T12   | [ ]  |
| 0x1518 | NtUserInitializeInputDeviceInjection     | §23 | T12   | [ ]  |
| 0x1519 | NtUserInitializePointerDeviceInjection   | §23 | T12   | [ ]  |
| 0x151A | NtUserInitializeTouchInjection           | §23 | T12   | [ ]  |
| 0x151B | NtUserInjectDeviceInput                  | §23 | T12   | [ ]  |
| 0x151C | NtUserInjectGenericHidInput              | §23 | T12   | [ ]  |
| 0x151D | NtUserInjectGesture                      | §23 | T12   | [ ]  |
| 0x151E | NtUserInjectKeyboardInput                | §23 | T12   | [ ]  |
| 0x151F | NtUserInjectMouseInput                   | §23 | T12   | [ ]  |
| 0x1520 | NtUserInjectPointerInput                 | §23 | T12   | [ ]  |
| 0x1521 | NtUserInjectTouchInput                   | §23 | T12   | [ ]  |
| 0x1522 | NtUserInteractiveControlQueryUsage       | §23 | T12   | [ ]  |
| 0x1523 | NtUserInternalClipCursor                 | §9  | T12   | [ ]  |
| 0x1524 | NtUserInternalGetWindowIcon              | §9  | T12   | [ ]  |
| 0x1525 | NtUserInternalGetWindowText              | §18 | T12   | [ ]  |
| 0x1526 | NtUserInvalidateRect2                    | §18 | T12   | [ ]  |
| 0x1527 | NtUserInvalidateRgn2                     | §18 | T12   | [ ]  |
| 0x1528 | NtUserIsClipboardFormatAvailable2        | §11 | T12   | [ ]  |
| 0x1529 | NtUserIsMouseInPointerEnabled            | §23 | T12   | [ ]  |
| 0x152A | NtUserIsMouseInputEnabled                | §9  | T12   | [ ]  |
| 0x152B | NtUserIsNonClientDpiScalingEnabled       | §22 | T12   | [ ]  |
| 0x152C | NtUserIsResizeLayoutSynchronizationEnabled | §22 | T12   | [ ]  |
| 0x152D | NtUserIsTopLevelWindow                   | §18 | T12   | [ ]  |
| 0x152E | NtUserIsTouchWindow2                     | §23 | T12   | [ ]  |
| 0x152F | NtUserIsWindowBroadcastingDpiToChildren  | §22 | T12   | [ ]  |
| 0x1530 | NtUserIsWindowGhosted                    | §18 | T12   | [ ]  |
| 0x1531 | NtUserKillTimer2                         | §8  | T12   | [ ]  |
| 0x1532 | NtUserLayoutCompleted                    | §22 | T12   | [ ]  |
| 0x1533 | NtUserLinkDpiCursor                      | §22 | T12   | [ ]  |
| 0x1534 | NtUserLoadCursorsAndIcons                | §9  | T12   | [ ]  |
| 0x1535 | NtUserLoadImage                          | §9  | T12   | [ ]  |
| 0x1536 | NtUserLoadKeyboardLayoutEx2              | §21 | T12   | [ ]  |
| 0x1537 | NtUserLogicalToPerMonitorDPIPhysicalPoint | §22 | T12   | [ ]  |
| 0x1538 | NtUserLogicalToPhysicalDpiPointForWindow | §22 | T12   | [ ]  |
| 0x1539 | NtUserMagControl                         | §22 | T12   | [ ]  |
| 0x153A | NtUserMagGetContextInformation           | §22 | T12   | [ ]  |
| 0x153B | NtUserMagSetContextInformation           | §22 | T12   | [ ]  |
| 0x153C | NtUserMenuItemFromPoint2                 | §10 | T12   | [ ]  |
| 0x153D | NtUserMinimizeWindowGroup                | §18 | T12   | [ ]  |
| 0x153E | NtUserModifyWindowTouchCapability        | §23 | T12   | [ ]  |
| 0x153F | NtUserMoveWindow2                        | §18 | T12   | [ ]  |
| 0x1540 | NtUserMsgWaitForMultipleObjectsEx2       | §22 | T12   | [ ]  |
| 0x1541 | NtUserNavigateFocus                      | §18 | T12   | [ ]  |
| 0x1542 | NtUserNotifyIMEStatus                    | §21 | T12   | [ ]  |
| 0x1543 | NtUserNotifyProcessCreate                | §25 | T12   | [ ]  |
| 0x1544 | NtUserOpenDesktopEx                      | §25 | T12   | [ ]  |
| 0x1545 | NtUserOpenInputDesktop                   | §25 | T12   | [ ]  |
| 0x1546 | NtUserPaintMonitor                       | §24 | T12   | [ ]  |
| 0x1547 | NtUserPeekMessage2                       | §8  | T12   | [ ]  |
| 0x1548 | NtUserPerMonitorDPIPhysicalToLogicalPoint | §22 | T12   | [ ]  |
| 0x1549 | NtUserPhysicalToLogicalDpiPointForWindow | §22 | T12   | [ ]  |
| 0x154A | NtUserPostMessage2                       | §8  | T12   | [ ]  |
| 0x154B | NtUserPostThreadMessage2                 | §25 | T12   | [ ]  |
| 0x154C | NtUserPrintWindow2                       | §18 | T12   | [ ]  |
| 0x154D | NtUserProcessConnect                     | §25 | T12   | [ ]  |
| 0x154E | NtUserProcessInkFeedbackCommand          | §23 | T12   | [ ]  |
| 0x154F | NtUserPromotePointer                     | §23 | T12   | [ ]  |
| 0x1550 | NtUserQueryDisplayConfig2                | §24 | T12   | [ ]  |
| 0x1551 | NtUserQueryInformationThread2            | §22 | T12   | [ ]  |
| 0x1552 | NtUserQueryInputContext                  | §21 | T12   | [ ]  |
| 0x1553 | NtUserQuerySendMessage                   | §8  | T12   | [ ]  |
| 0x1554 | NtUserQueryWindow                        | §18 | T12   | [ ]  |
| 0x1555 | NtUserRealChildWindowFromPoint2          | §18 | T12   | [ ]  |
| 0x1556 | NtUserRealWaitMessageEx                  | §8  | T12   | [ ]  |
| 0x1557 | NtUserRedrawWindow2                      | §18 | T12   | [ ]  |
| 0x1558 | NtUserRegisterBSDRWindow                 | §25 | T12   | [ ]  |
| 0x1559 | NtUserRegisterDManipHook                 | §23 | T12   | [ ]  |
| 0x155A | NtUserRegisterEdgy                       | §27 | T12   | [ ]  |
| 0x155B | NtUserRegisterErrorReportingDialog       | §22 | T12   | [ ]  |
| 0x155C | NtUserRegisterHotKey2                    | §21 | T12   | [ ]  |
| 0x155D | NtUserRegisterManipulationThread         | §23 | T12   | [ ]  |
| 0x155E | NtUserRegisterPointerDeviceNotifications | §23 | T12   | [ ]  |
| 0x155F | NtUserRegisterPointerInputTarget2        | §23 | T12   | [ ]  |
| 0x1560 | NtUserRegisterRawInputDevices2           | §23 | T12   | [ ]  |
| 0x1561 | NtUserRegisterServicesProcess            | §25 | T12   | [ ]  |
| 0x1562 | NtUserRegisterSessionPort                | §25 | T12   | [ ]  |
| 0x1563 | NtUserRegisterTouchHitTestingWindow      | §23 | T12   | [ ]  |
| 0x1564 | NtUserRegisterUserApiHook                | §21 | T12   | [ ]  |
| 0x1565 | NtUserRemoveClipboardFormatListener2     | §11 | T12   | [ ]  |
| 0x1566 | NtUserRemoveInjectionDevice              | §23 | T12   | [ ]  |
| 0x1567 | NtUserRemoveVisualIdentifier             | §27 | T12   | [ ]  |
| 0x1568 | NtUserReportInertia                      | §23 | T12   | [ ]  |
| 0x1569 | NtUserRequestMoveSizeOperation           | §18 | T12   | [ ]  |
| 0x156A | NtUserResolveDesktopForWOW               | §25 | T12   | [ ]  |
| 0x156B | NtUserScaleSystemMetricForDpi            | §22 | T12   | [ ]  |
| 0x156C | NtUserScrollWindowEx2                    | §18 | T12   | [ ]  |
| 0x156D | NtUserSendInput2                         | §21 | T12   | [ ]  |
| 0x156E | NtUserSendTouchInput                     | §23 | T12   | [ ]  |
| 0x156F | NtUserSetActivationFilter                | §22 | T12   | [ ]  |
| 0x1570 | NtUserSetActiveProcessForMonitor         | §24 | T12   | [ ]  |
| 0x1571 | NtUserSetAdditionalForegroundBoost       | §18 | T12   | [ ]  |
| 0x1572 | NtUserSetAutoRotation                    | §24 | T12   | [ ]  |
| 0x1573 | NtUserSetBridgeWindowChild               | §18 | T12   | [ ]  |
| 0x1574 | NtUserSetBrokeredForeground              | §18 | T12   | [ ]  |
| 0x1575 | NtUserSetCalibrationData                 | §23 | T12   | [ ]  |
| 0x1576 | NtUserSetChildWindowNoActivate           | §18 | T12   | [ ]  |
| 0x1577 | NtUserSetCoreWindow                      | §27 | T12   | [ ]  |
| 0x1578 | NtUserSetCoreWindowPartner               | §27 | T12   | [ ]  |
| 0x1579 | NtUserSetCursorContents                  | §9  | T12   | [ ]  |
| 0x157A | NtUserSetCursorIconData                  | §9  | T12   | [ ]  |
| 0x157B | NtUserSetDesktopColorTransform           | §24 | T12   | [ ]  |
| 0x157C | NtUserSetDesktopVisualInputSink          | §24 | T12   | [ ]  |
| 0x157D | NtUserSetDialogPointer                   | §19 | T12   | [ ]  |
| 0x157E | NtUserSetDialogSystemMenu                | §10 | T12   | [ ]  |
| 0x157F | NtUserSetDisplayAutoRotationPreferences  | §24 | T12   | [ ]  |
| 0x1580 | NtUserSetDisplayMapping                  | §24 | T12   | [ ]  |
| 0x1581 | NtUserSetFallbackForeground              | §18 | T12   | [ ]  |
| 0x1582 | NtUserSetFeatureReportResponse           | §23 | T12   | [ ]  |
| 0x1583 | NtUserSetForegroundRedirectionForHwnd    | §18 | T12   | [ ]  |
| 0x1584 | NtUserSetGestureConfig2                  | §23 | T12   | [ ]  |
| 0x1585 | NtUserSetImeHotKey                       | §21 | T12   | [ ]  |
| 0x1586 | NtUserSetImeInfoEx                       | §21 | T12   | [ ]  |
| 0x1587 | NtUserSetImeOwnerWindow                  | §21 | T12   | [ ]  |
| 0x1588 | NtUserSetInformationThread2              | §22 | T12   | [ ]  |
| 0x1589 | NtUserSetInteractiveControlFocus         | §23 | T12   | [ ]  |
| 0x158A | NtUserSetInteractiveCtrlRotationAngle    | §23 | T12   | [ ]  |
| 0x158B | NtUserSetInternalWindowPos2              | §18 | T12   | [ ]  |
| 0x158C | NtUserSetKeyboardState2                  | §21 | T12   | [ ]  |
| 0x158D | NtUserSetLayeredWindowAttributes2        | §18 | T12   | [ ]  |
| 0x158E | NtUserSetManipulationInputTarget         | §23 | T12   | [ ]  |
| 0x158F | NtUserSetMenu2                           | §10 | T12   | [ ]  |
| 0x1590 | NtUserSetMenuContextHelpId2              | §10 | T12   | [ ]  |
| 0x1591 | NtUserSetMenuDefaultItem2                | §10 | T12   | [ ]  |
| 0x1592 | NtUserSetMenuFlagRtoL                    | §10 | T12   | [ ]  |
| 0x1593 | NtUserSetMirrorRendering                 | §14 | T12   | [ ]  |
| 0x1594 | NtUserSetObjectInformation2              | §25 | T12   | [ ]  |
| 0x1595 | NtUserSetParent2                         | §18 | T12   | [ ]  |
| 0x1596 | NtUserSetProcessDpiAwarenessContext2     | §22 | T12   | [ ]  |
| 0x1597 | NtUserSetProcessInteractionFlags         | §22 | T12   | [ ]  |
| 0x1598 | NtUserSetProcessMousewheelRoutingMode    | §22 | T12   | [ ]  |
| 0x1599 | NtUserSetProcessRestrictionExemption     | §22 | T12   | [ ]  |
| 0x159A | NtUserSetProcessUIAccess                 | §22 | T12   | [ ]  |
| 0x159B | NtUserSetProp2                           | §18 | T12   | [ ]  |
| 0x159C | NtUserSetScrollInfo2                     | §20 | T12   | [ ]  |
| 0x159D | NtUserSetSensorPresence                  | §23 | T12   | [ ]  |
| 0x159E | NtUserSetShellChangeNotifyWindow         | §25 | T12   | [ ]  |
| 0x159F | NtUserSetSysColors2                      | §22 | T12   | [ ]  |
| 0x15A0 | NtUserSetSystemCursor2                   | §9  | T12   | [ ]  |
| 0x15A1 | NtUserSetSystemMenu2                     | §10 | T12   | [ ]  |
| 0x15A2 | NtUserSetThreadDesktop2                  | §25 | T12   | [ ]  |

> **Total: 1300 shadow SSDT entries** across 28 functional ranges -- full Windows 11 win32k.sys parity plus 55 Impossible OS exclusive graphics extensions. All entries initially return `STATUS_NOT_IMPLEMENTED` until their owning section is implemented.

**Test checkpoint:** `syscall_dispatch(0x1FFF)` (or first out-of-table shadow index) returns `STATUS_NOT_IMPLEMENTED`, not crash. `syscall_dispatch(0x1000)` reaches the Win32k handler registered for `NtGdiCreateCompatibleDC` once `TODO-12` §1 is wired. Serial: `"win32k: shadow SSDT registered, 1300 services"` during init when the table is fully populated.

**Audit:** Extend `/audit-ssdt` (or a Win32k-specific pass) so Table 1 `ssdt_register()` coverage is reconciled against this file the same way Table 0 is reconciled against `service_numbers.h` and `02-kernel-core/TODO-A-SSDT-Master-Table.md`.
