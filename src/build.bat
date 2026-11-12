@echo off
rem ============================================================
rem  fakedisk build script (requires VS MSVC, initialized via vcvars64)
rem  Outputs 4 executables:
rem    fakedisk-dos.exe            (readonly cli)
rem    fakedisk-dos-manager.exe    (read-write cli)
rem    fakedisk-windows.exe        (readonly gui)
rem    fakedisk-windows-manager.exe(read-write gui)
rem ============================================================
setlocal
call "E:\cpp\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo vcvars64.bat not found. Please check VS installation path.
    exit /b 1
)
cd /d "%~dp0.."
if not exist bin mkdir bin

set CFLAGS=/nologo /std:c++17 /O2 /utf-8 /EHsc /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS

echo === [1/5] build fakedisk-dos (readonly) ===
cl %CFLAGS% /DFDISK_READONLY src\fakedisk.cpp src\fakedisk_dos.cpp /Fe:bin\fakedisk-dos.exe /link shell32.lib shlwapi.lib
if errorlevel 1 goto :err

echo === [2/5] build fakedisk-dos-manager (read-write) ===
cl %CFLAGS% src\fakedisk.cpp src\fakedisk_dos.cpp /Fe:bin\fakedisk-dos-manager.exe /link shell32.lib shlwapi.lib
if errorlevel 1 goto :err

echo === [3/5] compile resources ===
rc /nologo /c65001 /fo src\fakedisk_win.res /i src src\fakedisk.rc
if errorlevel 1 goto :err

echo === [4/5] build fakedisk-windows (readonly) ===
cl %CFLAGS% /DFDISK_READONLY src\fakedisk.cpp src\fakedisk_win.cpp src\dialogs.cpp src\fakedisk_win.res /Fe:bin\fakedisk-windows.exe /link user32.lib gdi32.lib gdiplus.lib shell32.lib comctl32.lib comdlg32.lib ole32.lib advapi32.lib /SUBSYSTEM:WINDOWS
if errorlevel 1 goto :err

echo === [5/5] build fakedisk-windows-manager (read-write) ===
cl %CFLAGS% src\fakedisk.cpp src\fakedisk_win.cpp src\dialogs.cpp src\fakedisk_win.res /Fe:bin\fakedisk-windows-manager.exe /link user32.lib gdi32.lib gdiplus.lib shell32.lib comctl32.lib comdlg32.lib ole32.lib advapi32.lib /SUBSYSTEM:WINDOWS
if errorlevel 1 goto :err

echo.
echo Build OK: bin\fakedisk-dos.exe  bin\fakedisk-dos-manager.exe  bin\fakedisk-windows.exe  bin\fakedisk-windows-manager.exe
exit /b 0

:err
echo.
echo BUILD FAILED!
exit /b 1
