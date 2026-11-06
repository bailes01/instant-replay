#pragma once
#include "config.h"
#include "dxgi_capture.h"
#include "nvenc_encoder.h"
#include "audio_capture.h"
#include "replay_buffer.h"
#include <windows.h>
#include <shellapi.h>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

struct EncoderStream {
    int bitrate_kbps = 0;
    std::unique_ptr<NvencEncoder> encoder;
    std::unique_ptr<ReplayBuffer> buffer;
};

class TrayApp {
public:
    TrayApp() = default;
    ~TrayApp();

    bool Init(HINSTANCE hInstance, const std::string& configPath, int autoSaveTestSeconds = 0);
    int Run();
    void Shutdown();

    void TriggerSaveReplay();
    void UploadLatestClip();
    void CopyMostRecentUploadLink();
    void SwitchMonitor(int monitorIndex);
    void ChangeFps(int fps);
    void PromptCustomFps();
    void ChangeReplaySeconds(int seconds);
    void PromptCustomReplaySeconds();
    void ChangeCodec(VideoCodec codec);
    void ChangeBitrate(int kbps);
    void PromptCustomBitrate();
    void PromptChangeOutputDir();
    void PromptChangeServerUrl();
    void PromptChangeUploadToken();
    void ToggleBitrateSweep();
    void StartHotkeyRebind();
    void PromptChangeUsername();
    void OpenUserGallery();
    void CheckAndApplyUpdate();
    void ReloadConfig();
    void ShowBalloon(const std::string& title, const std::string& message);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    void ShowContextMenu();
    void BindHotkey();
    bool IsStartWithWindowsEnabled() const;
    void SetStartWithWindows(bool enable);
    bool StartPipeline();
    void StopPipeline();
    void CaptureLoop();
    void SaveWorker(std::vector<int> bitrates,
                    std::vector<std::vector<uint8_t>> seqParams,
                    std::vector<ReplaySnapshot> snapshots,
                    VideoCodec codec, int width, int height, int fps,
                    std::vector<uint8_t> asc, int audioRate, int audioCh,
                    std::string recordedAppName);

    HINSTANCE m_hInstance = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_osdHwnd = nullptr;
    HHOOK m_kbHook = nullptr;
    bool m_hotkeyRegistered = false;
    std::atomic<bool> m_rebindingHotkey{false};
    NOTIFYICONDATAW m_nid{};
    HICON m_icon = nullptr;

    std::string m_configPath = "config.ini";
    Config m_config;

    DxgiCapture m_capture;
    AudioCapture m_audio;
    std::vector<EncoderStream> m_streams;
    std::mutex m_pipelineMutex;

    std::atomic<bool> m_capturing{false};
    std::thread m_captureThread;
    std::atomic<bool> m_saving{false};

    std::string m_lastSavedPath;
    std::string m_lastSavedAppName = "Desktop";
    std::string m_lastActiveAppName = "Desktop";
    std::string m_lastShareUrl;
    int m_autoSaveTestSeconds = 0;
};
