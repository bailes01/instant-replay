#include "tray_app.h"
#include "logger.h"
#include <windows.h>
#include <objbase.h>
#include <filesystem>
#include <string>
#include <cstring>

int main(int argc, char** argv) {
    // Ensure only one instance runs at a time
    HANDLE hSingleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\InstantReplay_SingleInstance_Mutex");
    if (hSingleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hSingleInstanceMutex);
        return 0;
    }

    // Anchor working directory to the folder containing InstantReplay.exe so it is 100% portable
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        SetCurrentDirectoryW(exeDir.c_str());
    }

    // Run at BELOW_NORMAL_PRIORITY_CLASS so games and foreground apps never stutter
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Logger::Init("instant_replay.log");

    std::string configPath = "config.ini";
    int autoSaveSeconds = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            configPath = argv[++i];
        } else if (std::strcmp(argv[i], "--test-seconds") == 0 && i + 1 < argc) {
            autoSaveSeconds = std::atoi(argv[++i]);
        }
    }

    LOG_INFO("Starting InstantReplay (config: %s)", configPath.c_str());

    TrayApp app;
    if (!app.Init(GetModuleHandleW(nullptr), configPath, autoSaveSeconds)) {
        LOG_ERROR("InstantReplay failed to initialize.");
        CoUninitialize();
        if (hSingleInstanceMutex) CloseHandle(hSingleInstanceMutex);
        return 1;
    }

    int code = app.Run();
    CoUninitialize();
    if (hSingleInstanceMutex) CloseHandle(hSingleInstanceMutex);
    return code;
}
