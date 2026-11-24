#include "startup_stubs/overlay_window.h"

#include <cstdio>

int main()
{
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = x2w_startup::presenter_class;
    if (!RegisterClassW(&wc)) return 1;
    // Offscreen, unactivated windows exercise the real z-order enumerator.
    const auto make = [&](const wchar_t* name, HWND owner, bool visible) {
        HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, name, L"", WS_POPUP,
                                    -32000, -32000, 8, 8, owner, nullptr, wc.hInstance, nullptr);
        if (hwnd && visible) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        return hwnd;
    };
    HWND output = make(wc.lpszClassName, nullptr, true);
    HWND hidden = make(L"STATIC", nullptr, false);
    HWND owned = make(L"STATIC", output, true);
    if (!output || !hidden || !owned || x2w_startup::overlay_window()) return 2;
    HWND guest = make(L"STATIC", nullptr, true);
    if (!guest || x2w_startup::overlay_window() != guest) return 3;
    if (!SetWindowPos(output, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE) ||
        x2w_startup::overlay_window() != guest) return 4;
    DestroyWindow(guest);
    if (x2w_startup::overlay_window()) return 5;
    DestroyWindow(owned);
    DestroyWindow(hidden);
    DestroyWindow(output);
    std::puts("GDI overlay selected the guest below or above the presenter; no target after guest destruction");
    return 0;
}
