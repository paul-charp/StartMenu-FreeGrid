// ==WindhawkMod==
// @id              start-xaml-dump
// @name            Start XAML dump (spike)
// @description     Dumps the Start menu XAML tree to a text file
// @version         0.1
// @author          paul-charp
// @include         StartMenuExperienceHost.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

// Spike tool: writes the live XAML tree of Start to
// %TEMP%\start-xaml-tree.txt as seen by StartMenuExperienceHost (its package
// AC\Temp folder). Open Start once after enabling so the pinned grid exists.

#include <xamlom.h>

#undef GetCurrentTime  // winbase.h macro breaks the XAML projection

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Data.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace winrt::Windows::UI::Xaml;
namespace wuc = winrt::Windows::UI::Core;

namespace {

// {3B0F6E2A-8C41-4D7E-9A55-1F2C7B9D4E61}
constexpr CLSID CLSID_Tap = {
    0x3b0f6e2a, 0x8c41, 0x4d7e, {0x9a, 0x55, 0x1f, 0x2c, 0x7b, 0x9d, 0x4e, 0x61}};

struct Node {
    InstanceHandle parent;
    unsigned int childIndex;
    std::wstring type;
    std::wstring name;
};

std::mutex g_lock;
std::unordered_map<InstanceHandle, Node> g_nodes;
std::unordered_map<InstanceHandle, winrt::weak_ref<DependencyObject>> g_watched;
std::unordered_map<InstanceHandle, std::wstring> g_extra;
std::atomic<bool> g_dirty{false};
std::atomic<bool> g_stop{false};

winrt::com_ptr<IXamlDiagnostics> g_diag;
winrt::com_ptr<IVisualTreeService3> g_vts;
winrt::com_ptr<IVisualTreeServiceCallback2> g_callback;
wuc::CoreDispatcher g_dispatcher{nullptr};
HANDLE g_initThread;
HANDLE g_dumpThread;

bool IsWatched(const std::wstring& type) {
    for (auto key : {L"GridView", L"ListView", L"ItemsControl",
                     L"ItemsRepeater", L"Pinned", L"Folder"}) {
        if (type.find(key) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

std::wstring ClassName(const winrt::Windows::Foundation::IInspectable& obj) {
    return obj ? std::wstring(winrt::get_class_name(obj)) : L"null";
}

// Runs on the UI thread.
std::wstring Describe(const DependencyObject& obj) {
    std::wstring s;
    if (auto fe = obj.try_as<FrameworkElement>()) {
        s += L"size=" + std::to_wstring(int(fe.ActualWidth())) + L"x" +
             std::to_wstring(int(fe.ActualHeight()));
    }
    if (auto ic = obj.try_as<Controls::ItemsControl>()) {
        auto items = ic.Items();
        s += L" items=" + std::to_wstring(items.Size());
        s += L" source=" + ClassName(ic.ItemsSource());
        s += L" panel=" + ClassName(ic.ItemsPanelRoot());
        if (items.Size() > 0) {
            s += L" item0=" + ClassName(items.GetAt(0));
        }
    }
    if (auto gvi = obj.try_as<Controls::GridViewItem>()) {
        s += L" aName=" +
             std::wstring(Automation::AutomationProperties::GetName(gvi));
        s += L" aId=" + std::wstring(
                            Automation::AutomationProperties::GetAutomationId(gvi));
        auto content = gvi.Content();
        s += L" content=" + ClassName(content);
        if (auto str = content.try_as<winrt::Windows::Foundation::IStringable>()) {
            s += L" str=" + std::wstring(str.ToString());
        }
        if (auto cpp = content.try_as<Data::ICustomPropertyProvider>()) {
            for (auto prop : {L"AppId", L"AppUserModelId", L"Aumid", L"Id",
                              L"DisplayName", L"Path", L"ItemType",
                              L"IsFolder"}) {
                if (auto p = cpp.GetCustomProperty(prop); p && p.CanRead()) {
                    auto v = p.GetValue(content);
                    auto pv = v.try_as<winrt::Windows::Foundation::IPropertyValue>();
                    s += L" " + std::wstring(prop) + L"=" +
                         (pv && pv.Type() ==
                                    winrt::Windows::Foundation::PropertyType::String
                              ? std::wstring(pv.GetString())
                              : ClassName(v));
                }
            }
        } else {
            s += L" (no ICustomPropertyProvider)";
        }
    }
    if (auto lv = obj.try_as<Controls::ListViewBase>()) {
        s += L" reorder=" + std::to_wstring(lv.CanReorderItems());
        s += L" drag=" + std::to_wstring(lv.CanDragItems());
        s += L" allowDrop=" + std::to_wstring(lv.AllowDrop());
    }
    return s;
}

struct Callback : winrt::implements<Callback, IVisualTreeServiceCallback2,
                                     winrt::non_agile> {
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
        ParentChildRelation relation,
        VisualElement element,
        VisualMutationType mutation) override {
        std::lock_guard lock(g_lock);
        if (mutation == Remove) {
            g_nodes.erase(element.Handle);
            g_watched.erase(element.Handle);
            g_dirty = true;
            return S_OK;
        }
        Node node{relation.Parent, relation.ChildIndex,
                  element.Type ? element.Type : L"",
                  element.Name ? element.Name : L""};
        if (IsWatched(node.type) && g_diag) {
            winrt::com_ptr<::IInspectable> insp;
            if (SUCCEEDED(g_diag->GetIInspectableFromHandle(element.Handle,
                                                             insp.put()))) {
                if (auto obj = insp.try_as<DependencyObject>()) {
                    g_watched[element.Handle] = winrt::make_weak(obj);
                    if (!g_dispatcher) {
                        g_dispatcher = obj.Dispatcher();
                    }
                }
            }
        }
        g_nodes[element.Handle] = std::move(node);
        g_dirty = true;
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
    Wh_Log(L"AdviseVisualTreeChange: 0x%08X", hr);
    if (SUCCEEDED(hr)) {
        g_callback = cb;
    }
    return 0;
}

struct Tap : winrt::implements<Tap, IObjectWithSite, winrt::non_agile> {
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override {
        if (!site) {
            return S_OK;
        }
        site->QueryInterface(IID_PPV_ARGS(g_diag.put()));
        g_vts = g_diag ? g_diag.try_as<IVisualTreeService3>() : nullptr;
        if (!g_vts) {
            Wh_Log(L"No IVisualTreeService3");
            return S_OK;
        }
        // Advising from SetSite itself deadlocks.
        HANDLE t = CreateThread(nullptr, 0, AdviseThread, nullptr, 0, nullptr);
        if (t) {
            CloseHandle(t);
        }
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

void RefreshExtra() {
    if (!g_dispatcher) {
        return;
    }
    try {
        g_dispatcher
            .RunAsync(wuc::CoreDispatcherPriority::Normal,
                      [] {
                          std::lock_guard lock(g_lock);
                          for (auto& [handle, weak] : g_watched) {
                              if (auto obj = weak.get()) {
                                  try {
                                      g_extra[handle] = Describe(obj);
                                  } catch (...) {
                                      g_extra[handle] = L"describe failed";
                                  }
                              }
                          }
                      })
            .get();
    } catch (...) {
        Wh_Log(L"RunAsync failed");
    }
}

std::wstring DumpPath() {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"start-xaml-tree.txt";
}

void WriteDump() {
    std::wstring out;
    {
        std::lock_guard lock(g_lock);
        std::unordered_map<InstanceHandle, std::vector<InstanceHandle>> children;
        std::vector<InstanceHandle> roots;
        for (auto& [handle, node] : g_nodes) {
            if (node.parent && g_nodes.count(node.parent)) {
                children[node.parent].push_back(handle);
            } else {
                roots.push_back(handle);
            }
        }
        for (auto& [parent, list] : children) {
            std::sort(list.begin(), list.end(), [](auto a, auto b) {
                return g_nodes[a].childIndex < g_nodes[b].childIndex;
            });
        }

        out = L"nodes=" + std::to_wstring(g_nodes.size()) + L"\n";
        std::vector<std::pair<InstanceHandle, int>> stack;
        for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
            stack.push_back({*it, 0});
        }
        while (!stack.empty()) {
            auto [handle, depth] = stack.back();
            stack.pop_back();
            const Node& node = g_nodes[handle];
            out.append(depth * 2, L' ');
            out += node.type;
            if (!node.name.empty()) {
                out += L" #" + node.name;
            }
            if (auto it = g_extra.find(handle); it != g_extra.end()) {
                out += L"  [" + it->second + L"]";
            }
            out += L"\n";
            auto& list = children[handle];
            for (auto it = list.rbegin(); it != list.rend(); ++it) {
                stack.push_back({*it, depth + 1});
            }
        }
    }

    int len = WideCharToMultiByte(CP_UTF8, 0, out.data(), int(out.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, out.data(), int(out.size()), utf8.data(),
                        len, nullptr, nullptr);

    std::wstring path = DumpPath();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Wh_Log(L"Cannot write %s: %u", path.c_str(), GetLastError());
        return;
    }
    DWORD written;
    WriteFile(f, utf8.data(), DWORD(utf8.size()), &written, nullptr);
    CloseHandle(f);
}

DWORD WINAPI DumpThread(void*) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    Wh_Log(L"Dump file: %s", DumpPath().c_str());
    while (!g_stop) {
        Sleep(2000);
        if (g_dirty.exchange(false)) {
            RefreshExtra();
            WriteDump();
        }
    }
    return 0;
}

DWORD WINAPI InitThread(void*) {
    using InitFn = HRESULT(WINAPI*)(PCWSTR, DWORD, PCWSTR, PCWSTR, CLSID,
                                    PCWSTR);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&InitThread), &self);
    wchar_t selfPath[MAX_PATH];
    GetModuleFileNameW(self, selfPath, MAX_PATH);

    for (int i = 0; i < 60 && !g_stop; ++i) {
        HMODULE xaml = GetModuleHandleW(L"Windows.UI.Xaml.dll");
        auto init = xaml ? reinterpret_cast<InitFn>(GetProcAddress(
                               xaml, "InitializeXamlDiagnosticsEx"))
                         : nullptr;
        if (init) {
            // ERROR_NOT_FOUND means "try the next connection name".
            HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            int n = 1;
            for (; n <= 10000 && hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
                 ++n) {
                wchar_t name[64];
                swprintf_s(name, L"VisualDiagConnection%d", n);
                hr = init(name, GetCurrentProcessId(), L"", selfPath,
                          CLSID_Tap, nullptr);
            }
            Wh_Log(L"InitializeXamlDiagnosticsEx: 0x%08X (connection %d)", hr,
                   n - 1);
            if (SUCCEEDED(hr)) {
                return 0;
            }
        }
        Sleep(1000);
    }
    return 0;
}

}  // namespace

extern "C" __declspec(dllexport) HRESULT STDAPICALLTYPE
DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != CLSID_Tap) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    return winrt::make<TapFactory>().as<IUnknown>()->QueryInterface(riid, ppv);
}

extern "C" __declspec(dllexport) HRESULT STDAPICALLTYPE DllCanUnloadNow() {
    return S_FALSE;
}

BOOL Wh_ModInit() {
    g_initThread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    g_dumpThread = CreateThread(nullptr, 0, DumpThread, nullptr, 0, nullptr);
    return TRUE;
}

void Wh_ModUninit() {
    g_stop = true;
    for (HANDLE t : {g_initThread, g_dumpThread}) {
        if (t) {
            WaitForSingleObject(t, 5000);
            CloseHandle(t);
        }
    }
    if (g_vts && g_callback) {
        g_vts->UnadviseVisualTreeChange(g_callback.get());
    }
    g_callback = nullptr;
}
