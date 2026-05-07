@echo off
:: bootimg.bat -- Windows shim for tools/bootimg/bootimg.py.
::
:: Locates a Python 3 interpreter and forwards all arguments. The Python
:: tool itself is host-portable; this shim only exists to give Windows
:: users the same `bootimg <subcmd>` ergonomics that POSIX hosts get from
:: the shebang.
::
:: Lookup order (try each in turn; fall through on probe failure):
::   1. py -3        -- Python launcher with -3 selector
::   2. python       -- whichever python is first on PATH (must be Python 3)
::
:: Each candidate is probed with `-c "import sys; sys.exit(0 if
:: sys.version_info[0]==3 else 1)"` BEFORE dispatch, so a host with the
:: launcher installed but no Python 3 registration falls through to
:: python.exe (and a Python 2 python.exe gets rejected with the resolver
:: error rather than producing a SyntaxError from bootimg.py).

setlocal

set "_BOOTIMG_PY=%~dp0bootimg.py"

:: Probe py -3
where py >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    py -3 -c "import sys; sys.exit(0 if sys.version_info[0]==3 else 1)" >nul 2>&1
    if not errorlevel 1 (
        py -3 "%_BOOTIMG_PY%" %*
        exit /b %ERRORLEVEL%
    )
)

:: Probe python
where python >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    python -c "import sys; sys.exit(0 if sys.version_info[0]==3 else 1)" >nul 2>&1
    if not errorlevel 1 (
        python "%_BOOTIMG_PY%" %*
        exit /b %ERRORLEVEL%
    )
)

echo [ERROR] bootimg: no Python 3 interpreter found on PATH (need `py -3` or `python` resolving to Python 3) 1>&2
exit /b 127
