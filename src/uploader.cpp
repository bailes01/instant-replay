#include "uploader.h"
#include "logger.h"
#include <windows.h>
#include <winhttp.h>
#include <fstream>
#include <vector>
#include <filesystem>
#include <random>
#include <algorithm>

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

static std::string GenerateUploadId() {
    static const char kChars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::random_device rd;
    std::mt19937_64 gen((static_cast<uint64_t>(rd()) << 32) ^ rd() ^ GetTickCount64());
    std::uniform_int_distribution<size_t> dist(0, sizeof(kChars) - 2);
    std::string id;
    id.reserve(16);
    for (int i = 0; i < 16; ++i) {
        id.push_back(kChars[dist(gen)]);
    }
    return id;
}

bool Uploader::UploadFile(
    const std::string& filePath,
    const std::string& uploadUrl,
    const std::string& bearerToken,
    const std::string& username,
    const std::string& userToken,
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
    if (fileSize == 0) {
        LOG_ERROR("Uploader: file '%s' is empty", filePath.c_str());
        return false;
    }

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

    // Allow generous send/receive timeouts for large clip uploads
    WinHttpSetTimeouts(hSession, 15000, 15000, 180000, 180000);

    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Split uploads into <= 40 MB chunks so Cloudflare Free / Tunnel (100 MB limit) never returns 413
    constexpr uint64_t kMaxChunkBytes = 40ULL * 1024ULL * 1024ULL;
    const uint64_t totalChunks = (fileSize + kMaxChunkBytes - 1) / kMaxChunkBytes;
    const std::string uploadId = (totalChunks > 1) ? GenerateUploadId() : "";
    const std::string filename = std::filesystem::path(filePath).filename().string();
    const DWORD reqFlags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;

    if (totalChunks > 1) {
        LOG_INFO("Uploading '%s' (%.2f MB) in %llu chunks of up to 40 MB...",
                 filename.c_str(), fileSize / (1024.0 * 1024.0), (unsigned long long)totalChunks);
    }

    std::vector<char> ioBuf(256 * 1024);
    std::string finalResponseBody;

    for (uint64_t chunkIdx = 0; chunkIdx < totalChunks; ++chunkIdx) {
        uint64_t chunkOffset = chunkIdx * kMaxChunkBytes;
        uint64_t chunkSize = std::min<uint64_t>(kMaxChunkBytes, fileSize - chunkOffset);

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", urlPath,
                                                nullptr, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        std::string headersUtf8 = "Content-Type: video/mp4\r\n"
                                  "Authorization: Bearer " + SanitizeHeaderVal(bearerToken) + "\r\n"
                                  "X-Filename: " + SanitizeHeaderVal(filename) + "\r\n"
                                  "X-Username: " + SanitizeHeaderVal(username) + "\r\n"
                                  "X-User-Token: " + SanitizeHeaderVal(userToken) + "\r\n"
                                  "X-App-Name: " + SanitizeHeaderVal(appName) + "\r\n";
        if (totalChunks > 1) {
            headersUtf8 += "X-Upload-Id: " + uploadId + "\r\n"
                           "X-Chunk-Index: " + std::to_string(chunkIdx) + "\r\n"
                           "X-Total-Chunks: " + std::to_string(totalChunks) + "\r\n";
        }
        std::wstring headersWide = ToWide(headersUtf8);

        BOOL ok = WinHttpSendRequest(
            hRequest,
            headersWide.c_str(),
            (DWORD)headersWide.size(),
            WINHTTP_NO_REQUEST_DATA,
            0,
            (DWORD)chunkSize,
            0
        );

        uint64_t remaining = chunkSize;
        while (ok && remaining > 0 && in.good()) {
            std::streamsize toRead = static_cast<std::streamsize>(std::min<uint64_t>(ioBuf.size(), remaining));
            in.read(ioBuf.data(), toRead);
            std::streamsize n = in.gcount();
            if (n <= 0) {
                ok = FALSE;
                break;
            }
            DWORD written = 0;
            ok = WinHttpWriteData(hRequest, ioBuf.data(), (DWORD)n, &written);
            remaining -= static_cast<uint64_t>(n);
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

        if (!ok || (statusCode != 200 && statusCode != 201)) {
            LOG_ERROR("Uploader: HTTP POST failed on chunk %llu/%llu (status %lu, body: %s)",
                      (unsigned long long)(chunkIdx + 1), (unsigned long long)totalChunks,
                      (unsigned long)statusCode, responseBody.c_str());
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        finalResponseBody = std::move(responseBody);
    }

    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    outShareUrl = ExtractJsonField(finalResponseBody, "url");
    if (outShareUrl.empty()) {
        LOG_ERROR("Uploader: missing 'url' in server response: %s", finalResponseBody.c_str());
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
