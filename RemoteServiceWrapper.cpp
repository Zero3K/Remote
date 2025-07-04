// RemoteServiceWrapper.cpp
// Windows Service wrapper that always runs remote.exe as a child in the active user session.
// If remote.exe exits unexpectedly (crash/close), it is relaunched (unless service is stopping).
// Reads port from config.txt in the same directory.
// Installs/uninstalls itself as a service with proper quoting for paths with spaces.
// All output is printed to the console during --install/--uninstall.

#include <windows.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <shlwapi.h>
#include <iostream>
#include <fstream>
#include <string>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Userenv.lib")
#pragma comment(lib, "Wtsapi32.lib")
#pragma comment(lib, "Shlwapi.lib")

SERVICE_STATUS_HANDLE g_ServiceStatusHandle = nullptr;
SERVICE_STATUS g_ServiceStatus = {};
HANDLE g_StopEvent = nullptr;
PROCESS_INFORMATION g_RemoteProcess = { 0 };

std::string GetExeDir() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    PathRemoveFileSpecA(exePath);
    return std::string(exePath);
}

std::string ReadPortFromConfig(const std::string& dir, int defaultPort = 27015) {
    std::ifstream fin(dir + "\\config.txt");
    std::string line;
    while (std::getline(fin, line)) {
        auto pos = line.find("port");
        if (pos != std::string::npos) {
            pos = line.find('=', pos);
            if (pos != std::string::npos) {
                try {
                    int port = std::stoi(line.substr(pos + 1));
                    return std::to_string(port);
                }
                catch (...) {}
            }
        }
    }
    return std::to_string(defaultPort);
}

// Launch remote.exe as the active user in their session
bool StartRemoteExeAsActiveUser(const std::string& dir, const std::string& port, PROCESS_INFORMATION& pi) {
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    HANDLE userToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &userToken)) {
        return false;
    }
    HANDLE userTokenDup = nullptr;
    if (!DuplicateTokenEx(userToken, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &userTokenDup)) {
        CloseHandle(userToken);
        return false;
    }
    CloseHandle(userToken);

    // Build command line
    std::string exe = dir + "\\remote.exe";
    std::string cmdLine = "\"" + exe + "\" --server --port " + port;

    // Set up environment for user
    LPVOID env = nullptr;
    if (!CreateEnvironmentBlock(&env, userTokenDup, FALSE)) {
        CloseHandle(userTokenDup);
        return false;
    }

    STARTUPINFOA si = { sizeof(si) };
    si.lpDesktop = (LPSTR)"winsta0\\default";
    BOOL ok = CreateProcessAsUserA(
        userTokenDup, exe.c_str(), &cmdLine[0], NULL, NULL, FALSE,
        CREATE_UNICODE_ENVIRONMENT, env, dir.c_str(), &si, &pi
    );
    DestroyEnvironmentBlock(env);
    CloseHandle(userTokenDup);
    return ok == TRUE;
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

void WINAPI ServiceMain(DWORD, LPSTR*) {
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

    // Main loop: keep remote.exe running as user, restart if it exits unexpectedly
    while (WaitForSingleObject(g_StopEvent, 0) == WAIT_TIMEOUT) {
        // Launch as active user
        if (!StartRemoteExeAsActiveUser(dir, port, g_RemoteProcess)) {
            Sleep(3000); // Wait and try again if launch fails (no user logged in?)
            continue;
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

// Install as service, quoting path for spaces, prints status to console
bool InstallService() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string quotedExePath = std::string("\"") + exePath + "\"";
    SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!hSCM) {
        std::cout << "Failed to open Service Control Manager. Error: " << GetLastError() << "\n";
        return false;
    }
    SC_HANDLE hService = CreateServiceA(
        hSCM, "RemoteServiceWrapper", "Remote Service Wrapper",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        quotedExePath.c_str(), NULL, NULL, NULL, NULL, NULL
    );
    if (!hService) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            std::cout << "Service already exists.\n";
        }
        else {
            std::cout << "CreateServiceA failed. Error: " << err << "\n";
        }
        CloseServiceHandle(hSCM);
        return false;
    }
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    std::cout << "Service installed successfully.\n";
    return true;
}

// Uninstall service, prints status to console
bool UninstallService() {
    SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSCM) {
        std::cout << "Failed to open Service Control Manager. Error: " << GetLastError() << "\n";
        return false;
    }
    SC_HANDLE hService = OpenServiceA(hSCM, "RemoteServiceWrapper", DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!hService) {
        std::cout << "Failed to open service. Error: " << GetLastError() << "\n";
        CloseServiceHandle(hSCM);
        return false;
    }
    SERVICE_STATUS status;
    ControlService(hService, SERVICE_CONTROL_STOP, &status);
    BOOL ok = DeleteService(hService);
    if (ok) {
        std::cout << "Service uninstalled successfully.\n";
    }
    else {
        std::cout << "Failed to delete service. Error: " << GetLastError() << "\n";
    }
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return ok == TRUE;
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