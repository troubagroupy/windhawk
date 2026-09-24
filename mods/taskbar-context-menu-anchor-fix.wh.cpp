// ==WindhawkMod==
// @id              taskbar-context-menu-anchor-fix
// @name            Taskbar context menu anchor fix
// @description     Repositions taskbar tray icon context menus (e.g. Notification/Action Center) to open near the click point instead of a wrong fixed position
// @version         8.5.0
// @author          kuba
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar context menu anchor fix

EXPERIMENTAL. Fixes the right-click context menu on certain taskbar tray
icons (for example the Notification Center / clock area, "Adjust date and
time" / "Notification settings") appearing far away from where you actually
clicked.

## How it works (v8.3)

The taskbar's tray context menus are XAML `MenuFlyout`s. Earlier versions
moved the finished popup window (`Xaml_WindowedPopupClass`) with
`SetWindowPos`. That put the menu in the right place visually, but XAML
still thought the popup was at its original position, so mouse input was
hit-tested against the wrong place: the menu items didn't highlight on
hover and couldn't be clicked.

This version instead opens the tray icons' menus itself (by hooking their
`ShowContextMenu`, the same way the "Taskbar on top" mod does), and also
hooks the taskbar's `MenuFlyout::ShowAt` calls, and in both cases sets the
placement it asks XAML for. Menus opened in other ways (for example the
battery, network and volume icons' menus) are caught by a hook on the XAML
`MenuFlyout::ShowAt` implementation itself. The menu is centered
horizontally on the click and opens just above the taskbar's top edge, with
a configurable gap. XAML positions the popup itself, so the position and the input stay in
sync.

By default this applies to all the taskbar's XAML menus, including the
taskbar's own right-click menu. The "Zone width" setting can limit it to
clicks near the taskbar's right edge (system tray, clock,
notification/action center).

The default gap above the taskbar (12 logical pixels) matches the Start
menu's.

## If something looks wrong

Turn on logging for this mod and reproduce the issue; the log shows the
decision made for every menu flyout the taskbar opens.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- zoneWidth: 0
  $name: Zone width (px)
  $description: >-
    Only menus opened by clicks within this many pixels of the taskbar's right
    edge are repositioned (system tray, clock, notification/action center).
    0 means the whole taskbar, including the taskbar's own right-click menu.
- menuGap: 12
  $name: Gap above taskbar
  $description: >-
    Vertical gap between the bottom of the repositioned menu and the top
    edge of the taskbar, in logical pixels (scaled with the display scale).
    12 matches the gap of the Start menu.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <atomic>
#include <memory>

using namespace winrt::Windows::UI::Xaml;

struct {
    int zoneWidth;
    int menuGap;
} g_settings;

bool IsTaskbarWindow(HWND hWnd) {
    WCHAR szClassName[32];
    if (!GetClassName(hWnd, szClassName, ARRAYSIZE(szClassName))) {
        return false;
    }

    return _wcsicmp(szClassName, L"Shell_TrayWnd") == 0 ||
           _wcsicmp(szClassName, L"Shell_SecondaryTrayWnd") == 0;
}

constexpr WCHAR kContentBridgeClassName[] =
    L"Windows.UI.Composition.DesktopWindowContentBridge";

// Where a menu should end up, in screen pixels, and what's needed to convert
// XAML coordinates of the taskbar's island to screen pixels.
struct MenuAnchor {
    POINT islandOrigin;
    double scale;
    int anchorX;  // The menu's horizontal center.
    int anchorY;  // The menu's bottom edge.
    RECT workArea;
};

// Adjusts the flyout show options so that the menu opens centered on the
// click, just above the taskbar. Returns false if the menu should be left
// untouched.
bool AdjustShowOptions(DependencyObject* placementTarget,
                       Controls::Primitives::FlyoutShowOptions* showOptions,
                       MenuAnchor* menuAnchor = nullptr) {
    POINT pt;
    if (!GetCursorPos(&pt)) {
        return false;
    }

    HWND hTaskbarWnd = GetAncestor(WindowFromPoint(pt), GA_ROOT);
    if (!hTaskbarWnd || !IsTaskbarWindow(hTaskbarWnd)) {
        Wh_Log(L"Cursor (%d,%d) isn't over a taskbar, skipping", pt.x, pt.y);
        return false;
    }

    RECT taskbarRc{};
    if (!GetWindowRect(hTaskbarWnd, &taskbarRc)) {
        return false;
    }

    bool inZone = g_settings.zoneWidth <= 0 ||
                  pt.x >= taskbarRc.right - g_settings.zoneWidth;

    Wh_Log(L"Menu flyout, click at (%d,%d), taskbar (%d,%d)-(%d,%d), "
           L"inZone=%d",
           pt.x, pt.y, (int)taskbarRc.left, (int)taskbarRc.top,
           (int)taskbarRc.right, (int)taskbarRc.bottom, inZone);

    if (!inZone) {
        return false;
    }

    UIElement targetElement = placementTarget->try_as<UIElement>();
    if (!targetElement) {
        Wh_Log(L"Placement target isn't a UIElement");
        return false;
    }

    auto xamlRoot = targetElement.XamlRoot();
    if (!xamlRoot) {
        Wh_Log(L"No XamlRoot");
        return false;
    }

    double scale = xamlRoot.RasterizationScale();
    if (scale <= 0) {
        scale = 1;
    }

    // The XAML island's origin on screen. XAML coordinates relative to the
    // root are relative to this window.
    POINT islandOrigin{};
    HWND hBridgeWnd =
        FindWindowEx(hTaskbarWnd, nullptr, kContentBridgeClassName, nullptr);
    RECT bridgeRc{};
    if (hBridgeWnd && GetWindowRect(hBridgeWnd, &bridgeRc)) {
        islandOrigin = {bridgeRc.left, bridgeRc.top};
    } else {
        Wh_Log(L"No DesktopWindowContentBridge, using the taskbar client area");
        ClientToScreen(hTaskbarWnd, &islandOrigin);
    }

    // The target's top-left corner, relative to the island, in DIPs.
    winrt::Windows::Foundation::Point targetOrigin =
        targetElement.TransformToVisual(nullptr).TransformPoint(
            winrt::Windows::Foundation::Point{0, 0});

    // The anchor point in screen pixels: horizontally at the click,
    // vertically at the taskbar's top edge minus the gap. With the Top
    // placement, XAML opens the menu above this point, centered on it.
    int anchorX = pt.x;
    int anchorY = taskbarRc.top - (int)(g_settings.menuGap * scale + 0.5);

    winrt::Windows::Foundation::Point position{
        static_cast<float>((anchorX - islandOrigin.x) / scale -
                           targetOrigin.X),
        static_cast<float>((anchorY - islandOrigin.y) / scale -
                           targetOrigin.Y),
    };

    Wh_Log(L"Island origin (%d,%d), scale %f, target %s origin (%f,%f), "
           L"placement %d -> Top, show mode %d, exclusion rect %d, "
           L"position (%f,%f)",
           islandOrigin.x, islandOrigin.y, scale,
           winrt::get_class_name(targetElement).c_str(), targetOrigin.X,
           targetOrigin.Y, (int)showOptions->Placement(),
           (int)showOptions->ShowMode(), !!showOptions->ExclusionRect(),
           position.X, position.Y);

    showOptions->Placement(Controls::Primitives::FlyoutPlacementMode::Top);
    showOptions->Position(position);
    // Explicitly request the standard (non-transient) mode that the
    // taskbar's own context menus use.
    showOptions->ShowMode(Controls::Primitives::FlyoutShowMode::Standard);
    // Some menus (e.g. the battery/network/volume icons') come with an
    // exclusion rect covering their button, which XAML keeps the menu clear
    // of - that would push it away from the position above.
    showOptions->ExclusionRect(nullptr);

    if (menuAnchor) {
        menuAnchor->islandOrigin = islandOrigin;
        menuAnchor->scale = scale;
        menuAnchor->anchorX = anchorX;
        menuAnchor->anchorY = anchorY;
        MONITORINFO monitorInfo{
            .cbSize = sizeof(MONITORINFO),
        };
        GetMonitorInfo(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST),
                       &monitorInfo);
        menuAnchor->workArea = monitorInfo.rcWork;
    }

    return true;
}

bool IsSameObject(winrt::Windows::Foundation::IUnknown const& a,
                  winrt::Windows::Foundation::IUnknown const& b) {
    return a && b &&
           winrt::get_abi(a.as<winrt::Windows::Foundation::IUnknown>()) ==
               winrt::get_abi(b.as<winrt::Windows::Foundation::IUnknown>());
}

// Some menus (e.g. the battery icon's) are moved by the taskbar after they
// open. Once a menu is open, measure where it actually is and move it to
// where it should be. The popup's offsets are changed, not its window, so
// that XAML's input handling stays in sync.
void CorrectOpenMenuPosition(Controls::Primitives::FlyoutBase const& flyout,
                             MenuAnchor const& a) {
    auto menuFlyout = flyout.try_as<Controls::MenuFlyout>();
    auto xamlRoot = flyout.XamlRoot();
    if (!menuFlyout || !xamlRoot || menuFlyout.Items().Size() == 0) {
        return;
    }

    auto rootContent = xamlRoot.Content();
    auto firstItem = menuFlyout.Items().GetAt(0);

    for (auto const& popup :
         Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(xamlRoot)) {
        auto presenter =
            popup.Child().try_as<Controls::MenuFlyoutPresenter>();
        if (!presenter || presenter.Items().Size() == 0 ||
            !IsSameObject(presenter.Items().GetAt(0), firstItem)) {
            continue;
        }

        presenter.UpdateLayout();

        auto origin =
            presenter.TransformToVisual(rootContent)
                .TransformPoint(winrt::Windows::Foundation::Point{0, 0});
        double width = presenter.ActualWidth() * a.scale;
        double height = presenter.ActualHeight() * a.scale;
        if (width <= 0 || height <= 0) {
            continue;
        }

        double left = a.islandOrigin.x + origin.X * a.scale;
        double top = a.islandOrigin.y + origin.Y * a.scale;

        double newLeft = a.anchorX - width / 2;
        double newTop = a.anchorY - height;

        if (newLeft > a.workArea.right - width) {
            newLeft = a.workArea.right - width;
        }
        if (newLeft < a.workArea.left) {
            newLeft = a.workArea.left;
        }
        if (newTop < a.workArea.top) {
            newTop = a.workArea.top;
        }

        double dx = (newLeft - left) / a.scale;
        double dy = (newTop - top) / a.scale;

        Wh_Log(L"Open menu at (%f,%f) size %fx%f, moving by (%f,%f) DIPs, "
               L"animations enabled: %d",
               left, top, width, height, dx, dy,
               flyout.AreOpenCloseAnimationsEnabled());

        if (dx > 0.5 || dx < -0.5) {
            popup.HorizontalOffset(popup.HorizontalOffset() + dx);
        }
        if (dy > 0.5 || dy < -0.5) {
            popup.VerticalOffset(popup.VerticalOffset() + dy);
        }
    }
}

// Menus that the taskbar opens without going through any of the functions
// hooked below (for example the battery, network and volume icons' menus)
// are handled by hooking the XAML MenuFlyout::ShowAt implementation itself.
// Its address is taken from the vtable of a throwaway MenuFlyout, which is
// shared by every MenuFlyout instance. The IFlyoutBase vtable starts with the
// 6 IInspectable slots, then get/put_Placement, add/remove_Opened,
// add/remove_Closed, add/remove_Opening, then ShowAt. The IFlyoutBase5 vtable
// starts with the 6 IInspectable slots, then get/put_ShowMode,
// get_InputDevicePrefersPrimaryCommands, get/put_AreOpenCloseAnimationsEnabled,
// get_IsOpen, then ShowAt(options).
constexpr size_t kFlyoutBaseShowAtSlot = 14;
constexpr size_t kFlyoutBase5ShowAtSlot = 12;

using XamlShowAt_t = HRESULT(WINAPI*)(void* pThis, void* placementTarget);
using XamlShowAtWithOptions_t = HRESULT(WINAPI*)(void* pThis,
                                                 void* placementTarget,
                                                 void* showOptions);

XamlShowAt_t XamlShowAt_Original;
XamlShowAtWithOptions_t XamlShowAtWithOptions_Original;
std::atomic<bool> g_xamlShowAtHooksInstalled;

// Enables the open/close animations (some menus, e.g. the battery icon's,
// have them disabled), and corrects the menu's position once it's open.
void PrepareFlyout(void* flyoutAbi, MenuAnchor const& menuAnchor) {
    Controls::Primitives::FlyoutBase flyout = nullptr;
    ((IUnknown*)flyoutAbi)
        ->QueryInterface(winrt::guid_of<Controls::Primitives::FlyoutBase>(),
                         winrt::put_abi(flyout));
    if (!flyout) {
        return;
    }

    Wh_Log(L"Flyout %s, animations enabled: %d",
           winrt::get_class_name(flyout).c_str(),
           flyout.AreOpenCloseAnimationsEnabled());
    flyout.AreOpenCloseAnimationsEnabled(true);

    // One-shot handler, removes itself when it runs.
    auto token = std::make_shared<winrt::event_token>();
    *token = flyout.Opened(
        [token, menuAnchor](winrt::Windows::Foundation::IInspectable const&
                                sender,
                            winrt::Windows::Foundation::IInspectable const&) {
            auto flyout =
                sender.try_as<Controls::Primitives::FlyoutBase>();
            if (!flyout) {
                return;
            }

            flyout.Opened(*token);

            try {
                CorrectOpenMenuPosition(flyout, menuAnchor);
            } catch (...) {
                HRESULT hr = winrt::to_hresult();
                Wh_Log(L"Error %08X", hr);
            }
        });
}

HRESULT WINAPI XamlShowAtWithOptions_Hook(void* pThis,
                                          void* placementTarget,
                                          void* showOptions) {
    Wh_Log(L">");

    if (placementTarget && showOptions) {
        try {
            DependencyObject target = nullptr;
            winrt::copy_from_abi(target, placementTarget);
            Controls::Primitives::FlyoutShowOptions options = nullptr;
            winrt::copy_from_abi(options, showOptions);
            MenuAnchor menuAnchor;
            if (AdjustShowOptions(&target, &options, &menuAnchor)) {
                PrepareFlyout(pThis, menuAnchor);
            }
        } catch (...) {
            HRESULT hr = winrt::to_hresult();
            Wh_Log(L"Error %08X", hr);
        }
    }

    return XamlShowAtWithOptions_Original(pThis, placementTarget,
                                          showOptions);
}

HRESULT WINAPI XamlShowAt_Hook(void* pThis, void* placementTarget) {
    Wh_Log(L">");

    if (placementTarget) {
        try {
            DependencyObject target = nullptr;
            winrt::copy_from_abi(target, placementTarget);
            Controls::Primitives::FlyoutShowOptions options;
            MenuAnchor menuAnchor;
            if (AdjustShowOptions(&target, &options, &menuAnchor)) {
                // Show the menu with our options instead, through the
                // IFlyoutBase5 interface of the same object.
                Controls::Primitives::IFlyoutBase5 flyout5 = nullptr;
                ((IUnknown*)pThis)
                    ->QueryInterface(
                        winrt::guid_of<Controls::Primitives::IFlyoutBase5>(),
                        winrt::put_abi(flyout5));
                if (flyout5) {
                    Wh_Log(L"Redirecting ShowAt to ShowAt with options");
                    PrepareFlyout(pThis, menuAnchor);
                    return XamlShowAtWithOptions_Original(
                        winrt::get_abi(flyout5), placementTarget,
                        winrt::get_abi(options));
                }
            }
        } catch (...) {
            HRESULT hr = winrt::to_hresult();
            Wh_Log(L"Error %08X", hr);
        }
    }

    return XamlShowAt_Original(pThis, placementTarget);
}

void* GetVtableSlotFunction(void* interfaceAbi, size_t slotIndex) {
    void** vtable = *(void***)interfaceAbi;
    return vtable ? vtable[slotIndex] : nullptr;
}

// Must run on a thread with XAML initialized (e.g. the taskbar thread).
void EnsureXamlShowAtHooks() {
    if (g_xamlShowAtHooksInstalled || g_xamlShowAtHooksInstalled.exchange(true)) {
        return;
    }

    try {
        Controls::MenuFlyout menuFlyout;
        auto flyoutBase = menuFlyout.as<Controls::Primitives::IFlyoutBase>();
        auto flyoutBase5 = menuFlyout.as<Controls::Primitives::IFlyoutBase5>();

        void* showAt =
            GetVtableSlotFunction(winrt::get_abi(flyoutBase),
                                  kFlyoutBaseShowAtSlot);
        void* showAtWithOptions =
            GetVtableSlotFunction(winrt::get_abi(flyoutBase5),
                                  kFlyoutBase5ShowAtSlot);
        if (!showAt || !showAtWithOptions || showAt == showAtWithOptions) {
            Wh_Log(L"Unexpected ShowAt functions %p, %p", showAt,
                   showAtWithOptions);
            return;
        }

        Wh_Log(L"Hooking XAML MenuFlyout ShowAt %p, %p", showAt,
               showAtWithOptions);

        WindhawkUtils::SetFunctionHook((XamlShowAt_t)showAt, XamlShowAt_Hook,
                                       &XamlShowAt_Original);
        WindhawkUtils::SetFunctionHook(
            (XamlShowAtWithOptions_t)showAtWithOptions,
            XamlShowAtWithOptions_Hook, &XamlShowAtWithOptions_Original);
        Wh_ApplyHookOperations();
    } catch (...) {
        HRESULT hr = winrt::to_hresult();
        Wh_Log(L"Failed to hook XAML MenuFlyout ShowAt: %08X", hr);
    }
}

HWND FindCurrentProcessTaskbarWnd() {
    HWND hTaskbarWnd = nullptr;

    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD dwProcessId;
            WCHAR className[32];
            if (GetWindowThreadProcessId(hWnd, &dwProcessId) &&
                dwProcessId == GetCurrentProcessId() &&
                GetClassName(hWnd, className, ARRAYSIZE(className)) &&
                _wcsicmp(className, L"Shell_TrayWnd") == 0) {
                *reinterpret_cast<HWND*>(lParam) = hWnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&hTaskbarWnd));

    return hTaskbarWnd;
}

using RunFromWindowThreadProc_t = void (*)(PVOID parameter);

bool RunFromWindowThread(HWND hWnd,
                         RunFromWindowThreadProc_t proc,
                         PVOID procParam) {
    static const UINT runFromWindowThreadRegisteredMsg =
        RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    struct RUN_FROM_WINDOW_THREAD_PARAM {
        RunFromWindowThreadProc_t proc;
        PVOID procParam;
    };

    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (dwThreadId == 0) {
        return false;
    }

    if (dwThreadId == GetCurrentThreadId()) {
        proc(procParam);
        return true;
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HC_ACTION) {
                const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;
                if (cwp->message == runFromWindowThreadRegisteredMsg) {
                    RUN_FROM_WINDOW_THREAD_PARAM* param =
                        (RUN_FROM_WINDOW_THREAD_PARAM*)cwp->lParam;
                    param->proc(param->procParam);
                }
            }

            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr, dwThreadId);
    if (!hook) {
        return false;
    }

    RUN_FROM_WINDOW_THREAD_PARAM param;
    param.proc = proc;
    param.procParam = procParam;
    SendMessage(hWnd, runFromWindowThreadRegisteredMsg, 0, (LPARAM)&param);

    UnhookWindowsHookEx(hook);

    return true;
}

// The ShowAt call is a C++/WinRT template instantiated separately in every
// module that uses it, so it may need to be hooked in several modules. Each
// hooked instance needs its own original function pointer, hence the
// template.
using ShowAt_t =
    void(WINAPI*)(void* pThis,
                  DependencyObject* placementTarget,
                  Controls::Primitives::FlyoutShowOptions* showOptions);

template <int N>
struct ShowAtHook {
    static inline ShowAt_t original;

    static void WINAPI Hook(
        void* pThis,
        DependencyObject* placementTarget,
        Controls::Primitives::FlyoutShowOptions* showOptions) {
        Wh_Log(L">");

        EnsureXamlShowAtHooks();

        if (placementTarget && showOptions) {
            try {
                if (AdjustShowOptions(placementTarget, showOptions)) {
                    // pThis is the projected flyout object, i.e. a pointer
                    // to its ABI interface pointer.
                    Controls::Primitives::FlyoutBase flyout = nullptr;
                    (*(IUnknown**)pThis)
                        ->QueryInterface(
                            winrt::guid_of<Controls::Primitives::FlyoutBase>(),
                            winrt::put_abi(flyout));
                    if (flyout) {
                        flyout.AreOpenCloseAnimationsEnabled(true);
                    }
                }
            } catch (...) {
                HRESULT hr = winrt::to_hresult();
                Wh_Log(L"Error %08X", hr);
            }
        }

        original(pThis, placementTarget, showOptions);
    }
};

FrameworkElement FindChildByName(FrameworkElement element, PCWSTR name) {
    int childrenCount = Media::VisualTreeHelper::GetChildrenCount(element);
    for (int i = 0; i < childrenCount; i++) {
        auto child = Media::VisualTreeHelper::GetChild(element, i)
                         .try_as<FrameworkElement>();
        if (child && child.Name() == name) {
            return child;
        }
    }

    return nullptr;
}

// The tray icons (clock/notification center, and other XAML tray icons) open
// their context menus in ShowContextMenu, which doesn't go through a ShowAt
// call with show options that we can adjust. Instead, open the same menu
// ourselves with our own show options, like the taskbar-on-top mod does.
bool ShowTrayContextMenu(void* pThis) {
    FrameworkElement element = nullptr;
    ((IUnknown**)pThis)[1]->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                           winrt::put_abi(element));
    if (!element) {
        Wh_Log(L"No element");
        return false;
    }

    FrameworkElement childElement = FindChildByName(element, L"ContainerGrid");
    if (!childElement) {
        Wh_Log(L"No ContainerGrid");
        return false;
    }

    auto flyout =
        Controls::Primitives::FlyoutBase::GetAttachedFlyout(childElement);
    if (!flyout) {
        Wh_Log(L"No attached flyout");
        return false;
    }

    Controls::Primitives::FlyoutShowOptions options;
    DependencyObject placementTarget = childElement;
    if (!AdjustShowOptions(&placementTarget, &options)) {
        return false;
    }

    flyout.AreOpenCloseAnimationsEnabled(true);
    flyout.ShowAt(childElement, options);
    return true;
}

using ShowContextMenu_t = void(WINAPI*)(void* pThis);

template <int N>
struct ShowContextMenuHook {
    static inline ShowContextMenu_t original;

    static void WINAPI Hook(void* pThis) {
        Wh_Log(L">");

        EnsureXamlShowAtHooks();

        bool handled = false;
        try {
            handled = ShowTrayContextMenu(pThis);
        } catch (...) {
            HRESULT hr = winrt::to_hresult();
            Wh_Log(L"Error %08X", hr);
        }

        if (!handled) {
            original(pThis);
        }
    }
};

constexpr WCHAR kTextIconContentShowContextMenuSymbol[] =
    LR"(public: void __cdecl winrt::SystemTray::implementation::TextIconContent::ShowContextMenu(void))";
constexpr WCHAR kDateTimeIconContentShowContextMenuSymbol[] =
    LR"(public: void __cdecl winrt::SystemTray::implementation::DateTimeIconContent::ShowContextMenu(void))";

constexpr WCHAR kMenuFlyoutShowAtSymbol[] =
    LR"(public: __cdecl winrt::impl::consume_Windows_UI_Xaml_Controls_Primitives_IFlyoutBase5<struct winrt::Windows::UI::Xaml::Controls::MenuFlyout>::ShowAt(struct winrt::Windows::UI::Xaml::DependencyObject const &,struct winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutShowOptions const &)const )";
constexpr WCHAR kFlyoutBaseShowAtSymbol[] =
    LR"(public: __cdecl winrt::impl::consume_Windows_UI_Xaml_Controls_Primitives_IFlyoutBase5<struct winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutBase>::ShowAt(struct winrt::Windows::UI::Xaml::DependencyObject const &,struct winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutShowOptions const &)const )";

// The modules that host the taskbar's XAML code, depending on the Windows
// version.
constexpr PCWSTR kModuleNames[] = {
    L"SystemTray.dll",
    L"Taskbar.View.dll",
    L"ExplorerExtensions.dll",
};

std::atomic<bool> g_moduleHooked[ARRAYSIZE(kModuleNames)];

template <int N>
bool HookModuleSymbols(HMODULE module) {
    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {kMenuFlyoutShowAtSymbol},
            &ShowAtHook<N * 2>::original,
            ShowAtHook<N * 2>::Hook,
            true,
        },
        {
            {kFlyoutBaseShowAtSymbol},
            &ShowAtHook<N * 2 + 1>::original,
            ShowAtHook<N * 2 + 1>::Hook,
            true,
        },
        {
            {kTextIconContentShowContextMenuSymbol},
            &ShowContextMenuHook<N * 2>::original,
            ShowContextMenuHook<N * 2>::Hook,
            true,
        },
        {
            {kDateTimeIconContentShowContextMenuSymbol},
            &ShowContextMenuHook<N * 2 + 1>::original,
            ShowContextMenuHook<N * 2 + 1>::Hook,
            true,
        },
    };

    return WindhawkUtils::HookSymbols(module, symbolHooks,
                                      ARRAYSIZE(symbolHooks));
}

bool HookModuleIfNeeded(HMODULE module) {
    bool hooked = false;

    for (int i = 0; i < ARRAYSIZE(kModuleNames); i++) {
        if (g_moduleHooked[i] || GetModuleHandle(kModuleNames[i]) != module ||
            g_moduleHooked[i].exchange(true)) {
            continue;
        }

        Wh_Log(L"Hooking %s", kModuleNames[i]);

        bool result = false;
        switch (i) {
            case 0:
                result = HookModuleSymbols<0>(module);
                break;
            case 1:
                result = HookModuleSymbols<1>(module);
                break;
            case 2:
                result = HookModuleSymbols<2>(module);
                break;
        }

        if (!result) {
            Wh_Log(L"HookSymbols failed for %s", kModuleNames[i]);
        }

        hooked = hooked || result;
    }

    return hooked;
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original;
HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName,
                                   HANDLE hFile,
                                   DWORD dwFlags) {
    HMODULE module = LoadLibraryExW_Original(lpLibFileName, hFile, dwFlags);
    if (module && HookModuleIfNeeded(module)) {
        Wh_ApplyHookOperations();
    }

    return module;
}

void LoadSettings() {
    g_settings.zoneWidth = Wh_GetIntSetting(L"zoneWidth");
    g_settings.menuGap = Wh_GetIntSetting(L"menuGap");
    Wh_Log(L"Settings loaded: zoneWidth=%d menuGap=%d", g_settings.zoneWidth,
           g_settings.menuGap);
}

BOOL Wh_ModInit(void) {
    Wh_Log(L"Init");

    LoadSettings();

    for (PCWSTR moduleName : kModuleNames) {
        if (HMODULE module = GetModuleHandle(moduleName)) {
            HookModuleIfNeeded(module);
        }
    }

    // Some of the modules may load later.
    HMODULE kernelBaseModule = GetModuleHandle(L"kernelbase.dll");
    auto pKernelBaseLoadLibraryExW =
        (decltype(&LoadLibraryExW))GetProcAddress(kernelBaseModule,
                                                  "LoadLibraryExW");
    WindhawkUtils::SetFunctionHook(pKernelBaseLoadLibraryExW,
                                   LoadLibraryExW_Hook,
                                   &LoadLibraryExW_Original);

    return TRUE;
}

HANDLE g_xamlHookThread;
HANDLE g_xamlHookThreadStopEvent;

// Hooks the XAML MenuFlyout::ShowAt implementation from the taskbar thread,
// where XAML is initialized. Returns false if the taskbar's XAML isn't there
// yet.
bool TryEnsureXamlShowAtHooksFromTaskbarThread() {
    HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
    if (!hTaskbarWnd ||
        !FindWindowEx(hTaskbarWnd, nullptr, kContentBridgeClassName,
                      nullptr)) {
        return false;
    }

    return RunFromWindowThread(
        hTaskbarWnd, [](PVOID) { EnsureXamlShowAtHooks(); }, nullptr);
}

// If the mod is loaded before the taskbar exists (e.g. while Explorer is
// starting), wait for it in the background.
DWORD WINAPI XamlHookThreadProc(LPVOID) {
    for (int i = 0; i < 120 && !g_xamlShowAtHooksInstalled; i++) {
        if (WaitForSingleObject(g_xamlHookThreadStopEvent, 1000) !=
            WAIT_TIMEOUT) {
            break;
        }

        TryEnsureXamlShowAtHooksFromTaskbarThread();
    }

    return 0;
}

void Wh_ModAfterInit(void) {
    if (!TryEnsureXamlShowAtHooksFromTaskbarThread()) {
        Wh_Log(L"Taskbar not ready yet, waiting for it");
        g_xamlHookThreadStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (g_xamlHookThreadStopEvent) {
            g_xamlHookThread = CreateThread(nullptr, 0, XamlHookThreadProc,
                                            nullptr, 0, nullptr);
        }
    }

    // Handle modules that were loaded while the mod was initializing.
    bool hooked = false;
    for (PCWSTR moduleName : kModuleNames) {
        if (HMODULE module = GetModuleHandle(moduleName)) {
            hooked = HookModuleIfNeeded(module) || hooked;
        }
    }

    if (hooked) {
        Wh_ApplyHookOperations();
    }
}

void Wh_ModBeforeUninit(void) {
    if (g_xamlHookThread) {
        SetEvent(g_xamlHookThreadStopEvent);
        WaitForSingleObject(g_xamlHookThread, INFINITE);
        CloseHandle(g_xamlHookThread);
        g_xamlHookThread = nullptr;
    }

    if (g_xamlHookThreadStopEvent) {
        CloseHandle(g_xamlHookThreadStopEvent);
        g_xamlHookThreadStopEvent = nullptr;
    }
}

void Wh_ModUninit(void) {
    Wh_Log(L"Uninit");
}

void Wh_ModSettingsChanged(void) {
    LoadSettings();
}
