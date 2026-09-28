// ==WindhawkMod==
// @id              jump-list-autohide-animation-fix
// @name            Jump list animation fix
// @description     Stops taskbar jump lists from animating in over an auto-hidden taskbar
// @version         0.1.0
// @author          kuba
// @include         ShellExperienceHost.exe
// @architecture    x86-64
// @compilerOptions -ldwmapi
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Jump list animation fix

EXPERIMENTAL. With an auto-hidden taskbar, the jump list (the menu of a
taskbar app icon) animates in from the bottom of the screen, over the
taskbar. The jump list is hosted in ShellExperienceHost.exe, so this mod runs
there.

## How it works (v0.1)

This first version disables the system's show/hide animation of the jump
list window (DWMWA_TRANSITIONS_FORCEDISABLED), which only the process that
owns the window may do, and logs how the jump list window is shown and
moved, to find out how the animation is done.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- disableSystemAnimation: true
  $name: Disable the system animation
  $description: >-
    Disable the system's show/hide animation of the jump list window.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <dwmapi.h>

struct {
    bool disableSystemAnimation;
} g_settings;

using GetThreadDescription_t = HRESULT(WINAPI*)(HANDLE hThread,
                                                PWSTR* ppszThreadDescription);
GetThreadDescription_t pGetThreadDescription;

constexpr WCHAR kHandledPropName[] = L"Handled_Windhawk_JumpListAnimFix";

bool IsJumpViewWindow(HWND hWnd) {
    WCHAR szClassName[64];
    if (!GetClassName(hWnd, szClassName, ARRAYSIZE(szClassName)) ||
        _wcsicmp(szClassName, L"Windows.UI.Core.CoreWindow") != 0) {
        return false;
    }

    DWORD threadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (!threadId || !pGetThreadDescription) {
        return false;
    }

    HANDLE thread =
        OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId);
    if (!thread) {
        return false;
    }

    PWSTR threadDescription = nullptr;
    HRESULT hr = pGetThreadDescription(thread, &threadDescription);
    CloseHandle(thread);
    if (FAILED(hr) || !threadDescription) {
        return false;
    }

    bool isJumpView = wcscmp(threadDescription, L"JumpViewUI") == 0;
    LocalFree(threadDescription);
    return isJumpView;
}

void HandleJumpViewWindow(HWND hWnd, PCWSTR source) {
    RECT rc{};
    GetWindowRect(hWnd, &rc);
    Wh_Log(L"%s: jump list window %p at (%d,%d)-(%d,%d), visible: %d", source,
           hWnd, rc.left, rc.top, rc.right, rc.bottom, IsWindowVisible(hWnd));

    if (!g_settings.disableSystemAnimation ||
        GetProp(hWnd, kHandledPropName)) {
        return;
    }

    SetProp(hWnd, kHandledPropName, (HANDLE)1);

    BOOL disable = TRUE;
    HRESULT hr = DwmSetWindowAttribute(hWnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                                       &disable, sizeof(disable));
    Wh_Log(L"Disabling the system animation: %08X", hr);
}

using SetWindowPos_t = decltype(&SetWindowPos);
SetWindowPos_t SetWindowPos_Original;
BOOL WINAPI SetWindowPos_Hook(HWND hWnd,
                              HWND hWndInsertAfter,
                              int X,
                              int Y,
                              int cx,
                              int cy,
                              UINT uFlags) {
    if (IsJumpViewWindow(hWnd)) {
        Wh_Log(L"SetWindowPos (%d,%d) %dx%d flags %08X", X, Y, cx, cy,
               uFlags);
        HandleJumpViewWindow(hWnd, L"SetWindowPos");
    }

    return SetWindowPos_Original(hWnd, hWndInsertAfter, X, Y, cx, cy,
                                 uFlags);
}

using ShowWindow_t = decltype(&ShowWindow);
ShowWindow_t ShowWindow_Original;
BOOL WINAPI ShowWindow_Hook(HWND hWnd, int nCmdShow) {
    if (IsJumpViewWindow(hWnd)) {
        Wh_Log(L"ShowWindow %d", nCmdShow);
        HandleJumpViewWindow(hWnd, L"ShowWindow");
    }

    return ShowWindow_Original(hWnd, nCmdShow);
}

using DwmSetWindowAttribute_t = decltype(&DwmSetWindowAttribute);
DwmSetWindowAttribute_t DwmSetWindowAttribute_Original;
HRESULT WINAPI DwmSetWindowAttribute_Hook(HWND hwnd,
                                          DWORD dwAttribute,
                                          LPCVOID pvAttribute,
                                          DWORD cbAttribute) {
    if (IsJumpViewWindow(hwnd)) {
        Wh_Log(L"DwmSetWindowAttribute %u, size %u, first DWORD %u",
               dwAttribute, cbAttribute,
               pvAttribute && cbAttribute >= sizeof(DWORD)
                   ? *(const DWORD*)pvAttribute
                   : 0);

        // Keep the system animation disabled if something tries to enable
        // it again.
        if (g_settings.disableSystemAnimation &&
            dwAttribute == DWMWA_TRANSITIONS_FORCEDISABLED) {
            BOOL disable = TRUE;
            return DwmSetWindowAttribute_Original(hwnd, dwAttribute, &disable,
                                                  sizeof(disable));
        }
    }

    return DwmSetWindowAttribute_Original(hwnd, dwAttribute, pvAttribute,
                                          cbAttribute);
}

void LoadSettings() {
    g_settings.disableSystemAnimation =
        Wh_GetIntSetting(L"disableSystemAnimation");
}

BOOL Wh_ModInit(void) {
    Wh_Log(L"Init");

    LoadSettings();

    if (HMODULE kernel32Module = LoadLibraryEx(L"kernel32.dll", nullptr,
                                               LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        pGetThreadDescription = (GetThreadDescription_t)GetProcAddress(
            kernel32Module, "GetThreadDescription");
    }

    WindhawkUtils::SetFunctionHook(SetWindowPos, SetWindowPos_Hook,
                                   &SetWindowPos_Original);
    WindhawkUtils::SetFunctionHook(ShowWindow, ShowWindow_Hook,
                                   &ShowWindow_Original);
    WindhawkUtils::SetFunctionHook(DwmSetWindowAttribute,
                                   DwmSetWindowAttribute_Hook,
                                   &DwmSetWindowAttribute_Original);

    return TRUE;
}

void Wh_ModAfterInit(void) {
    // Handle jump list windows that already exist.
    EnumWindows(
        [](HWND hWnd, LPARAM) -> BOOL {
            DWORD processId = 0;
            GetWindowThreadProcessId(hWnd, &processId);
            if (processId == GetCurrentProcessId() && IsJumpViewWindow(hWnd)) {
                HandleJumpViewWindow(hWnd, L"Existing");
            }
            return TRUE;
        },
        0);
}

void Wh_ModUninit(void) {
    Wh_Log(L"Uninit");

    // Restore the system animation.
    EnumWindows(
        [](HWND hWnd, LPARAM) -> BOOL {
            DWORD processId = 0;
            GetWindowThreadProcessId(hWnd, &processId);
            if (processId == GetCurrentProcessId() &&
                GetProp(hWnd, kHandledPropName)) {
                RemoveProp(hWnd, kHandledPropName);
                BOOL disable = FALSE;
                DwmSetWindowAttribute_Original(hWnd,
                                               DWMWA_TRANSITIONS_FORCEDISABLED,
                                               &disable, sizeof(disable));
            }
            return TRUE;
        },
        0);
}

void Wh_ModSettingsChanged(void) {
    LoadSettings();
}
