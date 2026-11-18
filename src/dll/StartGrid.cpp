#include "host.h"

#include <unknwn.h>
#include <CoreWindow.h>
#include <xamlom.h>

#undef GetCurrentTime  // winbase.h macro breaks the XAML projection

#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Animation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace winrt::Windows::UI::Xaml;
namespace wuc = winrt::Windows::UI::Core;
namespace wf = winrt::Windows::Foundation;
using Controls::GridView;
using Controls::GridViewItem;

namespace {

constexpr wchar_t kPinnedType[] = L"Windows.UI.Xaml.Controls.GridView";
constexpr wchar_t kPinnedName[] = L"PinnedList";
constexpr ULONGLONG kPendingDropMs = 5000;

// ---------------------------------------------------------------- settings

struct Settings {
    int columns = 0;
    int rows = 0;
};

std::mutex g_settingsLock;
Settings g_settings;

void LoadSettings() {
    Settings s;
    s.columns = GetGridInt(L"columns");
    s.rows = GetGridInt(L"rows");
    std::lock_guard lock(g_settingsLock);
    g_settings = s;
}

Settings CurrentSettings() {
    std::lock_guard lock(g_settingsLock);
    return g_settings;
}

// ------------------------------------------------------------ cell storage

// Key -> cell. Key is the tile's AUMID, or "folder:<name>" for folders.
// Loaded on activation, then touched on Start's UI thread only.
struct Cell {
    int row;
    int col;
    bool operator==(const Cell&) const = default;
};
std::map<std::wstring, Cell> g_cells;

void LoadCells() {
    g_cells.clear();
    std::wstring text = ReadCells();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) {
            end = text.size();
        }
        std::wstring line = text.substr(pos, end - pos);
        pos = end + 1;
        size_t t2 = line.rfind(L'\t');
        size_t t1 = t2 == std::wstring::npos ? t2 : line.rfind(L'\t', t2 - 1);
        if (t1 == std::wstring::npos || t1 == 0) {
            continue;
        }
        g_cells[line.substr(0, t1)] = {_wtoi(line.c_str() + t1 + 1),
                                       _wtoi(line.c_str() + t2 + 1)};
    }
    Log(L"Loaded %zu cells", g_cells.size());
}

void SaveCells() {
    std::wstring text;
    for (auto& [key, cell] : g_cells) {
        text += key + L"\t" + std::to_wstring(cell.row) + L"\t" +
                std::to_wstring(cell.col) + L"\n";
    }
    WriteCells(text);
}

// ---------------------------------------------------------- grid state (UI)

// One pinned item as seen on the last layout pass.
struct Known {
    wf::IInspectable item;
    std::wstring key;
};

// Everything below is touched on Start's UI thread only.
struct State {
    InstanceHandle handle = 0;
    winrt::weak_ref<GridView> view;
    winrt::weak_ref<Controls::Panel> panel;
    winrt::weak_ref<FrameworkElement> host;
    winrt::weak_ref<Controls::Panel> overlayParent;
    Controls::Border highlight{nullptr};
    bool applied = false;
    double cellW = 96;
    double cellH = 84;
    double height = 0;
    int cols = 1;

    // Original local values, restored on disable.
    bool origReorder = false;
    wf::IInspectable origViewHeight{nullptr};
    wf::IInspectable origHostHeight{nullptr};
    wf::IInspectable origPanelBackground{nullptr};
    wf::IInspectable origPanelMinHeight{nullptr};
    wf::IInspectable origPanelAllowDrop{nullptr};
    wf::IInspectable origTransitions{nullptr};

    // Last pass, for folder renames, move commands and replaced items.
    std::vector<Known> known;

    // Drag and context menu.
    wf::IInspectable dragItem{nullptr};
    wf::IInspectable menuItem{nullptr};
    std::optional<Cell> pendingDrop;
    ULONGLONG pendingDropTime = 0;
    // Native reorder also shuffles Start's list during a drag: ignore list
    // order until shortly after it ends.
    ULONGLONG ignoreOrderUntil = 0;
    ULONGLONG externalLogTime = 0;

    FrameworkElement::LayoutUpdated_revoker layoutRevoker;
    Controls::ListViewBase::DragItemsStarting_revoker dragStartRevoker;
    Controls::ListViewBase::DragItemsCompleted_revoker dragEndRevoker;
    UIElement::DragOver_revoker panelDragOverRevoker;
    UIElement::Drop_revoker panelDropRevoker;
    winrt::weak_ref<UIElement> dragRoot;
    wf::IInspectable dragOverHandler{nullptr};
    wf::IInspectable dragLeaveHandler{nullptr};
    wf::IInspectable dropHandler{nullptr};
    wf::IInspectable rightTappedHandler{nullptr};
};

State g_state;
wuc::CoreDispatcher g_dispatcher{nullptr};
std::mutex g_dispatcherLock;
std::atomic<bool> g_unloading{false};
std::atomic<bool> g_failed{false};  // after an error, stay out until Start restarts

void Layout(const GridView& gv);
std::optional<Cell> CursorCell();

wuc::CoreDispatcher GetDispatcher() {
    std::lock_guard lock(g_dispatcherLock);
    return g_dispatcher;
}

void SetDispatcherIfNone(const wuc::CoreDispatcher& d) {
    std::lock_guard lock(g_dispatcherLock);
    if (!g_dispatcher) {
        g_dispatcher = d;
    }
}

std::wstring ItemId(const GridViewItem& c) {
    std::wstring id(Automation::AutomationProperties::GetAutomationId(c));
    if (id.rfind(L"tile-", 0) == 0) {
        id.erase(0, 5);
    }
    return id;
}

std::wstring KeyOf(const GridViewItem& c) {
    std::wstring id = ItemId(c);
    if (!id.empty()) {
        return id;
    }
    return L"folder:" +
           std::wstring(Automation::AutomationProperties::GetName(c));
}

std::wstring KeyOfItem(const wf::IInspectable& item) {
    for (auto& k : g_state.known) {
        if (k.item == item) {
            return k.key;
        }
    }
    return {};
}

int Index(Cell c) { return c.row * g_state.cols + c.col; }
Cell FromIndex(int i) { return {i / g_state.cols, i % g_state.cols}; }

// Key of the present item at cell, if any.
std::wstring Occupant(Cell cell, const std::wstring& except) {
    for (auto& k : g_state.known) {
        if (k.key == except) {
            continue;
        }
        auto it = g_cells.find(k.key);
        if (it != g_cells.end() && it->second == cell) {
            return k.key;
        }
    }
    return {};
}

// Moves key to cell, swapping with the tile already there.
void MoveTo(const std::wstring& key, Cell target) {
    auto it = g_cells.find(key);
    if (it == g_cells.end() || it->second == target) {
        return;
    }
    Cell from = it->second;
    std::wstring other = Occupant(target, key);
    if (!other.empty()) {
        g_cells[other] = from;
    }
    it->second = target;
    SaveCells();
    Log(L"Moved %s to row %d, column %d%s", key.c_str(), target.row,
        target.col, other.empty() ? L"" : L" (swapped)");
}

// --------------------------------------------------------------- highlight

void SetLocal(const DependencyObject& obj,
              const DependencyProperty& prop,
              const wf::IInspectable& value) {
    if (!obj) {
        return;
    }
    if (!value || value == DependencyProperty::UnsetValue()) {
        obj.ClearValue(prop);
    } else {
        obj.SetValue(prop, value);
    }
}

void HideHighlight() {
    if (g_state.highlight) {
        g_state.highlight.Visibility(Visibility::Collapsed);
    }
}

void ShowHighlight(Cell cell) {
    auto parent = g_state.overlayParent.get();
    auto panel = g_state.panel.get();
    if (!parent || !panel) {
        return;
    }
    if (!g_state.highlight) {
        auto accent = winrt::Windows::UI::ViewManagement::UISettings().GetColorValue(
            winrt::Windows::UI::ViewManagement::UIColorType::Accent);
        auto fill = accent;
        fill.A = 0x40;
        Controls::Border b;
        b.IsHitTestVisible(false);
        b.HorizontalAlignment(HorizontalAlignment::Left);
        b.VerticalAlignment(VerticalAlignment::Top);
        b.CornerRadius({6, 6, 6, 6});
        b.BorderThickness({1.5, 1.5, 1.5, 1.5});
        b.BorderBrush(Media::SolidColorBrush(accent));
        b.Background(Media::SolidColorBrush(fill));
        Controls::Grid::SetRowSpan(b, 100);
        Controls::Grid::SetColumnSpan(b, 100);
        b.RenderTransform(Media::TranslateTransform{});
        parent.Children().Append(b);
        g_state.highlight = b;
    }
    // Anchor on a real tile's drawn position (padding, margins, transform
    // included), then step by cells from there.
    auto gv = g_state.view.get();
    if (!gv || g_state.known.empty()) {
        return;
    }
    FrameworkElement ref{nullptr};
    Cell refCell{};
    for (auto& k : g_state.known) {
        auto it = g_cells.find(k.key);
        auto c = gv.ContainerFromItem(k.item).try_as<FrameworkElement>();
        if (c && it != g_cells.end()) {
            ref = c;
            refCell = it->second;
            break;
        }
    }
    if (!ref) {
        return;
    }
    auto p = ref.TransformToVisual(parent).TransformPoint({0, 0});
    auto b = g_state.highlight;
    b.Width(ref.ActualWidth());
    b.Height(ref.ActualHeight());
    double x = p.X + (cell.col - refCell.col) * g_state.cellW;
    double y = p.Y + (cell.row - refCell.row) * g_state.cellH;
    // The highlight's own layout box may not sit at the parent's origin
    // (Grid padding): measure it and compensate.
    auto t = b.RenderTransform().as<Media::TranslateTransform>();
    auto self = b.TransformToVisual(parent).TransformPoint({0, 0});
    double boxX = self.X - t.X();
    double boxY = self.Y - t.Y();
    if (b.Visibility() == Visibility::Collapsed) {
        boxX = 0;
        boxY = 0;
        if (auto g = parent.try_as<Controls::Grid>()) {
            boxX = g.Padding().Left;
            boxY = g.Padding().Top;
        }
    }
    t.X(x - boxX);
    t.Y(y - boxY);
    b.Visibility(Visibility::Visible);
}

void RemoveHighlight() {
    if (g_state.highlight) {
        if (auto parent = g_state.overlayParent.get()) {
            uint32_t i;
            if (parent.Children().IndexOf(g_state.highlight, i)) {
                parent.Children().RemoveAt(i);
            }
        }
        g_state.highlight = nullptr;
    }
}

// ----------------------------------------------------------------- restore

void SetHeightIfNeeded(const FrameworkElement& fe, double h) {
    if (fe && fe.Height() != h) {
        fe.Height(h);
    }
}

void Restore() {
    g_state.layoutRevoker.revoke();
    g_state.dragStartRevoker.revoke();
    g_state.dragEndRevoker.revoke();
    if (g_state.applied) {
        try {
            RemoveHighlight();
            if (auto gv = g_state.view.get()) {
                auto unhook = [&](const RoutedEvent& ev, const wf::IInspectable& h) {
                    if (h) {
                        gv.RemoveHandler(ev, h);
                    }
                };
                unhook(UIElement::RightTappedEvent(), g_state.rightTappedHandler);
                if (auto root = g_state.dragRoot.get()) {
                    if (g_state.dragOverHandler) {
                        root.RemoveHandler(UIElement::DragOverEvent(), g_state.dragOverHandler);
                        root.RemoveHandler(UIElement::DragLeaveEvent(), g_state.dragLeaveHandler);
                        root.RemoveHandler(UIElement::DropEvent(), g_state.dropHandler);
                    }
                }
                auto items = gv.Items();
                for (uint32_t i = 0; i < items.Size(); ++i) {
                    if (auto c = gv.ContainerFromIndex(i).try_as<UIElement>()) {
                        c.ClearValue(UIElement::RenderTransformProperty());
                    }
                }
                gv.CanReorderItems(g_state.origReorder);
                SetLocal(gv, Controls::ItemsControl::ItemContainerTransitionsProperty(),
                         g_state.origTransitions);
                SetLocal(gv, FrameworkElement::HeightProperty(),
                         g_state.origViewHeight);
            }
            SetLocal(g_state.host.get(), FrameworkElement::HeightProperty(),
                     g_state.origHostHeight);
            if (auto panel = g_state.panel.get()) {
                SetLocal(panel, Controls::Panel::BackgroundProperty(),
                         g_state.origPanelBackground);
                SetLocal(panel, FrameworkElement::MinHeightProperty(),
                         g_state.origPanelMinHeight);
                SetLocal(panel, UIElement::AllowDropProperty(),
                         g_state.origPanelAllowDrop);
            }
            Log(L"Stock layout restored");
        } catch (...) {
            Log(L"Restore failed: 0x%08X", winrt::to_hresult());
        }
    }
    g_state = State{};
}

// ------------------------------------------------------------------ layout

// Start's menu commands reorder its list by one step or to the front.
// Detect which item moved and turn it into a cell move.
void DetectMove(const std::vector<std::wstring>& order) {
    std::vector<std::wstring> last;
    for (auto& k : g_state.known) {
        last.push_back(k.key);
    }
    if (g_state.dragItem || GetTickCount64() < g_state.ignoreOrderUntil) {
        return;
    }
    if (last.size() != order.size() || last == order ||
        !std::is_permutation(last.begin(), last.end(), order.begin())) {
        return;
    }
    std::wstring menuKey =
        g_state.menuItem ? KeyOfItem(g_state.menuItem) : std::wstring{};
    std::wstring moved;
    int oldIdx = -1;
    int newIdx = -1;
    for (size_t n = 0; n < order.size(); ++n) {
        auto a = order;
        auto b = last;
        int ni = int(n);
        int oi = int(std::find(last.begin(), last.end(), order[n]) - last.begin());
        a.erase(a.begin() + ni);
        b.erase(b.begin() + oi);
        if (a != b) {
            continue;
        }
        // Swapping neighbours fits two candidates: prefer the right-clicked one.
        if (moved.empty() || order[n] == menuKey) {
            moved = order[n];
            oldIdx = oi;
            newIdx = ni;
        }
    }
    if (moved.empty()) {
        return;
    }
    auto it = g_cells.find(moved);
    if (it == g_cells.end()) {
        return;
    }
    int cur = Index(it->second);
    int target;
    if (newIdx == 0) {
        target = 0;
    } else if (newIdx == oldIdx + 1) {
        target = cur + 1;
    } else if (newIdx == oldIdx - 1) {
        target = cur - 1;
    } else {
        return;
    }
    if (target >= 0) {
        MoveTo(moved, FromIndex(target));
    }
}

void Layout(const GridView& gv) {
    auto panel = gv.ItemsPanelRoot();
    if (!panel) {
        return;
    }
    Settings s = CurrentSettings();
    g_state.cols =
        s.columns > 0
            ? s.columns
            : std::max(1, int(std::floor(gv.ActualWidth() / g_state.cellW)));
    int cols = g_state.cols;

    struct Entry {
        wf::IInspectable item;
        GridViewItem container{nullptr};
        std::wstring key;
        int cell = -1;
    };
    std::vector<Entry> entries;
    bool incomplete = false;
    auto items = gv.Items();
    for (uint32_t i = 0; i < items.Size(); ++i) {
        auto c = gv.ContainerFromIndex(i).try_as<GridViewItem>();
        if (!c) {
            incomplete = true;
            continue;
        }
        entries.push_back({items.GetAt(i), c, KeyOf(c)});
        if (ItemId(c).empty() &&
            Automation::AutomationProperties::GetName(c).empty()) {
            incomplete = true;  // bindings not ready yet
        }
    }
    if (incomplete) {
        return;
    }

    bool changed = false;

    // A folder's key follows its name: carry its cell across renames.
    for (auto& e : entries) {
        for (auto& k : g_state.known) {
            if (k.item == e.item && k.key != e.key) {
                auto old = g_cells.find(k.key);
                if (old != g_cells.end() && !g_cells.count(e.key)) {
                    g_cells[e.key] = old->second;
                    g_cells.erase(old);
                    changed = true;
                }
                k.key = e.key;
            }
        }
    }

    std::vector<std::wstring> order;
    for (auto& e : entries) {
        order.push_back(e.key);
    }
    DetectMove(order);

    std::vector<bool> used;
    auto isUsed = [&](int cell) { return cell < int(used.size()) && used[cell]; };
    auto take = [&](int cell) {
        if (cell >= int(used.size())) {
            used.resize(cell + 1, false);
        }
        used[cell] = true;
    };
    auto stored = [&](const std::wstring& key) -> int {
        auto it = g_cells.find(key);
        if (it == g_cells.end() || it->second.col >= cols || it->second.row < 0 ||
            it->second.col < 0) {
            return -1;
        }
        return Index(it->second);
    };
    auto wasShown = [&](const std::wstring& key) {
        return std::any_of(g_state.known.begin(), g_state.known.end(),
                           [&](auto& k) { return k.key == key; });
    };

    // 1. Tiles already on screen keep their cell.
    for (auto& e : entries) {
        if (!wasShown(e.key)) {
            continue;
        }
        int cell = stored(e.key);
        if (cell >= 0 && !isUsed(cell)) {
            e.cell = cell;
            take(cell);
        }
    }

    auto place = [&](size_t i, int cell, const wchar_t* why) {
        auto& e = entries[i];
        e.cell = cell;
        take(cell);
        if (stored(e.key) != cell) {
            g_cells[e.key] = FromIndex(cell);
            changed = true;
        }
        Log(L"Placed %s at row %d, column %d (%s)", e.key.c_str(),
            cell / cols, cell % cols, why);
    };

    // 2. A tile that took the list index of a vanished one takes its cell:
    //    a new folder replaces its tile, a dissolved folder its last app.
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].cell >= 0 || i >= g_state.known.size()) {
            continue;
        }
        const auto& prev = g_state.known[i].key;
        bool gone = std::none_of(entries.begin(), entries.end(),
                                 [&](auto& x) { return x.key == prev; });
        int prevCell = gone ? stored(prev) : -1;
        if (prevCell >= 0 && !isUsed(prevCell)) {
            place(i, prevCell, L"replaced");
        }
    }

    // 3. A tile dropped in from outside the grid goes where it was dropped:
    //    the recorded drop cell (from All apps), or for an app that just left
    //    a folder, the cell under the cursor (folder drops aren't visible).
    std::optional<Cell> dropCell;
    const wchar_t* dropWhy = L"dropped";
    if (g_state.pendingDrop &&
        GetTickCount64() - g_state.pendingDropTime < kPendingDropMs) {
        dropCell = g_state.pendingDrop;
    } else if (!g_state.known.empty() &&
               std::count_if(entries.begin(), entries.end(),
                             [](auto& x) { return x.cell < 0; }) == 1) {
        // One new tile while Start is already shown: it was just dropped
        // (out of a folder) or pinned. Under the cursor if that's the grid.
        dropCell = CursorCell();
        dropWhy = L"under cursor";
    }
    if (dropCell && dropCell->col < cols && !isUsed(Index(*dropCell))) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].cell < 0) {
                place(i, Index(*dropCell), dropWhy);
                g_state.pendingDrop.reset();
                break;
            }
        }
    }

    // 4. Returning tiles get their old cell back, 5. others the first free.
    int next = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].cell >= 0) {
            continue;
        }
        int cell = stored(entries[i].key);
        if (cell >= 0 && !isUsed(cell)) {
            place(i, cell, L"returning");
            continue;
        }
        while (isUsed(next)) {
            ++next;
        }
        place(i, next, L"first free");
    }
    if (changed) {
        SaveCells();
    }

    // Shift each tile from its native slot to its cell. RenderTransform
    // doesn't trigger layout.
    for (auto& e : entries) {
        Cell cell = FromIndex(e.cell);
        auto slot = Controls::Primitives::LayoutInformation::GetLayoutSlot(e.container);
        double dx = cell.col * g_state.cellW - slot.X;
        double dy = cell.row * g_state.cellH - slot.Y;
        auto t = e.container.RenderTransform().try_as<Media::TranslateTransform>();
        if (!t) {
            t = Media::TranslateTransform{};
            e.container.RenderTransform(t);
        }
        if (t.X() != dx || t.Y() != dy) {
            t.X(dx);
            t.Y(dy);
        }
    }

    // One spare row while dragging, so tiles can go below the last one.
    int rows = std::max(s.rows, int(used.size() + cols - 1) / cols);
    rows = std::max(rows, 1) + (g_state.dragItem ? 1 : 0);
    double height = rows * g_state.cellH;
    if (g_state.height != height) {
        g_state.height = height;
        Log(L"Grid %dx%d cells", cols, rows);
    }
    SetHeightIfNeeded(gv, height);
    SetHeightIfNeeded(g_state.host.get(), height);
    if (auto p = panel.try_as<FrameworkElement>(); p && p.MinHeight() != height) {
        p.MinHeight(height);
    }

    g_state.known.clear();
    for (auto& e : entries) {
        g_state.known.push_back({e.item, e.key});
    }
}

// -------------------------------------------------------------- drag, menu

Cell CellAt(wf::Point pt) {
    int col = std::clamp(int(std::floor(pt.X / g_state.cellW)), 0,
                         g_state.cols - 1);
    int row = std::max(0, int(std::floor(pt.Y / g_state.cellH)));
    return {row, col};
}

std::optional<Cell> CellIfInside(wf::Point pt) {
    if (pt.X < 0 || pt.Y < 0 || pt.X >= g_state.cols * g_state.cellW ||
        pt.Y >= g_state.height) {
        return std::nullopt;
    }
    return CellAt(pt);
}

// Cell under the pointer, if it is over the grid area.
std::optional<Cell> CellIfInside(DragEventArgs const& e) {
    auto p = g_state.panel.get();
    if (!p) {
        return std::nullopt;
    }
    return CellIfInside(e.GetPosition(p));
}

// Cell under the mouse cursor right now, if it is over the grid. Drops out
// of an open folder never reach the grid's events, but when the app shows up
// in the grid the cursor is still where it was dropped.
std::optional<Cell> CursorCell() {
    auto panel = g_state.panel.get();
    auto window = Window::Current();
    if (!panel || !window || !window.CoreWindow() || !window.Content()) {
        return std::nullopt;
    }
    auto interop = window.CoreWindow().try_as<ICoreWindowInterop>();
    HWND hwnd = nullptr;
    if (!interop || FAILED(interop->get_WindowHandle(&hwnd))) {
        return std::nullopt;
    }
    POINT pt;
    if (!hwnd || !GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt)) {
        return std::nullopt;
    }
    double scale = panel.XamlRoot() ? panel.XamlRoot().RasterizationScale() : 1.0;
    wf::Point client{float(pt.x / scale), float(pt.y / scale)};
    auto local = window.Content().TransformToVisual(panel).TransformPoint(client);
    return CellIfInside(local);
}

void Relayout() {
    if (auto gv = g_state.view.get()) {
        Layout(gv);
    }
}

template <typename F>
auto Guarded(F fn) {
    return [fn](auto&&... args) {
        try {
            fn(args...);
        } catch (...) {
            Log(L"Handler error 0x%08X", winrt::to_hresult());
        }
    };
}

void HookPanel(const GridView& gv, const Controls::Panel& panel) {
    g_state.panel = winrt::make_weak(panel);
    g_state.origPanelBackground = panel.ReadLocalValue(Controls::Panel::BackgroundProperty());
    g_state.origPanelMinHeight = panel.ReadLocalValue(FrameworkElement::MinHeightProperty());
    g_state.origPanelAllowDrop = panel.ReadLocalValue(UIElement::AllowDropProperty());
    // Transparent background makes empty cells hit-testable for drops.
    panel.Background(Media::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
    panel.AllowDrop(true);

    using DragArgs = DragEventArgs const&;

    // Drags from inside the grid: handled on the panel, before Start's own
    // drag code sees them, with native reorder off. Start computes drop
    // targets from its untransformed layout, so it can't be trusted with
    // drops onto folders either: folders are managed from the context menu.
    g_state.panelDragOverRevoker = panel.DragOver(
        winrt::auto_revoke, Guarded([](auto&&, DragArgs e) {
            auto p = g_state.panel.get();
            if (!p || !g_state.dragItem) {
                return;
            }
            Cell cell = CellAt(e.GetPosition(p));
            e.AcceptedOperation(
                winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Move);
            ShowHighlight(cell);
            e.Handled(true);
        }));
    g_state.panelDropRevoker = panel.Drop(
        winrt::auto_revoke, Guarded([](auto&&, DragArgs e) {
            auto p = g_state.panel.get();
            if (!p || !g_state.dragItem) {
                return;
            }
            HideHighlight();
            Cell cell = CellAt(e.GetPosition(p));
            std::wstring key = KeyOfItem(g_state.dragItem);
            g_state.ignoreOrderUntil = GetTickCount64() + 1500;
            if (!key.empty()) {
                MoveTo(key, cell);
            }
            e.Handled(true);
            Relayout();
        }));

    // Drags from outside the grid (All apps, a folder) stay Start's. Hooked
    // on Start's root with handledEventsToo: an open folder covers the grid,
    // so the drop lands on the folder overlay, not on the grid. The mod only
    // remembers the drop cell for the new tile.
    g_state.dragOverHandler = winrt::box_value(DragEventHandler(
        Guarded([](auto&&, DragArgs e) {
            if (g_state.dragItem) {
                return;
            }
            auto cell = CellIfInside(e);
            if (cell) {
                ShowHighlight(*cell);
            } else {
                HideHighlight();
            }
            if (cell && GetTickCount64() - g_state.externalLogTime > 1000) {
                g_state.externalLogTime = GetTickCount64();
                Log(L"External drag over row %d, column %d", cell->row, cell->col);
            }
        })));
    g_state.dragLeaveHandler = winrt::box_value(DragEventHandler(
        Guarded([](auto&&, DragArgs) { HideHighlight(); })));
    g_state.dropHandler = winrt::box_value(DragEventHandler(
        Guarded([](auto&&, DragArgs e) {
            if (g_state.dragItem) {
                return;
            }
            HideHighlight();
            if (auto cell = CellIfInside(e)) {
                g_state.pendingDrop = *cell;
                g_state.pendingDropTime = GetTickCount64();
                Log(L"External drop at row %d, column %d", cell->row, cell->col);
            } else {
                auto pt = g_state.panel.get() ? e.GetPosition(g_state.panel.get())
                                              : wf::Point{};
                Log(L"External drop outside the grid (%.0f, %.0f)", pt.X, pt.Y);
            }
        })));
    UIElement root = gv;
    if (auto xr = gv.XamlRoot()) {
        if (auto content = xr.Content()) {
            root = content;
        }
    }
    g_state.dragRoot = winrt::make_weak(root);
    root.AddHandler(UIElement::DragOverEvent(), g_state.dragOverHandler, true);
    root.AddHandler(UIElement::DragLeaveEvent(), g_state.dragLeaveHandler, true);
    root.AddHandler(UIElement::DropEvent(), g_state.dropHandler, true);

    g_state.dragStartRevoker = gv.DragItemsStarting(
        winrt::auto_revoke,
        Guarded([](auto&&, Controls::DragItemsStartingEventArgs const& a) {
            if (a.Items().Size() > 0) {
                g_state.dragItem = a.Items().GetAt(0);
                Log(L"Drag started: %s", KeyOfItem(g_state.dragItem).c_str());
                // Changing CanReorderItems inside DragItemsStarting cancels
                // the drag: switch it once the drag is running.
                if (auto dispatcher = GetDispatcher()) {
                    dispatcher.RunAsync(
                        wuc::CoreDispatcherPriority::Low, Guarded([] {
                            if (auto gv = g_state.view.get(); gv && g_state.dragItem) {
                                gv.CanReorderItems(false);
                                Log(L"Native reorder off for the drag");
                            }
                        }));
                }
                Relayout();  // spare row
            }
        }));
    g_state.dragEndRevoker = gv.DragItemsCompleted(
        winrt::auto_revoke,
        Guarded([](auto&&, Controls::DragItemsCompletedEventArgs const&) {
            g_state.dragItem = nullptr;
            g_state.ignoreOrderUntil = GetTickCount64() + 1500;
            if (auto gv = g_state.view.get()) {
                gv.CanReorderItems(g_state.origReorder);
            }
            HideHighlight();
            Relayout();
        }));

    // Remember the right-clicked tile, even when Start handles the event.
    g_state.rightTappedHandler = winrt::box_value(Input::RightTappedEventHandler(
        Guarded([](auto&&, Input::RightTappedRoutedEventArgs const& e) {
            DependencyObject node = e.OriginalSource().try_as<DependencyObject>();
            while (node && !node.try_as<GridViewItem>()) {
                node = Media::VisualTreeHelper::GetParent(node);
            }
            if (auto gv = g_state.view.get(); gv && node) {
                g_state.menuItem = gv.ItemFromContainer(node);
            }
        })));
    gv.AddHandler(UIElement::RightTappedEvent(), g_state.rightTappedHandler, true);
}

bool TryApply(const GridView& gv) {
    auto root = gv.ItemsPanelRoot();
    if (!root) {
        return false;  // template not applied yet
    }
    auto wrap = root.try_as<Controls::ItemsWrapGrid>();
    if (!wrap) {
        Log(L"Unexpected pinned panel %s, leaving Start as is",
            winrt::get_class_name(root).c_str());
        return true;  // give up, keep the stock layout
    }

    double w = wrap.ItemWidth();
    double h = wrap.ItemHeight();
    if (!(w > 0) || !(h > 0)) {
        if (auto first = gv.ContainerFromIndex(0).try_as<FrameworkElement>()) {
            auto m = first.Margin();
            w = first.ActualWidth() + m.Left + m.Right;
            h = first.ActualHeight() + m.Top + m.Bottom;
        }
    }
    if (w > 0 && h > 0) {
        g_state.cellW = w;
        g_state.cellH = h;
    }

    auto heightProp = FrameworkElement::HeightProperty();
    g_state.origViewHeight = gv.ReadLocalValue(heightProp);
    DependencyObject node = gv;
    for (int depth = 0; node && depth < 4; ++depth) {
        node = Media::VisualTreeHelper::GetParent(node);
        if (depth == 0) {
            if (auto p = node.try_as<Controls::Panel>()) {
                g_state.overlayParent = winrt::make_weak(p);
            }
        }
        if (node && winrt::get_class_name(node) == L"StartMenu.PinnedList") {
            auto host = node.as<FrameworkElement>();
            g_state.host = winrt::make_weak(host);
            g_state.origHostHeight = host.ReadLocalValue(heightProp);
            break;
        }
    }
    if (!g_state.host.get()) {
        Log(L"StartMenu.PinnedList host not found, extra rows may clip");
    }

    g_state.origReorder = gv.CanReorderItems();
    g_state.applied = true;
    // Native reorder stays on: Start's folder drops depend on it. The mod
    // takes over drops inside the grid that aren't onto a folder.
    // Start animates tiles between list slots; with cells that reads as
    // tiles flying in from the wrong place.
    g_state.origTransitions = gv.ReadLocalValue(
        Controls::ItemsControl::ItemContainerTransitionsProperty());
    gv.ItemContainerTransitions(Media::Animation::TransitionCollection{});
    HookPanel(gv, wrap);
    Log(L"Applied, cell %.0fx%.0f", g_state.cellW, g_state.cellH);
    return true;
}

void OnLayoutUpdated() {
    auto gv = g_state.view.get();
    if (!gv || g_unloading) {
        return;
    }
    try {
        if (!g_state.applied) {
            if (TryApply(gv) && !g_state.applied) {
                g_state.layoutRevoker.revoke();  // unexpected tree, stay out
            }
            return;
        }
        // Layout only writes what changed, so every pass is cheap.
        Layout(gv);
    } catch (...) {
        Log(L"Error 0x%08X, restoring stock layout", winrt::to_hresult());
        g_failed = true;
        Restore();
    }
}

void OnPinnedListAdded(InstanceHandle handle, const GridView& gv) {
    if (g_failed) {
        return;
    }
    Restore();
    g_state.handle = handle;
    g_state.view = winrt::make_weak(gv);
    g_state.layoutRevoker =
        gv.LayoutUpdated(winrt::auto_revoke, [](auto&&, auto&&) { OnLayoutUpdated(); });
    gv.InvalidateMeasure();
    Log(L"Pinned list found");
}

// ------------------------------------------------------- XAML diagnostics

winrt::com_ptr<IXamlDiagnostics> g_diag;
winrt::com_ptr<IVisualTreeService3> g_vts;
winrt::com_ptr<IVisualTreeServiceCallback2> g_callback;
std::atomic<bool> g_active{false};
HANDLE g_activeMutex = nullptr;  // exists while a copy is active
HANDLE g_adviseThread = nullptr;

// Runs fn on Start's UI thread and waits for it. False if it didn't finish
// in time.
template <typename F>
bool RunOnUi(F fn) {
    auto dispatcher = GetDispatcher();
    if (!dispatcher) {
        return true;
    }
    try {
        return dispatcher.RunAsync(wuc::CoreDispatcherPriority::Normal, fn)
                   .wait_for(std::chrono::seconds(5)) == wf::AsyncStatus::Completed;
    } catch (...) {
        Log(L"Dispatch failed 0x%08X", winrt::to_hresult());
        return false;
    }
}

void OnSettingsChanged() {
    LoadSettings();
    RunOnUi(Guarded([] {
        if (auto gv = g_state.view.get()) {
            gv.InvalidateMeasure();
        }
    }));
}

struct Callback : winrt::implements<Callback, IVisualTreeServiceCallback2,
                                     winrt::non_agile> {
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
        ParentChildRelation,
        VisualElement element,
        VisualMutationType mutation) override {
        if (g_unloading) {
            return S_OK;
        }
        try {
            if (mutation == Remove) {
                if (element.Handle == g_state.handle) {
                    g_state.layoutRevoker.revoke();
                    g_state = State{};
                }
                return S_OK;
            }
            if (!element.Type || !element.Name ||
                wcscmp(element.Type, kPinnedType) != 0 ||
                wcscmp(element.Name, kPinnedName) != 0) {
                return S_OK;
            }
            winrt::com_ptr<::IInspectable> insp;
            if (FAILED(g_diag->GetIInspectableFromHandle(element.Handle,
                                                          insp.put()))) {
                return S_OK;
            }
            if (auto gv = insp.try_as<GridView>()) {
                SetDispatcherIfNone(gv.Dispatcher());
                OnPinnedListAdded(element.Handle, gv);
            }
        } catch (...) {
            Log(L"Tree callback error 0x%08X", winrt::to_hresult());
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle,
                                                    VisualElementState,
                                                    LPCWSTR) override {
        return S_OK;
    }
};

DWORD WINAPI AdviseThread(void*) {
    auto cb = winrt::make_self<Callback>();
    HRESULT hr = g_vts->AdviseVisualTreeChange(cb.get());
    Log(L"AdviseVisualTreeChange: 0x%08X", hr);
    if (SUCCEEDED(hr)) {
        g_callback = cb;
    }
    return 0;
}

// Waits for the disable event, then puts Start back as it was.
DWORD WINAPI ControlThread(void*) {
    HANDLE ev[2] = {CreateEventW(nullptr, TRUE, FALSE, kDisableEvent),
                    CreateEventW(nullptr, FALSE, FALSE, kResetEvent)};
    while (ev[0] && ev[1]) {
        if (WaitForMultipleObjects(2, ev, FALSE, INFINITE) == WAIT_OBJECT_0) {
            break;
        }
        Log(L"Reset layout");
        RunOnUi(Guarded([] {
            g_cells.clear();
            g_state.known.clear();
            g_state.pendingDrop.reset();
            SaveCells();
            Relayout();
        }));
    }
    for (HANDLE h : ev) {
        if (h) {
            CloseHandle(h);
        }
    }
    g_unloading = true;
    if (g_adviseThread) {
        WaitForSingleObject(g_adviseThread, INFINITE);
        CloseHandle(g_adviseThread);
        g_adviseThread = nullptr;
    }
    if (!RunOnUi([] { Restore(); })) {
        Log(L"Restore didn't finish, waiting");
        if (!RunOnUi([] { Restore(); })) {
            Log(L"Restore still pending, giving up");
        }
    }
    if (g_vts && g_callback) {
        g_vts->UnadviseVisualTreeChange(g_callback.get());
    }
    g_callback = nullptr;
    StopSettingsWatcher();
    CloseHandle(g_activeMutex);
    g_activeMutex = nullptr;
    g_active = false;
    Log(L"Disabled");
    return 0;
}

struct Tap : winrt::implements<Tap, IObjectWithSite, winrt::non_agile> {
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override {
        if (!site) {
            return S_OK;
        }
        if (g_active.exchange(true)) {
            Log(L"Already active, ignoring SetSite");
            return S_OK;
        }
        // One copy of the dll at a time: a newer install replaces the old one
        // once that has disabled itself.
        g_activeMutex = CreateMutexW(nullptr, FALSE, kActiveMutex);
        if (!g_activeMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (g_activeMutex) {
                CloseHandle(g_activeMutex);
                g_activeMutex = nullptr;
            }
            g_active = false;
            Log(L"Another copy is active");
            return S_OK;
        }
        g_unloading = false;
        g_failed = false;
        g_vts = nullptr;
        g_diag = nullptr;
        site->QueryInterface(IID_PPV_ARGS(g_diag.put()));
        g_vts = g_diag ? g_diag.try_as<IVisualTreeService3>() : nullptr;
        if (!g_vts) {
            Log(L"No IVisualTreeService3");
            CloseHandle(g_activeMutex);
            g_activeMutex = nullptr;
            g_active = false;
            return S_OK;
        }
        LoadSettings();
        LoadCells();
        StartSettingsWatcher(OnSettingsChanged);
        // Advising from SetSite itself deadlocks.
        g_adviseThread = CreateThread(nullptr, 0, AdviseThread, nullptr, 0, nullptr);
        if (HANDLE t = CreateThread(nullptr, 0, ControlThread, nullptr, 0, nullptr)) {
            CloseHandle(t);
        }
        Log(L"Activated");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppv) override {
        return g_diag ? g_diag->QueryInterface(riid, ppv) : E_FAIL;
    }
};

struct TapFactory : winrt::implements<TapFactory, IClassFactory> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer,
                                             REFIID riid,
                                             void** ppv) override {
        *ppv = nullptr;
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }
        return winrt::make<Tap>().as<IUnknown>()->QueryInterface(riid, ppv);
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Pinned: XAML keeps a reference to the TAP after the tool is disabled.
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&DllMain), &self);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != CLSID_Tap) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return winrt::make<TapFactory>().as<IUnknown>()->QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() {
    return S_FALSE;
}
