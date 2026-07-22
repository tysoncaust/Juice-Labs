#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsvc.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::wstring widen(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

static std::string read_all(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::wstring timestamp_suffix() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t b[64]{};
    swprintf(b, 64, L"%04u%02u%02u-%02u%02u%02u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

static bool wait_service(SC_HANDLE service, DWORD desired, DWORD timeout_ms) {
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    while (GetTickCount64() < deadline) {
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed)) {
            return false;
        }
        if (ssp.dwCurrentState == desired) return true;
        Sleep(250);
    }
    return false;
}

static bool restart_sunshine() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        std::fprintf(stderr, "OpenSCManager failed: %lu\n", GetLastError());
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, L"SunshineService",
                                 SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_START);
    if (!svc) {
        std::fprintf(stderr, "OpenService SunshineService failed: %lu\n", GetLastError());
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    bool ok = QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                   reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed) != FALSE;
    if (ok && ssp.dwCurrentState != SERVICE_STOPPED) {
        SERVICE_STATUS status{};
        if (!ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
            DWORD e = GetLastError();
            if (e != ERROR_SERVICE_NOT_ACTIVE) {
                std::fprintf(stderr, "Stopping SunshineService failed: %lu\n", e);
                ok = false;
            }
        }
        if (ok) ok = wait_service(svc, SERVICE_STOPPED, 30000);
    }

    if (ok && !StartServiceW(svc, 0, nullptr)) {
        DWORD e = GetLastError();
        if (e != ERROR_SERVICE_ALREADY_RUNNING) {
            std::fprintf(stderr, "Starting SunshineService failed: %lu\n", e);
            ok = false;
        }
    }
    if (ok) ok = wait_service(svc, SERVICE_RUNNING, 30000);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

static bool atomic_replace(const std::wstring &source, const std::wstring &dest) {
    std::wstring temp = dest + L".rgpu-new";
    DeleteFileW(temp.c_str());
    if (!CopyFileW(source.c_str(), temp.c_str(), FALSE)) {
        std::fprintf(stderr, "CopyFile to staging failed for destination: %lu\n", GetLastError());
        return false;
    }
    if (!MoveFileExW(temp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::fprintf(stderr, "MoveFileEx replacement failed: %lu\n", GetLastError());
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

static bool restore_file(const std::wstring &backup, const std::wstring &dest) {
    if (GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    return atomic_replace(backup, dest);
}

int main(int argc, char **argv) {
    if (argc != 6) {
        std::fprintf(stderr,
            "Usage: install_sunshine_txr <apps-source> <conf-source> <apps-dest> <conf-dest> <backup-dir>\n");
        return 64;
    }

    const std::string apps_text = read_all(argv[1]);
    const std::string conf_text = read_all(argv[2]);
    if (apps_text.find("Tokyo Xtreme Racer") == std::string::npos ||
        apps_text.find("Remote.cmd") == std::string::npos) {
        std::fprintf(stderr, "Refusing install: staged apps.json lacks guarded TXR launcher.\n");
        return 65;
    }
    if (conf_text.find("encoder = quicksync") == std::string::npos) {
        std::fprintf(stderr, "Refusing install: staged sunshine.conf does not select verified Quick Sync encoder.\n");
        return 66;
    }

    const std::wstring apps_src = widen(argv[1]);
    const std::wstring conf_src = widen(argv[2]);
    const std::wstring apps_dst = widen(argv[3]);
    const std::wstring conf_dst = widen(argv[4]);
    const std::wstring backup_dir = widen(argv[5]);
    const std::wstring suffix = timestamp_suffix();
    const std::wstring apps_backup = backup_dir + L"\\sunshine-apps-before-" + suffix + L".json";
    const std::wstring conf_backup = backup_dir + L"\\sunshine-conf-before-" + suffix + L".conf";

    if (!CopyFileW(apps_dst.c_str(), apps_backup.c_str(), TRUE)) {
        std::fprintf(stderr, "Backing up live apps.json failed: %lu\n", GetLastError());
        return 67;
    }
    if (!CopyFileW(conf_dst.c_str(), conf_backup.c_str(), TRUE)) {
        std::fprintf(stderr, "Backing up live sunshine.conf failed: %lu\n", GetLastError());
        DeleteFileW(apps_backup.c_str());
        return 68;
    }

    if (!atomic_replace(apps_src, apps_dst) || !atomic_replace(conf_src, conf_dst)) {
        restore_file(apps_backup, apps_dst);
        restore_file(conf_backup, conf_dst);
        std::fprintf(stderr, "Configuration replacement failed; backups restored.\n");
        return 69;
    }

    if (!restart_sunshine()) {
        restore_file(apps_backup, apps_dst);
        restore_file(conf_backup, conf_dst);
        restart_sunshine();
        std::fprintf(stderr, "Sunshine restart failed; configuration rolled back.\n");
        return 70;
    }

    std::printf("Sunshine TXR application installed and SunshineService is running.\n");
    std::printf("Backup apps: %ls\n", apps_backup.c_str());
    std::printf("Backup config: %ls\n", conf_backup.c_str());
    return 0;
}
