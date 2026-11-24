#pragma once

#include <windows.h>

namespace x2w_startup {
inline constexpr wchar_t presenter_class[] = L"X2WOutputWindow";

inline HWND overlay_window()
{
    HWND window = nullptr;
    EnumWindows([](HWND candidate, LPARAM out) -> BOOL {
        DWORD process = 0;
        GetWindowThreadProcessId(candidate, &process);
        if (process != GetCurrentProcessId() || !IsWindowVisible(candidate) || GetWindow(candidate, GW_OWNER))
            return TRUE;
        wchar_t name[256]{};
        if (!GetClassNameW(candidate, name, 256) || lstrcmpW(name, presenter_class) == 0)
            return TRUE;
        *reinterpret_cast<HWND*>(out) = candidate;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&window));
    return window;
}
}  // namespace x2w_startup
