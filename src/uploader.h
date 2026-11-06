#pragma once
#include <string>

class Uploader {
public:
    static bool UploadFile(
        const std::string& filePath,
        const std::string& uploadUrl,
        const std::string& bearerToken,
        const std::string& username,
        const std::string& userToken,
        const std::string& appName,
        std::string& outShareUrl
    );

    static bool CopyToClipboard(const std::string& text);
};
