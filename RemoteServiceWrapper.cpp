#include <windows.h>
#include <string>
#include <fstream>
#include <thread>

// --- Service Globals ---
SERVICE_STATUS_HANDLE g_ServiceStatusHandle = nullptr;
SERVICE_STATUS g_ServiceStatus = {};
HANDLE g_StopEvent = nullptr;
PROCESS_INFORMATION g_RemoteProcess = { 0 };

// --- Utilities ---
std::string GetExeDir() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string path(exePath);
    size_t pos = path.find_last_of("\\/");
    return (pos == std::string::npos) ? "." : path.substr(0, pos);
}

std::string ReadPortFromConfig(const std::string& dir, int defaultPort = 27015) {
    std::ifstream fin(dir + "\\config.txt");
    std::string line;
    while (std::getline(fin, line)) {
        auto pos = line.find("port");
        if (pos != std::string::npos) {
            pos = line.find('=', pos);
            if (pos != std::string::npos) {
                int port = std::stoi(line.substr(pos + 1));
                return std::to_string(port);
            }
        }
    }
    return std::to_string(defaultPort);
}

bool StartRemoteExe(const std::string& dir, const std::string& port) {
    std::string exe = dir + "\\remote.exe";
    std::string args = "\"" + exe + "\" --server --port " + port;
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(
        NULL, &args[0], NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi
    );
    if (ok) {
        // Close previous handles if needed
        if (g_RemoteProcess.hProcess) {
            CloseHandle(g_RemoteProcess.hProcess);
            CloseHandle(g_RemoteProcess.hThread);
        }
        g_RemoteProcess = pi;
        return true;
    }
    return false;
}

void StopRemoteExe() {
    if (g_RemoteProcess.hProcess) {
        TerminateProcess(g_RemoteProcess.hProcess, 0);
        CloseHandle(g_RemoteProcess.hProcess);
        CloseHandle(g_RemoteProcess.hThread);
        g_RemoteProcess.hProcess = nullptr;
        g_RemoteProcess.hThread = nullptr;
    }
}

// --- Service Logic #2: Internal Process Restart ---
void WINAPI ServiceCtrlHandler(DWORD ctrlCode) {
    switch (ctrlCode) {
    case SERVICE_CONTROL_STOP:
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);
        SetEvent(g_StopEvent);
        break;
    default:
        break;
    }
}

void WINAPI ServiceMain(DWORD argc, LPSTR* argv) {
    g_ServiceStatusHandle = RegisterServiceCtrlHandlerA("RemoteServiceWrapper", ServiceCtrlHandler);
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);

    g_StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    std::string dir = GetExeDir();
    std::string port = ReadPortFromConfig(dir);

    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);

    // #2: Keep remote.exe running, restart if it exits unexpectedly (unless service is stopping)
    while (WaitForSingleObject(g_StopEvent, 0) == WAIT_TIMEOUT) {
        if (!StartRemoteExe(dir, port)) {
            // Optional: log failure
            break;
        }
        // Wait until remote.exe exits or stop event is signaled
        HANDLE handles[2] = { g_RemoteProcess.hProcess, g_StopEvent };
        DWORD waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);

        StopRemoteExe();

        if (waitResult == 1) break; // Stop event signaled, exit loop

        // remote.exe exited, but stop not signaled: restart after short delay
        Sleep(2000);
    }

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);
}

// --- Bootstrap/Install/Uninstall ---

bool InstallService() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!hSCM) return false;
    std::string quotedExePath = std::string("\"") + exePath + "\"";
    SC_HANDLE hService = CreateServiceA(
        hSCM, "RemoteServiceWrapper", "Remote Service Wrapper",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        quotedExePath.c_str(), NULL, NULL, NULL, NULL, NULL
    );
    if (!hService) { CloseServiceHandle(hSCM); return false; }
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return true;
}

bool UninstallService() {
    SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSCM) return false;
    SC_HANDLE hService = OpenServiceA(hSCM, "RemoteServiceWrapper", DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!hService) { CloseServiceHandle(hSCM); return false; }
    SERVICE_STATUS status;
    ControlService(hService, SERVICE_CONTROL_STOP, &status);
    BOOL ok = DeleteService(hService);
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return ok;
}

int main(int argc, char* argv[]) {
    if (argc > 1) {
        if (strcmp(argv[1], "--install") == 0) {
            return InstallService() ? 0 : 1;
        }
        if (strcmp(argv[1], "--uninstall") == 0) {
            return UninstallService() ? 0 : 1;
        }
    }
    char serviceName[] = "RemoteServiceWrapper";
    SERVICE_TABLE_ENTRYA serviceTable[] = {
        { serviceName, (LPSERVICE_MAIN_FUNCTIONA)ServiceMain },
        { NULL, NULL }
    };
    StartServiceCtrlDispatcherA(serviceTable);
    return 0;
}