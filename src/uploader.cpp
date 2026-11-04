#include "uploader.h"
#include "logger.h"
#include <windows.h>
#include <winhttp.h>
#include <fstream>
#include <vector>
#include <filesystem>

static std::wstring ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), len);
    return w;
}

static std::string ExtractJsonField(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";
    size_t colon = json.find(':', pos + pattern.size());
    if (colon == std::string::npos) return "";
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

static std::string SanitizeHeaderVal(std::string s) {
    for (char& c : s) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    return s;
}

bool Uploader::UploadFile(
    const std::string& filePath,
    const std::string& uploadUrl,
    const std::string& bearerToken,
    const std::string& username,
    const std::string& appName,
    std::string& outShareUrl
) {
    std::ifstream in(filePath, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        LOG_ERROR("Uploader: cannot open '%s'", filePath.c_str());
        return false;
    }
    uint64_t fileSize = static_cast<uint64_t>(in.tellg());
    in.seekg(0, std::ios::beg);

    std::wstring wUrl = ToWide(uploadUrl);
    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostName[256]{};
    wchar_t urlPath[1024]{};
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = 1024;

    if (!WinHttpCrackUrl(wUrl.c_str(), (DWORD)wUrl.size(), 0, &urlComp)) {
        LOG_ERROR("Uploader: invalid upload_url '%s'", uploadUrl.c_str());
        return false;
    }

    HINTERNET hSession = WinHttpOpen(L"InstantReplay/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", urlPath,
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::string filename = std::filesystem::path(filePath).filename().string();
    std::string headersUtf8 = "Content-Type: video/mp4\r\n"
                              "Authorization: Bearer " + SanitizeHeaderVal(bearerToken) + "\r\n"
                              "X-Filename: " + SanitizeHeaderVal(filename) + "\r\n"
                              "X-Username: " + SanitizeHeaderVal(username) + "\r\n"
                              "X-App-Name: " + SanitizeHeaderVal(appName) + "\r\n";
    std::wstring headersWide = ToWide(headersUtf8);

    BOOL ok = WinHttpSendRequest(
        hRequest,
        headersWide.c_str(),
        (DWORD)headersWide.size(),
        WINHTTP_NO_REQUEST_DATA,
        0,
        (DWORD)fileSize,
        0
    );

    std::vector<char> chunk(256 * 1024);
    while (ok && in.good()) {
        in.read(chunk.data(), chunk.size());
        std::streamsize n = in.gcount();
        if (n <= 0) break;
        DWORD written = 0;
        ok = WinHttpWriteData(hRequest, chunk.data(), (DWORD)n, &written);
    }

    if (ok) {
        ok = WinHttpReceiveResponse(hRequest, nullptr);
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (ok) {
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    }

    std::string responseBody;
    if (ok) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (WinHttpReadData(hRequest, buf.data(), avail, &read)) {
                responseBody.append(buf.data(), read);
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (!ok || (statusCode != 200 && statusCode != 201)) {
        LOG_ERROR("Uploader: HTTP POST failed (status %lu, body: %s)",
                  (unsigned long)statusCode, responseBody.c_str());
        return false;
    }

    outShareUrl = ExtractJsonField(responseBody, "url");
    if (outShareUrl.empty()) {
        LOG_ERROR("Uploader: missing 'url' in server response: %s", responseBody.c_str());
        return false;
    }

    LOG_INFO("Uploaded '%s' -> %s", filename.c_str(), outShareUrl.c_str());
    return true;
}

bool Uploader::CopyToClipboard(const std::string& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (!hMem) {
        CloseClipboard();
        return false;
    }
    char* dst = static_cast<char*>(GlobalLock(hMem));
    memcpy(dst, text.c_str(), text.size() + 1);
    GlobalUnlock(hMem);
    SetClipboardData(CF_TEXT, hMem);
    CloseClipboard();
    return true;
}
