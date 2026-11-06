#include "tray_app.h"
#include "mp4_muxer.h"
#include "uploader.h"
#include "logger.h"
#include <psapi.h>
#include <algorithm>
#include <filesystem>
#include <ctime>
#include <cstdio>
#include <cctype>

namespace {
constexpr UINT WM_TRAYICON = WM_USER + 100;
constexpr UINT WM_AUTOSAVE_TEST = WM_USER + 101;
constexpr UINT WM_HOTKEY_REBOUND = WM_USER + 102;
constexpr UINT WM_SHOW_OSD = WM_USER + 103;
constexpr UINT WM_OSD_HIDE_TIMER = WM_USER + 104;
constexpr int HOTKEY_ID_SAVE = 1;
static TrayApp* g_trayAppInstance = nullptr;
static ULONGLONG g_lastHookTriggerMs = 0;
static std::mutex g_osdMutex;
static std::wstring g_osdTitle;
static std::wstring g_osdMessage;

static LRESULT CALLBACK OsdWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);

        // Dark background (#141821) + subtle border (#2A3142) + green accent bar (#22E55E)
        HBRUSH bgBrush = CreateSolidBrush(RGB(20, 24, 33));
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);

        HBRUSH borderBrush = CreateSolidBrush(RGB(42, 49, 66));
        FrameRect(hdc, &rc, borderBrush);
        DeleteObject(borderBrush);

        RECT barRc = {0, 0, 5, rc.bottom};
        HBRUSH barBrush = CreateSolidBrush(RGB(34, 229, 94));
        FillRect(hdc, &barRc, barBrush);
        DeleteObject(barBrush);

        std::wstring title, sub;
        {
            std::lock_guard<std::mutex> lk(g_osdMutex);
            title = g_osdTitle;
            sub = g_osdMessage;
        }

        SetBkMode(hdc, TRANSPARENT);
        HFONT hTitleFont = CreateFontW(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT hSubFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        HGDIOBJ oldFont = SelectObject(hdc, hTitleFont);
        SetTextColor(hdc, RGB(240, 244, 250));
        RECT titleRc = {16, 10, rc.right - 12, 32};
        DrawTextW(hdc, title.c_str(), -1, &titleRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        SelectObject(hdc, hSubFont);
        SetTextColor(hdc, RGB(154, 165, 180));
        RECT subRc = {16, 33, rc.right - 12, rc.bottom - 8};
        DrawTextW(hdc, sub.c_str(), -1, &subRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        SelectObject(hdc, oldFont);
        DeleteObject(hTitleFont);
        DeleteObject(hSubFont);

        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

enum MenuIds {
    IDM_SAVE_NOW = 1001,
    IDM_UPLOAD_LAST = 1002,
    IDM_OPEN_FOLDER = 1003,
    IDM_EDIT_CONFIG = 1004,
    IDM_RELOAD_CONFIG = 1005,
    IDM_TOGGLE_SWEEP = 1006,
    IDM_TOGGLE_AUTOUPLOAD = 1007,
    IDM_TOGGLE_AUTORUN = 1008,
    IDM_REBIND_HOTKEY = 1009,
    IDM_AUDIO_ACTIVE = 1010,
    IDM_AUDIO_DESKTOP = 1011,
    IDM_AUDIO_OFF = 1012,
    IDM_CHANGE_USERNAME = 1013,
    IDM_OPEN_GALLERY = 1014,
    IDM_COPY_LAST_LINK = 1015,
    IDM_TOGGLE_DELETE_ON_UPLOAD = 1016,
    IDM_SET_OUTPUT_DIR = 1017,
    IDM_SET_UPLOAD_TOKEN = 1018,
    IDM_SET_SERVER_URL = 1019,
    IDM_FPS_30 = 1020,
    IDM_FPS_60 = 1021,
    IDM_FPS_90 = 1022,
    IDM_FPS_120 = 1023,
    IDM_FPS_144 = 1024,
    IDM_FPS_CUSTOM = 1025,
    IDM_REPLAY_15 = 1030,
    IDM_REPLAY_30 = 1031,
    IDM_REPLAY_45 = 1032,
    IDM_REPLAY_60 = 1033,
    IDM_REPLAY_120 = 1034,
    IDM_REPLAY_300 = 1035,
    IDM_REPLAY_CUSTOM = 1036,
    IDM_UPDATE_APP = 1040,
    IDM_EXIT = 1099,
    IDM_MONITOR_BASE = 1100
};

static uint64_t GetQpcUs() {
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>((now.QuadPart * 1000000ULL) / freq.QuadPart);
}

static std::wstring ToWideStr(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), len);
    return w;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), len, nullptr, nullptr);
    return s;
}

static bool IsModifierVk(DWORD vk) {
    return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_LWIN || vk == VK_RWIN;
}

static std::string FormatHotkeyString(UINT mods, UINT vk) {
    std::string out;
    if (mods & MOD_CONTROL) out += "Ctrl+";
    if (mods & MOD_ALT)     out += "Alt+";
    if (mods & MOD_SHIFT)   out += "Shift+";
    if (mods & MOD_WIN)     out += "Win+";

    if (vk >= VK_F1 && vk <= VK_F24) {
        out += "F" + std::to_string((vk - VK_F1) + 1);
    } else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        out.push_back(static_cast<char>(vk));
    } else {
        UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        wchar_t keyName[64]{};
        if (GetKeyNameTextW(static_cast<LONG>(scan << 16), keyName, 64) > 0) {
            out += WideToUtf8(keyName);
        } else {
            out += "Key" + std::to_string(vk);
        }
    }
    return out;
}

// Detects the active foreground application name (process name or window title)
static std::string DetectForegroundAppName() {
    HWND fg = GetForegroundWindow();
    if (!fg) return "";

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return "";

    std::string exeStem;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        wchar_t pathBuf[MAX_PATH]{};
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &sz)) {
            exeStem = WideToUtf8(std::filesystem::path(pathBuf).stem().wstring());
        }
        CloseHandle(hProc);
    }

    // Ignore Windows shell chrome if another app was active
    std::string lowerStem = exeStem;
    std::transform(lowerStem.begin(), lowerStem.end(), lowerStem.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lowerStem == "explorer" || lowerStem == "shellexperiencehost" ||
        lowerStem == "searchhost" || lowerStem == "applicationframehost") {
        return "";
    }

    wchar_t titleBuf[256]{};
    GetWindowTextW(fg, titleBuf, 256);
    std::string title = WideToUtf8(titleBuf);

    // Prefer a clean process name or concise window title
    if (!exeStem.empty()) {
        // Capitalize first letter of executable stem if lowercase
        exeStem[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(exeStem[0])));
        if (!title.empty() && title.size() <= 42) {
            return title;
        }
        return exeStem;
    }
    return title;
}

static HICON CreateGreenDotIcon() {
    const int w = 16, h = 16;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w;
    bi.bV5Height = -h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC hdc = GetDC(nullptr);
    uint32_t* bits = nullptr;
    HBITMAP hColor = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi),
                                      DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hColor || !bits) return LoadIcon(nullptr, IDI_APPLICATION);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float dx = (x - 7.5f), dy = (y - 7.5f);
            float dist2 = dx * dx + dy * dy;
            if (dist2 <= 38.0f) {
                bits[y * w + x] = 0xFF22E55E;
            } else if (dist2 <= 49.0f) {
                bits[y * w + x] = 0xFF103018;
            } else {
                bits[y * w + x] = 0x00000000;
            }
        }
    }

    HBITMAP hMask = CreateBitmap(w, h, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = hMask;
    ii.hbmColor = hColor;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(hColor);
    DeleteObject(hMask);
    return icon ? icon : LoadIcon(nullptr, IDI_APPLICATION);
}

// Reusable modal text input dialog
struct TextInputDialogState {
    std::wstring label;
    std::wstring value;
    bool accepted = false;
    HWND hEdit = nullptr;
};

static LRESULT CALLBACK TextInputDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<TextInputDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_CREATE: {
            HFONT hFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            HWND hLbl = CreateWindowExW(0, L"STATIC", st->label.c_str(),
                                        WS_CHILD | WS_VISIBLE, 14, 14, 340, 20, hwnd, nullptr, nullptr, nullptr);
            st->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st->value.c_str(),
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                        14, 38, 340, 24, hwnd, reinterpret_cast<HMENU>(100), nullptr, nullptr);
            HWND hOk = CreateWindowExW(0, L"BUTTON", L"Save",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                       186, 74, 80, 26, hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
            HWND hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                           274, 74, 80, 26, hwnd, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
            SendMessageW(hLbl, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
            SendMessageW(st->hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
            SendMessageW(hOk, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
            SendMessageW(hCancel, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
            SetFocus(st->hEdit);
            SendMessageW(st->hEdit, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                wchar_t buf[512]{};
                GetWindowTextW(st->hEdit, buf, 512);
                st->value = buf;
                st->accepted = true;
                DestroyWindow(hwnd);
                return 0;
            } else if (LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool PromptTextDialog(HINSTANCE hInstance, const wchar_t* title, const wchar_t* label,
                             const std::string& initialUtf8, std::string& outUtf8) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = TextInputDlgProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"InstantReplayTextInputDlg";
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    TextInputDialogState state;
    state.label = label;
    state.value = ToWideStr(initialUtf8);

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int w = 380, h = 148;
    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"InstantReplayTextInputDlg",
        title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        (scrW - w) / 2, (scrH - h) / 2, w, h,
        nullptr, nullptr, hInstance, &state
    );
    if (!hDlg) return false;

    SetForegroundWindow(hDlg);
    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (state.accepted) {
        outUtf8 = WideToUtf8(state.value);
        return true;
    }
    return false;
}
} // namespace

TrayApp::~TrayApp() {
    Shutdown();
}

LRESULT CALLBACK TrayApp::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) && g_trayAppInstance) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

        // Interactive hotkey rebind mode
        if (g_trayAppInstance->m_rebindingHotkey.load()) {
            if (kb->vkCode == VK_ESCAPE) {
                g_trayAppInstance->m_rebindingHotkey = false;
                PostMessageW(g_trayAppInstance->m_hwnd, WM_HOTKEY_REBOUND, 0, 0);
                return 1;
            }
            if (!IsModifierVk(kb->vkCode)) {
                UINT mods = 0;
                if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0 || (kb->flags & LLKHF_ALTDOWN) != 0) mods |= MOD_ALT;
                if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) mods |= MOD_CONTROL;
                if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) mods |= MOD_SHIFT;
                if ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0) mods |= MOD_WIN;

                g_trayAppInstance->m_config.hotkey_modifiers = mods;
                g_trayAppInstance->m_config.hotkey_vk = kb->vkCode;
                g_trayAppInstance->m_config.hotkey_string = FormatHotkeyString(mods, kb->vkCode);
                g_trayAppInstance->m_rebindingHotkey = false;
                PostMessageW(g_trayAppInstance->m_hwnd, WM_HOTKEY_REBOUND, 1, 0);
                return 1;
            }
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        const UINT targetVk = g_trayAppInstance->m_config.hotkey_vk;
        if (kb->vkCode == targetVk) {
            const UINT mods = g_trayAppInstance->m_config.hotkey_modifiers;
            bool altReq   = (mods & MOD_ALT) != 0;
            bool ctrlReq  = (mods & MOD_CONTROL) != 0;
            bool shiftReq = (mods & MOD_SHIFT) != 0;
            bool winReq   = (mods & MOD_WIN) != 0;

            bool altDown   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 || (kb->flags & LLKHF_ALTDOWN) != 0;
            bool ctrlDown  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            bool winDown   = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;

            if (altReq == altDown && ctrlReq == ctrlDown && shiftReq == shiftDown && winReq == winDown) {
                ULONGLONG nowMs = GetTickCount64();
                if (nowMs - g_lastHookTriggerMs > 600) {
                    g_lastHookTriggerMs = nowMs;
                    PostMessageW(g_trayAppInstance->m_hwnd, WM_HOTKEY, HOTKEY_ID_SAVE, 0);
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

bool TrayApp::IsStartWithWindowsEnabled() const {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0, size = 0;
    bool exists = (RegQueryValueExW(hKey, L"InstantReplay", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && size > 0);
    RegCloseKey(hKey);
    return exists;
}

void TrayApp::SetStartWithWindows(bool enable) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exePath) + L"\"";
        RegSetValueExW(hKey, L"InstantReplay", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        LOG_INFO("Enabled 'Start with Windows' in HKCU\\...\\Run");
    } else {
        RegDeleteValueW(hKey, L"InstantReplay");
        LOG_INFO("Disabled 'Start with Windows'");
    }
    RegCloseKey(hKey);
}

void TrayApp::BindHotkey() {
    if (m_hotkeyRegistered) {
        UnregisterHotKey(m_hwnd, HOTKEY_ID_SAVE);
        m_hotkeyRegistered = false;
    }
    if (m_kbHook) {
        UnhookWindowsHookEx(m_kbHook);
        m_kbHook = nullptr;
    }

    m_hotkeyRegistered = (RegisterHotKey(m_hwnd, HOTKEY_ID_SAVE,
                                         m_config.hotkey_modifiers | MOD_NOREPEAT, m_config.hotkey_vk) != FALSE);
    if (m_hotkeyRegistered) {
        LOG_INFO("Registered global hotkey: %s", m_config.hotkey_string.c_str());
    } else {
        m_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, TrayApp::LowLevelKeyboardProc, m_hInstance, 0);
        if (m_kbHook) {
            LOG_INFO("Installed low-level keyboard hook for '%s'", m_config.hotkey_string.c_str());
        } else {
            LOG_WARN("Failed to register hotkey '%s' and fallback hook.", m_config.hotkey_string.c_str());
        }
    }

    std::wstring tip = L"InstantReplay - Always Recording (" + ToWideStr(m_config.hotkey_string) + L" to save)";
    wcsncpy_s(m_nid.szTip, tip.c_str(), _TRUNCATE);
    m_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

void TrayApp::StartHotkeyRebind() {
    if (m_hotkeyRegistered) {
        UnregisterHotKey(m_hwnd, HOTKEY_ID_SAVE);
        m_hotkeyRegistered = false;
    }
    if (!m_kbHook) {
        m_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, TrayApp::LowLevelKeyboardProc, m_hInstance, 0);
    }
    m_rebindingHotkey = true;
    ShowBalloon("Press New Hotkey Now",
                "Press any key combination (e.g. Alt+F10, Ctrl+Shift+S, F8), or Esc to cancel.");
}

void TrayApp::PromptChangeUsername() {
    std::string raw;
    if (PromptTextDialog(m_hInstance, L"InstantReplay - Set Username",
                         L"Username (for server clip gallery):", m_config.username, raw)) {
        std::string clean;
        for (char c : raw) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
                clean.push_back(c);
            }
        }
        if (!clean.empty()) {
            m_config.username = clean;
            m_config.Save(m_configPath);
            ShowBalloon("Username Updated", "Uploads will now be tagged under user: " + m_config.username);
        }
    }
}

void TrayApp::CopyMostRecentUploadLink() {
    if (m_lastShareUrl.empty()) {
        ShowBalloon("InstantReplay", "No clip uploaded in this session yet.");
        return;
    }
    Uploader::CopyToClipboard(m_lastShareUrl);
    ShowBalloon("Link Copied", m_lastShareUrl);
}

void TrayApp::ChangeFps(int fps) {
    fps = std::clamp(fps, 10, 240);
    if (fps == m_config.fps) return;
    m_config.fps = fps;
    m_config.Save(m_configPath);
    StartPipeline();
    ShowBalloon("Frame Rate Updated", "Now recording at " + std::to_string(m_config.fps) + " FPS");
}

void TrayApp::PromptCustomFps() {
    std::string raw;
    if (PromptTextDialog(m_hInstance, L"InstantReplay - Custom FPS",
                         L"Target frame rate (10 to 240 FPS):", std::to_string(m_config.fps), raw)) {
        try {
            int val = std::stoi(raw);
            ChangeFps(val);
        } catch (...) {
            ShowBalloon("Invalid FPS", "Please enter a number between 10 and 240.");
        }
    }
}

void TrayApp::ChangeReplaySeconds(int seconds) {
    seconds = std::clamp(seconds, 3, 600);
    if (seconds == m_config.replay_seconds) return;
    m_config.replay_seconds = seconds;
    m_config.Save(m_configPath);
    {
        std::lock_guard<std::mutex> lock(m_pipelineMutex);
        for (auto& s : m_streams) {
            if (s.buffer) s.buffer->SetMaxSeconds(m_config.replay_seconds);
        }
    }
    ShowBalloon("Replay Length Updated", "Buffer length set to last " + std::to_string(m_config.replay_seconds) + " seconds");
}

void TrayApp::PromptCustomReplaySeconds() {
    std::string raw;
    if (PromptTextDialog(m_hInstance, L"InstantReplay - Custom Replay Length",
                         L"Replay buffer duration in seconds (3 to 600):",
                         std::to_string(m_config.replay_seconds), raw)) {
        try {
            int val = std::stoi(raw);
            ChangeReplaySeconds(val);
        } catch (...) {
            ShowBalloon("Invalid Replay Length", "Please enter a number between 3 and 600.");
        }
    }
}

void TrayApp::PromptChangeOutputDir() {
    std::string raw;
    if (PromptTextDialog(m_hInstance, L"InstantReplay - Set Output Directory",
                         L"Folder path for saved clips (e.g. D:/Clips/ or captures/):",
                         m_config.output_dir, raw)) {
        while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t' || raw.front() == '"')) raw.erase(raw.begin());
        while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '"')) raw.pop_back();
        if (!raw.empty()) {
            if (raw.back() != '/' && raw.back() != '\\') raw += '/';
            std::error_code ec;
            std::filesystem::create_directories(raw, ec);
            if (!ec && std::filesystem::exists(raw)) {
                m_config.output_dir = raw;
                m_config.Save(m_configPath);
                ShowBalloon("Output Folder Updated", "Saving clips to: " + m_config.output_dir);
            } else {
                ShowBalloon("Invalid Output Folder", "Could not create or access: " + raw);
            }
        }
    }
}

void TrayApp::PromptChangeServerUrl() {
    std::string raw;
    if (PromptTextDialog(m_hInstance, L"InstantReplay - Set Server URL",
                         L"Share server URL (e.g. https://clips.example.com):",
                         m_config.server_url, raw)) {
        while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t' || raw.front() == '"')) raw.erase(raw.begin());
        while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '"' || raw.back() == '/')) raw.pop_back();
        size_t apiPos = raw.find("/api/");
        if (apiPos != std::string::npos) {
            raw = raw.substr(0, apiPos);
        }
        m_config.server_url = raw;
        m_config.Save(m_configPath);
        ShowBalloon("Server URL Updated",
                    m_config.server_url.empty() ? "Cleared server URL" : ("Server set to: " + m_config.server_url));
    }
}

void TrayApp::PromptChangeUploadToken() {
    std::string raw;
    if (PromptTextDialog(m_hInstance, L"InstantReplay - Set Upload Token",
                         L"Server upload bearer token (leave blank if none):",
                         m_config.upload_token, raw)) {
        while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t')) raw.erase(raw.begin());
        while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) raw.pop_back();
        m_config.upload_token = raw;
        m_config.Save(m_configPath);
        ShowBalloon("Upload Token Updated",
                    m_config.upload_token.empty() ? "Cleared upload token" : "Saved new upload bearer token");
    }
}

void TrayApp::OpenUserGallery() {
    std::string galleryUrl = m_config.GetGalleryUrl();
    ShellExecuteW(nullptr, L"open", ToWideStr(galleryUrl).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void TrayApp::CheckAndApplyUpdate() {
    ShowBalloon("Checking for Updates...", "Downloading latest InstantReplay.exe from GitHub Releases...");
    std::thread([this]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        wchar_t exePathW[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exePathW, MAX_PATH) == 0) {
            ShowBalloon("Update Failed", "Could not determine current executable path.");
            return;
        }
        std::filesystem::path curExe(exePathW);
        std::filesystem::path dir = curExe.parent_path();
        std::filesystem::path newExe = dir / "InstantReplay.new.exe";
        std::filesystem::path oldExe = dir / "InstantReplay.old.exe";

        std::string releaseTag;
        if (!Uploader::DownloadLatestReleaseExe(WideToUtf8(newExe.wstring()), releaseTag)) {
            ShowBalloon("Update Failed", "Could not download latest release from GitHub.");
            return;
        }

        // Windows allows renaming a currently running .exe on disk
        std::error_code ec;
        std::filesystem::remove(oldExe, ec);
        std::filesystem::rename(curExe, oldExe, ec);
        if (!ec) {
            std::filesystem::rename(newExe, curExe, ec);
            if (!ec) {
                ShowBalloon("Update Installed (" + releaseTag + ")", "Restarting InstantReplay...");
                Sleep(900);
                STARTUPINFOW si{};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                std::wstring cmd = L"\"" + curExe.wstring() + L"\" --restarted";
                std::wstring workDir = dir.wstring();
                if (CreateProcessW(curExe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0,
                                   nullptr, workDir.c_str(), &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
                if (m_hwnd) {
                    PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
                }
                return;
            } else {
                // Rollback if moving newExe failed
                std::filesystem::rename(oldExe, curExe, ec);
            }
        }

        // Fallback if filesystem prevented renaming the running .exe: spawn a tiny cmd swapper
        std::filesystem::path batPath = dir / "update_swap.bat";
        {
            FILE* f = nullptr;
            _wfopen_s(&f, batPath.c_str(), L"w");
            if (f) {
                fprintf(f, "@echo off\n"
                           "timeout /t 2 /nobreak >nul\n"
                           "move /y \"%s\" \"%s\" >nul\n"
                           "start \"\" \"%s\"\n"
                           "del \"%%~f0\"\n",
                        WideToUtf8(newExe.wstring()).c_str(),
                        WideToUtf8(curExe.wstring()).c_str(),
                        WideToUtf8(curExe.wstring()).c_str());
                fclose(f);
            }
        }
        ShowBalloon("Applying Update (" + releaseTag + ")", "Closing to replace executable...");
        Sleep(800);
        ShellExecuteW(nullptr, L"open", batPath.c_str(), nullptr, dir.c_str(), SW_HIDE);
        if (m_hwnd) {
            PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        }
    }).detach();
}

bool TrayApp::Init(HINSTANCE hInstance, const std::string& configPath, int autoSaveTestSeconds) {
    g_trayAppInstance = this;
    m_hInstance = hInstance;
    m_configPath = configPath;
    m_autoSaveTestSeconds = autoSaveTestSeconds;
    m_config = Config::Load(m_configPath);

    // Clean up any leftover .old.exe from a previous self-update
    {
        wchar_t exePathW[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exePathW, MAX_PATH) > 0) {
            std::error_code cleanEc;
            std::filesystem::remove(std::filesystem::path(exePathW).parent_path() / "InstantReplay.old.exe", cleanEc);
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(m_config.output_dir, ec);
    if (ec || !std::filesystem::exists(m_config.output_dir)) {
        LOG_WARN("Output directory '%s' unavailable (drive missing?); falling back to portable './captures/' folder.",
                 m_config.output_dir.c_str());
        m_config.output_dir = "captures/";
        std::filesystem::create_directories(m_config.output_dir, ec);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayApp::WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"InstantReplayTrayWnd";
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(0, wc.lpszClassName, L"InstantReplay", 0,
                             0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, this);
    if (!m_hwnd) {
        LOG_ERROR("Failed to create message window.");
        return false;
    }

    WNDCLASSEXW osdWc{};
    osdWc.cbSize = sizeof(osdWc);
    osdWc.lpfnWndProc = OsdWndProc;
    osdWc.hInstance = hInstance;
    osdWc.lpszClassName = L"InstantReplayOsdToast";
    RegisterClassExW(&osdWc);

    m_osdHwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        osdWc.lpszClassName, L"", WS_POPUP,
        0, 0, 350, 60, nullptr, nullptr, hInstance, nullptr
    );
    if (m_osdHwnd) {
        SetLayeredWindowAttributes(m_osdHwnd, 0, 238, LWA_ALPHA);
    }

    m_icon = CreateGreenDotIcon();
    m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd = m_hwnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAYICON;
    m_nid.hIcon = m_icon;
    std::wstring tip = L"InstantReplay - Always Recording (" + ToWideStr(m_config.hotkey_string) + L" to save)";
    wcsncpy_s(m_nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &m_nid);

    BindHotkey();

    if (!StartPipeline()) {
        LOG_ERROR("Failed to start capture/encode pipeline.");
        return false;
    }

    if (m_autoSaveTestSeconds > 0) {
        SetTimer(m_hwnd, WM_AUTOSAVE_TEST, m_autoSaveTestSeconds * 1000, nullptr);
        LOG_INFO("Auto-save self-test timer armed for %d seconds.", m_autoSaveTestSeconds);
    }

    return true;
}

bool TrayApp::StartPipeline() {
    StopPipeline();
    std::lock_guard<std::mutex> lock(m_pipelineMutex);

    if (!m_capture.Init(m_config.monitor_index)) {
        return false;
    }

    std::vector<int> bitrates;
    if (m_config.debug_bitrate_sweep && !m_config.sweep_bitrates_kbps.empty()) {
        bitrates = m_config.sweep_bitrates_kbps;
    } else {
        bitrates.push_back(m_config.bitrate_kbps);
    }

    for (int br : bitrates) {
        auto enc = std::make_unique<NvencEncoder>();
        if (!enc->Init(m_capture.GetDevice(), m_capture.GetFrameTexture(),
                       m_capture.GetWidth(), m_capture.GetHeight(),
                       m_config.fps, m_config.codec, br, m_config.preset)) {
            LOG_WARN("Could not initialize NVENC session for %d kbps (session limit reached?)", br);
            continue;
        }
        EncoderStream s;
        s.bitrate_kbps = br;
        s.encoder = std::move(enc);
        s.buffer = std::make_unique<ReplayBuffer>(m_config.replay_seconds);
        m_streams.push_back(std::move(s));
    }

    if (m_streams.empty()) {
        LOG_ERROR("No NVENC encoder streams could be initialized.");
        return false;
    }

    m_audio.Start(m_config.audio_mode, m_config.audio_bitrate_kbps, [this](const EncodedAudioPacket& pkt) {
        std::lock_guard<std::mutex> lk(m_pipelineMutex);
        for (auto& s : m_streams) {
            s.buffer->PushAudio(pkt);
        }
    });

    m_capturing = true;
    m_captureThread = std::thread(&TrayApp::CaptureLoop, this);
    return true;
}

void TrayApp::StopPipeline() {
    m_capturing = false;
    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }
    m_audio.Stop();
    std::lock_guard<std::mutex> lock(m_pipelineMutex);
    m_streams.clear();
    m_capture.Shutdown();
}

void TrayApp::CaptureLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    const uint64_t frameIntervalUs = 1000000ULL / (m_config.fps > 0 ? m_config.fps : 60);
    uint64_t nextFrameTargetUs = GetQpcUs();
    uint64_t frameCount = 0;

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    uint64_t statWindowStartUs = GetQpcUs();
    uint64_t lastAppCheckUs = 0;
    std::vector<uint64_t> capTimesUs;
    std::vector<uint64_t> encTimesUs;
    capTimesUs.reserve(600);
    encTimesUs.reserve(600);
    uint64_t windowFrames = 0;
    uint64_t missedTicks = 0;

    FILETIME prevSysKernel{}, prevSysUser{}, prevProcKernel{}, prevProcUser{}, dummyFt{};
    GetSystemTimes(nullptr, &prevSysKernel, &prevSysUser);
    GetProcessTimes(GetCurrentProcess(), &dummyFt, &dummyFt, &prevProcKernel, &prevProcUser);

    auto ftToU64 = [](const FILETIME& ft) -> uint64_t {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };

    while (m_capturing) {
        uint64_t nowUs = GetQpcUs();
        if (nowUs < nextFrameTargetUs) {
            uint64_t waitUs = nextFrameTargetUs - nowUs;
            if (timer && waitUs > 500) {
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(waitUs * 10);
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 100);
            } else {
                Sleep(1);
            }
            continue;
        }

        nextFrameTargetUs += frameIntervalUs;
        if (nowUs > nextFrameTargetUs + frameIntervalUs * 2) {
            missedTicks += (nowUs - nextFrameTargetUs) / frameIntervalUs;
            nextFrameTargetUs = nowUs + frameIntervalUs;
        }

        // Poll foreground application name every 500ms
        if (nowUs - lastAppCheckUs >= 500000ULL) {
            lastAppCheckUs = nowUs;
            std::string fgApp = DetectForegroundAppName();
            if (!fgApp.empty()) {
                std::lock_guard<std::mutex> lock(m_pipelineMutex);
                m_lastActiveAppName = fgApp;
            }
        }

        bool needReinit = false;
        bool hasFrame = false;
        {
            std::lock_guard<std::mutex> lock(m_pipelineMutex);
            uint64_t t0 = GetQpcUs();
            hasFrame = m_capture.AcquireFrame(0, needReinit);
            uint64_t t1 = GetQpcUs();
            if (hasFrame) {
                bool forceIdr = (frameCount == 0);
                for (auto& s : m_streams) {
                    EncodedVideoPacket pkt;
                    if (s.encoder->EncodeFrame(t1, frameIntervalUs, forceIdr, pkt)) {
                        s.buffer->PushVideo(std::move(pkt));
                    }
                }
                uint64_t t2 = GetQpcUs();
                capTimesUs.push_back(t1 - t0);
                encTimesUs.push_back(t2 - t1);
                frameCount++;
                windowFrames++;
            }
        }

        uint64_t elapsedWindowUs = GetQpcUs() - statWindowStartUs;
        if (elapsedWindowUs >= 5000000ULL && !capTimesUs.empty()) {
            std::sort(capTimesUs.begin(), capTimesUs.end());
            std::sort(encTimesUs.begin(), encTimesUs.end());

            auto avgOf = [](const std::vector<uint64_t>& v) {
                uint64_t sum = 0;
                for (uint64_t x : v) sum += x;
                return (sum / (double)v.size()) / 1000.0;
            };
            double capAvgMs = avgOf(capTimesUs);
            double capP99Ms = capTimesUs[(capTimesUs.size() * 99) / 100] / 1000.0;
            double capMaxMs = capTimesUs.back() / 1000.0;

            double encAvgMs = avgOf(encTimesUs);
            double encP99Ms = encTimesUs[(encTimesUs.size() * 99) / 100] / 1000.0;
            double encMaxMs = encTimesUs.back() / 1000.0;

            double actualFps = (windowFrames * 1000000.0) / elapsedWindowUs;

            FILETIME curSysKernel{}, curSysUser{}, curProcKernel{}, curProcUser{};
            GetSystemTimes(nullptr, &curSysKernel, &curSysUser);
            GetProcessTimes(GetCurrentProcess(), &dummyFt, &dummyFt, &curProcKernel, &curProcUser);

            uint64_t sysDelta = (ftToU64(curSysKernel) - ftToU64(prevSysKernel)) +
                                (ftToU64(curSysUser) - ftToU64(prevSysUser));
            uint64_t procDelta = (ftToU64(curProcKernel) - ftToU64(prevProcKernel)) +
                                 (ftToU64(curProcUser) - ftToU64(prevProcUser));
            double cpuPct = (sysDelta > 0) ? (100.0 * procDelta / (double)sysDelta) : 0.0;

            prevSysKernel = curSysKernel;
            prevSysUser = curSysUser;
            prevProcKernel = curProcKernel;
            prevProcUser = curProcUser;

            PROCESS_MEMORY_COUNTERS_EX pmc{};
            pmc.cb = sizeof(pmc);
            GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
            double workingSetMb = pmc.WorkingSetSize / (1024.0 * 1024.0);
            double privateMb = pmc.PrivateUsage / (1024.0 * 1024.0);

            size_t ringBytes = 0;
            size_t streamCount = 0;
            {
                std::lock_guard<std::mutex> lock(m_pipelineMutex);
                streamCount = m_streams.size();
                for (const auto& s : m_streams) ringBytes += s.buffer->GetMemoryBytes();
            }
            double ringMb = ringBytes / (1024.0 * 1024.0);

            LOG_INFO("[PERF] fps=%.1f missed=%llu streams=%zu | CPU=%.2f%% RAM_WS=%.1fMB RAM_Priv=%.1fMB RingBuf=%.1fMB | "
                     "DXGI_ms(avg=%.3f p99=%.3f max=%.3f) NVENC_ms(avg=%.3f p99=%.3f max=%.3f)",
                     actualFps, (unsigned long long)missedTicks, streamCount,
                     cpuPct, workingSetMb, privateMb, ringMb,
                     capAvgMs, capP99Ms, capMaxMs,
                     encAvgMs, encP99Ms, encMaxMs);

            capTimesUs.clear();
            encTimesUs.clear();
            windowFrames = 0;
            missedTicks = 0;
            statWindowStartUs = GetQpcUs();
        }

        if (needReinit) {
            Sleep(250);
            std::lock_guard<std::mutex> lock(m_pipelineMutex);
            if (m_capture.Init(m_config.monitor_index)) {
                for (auto& s : m_streams) {
                    s.encoder->Init(m_capture.GetDevice(), m_capture.GetFrameTexture(),
                                    m_capture.GetWidth(), m_capture.GetHeight(),
                                    m_config.fps, m_config.codec, s.bitrate_kbps, m_config.preset);
                }
                frameCount = 0;
            }
        }
    }

    if (timer) CloseHandle(timer);
}

void TrayApp::TriggerSaveReplay() {
    bool expected = false;
    if (!m_saving.compare_exchange_strong(expected, true)) {
        LOG_INFO("Save already in progress; ignoring duplicate trigger.");
        return;
    }

    std::vector<int> bitrates;
    std::vector<std::vector<uint8_t>> seqParams;
    std::vector<ReplaySnapshot> snapshots;
    VideoCodec codec = m_config.codec;
    int width = 0, height = 0, fps = m_config.fps;
    std::string appName = "Desktop";

    {
        std::lock_guard<std::mutex> lock(m_pipelineMutex);
        if (m_streams.empty()) {
            m_saving = false;
            return;
        }
        width = m_capture.GetWidth();
        height = m_capture.GetHeight();
        codec = m_streams[0].encoder->GetCodec();

        std::string fgNow = DetectForegroundAppName();
        if (!fgNow.empty()) m_lastActiveAppName = fgNow;
        appName = m_lastActiveAppName;

        for (auto& s : m_streams) {
            bitrates.push_back(s.bitrate_kbps);
            seqParams.push_back(s.encoder->GetSeqParams());
            snapshots.push_back(s.buffer->Snapshot());
        }
    }

    std::vector<uint8_t> asc = m_audio.GetAudioSpecificConfig();
    int audioRate = m_audio.GetSampleRate();
    int audioCh = m_audio.GetChannels();

    std::thread(&TrayApp::SaveWorker, this,
                std::move(bitrates), std::move(seqParams), std::move(snapshots),
                codec, width, height, fps, std::move(asc), audioRate, audioCh,
                std::move(appName)).detach();
}

void TrayApp::SaveWorker(
    std::vector<int> bitrates,
    std::vector<std::vector<uint8_t>> seqParams,
    std::vector<ReplaySnapshot> snapshots,
    VideoCodec codec, int width, int height, int fps,
    std::vector<uint8_t> asc, int audioRate, int audioCh,
    std::string recordedAppName
) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    std::error_code ec;
    std::filesystem::create_directories(m_config.output_dir, ec);

    std::time_t now = std::time(nullptr);
    std::tm tm_now{};
    localtime_s(&tm_now, &now);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm_now);

    const char* codecTag = (codec == VideoCodec::AV1) ? "av1" : (codec == VideoCodec::HEVC ? "hevc" : "h264");
    bool multi = bitrates.size() > 1;
    std::string primarySaved;

    for (size_t i = 0; i < bitrates.size(); ++i) {
        char fname[256];
        if (multi) {
            snprintf(fname, sizeof(fname), "Replay_%s_%s_%dkbps.mp4", stamp, codecTag, bitrates[i]);
        } else {
            snprintf(fname, sizeof(fname), "Replay_%s.mp4", stamp);
        }
        std::string fullPath = m_config.output_dir + fname;
        if (Mp4Muxer::WriteClip(fullPath, codec, width, height, fps,
                                seqParams[i], asc, audioRate, audioCh, snapshots[i])) {
            primarySaved = fullPath;
        }
    }

    if (!primarySaved.empty()) {
        m_lastSavedPath = primarySaved;
        m_lastSavedAppName = recordedAppName;
        if (m_config.auto_upload) {
            ShowBalloon("Replay Saved (" + recordedAppName + ")",
                        "Saved last " + std::to_string(m_config.replay_seconds) + "s — uploading clip...");
            std::string shareUrl;
            if (Uploader::UploadFile(primarySaved, m_config.GetUploadEndpoint(), m_config.upload_token,
                                     m_config.username, m_config.user_token, recordedAppName, shareUrl)) {
                m_lastShareUrl = shareUrl;
                if (m_config.copy_link_to_clipboard) {
                    Uploader::CopyToClipboard(shareUrl);
                }
                if (m_config.delete_after_upload) {
                    std::error_code delEc;
                    std::filesystem::remove(primarySaved, delEc);
                    if (!delEc) {
                        LOG_INFO("Deleted local video after upload: %s", primarySaved.c_str());
                        if (m_lastSavedPath == primarySaved) m_lastSavedPath.clear();
                    }
                    ShowBalloon("Uploaded & Local File Deleted", "Share link copied: " + shareUrl);
                } else {
                    ShowBalloon("Replay Uploaded", "Share link copied: " + shareUrl);
                }
            } else {
                ShowBalloon("Replay Saved (Upload Failed)", primarySaved);
            }
        } else {
            if (multi) {
                ShowBalloon("Bitrate Sweep Saved",
                            "Saved " + std::to_string(bitrates.size()) + " clips to " + m_config.output_dir);
            } else {
                ShowBalloon("Replay Saved (" + recordedAppName + ")", primarySaved);
            }
        }
    }

    m_saving = false;
    if (m_autoSaveTestSeconds > 0 && m_hwnd) {
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    }
}

void TrayApp::UploadLatestClip() {
    if (m_lastSavedPath.empty()) {
        ShowBalloon("InstantReplay", "No clip saved in this session yet.");
        return;
    }
    std::string path = m_lastSavedPath;
    std::string appName = m_lastSavedAppName;
    std::thread([this, path, appName]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        std::string shareUrl;
        if (Uploader::UploadFile(path, m_config.GetUploadEndpoint(), m_config.upload_token,
                                 m_config.username, m_config.user_token, appName, shareUrl)) {
            m_lastShareUrl = shareUrl;
            if (m_config.copy_link_to_clipboard) {
                Uploader::CopyToClipboard(shareUrl);
            }
            if (m_config.delete_after_upload) {
                std::error_code delEc;
                std::filesystem::remove(path, delEc);
                if (!delEc) {
                    LOG_INFO("Deleted local video after upload: %s", path.c_str());
                    if (m_lastSavedPath == path) m_lastSavedPath.clear();
                }
                ShowBalloon("Clip Uploaded & Local Deleted", "Share link copied: " + shareUrl);
            } else {
                ShowBalloon("Clip Uploaded", "Share link copied: " + shareUrl);
            }
        } else {
            ShowBalloon("Upload Failed", "Check server status and server_url in config.ini");
        }
    }).detach();
}

void TrayApp::SwitchMonitor(int monitorIndex) {
    if (monitorIndex == m_config.monitor_index) return;
    m_config.monitor_index = monitorIndex;
    m_config.Save(m_configPath);
    StartPipeline();
    ShowBalloon("Monitor Changed", "Now capturing monitor " + std::to_string(monitorIndex));
}

void TrayApp::ToggleBitrateSweep() {
    m_config.debug_bitrate_sweep = !m_config.debug_bitrate_sweep;
    m_config.Save(m_configPath);
    StartPipeline();
    ShowBalloon("Bitrate Sweep Mode",
                m_config.debug_bitrate_sweep ? "Enabled simultaneous multi-bitrate encoding"
                                             : "Disabled (single bitrate mode)");
}

void TrayApp::ReloadConfig() {
    m_config = Config::Load(m_configPath);
    BindHotkey();
    StartPipeline();
    ShowBalloon("Config Reloaded", "Applied updated settings from " + m_configPath);
}

void TrayApp::ShowBalloon(const std::string& title, const std::string& message) {
    if (!m_hwnd) return;
    {
        std::lock_guard<std::mutex> lk(g_osdMutex);
        g_osdTitle = ToWideStr(title);
        g_osdMessage = ToWideStr(message);
    }
    PostMessageW(m_hwnd, WM_SHOW_OSD, 0, 0);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO | NIF_REALTIME;
    nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    wcsncpy_s(nid.szInfoTitle, ToWideStr(title).c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, ToWideStr(message).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayApp::ShowContextMenu() {
    HMENU hMenu = CreatePopupMenu();
    std::wstring saveLabel = L"Save Last " + std::to_wstring(m_config.replay_seconds) +
                             L"s (" + ToWideStr(m_config.hotkey_string) + L")";
    AppendMenuW(hMenu, MF_STRING, IDM_SAVE_NOW, saveLabel.c_str());
    AppendMenuW(hMenu, MF_STRING | (m_lastSavedPath.empty() ? MF_GRAYED : 0),
                IDM_UPLOAD_LAST, L"Upload Last Saved Clip && Copy Link");
    AppendMenuW(hMenu, MF_STRING | (m_lastShareUrl.empty() ? MF_GRAYED : 0),
                IDM_COPY_LAST_LINK, L"Copy Link for Most Recent Upload");
    std::wstring galleryLabel = L"Open My Clip Gallery (" + ToWideStr(m_config.username) + L")";
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN_GALLERY, galleryLabel.c_str());
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // Monitor submenu
    HMENU hMonMenu = CreatePopupMenu();
    for (const auto& m : m_capture.GetMonitors()) {
        UINT flags = MF_STRING | (m.index == m_capture.GetActiveMonitorIndex() ? MF_CHECKED : 0);
        AppendMenuW(hMonMenu, flags, IDM_MONITOR_BASE + m.index, ToWideStr(m.name).c_str());
    }
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hMonMenu), L"Select Monitor");

    // FPS submenu
    HMENU hFpsMenu = CreatePopupMenu();
    const int fpsPresets[] = {30, 60, 90, 120, 144};
    const int fpsIds[]     = {IDM_FPS_30, IDM_FPS_60, IDM_FPS_90, IDM_FPS_120, IDM_FPS_144};
    bool fpsMatched = false;
    for (size_t i = 0; i < 5; ++i) {
        bool checked = (m_config.fps == fpsPresets[i]);
        if (checked) fpsMatched = true;
        std::wstring lbl = std::to_wstring(fpsPresets[i]) + L" FPS";
        AppendMenuW(hFpsMenu, MF_STRING | (checked ? MF_CHECKED : 0), fpsIds[i], lbl.c_str());
    }
    std::wstring customFpsLbl = fpsMatched
        ? L"Custom..."
        : (L"Custom (" + std::to_wstring(m_config.fps) + L" FPS)...");
    AppendMenuW(hFpsMenu, MF_STRING | (!fpsMatched ? MF_CHECKED : 0), IDM_FPS_CUSTOM, customFpsLbl.c_str());
    std::wstring fpsRootLbl = L"Frame Rate (" + std::to_wstring(m_config.fps) + L" FPS)";
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hFpsMenu), fpsRootLbl.c_str());

    // Replay Seconds submenu
    HMENU hRepMenu = CreatePopupMenu();
    const int repPresets[] = {15, 30, 45, 60, 120, 300};
    const int repIds[]     = {IDM_REPLAY_15, IDM_REPLAY_30, IDM_REPLAY_45, IDM_REPLAY_60, IDM_REPLAY_120, IDM_REPLAY_300};
    bool repMatched = false;
    for (size_t i = 0; i < 6; ++i) {
        bool checked = (m_config.replay_seconds == repPresets[i]);
        if (checked) repMatched = true;
        std::wstring lbl = std::to_wstring(repPresets[i]) + L" seconds";
        AppendMenuW(hRepMenu, MF_STRING | (checked ? MF_CHECKED : 0), repIds[i], lbl.c_str());
    }
    std::wstring customRepLbl = repMatched
        ? L"Custom..."
        : (L"Custom (" + std::to_wstring(m_config.replay_seconds) + L"s)...");
    AppendMenuW(hRepMenu, MF_STRING | (!repMatched ? MF_CHECKED : 0), IDM_REPLAY_CUSTOM, customRepLbl.c_str());
    std::wstring repRootLbl = L"Replay Length (" + std::to_wstring(m_config.replay_seconds) + L"s)";
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hRepMenu), repRootLbl.c_str());

    // Audio mode submenu
    HMENU hAudMenu = CreatePopupMenu();
    AppendMenuW(hAudMenu, MF_STRING | (m_config.audio_mode == AudioMode::ActiveWindow ? MF_CHECKED : 0),
                IDM_AUDIO_ACTIVE, L"Active Window Only (Process Loopback)");
    AppendMenuW(hAudMenu, MF_STRING | (m_config.audio_mode == AudioMode::Desktop ? MF_CHECKED : 0),
                IDM_AUDIO_DESKTOP, L"Full Desktop Audio");
    AppendMenuW(hAudMenu, MF_STRING | (m_config.audio_mode == AudioMode::Off ? MF_CHECKED : 0),
                IDM_AUDIO_OFF, L"Muted / No Audio");
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hAudMenu), L"Audio Source");

    std::wstring rebindLabel = L"Change Save Hotkey... (Current: " + ToWideStr(m_config.hotkey_string) + L")";
    AppendMenuW(hMenu, MF_STRING, IDM_REBIND_HOTKEY, rebindLabel.c_str());

    std::wstring outDirLabel = L"Set Output Folder... (" + ToWideStr(m_config.output_dir) + L")";
    AppendMenuW(hMenu, MF_STRING, IDM_SET_OUTPUT_DIR, outDirLabel.c_str());

    std::wstring userLabel = L"Set Username... (Current: " + ToWideStr(m_config.username) + L")";
    AppendMenuW(hMenu, MF_STRING, IDM_CHANGE_USERNAME, userLabel.c_str());

    std::wstring urlStatus = m_config.server_url.empty() ? L"Not Set" : ToWideStr(m_config.server_url);
    std::wstring urlLabel = L"Set Server URL... (" + urlStatus + L")";
    AppendMenuW(hMenu, MF_STRING, IDM_SET_SERVER_URL, urlLabel.c_str());

    std::wstring tokenStatus = m_config.upload_token.empty() ? L"Not Set" : L"Configured";
    std::wstring tokenLabel = L"Set Upload Token... (" + tokenStatus + L")";
    AppendMenuW(hMenu, MF_STRING, IDM_SET_UPLOAD_TOKEN, tokenLabel.c_str());

    AppendMenuW(hMenu, MF_STRING | (m_config.auto_upload ? MF_CHECKED : 0),
                IDM_TOGGLE_AUTOUPLOAD, L"Auto-Upload on Save");
    AppendMenuW(hMenu, MF_STRING | (m_config.delete_after_upload ? MF_CHECKED : 0),
                IDM_TOGGLE_DELETE_ON_UPLOAD, L"Delete Local Video After Upload");
    AppendMenuW(hMenu, MF_STRING | (m_config.debug_bitrate_sweep ? MF_CHECKED : 0),
                IDM_TOGGLE_SWEEP, L"Debug: Bitrate Sweep Mode");
    AppendMenuW(hMenu, MF_STRING | (IsStartWithWindowsEnabled() ? MF_CHECKED : 0),
                IDM_TOGGLE_AUTORUN, L"Start with Windows (Always Recording)");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN_FOLDER, L"Open Captures Folder");
    AppendMenuW(hMenu, MF_STRING, IDM_EDIT_CONFIG, L"Edit config.ini");
    AppendMenuW(hMenu, MF_STRING, IDM_RELOAD_CONFIG, L"Reload config.ini");
    AppendMenuW(hMenu, MF_STRING, IDM_UPDATE_APP, L"Check for Updates (GitHub Release)");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(m_hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(hMenu);
}

LRESULT CALLBACK TrayApp::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TrayApp* app = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<TrayApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!app) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_HOTKEY:
            if (wParam == HOTKEY_ID_SAVE) {
                LOG_INFO("Global save hotkey pressed!");
                app->TriggerSaveReplay();
            }
            return 0;

        case WM_HOTKEY_REBOUND:
            app->BindHotkey();
            if (wParam == 1) {
                app->m_config.Save(app->m_configPath);
                app->ShowBalloon("Hotkey Updated", "Save hotkey bound to: " + app->m_config.hotkey_string);
            } else {
                app->ShowBalloon("Hotkey Rebind Cancelled", "Kept hotkey: " + app->m_config.hotkey_string);
            }
            return 0;

        case WM_SHOW_OSD:
            if (app->m_osdHwnd) {
                int scrW = GetSystemMetrics(SM_CXSCREEN);
                int w = 350, h = 60;
                SetWindowPos(app->m_osdHwnd, HWND_TOPMOST, scrW - w - 24, 24, w, h,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW);
                InvalidateRect(app->m_osdHwnd, nullptr, TRUE);
                UpdateWindow(app->m_osdHwnd);
                SetTimer(hwnd, WM_OSD_HIDE_TIMER, 3200, nullptr);
            }
            return 0;

        case WM_TIMER:
            if (wParam == WM_AUTOSAVE_TEST) {
                KillTimer(hwnd, WM_AUTOSAVE_TEST);
                LOG_INFO("Auto-save timer fired; saving replay clip now.");
                app->TriggerSaveReplay();
            } else if (wParam == WM_OSD_HIDE_TIMER) {
                KillTimer(hwnd, WM_OSD_HIDE_TIMER);
                if (app->m_osdHwnd) {
                    ShowWindow(app->m_osdHwnd, SW_HIDE);
                }
            }
            return 0;

        case WM_TRAYICON:
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
                app->ShowContextMenu();
            } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                ShellExecuteW(nullptr, L"open", ToWideStr(app->m_config.output_dir).c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDM_SAVE_NOW) app->TriggerSaveReplay();
            else if (id == IDM_UPLOAD_LAST) app->UploadLatestClip();
            else if (id == IDM_COPY_LAST_LINK) app->CopyMostRecentUploadLink();
            else if (id == IDM_OPEN_GALLERY) app->OpenUserGallery();
            else if (id == IDM_REBIND_HOTKEY) app->StartHotkeyRebind();
            else if (id == IDM_SET_OUTPUT_DIR) app->PromptChangeOutputDir();
            else if (id == IDM_CHANGE_USERNAME) app->PromptChangeUsername();
            else if (id == IDM_SET_SERVER_URL) app->PromptChangeServerUrl();
            else if (id == IDM_SET_UPLOAD_TOKEN) app->PromptChangeUploadToken();
            else if (id == IDM_FPS_30) app->ChangeFps(30);
            else if (id == IDM_FPS_60) app->ChangeFps(60);
            else if (id == IDM_FPS_90) app->ChangeFps(90);
            else if (id == IDM_FPS_120) app->ChangeFps(120);
            else if (id == IDM_FPS_144) app->ChangeFps(144);
            else if (id == IDM_FPS_CUSTOM) app->PromptCustomFps();
            else if (id == IDM_REPLAY_15) app->ChangeReplaySeconds(15);
            else if (id == IDM_REPLAY_30) app->ChangeReplaySeconds(30);
            else if (id == IDM_REPLAY_45) app->ChangeReplaySeconds(45);
            else if (id == IDM_REPLAY_60) app->ChangeReplaySeconds(60);
            else if (id == IDM_REPLAY_120) app->ChangeReplaySeconds(120);
            else if (id == IDM_REPLAY_300) app->ChangeReplaySeconds(300);
            else if (id == IDM_REPLAY_CUSTOM) app->PromptCustomReplaySeconds();
            else if (id == IDM_OPEN_FOLDER) {
                ShellExecuteW(nullptr, L"open", ToWideStr(app->m_config.output_dir).c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
            else if (id == IDM_EDIT_CONFIG) {
                ShellExecuteW(nullptr, L"open", L"notepad.exe",
                              ToWideStr(app->m_configPath).c_str(), nullptr, SW_SHOWNORMAL);
            }
            else if (id == IDM_RELOAD_CONFIG) app->ReloadConfig();
            else if (id == IDM_UPDATE_APP) app->CheckAndApplyUpdate();
            else if (id == IDM_TOGGLE_SWEEP) app->ToggleBitrateSweep();
            else if (id == IDM_TOGGLE_AUTOUPLOAD) {
                app->m_config.auto_upload = !app->m_config.auto_upload;
                app->m_config.Save(app->m_configPath);
            }
            else if (id == IDM_TOGGLE_DELETE_ON_UPLOAD) {
                app->m_config.delete_after_upload = !app->m_config.delete_after_upload;
                app->m_config.Save(app->m_configPath);
            }
            else if (id == IDM_TOGGLE_AUTORUN) {
                app->SetStartWithWindows(!app->IsStartWithWindowsEnabled());
            }
            else if (id == IDM_AUDIO_ACTIVE || id == IDM_AUDIO_DESKTOP || id == IDM_AUDIO_OFF) {
                app->m_config.audio_mode = (id == IDM_AUDIO_ACTIVE) ? AudioMode::ActiveWindow :
                                           (id == IDM_AUDIO_DESKTOP ? AudioMode::Desktop : AudioMode::Off);
                app->m_config.Save(app->m_configPath);
                app->StartPipeline();
            }
            else if (id >= IDM_MONITOR_BASE && id < IDM_MONITOR_BASE + 32) {
                app->SwitchMonitor(id - IDM_MONITOR_BASE);
            }
            else if (id == IDM_EXIT) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int TrayApp::Run() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Shutdown();
    return static_cast<int>(msg.wParam);
}

void TrayApp::Shutdown() {
    StopPipeline();
    if (m_kbHook) {
        UnhookWindowsHookEx(m_kbHook);
        m_kbHook = nullptr;
    }
    if (m_osdHwnd) {
        DestroyWindow(m_osdHwnd);
        m_osdHwnd = nullptr;
    }
    if (m_hwnd) {
        if (m_hotkeyRegistered) {
            UnregisterHotKey(m_hwnd, HOTKEY_ID_SAVE);
            m_hotkeyRegistered = false;
        }
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_hwnd = nullptr;
    }
    if (m_icon) {
        DestroyIcon(m_icon);
        m_icon = nullptr;
    }
    if (g_trayAppInstance == this) {
        g_trayAppInstance = nullptr;
    }
}
