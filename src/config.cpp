#include "config.h"
#include "logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <random>

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
        std::string rawTok = Trim(token);
        std::string t = ToLower(rawTok);
        if (t == "alt") mods |= MOD_ALT;
        else if (t == "ctrl" || t == "control") mods |= MOD_CONTROL;
        else if (t == "shift") mods |= MOD_SHIFT;
        else if (t == "win") mods |= MOD_WIN;
        else if (t.size() >= 2 && t[0] == 'f' && std::isdigit(static_cast<unsigned char>(t[1]))) {
            int fnum = std::stoi(t.substr(1));
            if (fnum >= 1 && fnum <= 24) vk = VK_F1 + (fnum - 1);
        } else if (t.size() > 3 && t.rfind("key", 0) == 0 && std::isdigit(static_cast<unsigned char>(t[3]))) {
            vk = static_cast<UINT>(std::stoi(t.substr(3)));
        } else if (t.size() == 1 && std::isalnum(static_cast<unsigned char>(t[0]))) {
            vk = static_cast<UINT>(std::toupper(static_cast<unsigned char>(t[0])));
        } else if (t.size() == 1) {
            switch (t[0]) {
                case '[': vk = VK_OEM_4; break;
                case ']': vk = VK_OEM_6; break;
                case '\\': vk = VK_OEM_5; break;
                case ';': vk = VK_OEM_1; break;
                case '\'': vk = VK_OEM_7; break;
                case ',': vk = VK_OEM_COMMA; break;
                case '.': vk = VK_OEM_PERIOD; break;
                case '/': vk = VK_OEM_2; break;
                case '-': vk = VK_OEM_MINUS; break;
                case '=': vk = VK_OEM_PLUS; break;
                case '`': vk = VK_OEM_3; break;
                default: {
                    SHORT scan = VkKeyScanA(t[0]);
                    if (scan != -1) vk = static_cast<UINT>(LOBYTE(scan));
                    break;
                }
            }
        } else if (!rawTok.empty()) {
            // Match localized key names from GetKeyNameTextW (e.g. "Page Up", "Insert", "]")
            for (UINT candidate = 8; candidate <= 254; ++candidate) {
                UINT scan = MapVirtualKeyW(candidate, MAPVK_VK_TO_VSC);
                if (scan == 0) continue;
                wchar_t keyName[64]{};
                if (GetKeyNameTextW(static_cast<LONG>(scan << 16), keyName, 64) > 0) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, keyName, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 1) {
                        std::string kStr(len - 1, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, keyName, -1, kStr.data(), len - 1, nullptr, nullptr);
                        if (ToLower(kStr) == t) {
                            vk = candidate;
                            break;
                        }
                    }
                }
            }
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

static std::string GenerateOrLoadUserToken() {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\InstantReplay", 0, nullptr,
                        0, KEY_READ | KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        char buf[128]{};
        DWORD sz = sizeof(buf) - 1;
        DWORD type = 0;
        if (RegQueryValueExA(hKey, "UserToken", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &sz) == ERROR_SUCCESS &&
            type == REG_SZ && sz > 8 && buf[0] != '\0') {
            RegCloseKey(hKey);
            return std::string(buf);
        }
    }

    static const char kChars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::random_device rd;
    std::mt19937_64 gen((static_cast<uint64_t>(rd()) << 32) ^ rd() ^ GetTickCount64());
    std::uniform_int_distribution<size_t> dist(0, sizeof(kChars) - 2);
    std::string token;
    token.reserve(32);
    for (int i = 0; i < 32; ++i) {
        token.push_back(kChars[dist(gen)]);
    }

    if (hKey) {
        RegSetValueExA(hKey, "UserToken", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(token.c_str()),
                       static_cast<DWORD>(token.size() + 1));
        RegCloseKey(hKey);
    }
    return token;
}

static void SaveUserTokenToRegistry(const std::string& token) {
    if (token.empty()) return;
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\InstantReplay", 0, nullptr,
                        0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, "UserToken", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(token.c_str()),
                       static_cast<DWORD>(token.size() + 1));
        RegCloseKey(hKey);
    }
}

Config Config::Load(const std::string& path) {
    Config cfg;
    cfg.username = GetDefaultWindowsUsername();
    cfg.user_token = GenerateOrLoadUserToken();
    std::ifstream in(path);
    if (!in.is_open()) {
        LOG_INFO("Config file '%s' not found; writing defaults with generated user_token.", path.c_str());
        cfg.Save(path);
        return cfg;
    }

    bool hadUserToken = false;
    bool hadCustomUsername = false;

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
                std::string u = val;
                if (!u.empty() && u != "player1" && u != "default") {
                    cfg.username = u;
                    hadCustomUsername = true;
                }
            }
            else if (key == "user_token") {
                if (!val.empty()) {
                    cfg.user_token = val;
                    SaveUserTokenToRegistry(val);
                    hadUserToken = true;
                }
            }
            else if (key == "auto_upload") {
                std::string v = ToLower(val);
                cfg.auto_upload = (v == "true" || v == "1" || v == "yes");
            }
            else if (key == "server_url" || key == "upload_url") {
                std::string u = val;
                size_t apiPos = u.find("/api/");
                if (apiPos != std::string::npos) {
                    u = u.substr(0, apiPos);
                }
                while (!u.empty() && u.back() == '/') u.pop_back();
                if (!u.empty() && u != "http://localhost:8080") {
                    cfg.server_url = u;
                }
            }
            else if (key == "upload_token") cfg.upload_token = val;
            else if (key == "copy_link_to_clipboard") {
                std::string v = ToLower(val);
                cfg.copy_link_to_clipboard = (v == "true" || v == "1" || v == "yes");
            }
            else if (key == "delete_after_upload" || key == "delete_video_on_upload") {
                std::string v = ToLower(val);
                cfg.delete_after_upload = (v == "true" || v == "1" || v == "yes");
            }
        } catch (...) {
            LOG_WARN("Invalid config value for key '%s': '%s'", key.c_str(), val.c_str());
        }
    }
    in.close();
    if (!hadUserToken || !hadCustomUsername) {
        cfg.Save(path);
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
        << "user_token = " << user_token << "\n"
        << "auto_upload = " << (auto_upload ? "true" : "false") << "\n"
        << "delete_after_upload = " << (delete_after_upload ? "true" : "false") << "\n"
        << "server_url = " << server_url << "\n"
        << "upload_token = " << upload_token << "\n"
        << "copy_link_to_clipboard = " << (copy_link_to_clipboard ? "true" : "false") << "\n";

    return true;
}
