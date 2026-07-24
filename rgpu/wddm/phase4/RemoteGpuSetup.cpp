#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <softpub.h>
#include <wintrust.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "wintrust.lib")

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kServiceName[] = L"RemoteGpuTransport";
constexpr wchar_t kStateDirectory[] = L"Juice Labs\\RemoteGPU";
constexpr wchar_t kStateFile[] = L"driver-inf.txt";

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &bytes);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool VerifySignature(const fs::path& path) {
    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = path.c_str();

    WINTRUST_DATA trust_data{};
    trust_data.cbStruct = sizeof(trust_data);
    trust_data.dwUIChoice = WTD_UI_NONE;
    trust_data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trust_data.dwUnionChoice = WTD_CHOICE_FILE;
    trust_data.pFile = &file_info;
    trust_data.dwStateAction = WTD_STATEACTION_VERIFY;
    trust_data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &policy, &trust_data);
    trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &trust_data);
    return status == ERROR_SUCCESS;
}

struct Validation {
    bool ready = true;
    std::vector<std::wstring> errors;
};

Validation ValidatePackage(const fs::path& package) {
    Validation result;
    const std::vector<std::wstring> required = {
        L"RemoteGpuKmd.sys", L"RemoteGpuUmd.dll",
        L"RgpuTransportService.exe", L"RemoteGpuRoot.inf",
        L"RemoteGpuRoot.cat", L"production-ready.marker"
    };
    for (const auto& name : required) {
        if (!fs::is_regular_file(package / name)) {
            result.ready = false;
            result.errors.push_back(L"missing:" + name);
        }
    }
    const std::vector<std::wstring> signed_files = {
        L"RemoteGpuKmd.sys", L"RemoteGpuUmd.dll",
        L"RgpuTransportService.exe", L"RemoteGpuRoot.cat"
    };
    for (const auto& name : signed_files) {
        const auto path = package / name;
        if (fs::is_regular_file(path) && !VerifySignature(path)) {
            result.ready = false;
            result.errors.push_back(L"signature-invalid:" + name);
        }
    }
    return result;
}

fs::path ProgramDataStatePath() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD count = GetEnvironmentVariableW(L"ProgramData", buffer, MAX_PATH);
    if (count == 0 || count >= MAX_PATH) return {};
    return fs::path(buffer) / kStateDirectory;
}

bool InstallService(const fs::path& executable) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!manager) return false;
    SC_HANDLE service = CreateServiceW(
        manager, kServiceName, L"RemoteGPU Transport Service",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL, executable.c_str(), nullptr, nullptr, nullptr,
        L"NT AUTHORITY\\LocalService", nullptr);
    if (!service && GetLastError() == ERROR_SERVICE_EXISTS) {
        service = OpenServiceW(manager, kServiceName, SERVICE_ALL_ACCESS);
    }
    const bool ok = service != nullptr;
    if (service) CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return ok;
}

bool RemoveService() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return false;
    SC_HANDLE service = OpenServiceW(manager, kServiceName,
                                     SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!service) {
        const DWORD error = GetLastError();
        CloseServiceHandle(manager);
        return error == ERROR_SERVICE_DOES_NOT_EXIST;
    }
    SERVICE_STATUS status{};
    ControlService(service, SERVICE_CONTROL_STOP, &status);
    const bool ok = DeleteService(service) != FALSE || GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE;
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return ok;
}

int ValidateCommand(const fs::path& package) {
    const Validation validation = ValidatePackage(package);
    if (!validation.ready) {
        std::wcerr << L"PHASE4_PACKAGE_VALIDATION=REFUSED\n";
        for (const auto& error : validation.errors) std::wcerr << error << L"\n";
        return 20;
    }
    std::wcout << L"PHASE4_PACKAGE_VALIDATION=PASS\n";
    return 0;
}

int InstallCommand(const fs::path& package) {
    const int validation = ValidateCommand(package);
    if (validation != 0) return validation;
    if (!IsElevated()) {
        std::wcerr << L"PHASE4_INSTALL=REFUSED elevation-required\n";
        return 21;
    }

    wchar_t destination[MAX_PATH]{};
    DWORD required = 0;
    const fs::path inf = package / L"RemoteGpuRoot.inf";
    if (!SetupCopyOEMInfW(inf.c_str(), package.c_str(), SPOST_PATH, 0,
                          destination, MAX_PATH, &required, nullptr)) {
        std::wcerr << L"PHASE4_INSTALL=FAIL SetupCopyOEMInf error=" << GetLastError() << L"\n";
        return 22;
    }
    const fs::path state_dir = ProgramDataStatePath();
    fs::create_directories(state_dir);
    std::wofstream(state_dir / kStateFile) << fs::path(destination).filename().wstring();
    if (!InstallService(package / L"RgpuTransportService.exe")) {
        std::wcerr << L"PHASE4_INSTALL=FAIL service error=" << GetLastError() << L"\n";
        return 23;
    }
    std::wcout << L"PHASE4_INSTALL=STAGED driver-store-and-service\n";
    return 0;
}

int UninstallCommand() {
    if (!IsElevated()) {
        std::wcerr << L"PHASE4_UNINSTALL=REFUSED elevation-required\n";
        return 31;
    }
    const fs::path state_dir = ProgramDataStatePath();
    std::wstring published_inf;
    std::wifstream input(state_dir / kStateFile);
    std::getline(input, published_inf);
    const bool service_ok = RemoveService();
    bool inf_ok = true;
    if (!published_inf.empty()) {
        inf_ok = SetupUninstallOEMInfW(published_inf.c_str(), SUOI_FORCEDELETE, nullptr) != FALSE;
    }
    std::error_code ignored;
    fs::remove_all(state_dir, ignored);
    if (!service_ok || !inf_ok) {
        std::wcerr << L"PHASE4_UNINSTALL=FAIL service=" << service_ok
                   << L" inf=" << inf_ok << L"\n";
        return 32;
    }
    std::wcout << L"PHASE4_UNINSTALL=PASS\n";
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wcerr << L"usage: RemoteGpuSetup --validate <package> | --install <package> | --uninstall\n";
        return 64;
    }
    const std::wstring command = argv[1];
    if (command == L"--validate" && argc == 3) return ValidateCommand(argv[2]);
    if (command == L"--install" && argc == 3) return InstallCommand(argv[2]);
    if (command == L"--uninstall" && argc == 2) return UninstallCommand();
    return 64;
}
