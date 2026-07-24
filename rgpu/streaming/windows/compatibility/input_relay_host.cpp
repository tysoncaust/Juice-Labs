#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "User32.lib")

namespace {

struct Options {
    std::wstring server;
    unsigned short port = 50001;
    std::string token;
    std::wstring test_window;
    std::wstring marker;
};

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int characters = static_cast<int>(value.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), characters,
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(), characters,
                            result.data(), bytes, nullptr, nullptr) != bytes) {
        return {};
    }
    return result;
}

bool parse_options(int argc, wchar_t** argv, Options* output) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--server" && i + 1 < argc) output->server = argv[++i];
        else if (argument == L"--port" && i + 1 < argc) output->port = static_cast<unsigned short>(_wtoi(argv[++i]));
        else if (argument == L"--token" && i + 1 < argc) output->token = narrow(argv[++i]);
        else if (argument == L"--test-window" && i + 1 < argc) output->test_window = argv[++i];
        else if (argument == L"--marker" && i + 1 < argc) output->marker = argv[++i];
        else return false;
    }
    return !output->server.empty() && output->port != 0 && output->token.size() >= 16 &&
           !output->test_window.empty() && !output->marker.empty();
}

bool wait_for_file(const std::wstring& path, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
        Sleep(25);
    }
    return false;
}

HWND wait_for_test_window(DWORD expected_pid, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        HWND window = FindWindowW(L"RgpuCompatibilityInputTestWindow", L"RGPU Input Test");
        if (window) {
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid == expected_pid) return window;
        }
        Sleep(25);
    }
    return nullptr;
}

bool foreground_owned_window(HWND window) {
    const DWORD target_thread = GetWindowThreadProcessId(window, nullptr);
    const DWORD foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD current_thread = GetCurrentThreadId();
    bool attached_foreground = false;
    bool attached_target = false;
    if (foreground_thread && foreground_thread != current_thread) {
        attached_foreground = AttachThreadInput(current_thread, foreground_thread, TRUE) != FALSE;
    }
    if (target_thread && target_thread != current_thread) {
        attached_target = AttachThreadInput(current_thread, target_thread, TRUE) != FALSE;
    }
    ShowWindow(window, SW_RESTORE);
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    const bool result = SetForegroundWindow(window) != FALSE;
    if (attached_target) AttachThreadInput(current_thread, target_thread, FALSE);
    if (attached_foreground) AttachThreadInput(current_thread, foreground_thread, FALSE);
    return result || GetForegroundWindow() == window;
}

bool send_key_a() {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = 'A';
    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(2, inputs, sizeof(INPUT)) == 2;
}

bool send_all(SOCKET socket, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int count = send(socket, data.data() + sent,
                               static_cast<int>(data.size() - sent), 0);
        if (count <= 0) return false;
        sent += static_cast<size_t>(count);
    }
    return true;
}

std::string receive_line(SOCKET socket, size_t maximum) {
    std::string result;
    while (result.size() < maximum) {
        char character = 0;
        const int count = recv(socket, &character, 1, 0);
        if (count <= 0) return {};
        if (character == '\n') return result;
        if (character != '\r') result.push_back(character);
    }
    return {};
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        std::fprintf(stderr, "RGPU_INPUT_RELAY=FAIL invalid_arguments\n");
        return 2;
    }
    DeleteFileW(options.marker.c_str());

    std::wstring command = L"\"" + options.test_window + L"\" --marker \"" + options.marker + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(options.test_window.c_str(), mutable_command.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, nullptr, &startup, &process)) {
        std::fprintf(stderr, "RGPU_INPUT_RELAY=FAIL launch_test_window error=%lu\n", GetLastError());
        return 3;
    }
    CloseHandle(process.hThread);

    HWND test_window = wait_for_test_window(process.dwProcessId, 5000);
    if (!test_window) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        std::fprintf(stderr, "RGPU_INPUT_RELAY=FAIL owned_window_not_found\n");
        return 4;
    }

    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        PostMessageW(test_window, WM_CLOSE, 0, 0);
        CloseHandle(process.hProcess);
        return 5;
    }

    SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    const std::string server = narrow(options.server);
    if (socket_handle == INVALID_SOCKET ||
        inet_pton(AF_INET, server.c_str(), &address.sin_addr) != 1 ||
        connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        if (socket_handle != INVALID_SOCKET) closesocket(socket_handle);
        WSACleanup();
        PostMessageW(test_window, WM_CLOSE, 0, 0);
        CloseHandle(process.hProcess);
        std::fprintf(stderr, "RGPU_INPUT_RELAY=FAIL connect error=%d\n", WSAGetLastError());
        return 6;
    }

    const std::string hello = "RGPU1 " + options.token + " READY\n";
    const std::string expected = "RGPU1 " + options.token + " KEY_A";
    if (!send_all(socket_handle, hello)) {
        closesocket(socket_handle);
        WSACleanup();
        PostMessageW(test_window, WM_CLOSE, 0, 0);
        CloseHandle(process.hProcess);
        return 7;
    }
    const std::string command_line = receive_line(socket_handle, 512);
    if (command_line != expected) {
        send_all(socket_handle, "FAIL protocol\n");
        closesocket(socket_handle);
        WSACleanup();
        PostMessageW(test_window, WM_CLOSE, 0, 0);
        CloseHandle(process.hProcess);
        std::fprintf(stderr, "RGPU_INPUT_RELAY=FAIL protocol\n");
        return 8;
    }

    DWORD owner_pid = 0;
    GetWindowThreadProcessId(test_window, &owner_pid);
    const bool owned = owner_pid == process.dwProcessId;
    const bool focused = owned && foreground_owned_window(test_window);
    Sleep(100);
    const bool injected = focused && send_key_a();
    const bool marker = injected && wait_for_file(options.marker, 5000);

    send_all(socket_handle, marker ? "PASS\n" : "FAIL input\n");
    closesocket(socket_handle);
    WSACleanup();
    PostMessageW(test_window, WM_CLOSE, 0, 0);
    WaitForSingleObject(process.hProcess, 3000);
    CloseHandle(process.hProcess);

    std::printf("RGPU_REMOTE_INPUT_TEST=%s\n", marker ? "PASS" : "FAIL");
    std::printf("TARGET_WINDOW_OWNED=%s\n", owned ? "true" : "false");
    std::printf("SENDINPUT_USED=%s\n", injected ? "true" : "false");
    std::printf("PROTECTED_GAME_TARGETED=false\n");
    std::printf("ARBITRARY_PROCESS_TARGETING=false\n");
    return marker ? 0 : 9;
}
