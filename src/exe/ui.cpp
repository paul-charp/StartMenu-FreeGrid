#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <unknwn.h>

#undef GetCurrentTime  // winbase.h macro breaks the XAML projection

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "../common/shared.h"
#include "app.h"

using namespace winrt::Windows::UI::Xaml;
namespace ctl = winrt::Windows::UI::Xaml::Controls;
namespace med = winrt::Windows::UI::Xaml::Media;
using winrt::Windows::UI::Color;
using winrt::Windows::UI::ViewManagement::UIColorType;

namespace {

constexpr wchar_t kTitle[] = L"StartMenu FreeGrid";
constexpr UINT WM_DONE = WM_APP + 1;   // enable / disable finished
constexpr UINT WM_THEME = WM_APP + 2;  // system colors changed
constexpr UINT WM_RESET = WM_APP + 3;  // reset request done
constexpr int kWidth = 340;            // client size, DIPs
constexpr int kHeight = 64;
// Mica behind a transparent island. If the island paints black, set false
// for the solid fallback colours.
constexpr bool kMica = false;  // the system XAML island paints opaque over Mica
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

struct Ui {
    HWND hwnd = nullptr;
    HWND island = nullptr;
    winrt::com_ptr<IDesktopWindowXamlSourceNative2> native;
    Hosting::DesktopWindowXamlSource source{nullptr};
    ctl::Grid root{nullptr};
    ctl::Border card{nullptr};
    ctl::Grid normal{nullptr};
    ctl::Grid confirm{nullptr};
    ctl::TextBlock confirmText{nullptr};
    ctl::Button resetBtn{nullptr};
    ctl::Button cancelBtn{nullptr};
    Shapes::Ellipse dot{nullptr};
    ctl::TextBlock status{nullptr};
    ctl::ToggleSwitch toggle{nullptr};
    std::vector<ctl::Button> buttons;
    winrt::Windows::UI::ViewManagement::UISettings settings{nullptr};
    HBRUSH bgBrush = nullptr;
    bool busy = false;
    bool syncing = false;
    bool failed = false;
    bool confirming = false;
    bool dark = true;
    bool loaded = false;  // root Loaded fired: templates exist
};

Ui g;

void Open(const std::wstring& path) {
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

Color Rgb(uint32_t argb) {
    return {uint8_t(argb >> 24), uint8_t(argb >> 16), uint8_t(argb >> 8), uint8_t(argb)};
}

COLORREF ToRef(Color c) { return RGB(c.R, c.G, c.B); }

// Startup timing: "t+<ms since process start> <phase>" in log.txt.
void Trace(const wchar_t* phase) {
    FILETIME created, exited, kernel, user, now;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimeAsFileTime(&now);
    auto ticks = [](FILETIME f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    AppendLog(L"ui", L"t+" + std::to_wstring((ticks(now) - ticks(created)) / 10000) +
                         L" " + phase);
}

// Windows 11 control accent.
Color Accent() {
    return g.settings.GetColorValue(g.dark ? UIColorType::AccentLight2
                                           : UIColorType::AccentDark1);
}

// ------------------------------------------------------------------- state

void SetResource(const ctl::Button& b, const wchar_t* key, Color c) {
    b.Resources().Insert(winrt::box_value(key), med::SolidColorBrush(c));
}

// Window chrome: needs no XAML, so it can run before the island exists.
void ApplyWindowTheme() {
    auto bg = g.settings.GetColorValue(UIColorType::Background);
    g.dark = int(bg.R) + bg.G + bg.B < 3 * 128;
    Color window = Rgb(g.dark ? 0xFF202020 : 0xFFF3F3F3);
    BOOL immersive = g.dark;
    DwmSetWindowAttribute(g.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &immersive,
                          sizeof(immersive));
    int corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(g.hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners,
                          sizeof(corners));
    if (kMica) {
        int backdrop = DWMSBT_MAINWINDOW;
        DwmSetWindowAttribute(g.hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop,
                              sizeof(backdrop));
        MARGINS all{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(g.hwnd, &all);
    } else {
        COLORREF caption = ToRef(window);
        COLORREF captionText = ToRef(Rgb(g.dark ? 0xFFFFFFFF : 0xFF1A1A1A));
        DwmSetWindowAttribute(g.hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        DwmSetWindowAttribute(g.hwnd, DWMWA_TEXT_COLOR, &captionText, sizeof(captionText));
        if (g.bgBrush) {
            DeleteObject(g.bgBrush);
        }
        g.bgBrush = CreateSolidBrush(ToRef(window));
    }
    InvalidateRect(g.hwnd, nullptr, TRUE);
}

void SetToggleResources(Color accent) {
    Color knob = Rgb(g.dark ? 0xFF000000 : 0xFFFFFFFF);
    auto res = g.toggle.Resources();
    for (const wchar_t* key : {L"ToggleSwitchFillOn", L"ToggleSwitchFillOnPointerOver",
                               L"ToggleSwitchFillOnPressed", L"ToggleSwitchStrokeOn",
                               L"ToggleSwitchStrokeOnPointerOver",
                               L"ToggleSwitchStrokeOnPressed"}) {
        res.Insert(winrt::box_value(key), med::SolidColorBrush(accent));
    }
    for (const wchar_t* key : {L"ToggleSwitchKnobFillOn", L"ToggleSwitchKnobFillOnPointerOver",
                               L"ToggleSwitchKnobFillOnPressed"}) {
        res.Insert(winrt::box_value(key), med::SolidColorBrush(knob));
    }
}

// Resources changed on a live control: flip its theme to re-template it.
void Retemplate(const FrameworkElement& e) {
    if (!g.loaded) {
        return;  // the first template pass reads the resources anyway
    }
    e.RequestedTheme(g.dark ? ElementTheme::Light : ElementTheme::Dark);
    e.RequestedTheme(g.dark ? ElementTheme::Dark : ElementTheme::Light);
}

void ApplyTheme() {
    ApplyWindowTheme();
    Trace(L"theme: window");
    bool dark = g.dark;
    Color text = Rgb(dark ? 0xFFFFFFFF : 0xFF1A1A1A);
    Color clear = Rgb(0x00000000);
    for (auto& b : g.buttons) {
        SetResource(b, L"ButtonBackground", clear);
        SetResource(b, L"ButtonBorderBrush", clear);
        SetResource(b, L"ButtonBackgroundPointerOver", Rgb(dark ? 0x0FFFFFFF : 0x0A000000));
        SetResource(b, L"ButtonBackgroundPressed", Rgb(dark ? 0x0AFFFFFF : 0x06000000));
        SetResource(b, L"ButtonBorderBrushPointerOver", clear);
        SetResource(b, L"ButtonBorderBrushPressed", clear);
    }
    Trace(L"theme: icon buttons");
    // Accent: toggle and the inline confirm button.
    Color accent = Accent();
    Color onAccent = Rgb(dark ? 0xFF000000 : 0xFFFFFFFF);
    for (const wchar_t* key : {L"ButtonBackground", L"ButtonBackgroundPointerOver",
                               L"ButtonBackgroundPressed"}) {
        SetResource(g.resetBtn, key, accent);
    }
    for (const wchar_t* key : {L"ButtonForeground", L"ButtonForegroundPointerOver",
                               L"ButtonForegroundPressed"}) {
        SetResource(g.resetBtn, key, onAccent);
    }
    SetResource(g.resetBtn, L"ButtonBorderBrush", clear);
    SetResource(g.resetBtn, L"ButtonBorderBrushPointerOver", clear);
    SetResource(g.resetBtn, L"ButtonBorderBrushPressed", clear);
    Trace(L"theme: reset button");
    SetToggleResources(accent);
    Trace(L"theme: toggle resources");
    g.root.RequestedTheme(dark ? ElementTheme::Dark : ElementTheme::Light);
    Trace(L"theme: root theme");
    Retemplate(g.toggle);
    Retemplate(g.resetBtn);
    Trace(L"theme: retemplate");
    if (kMica) {
        g.root.Background(med::SolidColorBrush(clear));
        g.card.Background(med::SolidColorBrush(Rgb(dark ? 0x0DFFFFFF : 0xB3FFFFFF)));
        g.card.BorderBrush(med::SolidColorBrush(Rgb(dark ? 0x19000000 : 0x0F000000)));
    } else {
        g.root.Background(med::SolidColorBrush(Rgb(dark ? 0xFF202020 : 0xFFF3F3F3)));
        g.card.Background(med::SolidColorBrush(Rgb(dark ? 0xFF2B2B2B : 0xFFFBFBFB)));
        g.card.BorderBrush(med::SolidColorBrush(Rgb(dark ? 0xFF1D1D1D : 0xFFE5E5E5)));
    }
    Trace(L"theme: brushes");
    g.status.Foreground(med::SolidColorBrush(text));
    g.confirmText.Foreground(med::SolidColorBrush(text));
    Trace(L"theme: done");
}

void Refresh() {
    if (g.confirming) {
        return;
    }
    bool active = ActiveRunning();
    g.status.Text(g.failed ? L"Error"
                  : active ? L"Active"
                  : WatcherRunning() ? L"Waiting"
                                     : L"Off");
    Color color = active ? Accent() : Rgb(0xFF8A8A8A);
    g.dot.Fill(med::SolidColorBrush(color));
    if (!g.busy) {
        g.syncing = true;
        g.toggle.IsOn(RunValueSet());
        g.syncing = false;
    }
}

void OnToggled() {
    bool on = g.toggle.IsOn();
    // Toggled also fires when the switch loads or is synced: act on real changes.
    if (g.syncing || g.busy || on == RunValueSet()) {
        return;
    }
    g.busy = true;
    g.failed = false;
    g.toggle.IsEnabled(false);
    HWND hwnd = g.hwnd;
    std::thread([on, hwnd] {
        bool ok = true;
        if (on) {
            std::wstring err;
            ok = DoEnable(err);
            if (!ok) {
                AppendLog(L"exe", err);
            }
        } else {
            DoDisable(false);
        }
        PostMessageW(hwnd, WM_DONE, ok, 0);
    }).detach();
}

// -------------------------------------------------------------------- build

ctl::Button IconButton(const wchar_t* glyph, const wchar_t* tip) {
    ctl::FontIcon icon;
    icon.Glyph(glyph);
    icon.FontSize(16);
    icon.FontFamily(med::FontFamily(L"Segoe Fluent Icons, Segoe MDL2 Assets"));
    ctl::Button b;
    b.Content(icon);
    b.Width(28);
    b.Height(28);
    b.MinWidth(0);
    b.MinHeight(0);
    b.Padding({0, 0, 0, 0});
    b.CornerRadius({4, 4, 4, 4});
    b.BorderThickness({0, 0, 0, 0});
    b.Background(med::SolidColorBrush(Rgb(0x00000000)));
    b.UseSystemFocusVisuals(true);
    ctl::ToolTipService::SetToolTip(b, winrt::box_value(tip));
    g.buttons.push_back(b);
    return b;
}

void ShowConfirm(bool on) {
    g.confirming = on;
    g.normal.Visibility(on ? Visibility::Collapsed : Visibility::Visible);
    g.confirm.Visibility(on ? Visibility::Visible : Visibility::Collapsed);
    if (on) {
        g.cancelBtn.Focus(FocusState::Programmatic);
    } else {
        Refresh();
        g.toggle.Focus(FocusState::Programmatic);
    }
}

void DoConfirmedReset() {
    g.resetBtn.IsEnabled(false);
    g.cancelBtn.IsEnabled(false);
    HWND hwnd = g.hwnd;
    std::thread([hwnd] {
        DoReset();
        PostMessageW(hwnd, WM_RESET, 0, 0);
    }).detach();
}

ctl::Button TextButton(const wchar_t* text) {
    ctl::Button b;
    b.Content(winrt::box_value(text));
    b.Height(28);
    b.MinWidth(0);
    b.Padding({12, 0, 12, 0});
    b.CornerRadius({4, 4, 4, 4});
    b.BorderThickness({0, 0, 0, 0});
    b.VerticalAlignment(VerticalAlignment::Center);
    return b;
}

void FitToggle();

void BuildUi() {
    g.dot = Shapes::Ellipse();
    g.dot.Width(8);
    g.dot.Height(8);
    g.dot.VerticalAlignment(VerticalAlignment::Center);
    g.dot.Margin({0, 0, 6, 0});
    g.status = ctl::TextBlock();
    g.status.FontSize(14);
    g.status.FontFamily(med::FontFamily(L"Segoe UI Variable Text, Segoe UI"));
    g.status.VerticalAlignment(VerticalAlignment::Center);

    auto reset = IconButton(L"", L"Reset layout");
    reset.Click([](auto&&, auto&&) { ShowConfirm(true); });
    reset.Margin({0, 0, 4, 0});
    auto log = IconButton(L"", L"Log");
    log.Click([](auto&&, auto&&) { Open(DataDir() + L"log.txt"); });
    log.Margin({0, 0, 4, 0});
    auto folder = IconButton(L"", L"Folder");
    folder.Click([](auto&&, auto&&) { Open(DataDir()); });
    ctl::StackPanel buttons;
    buttons.Orientation(ctl::Orientation::Horizontal);
    buttons.VerticalAlignment(VerticalAlignment::Center);
    buttons.Margin({0, 0, 10, 0});
    buttons.Children().Append(reset);
    buttons.Children().Append(log);
    buttons.Children().Append(folder);

    g.toggle = ctl::ToggleSwitch();
    g.toggle.OnContent(nullptr);
    g.toggle.OffContent(nullptr);
    g.toggle.MinWidth(0);
    g.toggle.HorizontalAlignment(HorizontalAlignment::Right);
    g.toggle.VerticalAlignment(VerticalAlignment::Center);
    g.toggle.Toggled([](auto&&, auto&&) { OnToggled(); });

    // dot | status | spacer | buttons | toggle
    ctl::Grid row;
    GridUnitType kinds[] = {GridUnitType::Auto, GridUnitType::Auto, GridUnitType::Star,
                            GridUnitType::Auto, GridUnitType::Auto};
    for (auto kind : kinds) {
        ctl::ColumnDefinition c;
        c.Width({1, kind});
        row.ColumnDefinitions().Append(c);
    }
    ctl::Grid::SetColumn(g.status, 1);
    ctl::Grid::SetColumn(buttons, 3);
    ctl::Grid::SetColumn(g.toggle, 4);
    row.Children().Append(g.dot);
    row.Children().Append(g.status);
    row.Children().Append(buttons);
    row.Children().Append(g.toggle);

    row.HorizontalAlignment(HorizontalAlignment::Stretch);
    g.normal = row;

    // Inline confirm: label | Reset, Cancel
    g.confirmText = ctl::TextBlock();
    g.confirmText.Text(L"Reset layout?");
    g.confirmText.FontSize(14);
    g.confirmText.FontFamily(med::FontFamily(L"Segoe UI Variable Text, Segoe UI"));
    g.confirmText.VerticalAlignment(VerticalAlignment::Center);
    g.resetBtn = TextButton(L"Reset");
    g.resetBtn.Margin({0, 0, 4, 0});
    g.resetBtn.Click([](auto&&, auto&&) { DoConfirmedReset(); });
    g.cancelBtn = TextButton(L"Cancel");
    g.cancelBtn.Background(med::SolidColorBrush(Rgb(0x00000000)));
    g.cancelBtn.Click([](auto&&, auto&&) { ShowConfirm(false); });
    g.buttons.push_back(g.cancelBtn);
    ctl::StackPanel confirmButtons;
    confirmButtons.Orientation(ctl::Orientation::Horizontal);
    confirmButtons.VerticalAlignment(VerticalAlignment::Center);
    confirmButtons.Children().Append(g.resetBtn);
    confirmButtons.Children().Append(g.cancelBtn);
    g.confirm = ctl::Grid();
    g.confirm.Visibility(Visibility::Collapsed);
    for (auto kind : {GridUnitType::Star, GridUnitType::Auto}) {
        ctl::ColumnDefinition c;
        c.Width({1, kind});
        g.confirm.ColumnDefinitions().Append(c);
    }
    ctl::Grid::SetColumn(confirmButtons, 1);
    g.confirm.Children().Append(g.confirmText);
    g.confirm.Children().Append(confirmButtons);

    ctl::Grid content;
    content.Children().Append(g.normal);
    content.Children().Append(g.confirm);

    g.card = ctl::Border();
    g.card.Height(48);
    g.card.VerticalAlignment(VerticalAlignment::Top);
    g.card.Margin({12, 4, 12, 12});
    g.card.CornerRadius({6, 6, 6, 6});
    g.card.BorderThickness({1, 1, 1, 1});
    g.card.Padding({14, 0, 12, 0});
    g.card.Child(content);

    g.root = ctl::Grid();
    g.root.Children().Append(g.card);
    g.root.KeyDown([](auto&&, Input::KeyRoutedEventArgs const& a) {
        if (g.confirming && a.Key() == winrt::Windows::System::VirtualKey::Escape) {
            ShowConfirm(false);
            a.Handled(true);
        }
    });
    // Focus the toggle (programmatic: no focus rectangle) so XAML doesn't pick a button.
    g.root.Loaded([](auto&&, auto&&) {
        g.loaded = true;
        Trace(L"root loaded");
        FitToggle();
        g.toggle.Focus(FocusState::Programmatic);
    });
}

// The ToggleSwitch template keeps an On/Off content column even when empty.
// Pull it in with a negative right margin, measured from the drawn track.
FrameworkElement FindNamed(const DependencyObject& node, std::wstring_view name) {
    int n = med::VisualTreeHelper::GetChildrenCount(node);
    for (int i = 0; i < n; ++i) {
        auto child = med::VisualTreeHelper::GetChild(node, i);
        if (auto fe = child.try_as<FrameworkElement>(); fe && fe.Name() == name) {
            return fe;
        }
        if (auto found = FindNamed(child, name)) {
            return found;
        }
    }
    return nullptr;
}

void FitToggle() {
    g.toggle.UpdateLayout();
    auto track = FindNamed(g.toggle, L"OuterBorder");
    if (!track) {
        Trace(L"toggle: no OuterBorder");
        return;
    }
    auto right = track.TransformToVisual(g.toggle).TransformPoint({0, 0}).X +
                 float(track.ActualWidth());
    double extra = g.toggle.ActualWidth() - right;
    if (extra > 0) {
        g.toggle.Margin({0, 0, -extra, 0});
    }
    Trace(L"toggle fitted");
}

// ------------------------------------------------------------------ window

void Fit() {
    UINT dpi = GetDpiForWindow(g.hwnd);
    RECT r{0, 0, MulDiv(kWidth, dpi, 96), MulDiv(kHeight, dpi, 96)};
    AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, dpi);
    SetWindowPos(g.hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void LayoutIsland() {
    RECT r;
    GetClientRect(g.hwnd, &r);
    SetWindowPos(g.island, nullptr, 0, 0, r.right, r.bottom, SWP_SHOWWINDOW | SWP_NOZORDER);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (g.island) {
                SetWindowPos(g.island, nullptr, 0, 0, LOWORD(lp), HIWORD(lp),
                             SWP_SHOWWINDOW | SWP_NOZORDER);
            }
            return 0;
        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SETFOCUS:
            if (g.island) {
                SetFocus(g.island);
            }
            return 0;
        case WM_ERASEBKGND: {
            if (kMica) {
                return 1;  // the backdrop shows through
            }
            RECT r;
            GetClientRect(hwnd, &r);
            FillRect(reinterpret_cast<HDC>(wp), &r,
                     g.bgBrush ? g.bgBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            return 1;
        }
        case WM_DONE:
            g.failed = !wp;
            g.busy = false;
            g.toggle.IsEnabled(true);
            Refresh();
            return 0;
        case WM_RESET:
            g.resetBtn.IsEnabled(true);
            g.cancelBtn.IsEnabled(true);
            ShowConfirm(false);
            return 0;
        case WM_THEME:
            ApplyTheme();
            Refresh();
            return 0;
        case WM_CLOSE:
            Trace(L"close");
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int RunWindow() {
    Trace(L"start");
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kUiMutex);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(kWindowClass, nullptr)) {
            if (IsIconic(w)) {
                ShowWindow(w, SW_RESTORE);
            }
            SetForegroundWindow(w);
        }
        return 0;
    }
    Trace(L"mutex");

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    g.settings = winrt::Windows::UI::ViewManagement::UISettings();
    Trace(L"init_apartment");

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    // No redirection bitmap: DWM composes the island directly over Mica,
    // otherwise the client area paints opaque black.
    g.hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kWindowClass, kTitle, kStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                             kWidth, kHeight, nullptr, nullptr, inst, nullptr);
    if (!g.hwnd) {
        return 1;
    }
    // Show first, build after: the XAML parts are the slow ones.
    ApplyWindowTheme();
    Fit();
    ShowWindow(g.hwnd, SW_SHOW);
    UpdateWindow(g.hwnd);
    Trace(L"ShowWindow");

    auto manager = Hosting::WindowsXamlManager::InitializeForCurrentThread();
    Trace(L"WindowsXamlManager");
    g.source = Hosting::DesktopWindowXamlSource();
    g.native = g.source.as<IDesktopWindowXamlSourceNative2>();
    g.native->AttachToWindow(g.hwnd);
    g.native->get_WindowHandle(&g.island);
    LayoutIsland();
    Trace(L"source attached");
    BuildUi();
    g.source.Content(g.root);
    Trace(L"content built");
    ApplyTheme();
    Refresh();
    Trace(L"first Refresh");
    g.settings.ColorValuesChanged(
        [hwnd = g.hwnd](auto&&, auto&&) { PostMessageW(hwnd, WM_THEME, 0, 0); });

    DispatcherTimer timer;
    timer.Interval(std::chrono::seconds(1));
    timer.Tick([](auto&&, auto&&) { Refresh(); });
    timer.Start();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        BOOL handled = FALSE;
        g.native->PreTranslateMessage(&msg, &handled);
        if (!handled) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    Trace(L"loop ended");
    // Teardown has hung before (process left running without a window): don't wait for it.
    std::thread([] {
        Sleep(3000);
        Trace(L"exit forced");
        ExitProcess(0);
    }).detach();
    // No XAML object may outlive the manager.
    timer.Stop();
    timer = nullptr;
    g.buttons.clear();
    g.root = nullptr;
    g.card = nullptr;
    g.normal = nullptr;
    g.confirm = nullptr;
    g.confirmText = nullptr;
    g.resetBtn = nullptr;
    g.cancelBtn = nullptr;
    g.dot = nullptr;
    g.status = nullptr;
    g.toggle = nullptr;
    g.settings = nullptr;
    g.source.Close();
    g.source = nullptr;
    g.native = nullptr;
    manager.Close();
    if (g.bgBrush) {
        DeleteObject(g.bgBrush);
    }
    Trace(L"teardown done");
    Trace(L"exit");
    ExitProcess(int(msg.wParam));
}
