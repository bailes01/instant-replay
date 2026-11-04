#include "config.h"
#include "logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

static std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

static void ParseHotkey(const std::string& str, UINT& mods, UINT& vk) {
    mods = 0;
    vk = VK_F10;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, '+')) {
        std::string t = ToLower(Trim(token));
        if (t == "alt") mods |= MOD_ALT;
        else if (t == "ctrl" || t == "control") mods |= MOD_CONTROL;
        else if (t == "shift") mods |= MOD_SHIFT;
        else if (t == "win") mods |= MOD_WIN;
        else if (t.size() >= 2 && t[0] == 'f' && std::isdigit(t[1])) {
            int fnum = std::stoi(t.substr(1));
            if (fnum >= 1 && fnum <= 24) vk = VK_F1 + (fnum - 1);
        } else if (t.size() == 1 && std::isalnum(t[0])) {
            vk = std::toupper(t[0]);
        }
    }
}

static std::string GetDefaultWindowsUsername() {
    char buf[128]{};
    DWORD len = sizeof(buf);
    if (GetUserNameA(buf, &len) && buf[0] != '\0') {
        std::string out;
        for (char c : std::string(buf)) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        if (!out.empty()) return out;
    }
    return "player1";
}

Config Config::Load(const std::string& path) {
    Config cfg;
    cfg.username = GetDefaultWindowsUsername();
    std::ifstream in(path);
    if (!in.is_open()) {
        LOG_INFO("Config file '%s' not found; writing defaults.", path.c_str());
        cfg.Save(path);
        return cfg;
    }

    std::string line;
    while (std::getline(in, line)) {
        size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos) line = line.substr(0, comment);
        line = Trim(line);
        if (line.empty() || line[0] == '[') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = ToLower(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));

        try {
            if (key == "monitor_index") cfg.monitor_index = std::stoi(val);
            else if (key == "fps") cfg.fps = std::clamp(std::stoi(val), 10, 240);
            else if (key == "replay_seconds") cfg.replay_seconds = std::clamp(std::stoi(val), 3, 600);
            else if (key == "codec") {
                std::string c = ToLower(val);
                if (c == "av1") cfg.codec = VideoCodec::AV1;
                else if (c == "hevc" || c == "h265") cfg.codec = VideoCodec::HEVC;
                else if (c == "h264" || c == "avc") cfg.codec = VideoCodec::H264;
            }
            else if (key == "bitrate_kbps") cfg.bitrate_kbps = std::clamp(std::stoi(val), 500, 200000);
            else if (key == "preset") cfg.preset = std::clamp(std::stoi(val), 1, 7);
            else if (key == "debug_bitrate_sweep") {
                std::string v = ToLower(val);
                cfg.debug_bitrate_sweep = (v == "true" || v == "1" || v == "yes");
            }
            else if (key == "sweep_bitrates_kbps") {
                std::vector<int> rates;
                std::stringstream ss(val);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    std::string t = Trim(item);
                    if (!t.empty()) rates.push_back(std::clamp(std::stoi(t), 500, 200000));
                }
                if (!rates.empty()) cfg.sweep_bitrates_kbps = rates;
            }
            else if (key == "output_dir") {
                cfg.output_dir = val;
                if (!cfg.output_dir.empty() && cfg.output_dir.back() != '/' && cfg.output_dir.back() != '\\') {
                    cfg.output_dir += '/';
                }
            }
            else if (key == "audio_mode") {
                std::string a = ToLower(val);
                if (a == "active_window" || a == "window" || a == "process") cfg.audio_mode = AudioMode::ActiveWindow;
                else if (a == "desktop" || a == "all") cfg.audio_mode = AudioMode::Desktop;
                else if (a == "off" || a == "none" || a == "false") cfg.audio_mode = AudioMode::Off;
            }
            else if (key == "audio_bitrate_kbps") cfg.audio_bitrate_kbps = std::clamp(std::stoi(val), 64, 320);
            else if (key == "hotkey") {
                cfg.hotkey_string = val;
                ParseHotkey(val, cfg.hotkey_modifiers, cfg.hotkey_vk);
            }
            else if (key == "username") {
                if (!val.empty()) cfg.username = val;
            }
            else if (key == "auto_upload") {
                std::string v = ToLower(val);
                cfg.auto_upload = (v == "true" || v == "1" || v == "yes");
            }
            else if (key == "upload_url") cfg.upload_url = val;
            else if (key == "upload_token") cfg.upload_token = val;
            else if (key == "copy_link_to_clipboard") {
                std::string v = ToLower(val);
                cfg.copy_link_to_clipboard = (v == "true" || v == "1" || v == "yes");
            }
        } catch (...) {
            LOG_WARN("Invalid config value for key '%s': '%s'", key.c_str(), val.c_str());
        }
    }
    return cfg;
}

bool Config::Save(const std::string& path) const {
    std::ofstream out(path);
    if (!out.is_open()) return false;

    const char* codecStr = (codec == VideoCodec::AV1) ? "av1" : (codec == VideoCodec::HEVC ? "hevc" : "h264");
    const char* audioStr = (audio_mode == AudioMode::ActiveWindow) ? "active_window" :
                           (audio_mode == AudioMode::Desktop ? "desktop" : "off");

    out << "# InstantReplay Configuration\n\n"
        << "[capture]\n"
        << "# 0-based monitor index on the NVIDIA GPU\n"
        << "monitor_index = " << monitor_index << "\n"
        << "fps = " << fps << "\n"
        << "replay_seconds = " << replay_seconds << "\n"
        << "output_dir = " << output_dir << "\n"
        << "hotkey = " << hotkey_string << "\n\n"
        << "[video]\n"
        << "# Codec: av1 (default, best efficiency on RTX 40/50), hevc, or h264\n"
        << "codec = " << codecStr << "\n"
        << "# Target average bitrate in kbps (e.g. 16000 = 16 Mbps)\n"
        << "bitrate_kbps = " << bitrate_kbps << "\n"
        << "# NVENC quality preset: 1 (fastest) to 7 (slowest/best). 5 = P5 recommended.\n"
        << "preset = " << preset << "\n\n"
        << "[debug]\n"
        << "# When true, encodes simultaneous streams at every bitrate in sweep_bitrates_kbps\n"
        << "# so pressing the save hotkey outputs multiple files to compare compression quality.\n"
        << "debug_bitrate_sweep = " << (debug_bitrate_sweep ? "true" : "false") << "\n"
        << "sweep_bitrates_kbps = ";
    for (size_t i = 0; i < sweep_bitrates_kbps.size(); ++i) {
        if (i > 0) out << ", ";
        out << sweep_bitrates_kbps[i];
    }
    out << "\n\n[audio]\n"
        << "# Audio mode: active_window (only foreground window process tree), desktop, or off\n"
        << "audio_mode = " << audioStr << "\n"
        << "audio_bitrate_kbps = " << audio_bitrate_kbps << "\n\n"
        << "[upload]\n"
        << "username = " << username << "\n"
        << "auto_upload = " << (auto_upload ? "true" : "false") << "\n"
        << "upload_url = " << upload_url << "\n"
        << "upload_token = " << upload_token << "\n"
        << "copy_link_to_clipboard = " << (copy_link_to_clipboard ? "true" : "false") << "\n";

    return true;
}
