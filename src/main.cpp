#include "tray_app.h"
#include "uploader.h"
#include "logger.h"
#include <windows.h>
#include <objbase.h>
#include <filesystem>
#include <string>
#include <cstring>

int main(int argc, char** argv) {
    bool waitForRestart = false;
    bool testSelfUpdate = false;
    std::string configPath = "config.ini";
    int autoSaveSeconds = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            configPath = argv[++i];
        } else if (std::strcmp(argv[i], "--test-seconds") == 0 && i + 1 < argc) {
            autoSaveSeconds = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--restarted") == 0) {
            waitForRestart = true;
        } else if (std::strcmp(argv[i], "--test-update") == 0) {
            testSelfUpdate = true;
        }
    }

    // Anchor working directory to the folder containing InstantReplay.exe so it is 100% portable
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        SetCurrentDirectoryW(exeDir.c_str());
    }

    if (testSelfUpdate) {
        Logger::Init("instant_replay.log");
        std::string tag;
        std::filesystem::path curExe(exePath);
        std::filesystem::path newExe = curExe.parent_path() / "InstantReplay.new.exe";
        if (Uploader::DownloadLatestReleaseExe(newExe.string(), tag)) {
            LOG_INFO("Self-update test succeeded: downloaded %s (%llu bytes)",
                     tag.c_str(), (unsigned long long)std::filesystem::file_size(newExe));
            std::error_code ec;
            std::filesystem::remove(newExe, ec);
            return 0;
        }
        LOG_ERROR("Self-update test failed.");
        return 1;
    }

    // Ensure only one instance runs at a time (retry briefly if restarting after self-update)
    HANDLE hSingleInstanceMutex = nullptr;
    for (int attempt = 0; attempt < (waitForRestart ? 30 : 1); ++attempt) {
        hSingleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\InstantReplay_SingleInstance_Mutex");
        if (hSingleInstanceMutex && GetLastError() != ERROR_ALREADY_EXISTS) {
            break;
        }
        if (hSingleInstanceMutex) {
            CloseHandle(hSingleInstanceMutex);
            hSingleInstanceMutex = nullptr;
        }
        if (waitForRestart) {
            Sleep(200);
        }
    }
    if (!hSingleInstanceMutex) {
        return 0;
    }

    // Run at BELOW_NORMAL_PRIORITY_CLASS so games and foreground apps never stutter
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Logger::Init("instant_replay.log");

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
