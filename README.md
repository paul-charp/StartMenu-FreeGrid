# StartMenu FreeGrid

Place pinned apps and folders freely on a grid in the Windows 11 Start menu, empty cells kept.

![Screenshot](docs/screenshot.png)

## Install

Download `StartMenu-FreeGrid-Setup-<version>.exe` from [Releases](https://github.com/paul-charp/StartMenu-FreeGrid/releases) and run it. Per-user, no admin. The installer is unsigned, so SmartScreen warns: More info > Run anyway.

## Use

- Drag a tile to any cell. Dropping on a tile swaps the two.
- Move left / Move right / Move to front (tile menu) move by one cell, or to the first cell.
- Folders: right-click > Create a new app folder.

## Window and CLI

Start `StartMenu FreeGrid` from the Start menu: status, on/off switch, columns, rows, reset layout, log, data folder.

| Command | |
| --- | --- |
| `StartGrid.exe` | window |
| `StartGrid.exe enable` | install a copy, start at logon, run now |
| `StartGrid.exe disable` | restore stock Start, stop, remove autostart |
| `StartGrid.exe run` | watcher (used by autostart) |
| `StartGrid.exe status` | watcher, autostart, Start process |
| `StartGrid.exe reset` | forget all tile cells |
| `StartGrid.exe log` | last 50 log lines |
| `StartGrid.exe version` | version |

## Settings

`%LOCALAPPDATA%\StartGrid\settings.ini`, reloaded live:

```
[grid]
columns=0
rows=0
```

`0` = fit the Start width / up to the last used row. `columns` and `rows` are settings.ini only (not in the window). Cells are in `cells.txt` next to it, the log in `log.txt`.

## Uninstall

Settings > Apps. The uninstaller asks whether to keep the saved layout.

## How it works

A small watcher attaches a XAML diagnostics extension dll to `StartMenuExperienceHost.exe` (the same interface Visual Studio's live visual tree uses). No code injection, nothing patched on disk. The dll moves the pinned tiles with render transforms. Disable puts the stock layout back; the dll itself stays loaded in Start until Start restarts.

## Limits

- Dragging into or out of folders is Start's own: dropped tiles go to the cell under the cursor or the first free one.
- Keyboard navigation order follows Start's list, not the grid.
- Windows 11 new Start only. Tested on build 26300.

## Build

VS 2022 (C++ tools) and the Windows SDK. Inno Setup 6 for the installer.

```
./build.ps1 [-Version x.y.z] [-Installer]
```

Output in `build\`.

## Release

Bump `src/common/version.h` and `CHANGELOG.md`, tag `vX.Y.Z`, push the tag. The workflow builds and publishes.

## License

MIT
