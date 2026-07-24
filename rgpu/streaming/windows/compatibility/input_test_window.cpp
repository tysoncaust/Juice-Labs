#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>

namespace {

std::wstring g_marker_path;
bool g_recorded = false;

void write_marker(wchar_t character) {
    if (g_recorded || g_marker_path.empty()) return;
    HANDLE file = CreateFileW(g_marker_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char payload[128]{};
    const int bytes = std::snprintf(payload, sizeof(payload),
                                    "RGPU_INPUT_TEST=PASS\r\nCHARACTER=%lc\r\nPID=%lu\r\n",
                                    character, GetCurrentProcessId());
    DWORD written = 0;
    if (bytes > 0) {
        WriteFile(file, payload, static_cast<DWORD>(bytes), &written, nullptr);
    }
    CloseHandle(file);
    g_recorded = true;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CHAR:
            write_marker(static_cast<wchar_t>(wparam));
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            const wchar_t* text = g_recorded
                ? L"Remote input received."
                : L"RGPU Input Test — waiting for remote key A";
            DrawTextW(dc, text, -1, &client,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; ++i) {
        if (std::wstring(argv[i]) == L"--marker") {
            g_marker_path = argv[++i];
        }
    }
    if (argv) LocalFree(argv);
    if (g_marker_path.empty()) return 2;

    const wchar_t class_name[] = L"RgpuCompatibilityInputTestWindow";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return 3;
    }

    HWND window = CreateWindowExW(0, class_name, L"RGPU Input Test",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 720, 220,
                                  nullptr, nullptr, instance, nullptr);
    if (!window) return 4;
    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return g_recorded ? 0 : 5;
}
