# fakedisk — a single-file virtual disk for Windows

fakedisk packs an entire directory tree and all of its file contents into **one ordinary file**, then gives you a DOS-style shell and a Win32 GUI to work with it. It is **not** a mountable image — nothing appears in Explorer — but the handling is deliberately close to a real drive: volume label, capacity / used / free, `dir` listings, `fsck`, file attributes, timestamps, on-disk compression.

Four standalone executables. No installer, no runtime dependencies beyond Windows itself.

## Quick start

Read-write command line:

```
bin\fakedisk-dos-manager.exe
FDISK:\> format demo.fdk 64M /v:MYDISK
demo:\> mkdir \pics
demo:\> copy C:\Users\me\photo.png /pics\photo.png
demo:\> dir /pics
demo:\> exit
```

As a GUI, or read-only:

```
bin\fakedisk-windows-manager.exe demo.fdk    (read-write GUI, or File -> Open Disk)
bin\fakedisk-windows.exe demo.fdk            (read-only GUI)
bin\fakedisk-dos.exe demo.fdk                (read-only CLI)
```

`copy` does not create missing directories, so `mkdir \pics` comes first. Every internal path is normalized, so `/pics` and `\pics` are the same directory.

## The four tools

| Tool | Form | Role | Capabilities |
|---|---|---|---|
| `fakedisk-dos.exe` | CLI shell | **reader** (dos) | open/dir/cd/type/export/vol/info/fsck/run — never writes |
| `fakedisk-dos-manager.exe` | CLI shell | **writer** (dos-manager) | everything above plus format/copy/xcopy/mkdir/rmdir/del/attr/ren/move/touch/vol/defrag; saved on every change |
| `fakedisk-windows.exe` | Win32 GUI | **reader** (windows) | tree / list browsing, in-memory text and image preview, export, fsck, program extraction and run; edit commands are clickable but answer "you do not have permission", drag and drop is refused, never writes |
| `fakedisk-windows-manager.exe` | Win32 GUI | **writer** (windows-manager) | everything `windows` does plus new disk / import (file, folder, drag and drop) / new / delete / rename / attributes / defrag / save |

> A reader tool never touches the container: MD5 and modification time are identical before and after closing it.
> A writer tool writes the whole container back atomically after each modification.

## dos command reference

Full set for `dos-manager`; the read-only `dos` build rejects the items marked (W).

```
format <file> [size(K/M/G/T)] [/v:label] (W)  create a new disk and mount it
open <file> / close                          open / close
dir [path] / cd <path> / mkdir (W) / rmdir (W)   directory operations
copy <external file> <internal path> (W)     import one file (compressed automatically)
xcopy <external dir> <internal dir> (W)      import a whole directory recursively
export <internal path> <external file>       export
type <path>                                  read text in memory and print it
attr <path> [+R|-R][+H|-H][+S|-S][+A|-A] (W)  change attributes
ren (W) / move (W) / touch (W) / vol [label] (W)   rename, move, update time, volume label
run <path>                                   extract to %TEMP% and run / open
info / fsck / defrag (W) / cls / help / exit
```

Paths accept `/` or `\`; relative paths and `..` work; quote paths that contain spaces.

## GUI

- Directory tree on the left, file list on the right (name / size / type / attributes / modified) plus a status bar
- **windows-manager**: File (open, new, import, export), Edit (new folder, rename, delete, attributes), Tools (fsck, defrag, run), **View (desktop view / list view)**, **Language (中文 / English)**, drag and drop import
- **windows (read-only)**: same window; edit commands are clickable but reply "you do not have permission"; browsing, preview, export, fsck and run only
- **Desktop view**: the right pane becomes a Windows-desktop-style large-icon grid (48 px system icons, auto-arranged); the status bar names the current view; double-click behaves exactly as in list view
- Double-click: text opens the built-in viewer; images get a GDI+ preview; programs and everything else are extracted to `%TEMP%\fakedisk_extract\` and opened
- File data is always read in memory; the only thing ever written to disk is a program being run, and only into a temporary directory

## Localization

- **GUI**: a **Language (L)** menu switches between 中文 and English and applies immediately — menus, column headers, status bar, dialogs, message boxes, About box and title bar. The choice is persisted in `HKCU\Software\fakedisk\Lang` and survives a restart; with no stored value the UI follows the system language (a Chinese system starts in Chinese).
- **dos**: output follows the `FAKEDISK_LANG` environment variable. `en` (or `english`) selects English, `zh` / `cn` select Chinese, unset follows the system language:

  ```
  set FAKEDISK_LANG=en
  fakedisk-dos-manager.exe test.fdk
  ```

- Implementation: `src\lang.h` provides `L()` / `L8()`; GUI menu text is reset at runtime (DrawMenuBar forces the redraw), dialog static text is applied during initialization.

## Container format

The full specification is in [FORMAT.md](FORMAT.md) (Chinese: [FORMAT.zh-CN.md](FORMAT.zh-CN.md)): a 96-byte header, a tree-structured directory table and a data area, with UTF-8 file names, four attribute bits (R/H/S/A), Unix timestamps, LZNT1 compression and CRC-32 integrity checks.

## Building

You need Visual Studio with the MSVC toolset. Point the `vcvars64.bat` path at the top of `src\build.bat` at your own installation, then run:

```
src\build.bat
```

One run produces all four executables in `bin\`. The dos / dos-manager split is only the `FDISK_READONLY` macro. `src\make_icon.py` regenerates the icon.

## Repository layout

```
fakedisk\
├── bin\                 build output (4 exes; not tracked)
├── src\                 sources (fakedisk.h/.cpp core engine, dos, win, dialogs, resources, build.bat, make_icon.py)
├── test\                test assets and regression scripts (cmds, inspect_fdk.py, gui_sendinput.py)
├── FORMAT.md            container format specification (English)
├── FORMAT.zh-CN.md      container format specification (Chinese)
├── README.md            this file
└── README.zh-CN.md      Chinese README
```

## What has been tested

- create / open / save (atomic write), directory tree (multi-level, Chinese names), volume labels
- import with automatic LZNT1 compression (352.6 KB down to 241.5 KB stored), export byte-identical to the source
- attributes / rename / move / timestamps, recursive `xcopy`
- `fsck` end to end: directory-table CRC, offset overlap, per-file content CRC
- `run`: extract and execute (a batch script ran and produced its marker file)
- **reader tools do not write**: the container's MD5 and modification time are unchanged across open and close for both `dos` and `windows`; `dos` rejects write commands
- **read-only GUI**: edit commands are clickable and answer "you do not have permission" without entering any edit flow, while the same command in `windows-manager` opens its dialog normally
- **writer tools work**: every `dos-manager` command passes; `windows-manager` opens its new / import menus correctly
- GUI: rendering, double-click into a directory, text viewer, GDI+ image viewer
- **desktop view**: switching to the large-icon grid works, double-click into a directory works there too, and switching back restores the list headers and columns
- **localization**: `dos` produces correct output in both languages (`FAKEDISK_LANG=en` turns help, operations and rejections English); the GUI started in Chinese, switched to English through the Language menu (menus, column headers, status bar, title, New Disk dialog and the read-only rejection all English), kept English after a restart (registry) and switched back instantly; identical behaviour in both builds

## Requirements

Windows, any current x64 build. The executables link against system libraries only (user32, gdi32, gdiplus, shell32, comctl32, comdlg32, ole32, advapi32, and ntdll for LZNT1). No installer, no runtime, no redistributables.

## License

GPL-3.0 — see [LICENSE](LICENSE). Copyright (C) 2026 SYSICio.

You are free to use, study, share and modify this software; if you distribute it or a modified version, the same license and the corresponding source must travel with it.

