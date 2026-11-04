#pragma once
#include <string>

class Uploader {
public:
    // Uploads an MP4 file to the configured server via HTTP POST (/api/upload)
    // and returns the shareable URL (e.g. "http://localhost:8080/v/abc12345") on success.
    static bool UploadFile(
        const std::string& filePath,
        const std::string& uploadUrl,
        const std::string& bearerToken,
        const std::string& username,
        const std::string& appName,
        std::string& outShareUrl
    );

    static bool CopyToClipboard(const std::string& text);
};
