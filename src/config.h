#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <windows.h>

enum class VideoCodec {
    AV1,
    HEVC,
    H264
};

enum class AudioMode {
    ActiveWindow,
    Desktop,
    Off
};

struct Config {
    int monitor_index = 0;               // 0-based monitor index
    int fps = 60;                        // Capture & encode frame rate
    int replay_seconds = 30;             // Last X seconds to keep in memory
    VideoCodec codec = VideoCodec::AV1;  // av1, hevc, or h264
    int bitrate_kbps = 16000;            // Default 16 Mbps
    int preset = 5;                      // NVENC preset P1..P7 (5 = P5 high quality)

    bool debug_bitrate_sweep = false;    // When true, run parallel NVENC sessions for each bitrate below
    std::vector<int> sweep_bitrates_kbps = { 4000, 8000, 14000, 24000 };

    std::string output_dir = "captures/";

    AudioMode audio_mode = AudioMode::ActiveWindow;
    int audio_bitrate_kbps = 160;

    // Hotkey: default Alt + F10
    UINT hotkey_modifiers = MOD_ALT;
    UINT hotkey_vk = VK_F10;
    std::string hotkey_string = "Alt+F10";

    // Upload server settings
    std::string username = "player1";
    bool auto_upload = false;
    std::string upload_url = "http://localhost:8080/api/upload";
    std::string upload_token = "change-me-in-config";
    bool copy_link_to_clipboard = true;

    static Config Load(const std::string& path = "config.ini");
    bool Save(const std::string& path = "config.ini") const;
};
