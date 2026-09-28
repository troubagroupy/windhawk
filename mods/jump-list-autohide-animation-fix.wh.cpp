// ==WindhawkMod==
// @id              jump-list-autohide-animation-fix
// @name            Jump list animation fix
// @description     Stops taskbar jump lists from animating in over an auto-hidden taskbar
// @version         0.2.0
// @author          kuba
// @include         ShellExperienceHost.exe
// @architecture    x86-64
// @compilerOptions -lshell32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Jump list animation fix

EXPERIMENTAL. With an auto-hidden taskbar, the jump list (the menu of a
taskbar app icon) animates in from the bottom of the screen, over the
taskbar. The jump list is hosted in ShellExperienceHost.exe, so this mod runs
there.

## How it works (v0.2)

The jump list's XAML has no animation of its own, so the slide is most likely
computed from the monitor's work area, which reaches the bottom of the screen
when the taskbar auto-hides. This version tells the jump list thread (and only
that thread, the Notification Center isn't affected) that the taskbar is
always visible:

* The work area (`GetMonitorInfo`, `SystemParametersInfo(SPI_GETWORKAREA)`)
  excludes the taskbar.
* `SHAppBarMessage` reports that the taskbar doesn't auto-hide.

All of these calls are logged, to find out which of them the jump list uses.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- spoofWorkArea: true
  $name: Work area without the taskbar
  $description: >-
    Report a work area that excludes the auto-hidden taskbar to the jump list.
- spoofAppBarState: true
  $name: Taskbar never auto-hides
  $description: >-
    Report to the jump list that the taskbar doesn't auto-hide.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <shellapi.h>

#include <algorithm>

struct {
    bool spoofWorkArea;
    bool spoofAppBarState;
} g_settings;

using GetThreadDescription_t = HRESULT(WINAPI*)(HANDLE hThread,
                                                PWSTR* ppszThreadDescription);
GetThreadDescription_t pGetThreadDescription;

using SHAppBarMessage_t = decltype(&SHAppBarMessage);
SHAppBarMessage_t SHAppBarMessage_Original;

using GetMonitorInfoW_t = decltype(&GetMonitorInfoW);
GetMonitorInfoW_t GetMonitorInfoW_Original;

using GetMonitorInfoA_t = decltype(&GetMonitorInfoA);
GetMonitorInfoA_t GetMonitorInfoA_Original;

using SystemParametersInfoW_t = decltype(&SystemParametersInfoW);
SystemParametersInfoW_t SystemParametersInfoW_Original;

// Set while the mod queries the taskbar state itself, which might call the
// hooked functions again.
thread_local bool g_inModQuery;

bool IsJumpViewThread() {
    if (g_inModQuery) {
        return false;
    }

    // 0 = unknown, 1 = jump list thread, 2 = other thread.
    thread_local int state = 0;
    if (state) {
        return state == 1;
    }

    if (!pGetThreadDescription) {
        state = 2;
        return false;
    }

    PWSTR threadDescription = nullptr;
    HRESULT hr = pGetThreadDescription(GetCurrentThread(), &threadDescription);
    if (FAILED(hr) || !threadDescription) {
        return false;
    }

    if (*threadDescription) {
        state = wcscmp(threadDescription, L"JumpViewUI") == 0 ? 1 : 2;
    }

    LocalFree(threadDescription);
    return state == 1;
}

bool IsAutoHideTaskbar() {
    APPBARDATA abd{sizeof(abd)};
    return SHAppBarMessage_Original(ABM_GETSTATE, &abd) & ABS_AUTOHIDE;
}

// Shrinks the work area of the monitor the auto-hidden taskbar is on, as if
// the taskbar was always visible.
bool ExcludeTaskbarFromWorkArea(const RECT& monitorRect, RECT* workArea) {
    g_inModQuery = true;
    bool autoHide = IsAutoHideTaskbar();
    APPBARDATA abd{sizeof(abd)};
    bool gotPos = autoHide && SHAppBarMessage_Original(ABM_GETTASKBARPOS, &abd);
    g_inModQuery = false;

    if (!autoHide || !gotPos) {
        return false;
    }

    RECT intersection;
    if (!IntersectRect(&intersection, &abd.rc, &monitorRect)) {
        return false;
    }

    int thickness = abd.uEdge == ABE_LEFT || abd.uEdge == ABE_RIGHT
                        ? abd.rc.right - abd.rc.left
                        : abd.rc.bottom - abd.rc.top;
    if (thickness <= 0) {
        return false;
    }

    switch (abd.uEdge) {
        case ABE_BOTTOM:
            workArea->bottom = std::min<LONG>(workArea->bottom,
                                              monitorRect.bottom - thickness);
            break;
        case ABE_TOP:
            workArea->top =
                std::max<LONG>(workArea->top, monitorRect.top + thickness);
            break;
        case ABE_LEFT:
            workArea->left =
                std::max<LONG>(workArea->left, monitorRect.left + thickness);
            break;
        case ABE_RIGHT:
            workArea->right =
                std::min<LONG>(workArea->right, monitorRect.right - thickness);
            break;
        default:
            return false;
    }

    return true;
}

UINT_PTR WINAPI SHAppBarMessage_Hook(DWORD dwMessage, PAPPBARDATA pData) {
    UINT_PTR result = SHAppBarMessage_Original(dwMessage, pData);
    if (!IsJumpViewThread()) {
        return result;
    }

    UINT_PTR newResult = result;
    if (g_settings.spoofAppBarState) {
        switch (dwMessage) {
            case ABM_GETSTATE:
                newResult = result & ~ABS_AUTOHIDE;
                break;
            case ABM_GETAUTOHIDEBAR:
            case ABM_GETAUTOHIDEBAREX:
                newResult = 0;
                break;
        }
    }

    Wh_Log(L"SHAppBarMessage %u: %u -> %u", dwMessage, (DWORD)result,
           (DWORD)newResult);
    return newResult;
}

template <typename MONITORINFO_T>
void HandleMonitorInfo(MONITORINFO_T* lpmi) {
    RECT oldWork = lpmi->rcWork;
    bool changed = g_settings.spoofWorkArea &&
                   ExcludeTaskbarFromWorkArea(lpmi->rcMonitor, &lpmi->rcWork);
    Wh_Log(L"GetMonitorInfo: work (%d,%d)-(%d,%d) -> (%d,%d)-(%d,%d)%s",
           oldWork.left, oldWork.top, oldWork.right, oldWork.bottom,
           lpmi->rcWork.left, lpmi->rcWork.top, lpmi->rcWork.right,
           lpmi->rcWork.bottom, changed ? L"" : L" (unchanged)");
}

BOOL WINAPI GetMonitorInfoW_Hook(HMONITOR hMonitor, LPMONITORINFO lpmi) {
    BOOL result = GetMonitorInfoW_Original(hMonitor, lpmi);
    if (result && lpmi && IsJumpViewThread()) {
        HandleMonitorInfo(lpmi);
    }
    return result;
}

BOOL WINAPI GetMonitorInfoA_Hook(HMONITOR hMonitor, LPMONITORINFO lpmi) {
    BOOL result = GetMonitorInfoA_Original(hMonitor, lpmi);
    if (result && lpmi && IsJumpViewThread()) {
        HandleMonitorInfo(lpmi);
    }
    return result;
}

BOOL WINAPI SystemParametersInfoW_Hook(UINT uiAction,
                                       UINT uiParam,
                                       PVOID pvParam,
                                       UINT fWinIni) {
    BOOL result =
        SystemParametersInfoW_Original(uiAction, uiParam, pvParam, fWinIni);
    if (result && uiAction == SPI_GETWORKAREA && pvParam &&
        IsJumpViewThread()) {
        RECT* workArea = (RECT*)pvParam;
        RECT oldWork = *workArea;

        bool changed = false;
        if (g_settings.spoofWorkArea) {
            // SPI_GETWORKAREA returns the work area of the primary monitor.
            MONITORINFO mi{sizeof(mi)};
            if (GetMonitorInfoW_Original(
                    MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi)) {
                changed = ExcludeTaskbarFromWorkArea(mi.rcMonitor, workArea);
            }
        }

        Wh_Log(L"SPI_GETWORKAREA: (%d,%d)-(%d,%d) -> (%d,%d)-(%d,%d)%s",
               oldWork.left, oldWork.top, oldWork.right, oldWork.bottom,
               workArea->left, workArea->top, workArea->right,
               workArea->bottom, changed ? L"" : L" (unchanged)");
    }
    return result;
}

void LoadSettings() {
    g_settings.spoofWorkArea = Wh_GetIntSetting(L"spoofWorkArea");
    g_settings.spoofAppBarState = Wh_GetIntSetting(L"spoofAppBarState");
}

BOOL Wh_ModInit(void) {
    Wh_Log(L"Init");

    LoadSettings();

    if (HMODULE kernel32Module = LoadLibraryEx(L"kernel32.dll", nullptr,
                                               LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        pGetThreadDescription = (GetThreadDescription_t)GetProcAddress(
            kernel32Module, "GetThreadDescription");
    }

    WindhawkUtils::SetFunctionHook(SHAppBarMessage, SHAppBarMessage_Hook,
                                   &SHAppBarMessage_Original);
    WindhawkUtils::SetFunctionHook(GetMonitorInfoW, GetMonitorInfoW_Hook,
                                   &GetMonitorInfoW_Original);
    WindhawkUtils::SetFunctionHook(GetMonitorInfoA, GetMonitorInfoA_Hook,
                                   &GetMonitorInfoA_Original);
    WindhawkUtils::SetFunctionHook(SystemParametersInfoW,
                                   SystemParametersInfoW_Hook,
                                   &SystemParametersInfoW_Original);

    return TRUE;
}

void Wh_ModUninit(void) {
    Wh_Log(L"Uninit");
}

void Wh_ModSettingsChanged(void) {
    LoadSettings();
}
