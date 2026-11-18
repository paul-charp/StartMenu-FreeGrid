# Windhawk-StartGrid

Windhawk mod (C++) placing pinned apps and folders freely on a grid in the native Windows 11 Start menu. Brief: vault `🤖 Claude/Handoff/2026-11-18 Handoff Windhawk Start menu free grid.md`.

## Build and test

- Windhawk's own clang compiles mods. Syntax check outside the editor:
  `& "$env:ProgramFiles\Windhawk\Compiler\bin\clang++.exe" @(Get-Content "$env:ProgramFiles\Windhawk\Compiler\compile_flags.txt") -fsyntax-only <file>`
- Loading a mod goes through the Windhawk editor (paste, Compile). Its workspace file in `%ProgramData%\Windhawk\EditorWorkspace` needs admin to write.
- `#undef GetCurrentTime` before the winrt XAML headers.
- XAML diagnostics: call `InitializeXamlDiagnosticsEx` with `VisualDiagConnection1..N` until it stops returning `ERROR_NOT_FOUND` (0x80070490), with `L""` as the diagnostics dll. The mod dll exports `DllGetClassObject` for its TAP; advise the visual tree from a separate thread, never from `SetSite`.
- `GetTempPath` inside `StartMenuExperienceHost.exe` is the user `%TEMP%`, not the package `AC\Temp`.

## Start XAML (26H2 build 26300.9974, new Start)

Dumped with `spike/start-xaml-dump.wh.cpp` (writes `%TEMP%\start-xaml-tree.txt`).

Start uses system XAML (`Windows.UI.Xaml`) plus WinUI 2 (`Microsoft.UI.Xaml.CBS`).

Path to the pinned grid:
```
Window #Start > … > StartMenu.StartHome > Grid #PageRoot > SemanticZoom #TopLevelRoot > … >
GridView #AllAppsGrid (header: ContentControl > Grid #TopLevelHeader) >
StartMenu.PinnedList #StartMenuPinnedList > Grid #Root > GridView #PinnedList
```
- Pinned is the header of the All apps grid; both scroll together.
- `#PinnedList`: plain `GridView`, `ItemsPanel` = `ItemsWrapGrid`, `ItemsSource` = `IObservableVector<StartMenu.IPinnedListItemViewModel>`, items `StartMenu.PinnedListTileViewModel`. `CanReorderItems`, `CanDragItems`, `AllowDrop` all on.
- Containers: `GridViewItem` 96x84 > … > `StartMenu.PinnedListTile` > `Grid #Root` (`#FolderHintPlate`, `#LogoContainer` > `#Logo`, `#DisplayName`).
- Item ID: the `GridViewItem`'s `AutomationProperties.AutomationId` is `tile-` + AUMID (`P~<PackageFamily>!<App>` for packaged apps, `W~<AUMID or known-folder path>` for desktop apps). `AutomationProperties.Name` is the display name. The view model is internal (no `ICustomPropertyProvider`).
- Start sets a fixed local `Height` (rows × 84) on both `#PinnedList` and `StartMenu.PinnedList`. Extra rows clip unless the mod overrides both.
- Swapping `#PinnedList.ItemsPanel` for another panel crashes Start's own code (`StartMenu.dll`, access violation) on folder creation. Keep the native `ItemsWrapGrid` and move tiles with `RenderTransform` from their `LayoutInformation.GetLayoutSlot`.
- Start's drop logic (reorder, add to folder, folder from tile-on-tile) targets the untransformed layout, so it can't be used with moved tiles. The mod handles drags inside the grid with `CanReorderItems` off; folders are managed from the context menu. `CanReorderItems` can't be changed inside `DragItemsStarting` (the drag never starts, and the grid can stay stuck until Start restarts) and Start resets it during a drag.
- Folders have an empty AutomationId and a name that follows their content ("Folder with 2 items including …").
- An open folder (`StartMenu.FolderModal`) covers the grid: drops from it land on the overlay, so listen on the XAML root with `handledEventsToo`.
- Folders open in `StartMenu.FolderModal #StartFolderModal`.
