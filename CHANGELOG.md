# Changelog

## [0.2.0] - 2026-11-22

### Changed
- Tiles slide to their new cell instead of jumping.
- Dragging over an occupied cell pushes tiles aside live, up to the next empty cell, instead of swapping on drop.
- Drops from All apps onto an occupied cell push tiles the same way.
- The dragged tile fades out during the drag and scales back in on drop.
- New tiles (pinned, dragged out of a folder, new folders) slide and fade in.

### Added
- `settings.ini` `probe=1`: logs drag events and tile states.

## [0.1.0] - 2026-11-18

### Added
- Free grid placement of pinned Start items, empty cells kept.
- Drag a tile to any cell; dropping on a tile swaps the two.
- Move left, Move right and Move to front act as cell moves.
- Cell per item saved across restarts.
- Folders created from the context menu.
- `settings.ini` with `columns` and `rows`.
- CLI: `enable`, `disable`, `run`, `status`, `reset`, `log`, `version`.
- Window: status, on/off switch, columns and rows, reset layout, log, data folder.
- Installer (per-user, Windows 11).
