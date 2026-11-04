#pragma once
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <mutex>
#include <string>
#include <windows.h>

class Logger {
public:
    static void Init(const std::string& logPath = "instant_replay.log") {
        std::lock_guard<std::mutex> lock(GetMutex());
        if (GetFile()) fclose(GetFile());
        GetFile() = fopen(logPath.c_str(), "a");
    }

    static void Log(const char* level, const char* fmt, ...) {
        std::lock_guard<std::mutex> lock(GetMutex());
        std::time_t now = std::time(nullptr);
        std::tm tm_now{};
        localtime_s(&tm_now, &now);
        char timeBuf[32];
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm_now);

        char msgBuf[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
        va_end(args);

        fprintf(stdout, "[%s] [%s] %s\n", timeBuf, level, msgBuf);
        fflush(stdout);

        if (GetFile()) {
            fprintf(GetFile(), "[%s] [%s] %s\n", timeBuf, level, msgBuf);
            fflush(GetFile());
        }
    }

private:
    static std::mutex& GetMutex() {
        static std::mutex mtx;
        return mtx;
    }
    static FILE*& GetFile() {
        static FILE* f = nullptr;
        return f;
    }
};

#define LOG_INFO(...)  Logger::Log("INFO",  __VA_ARGS__)
#define LOG_WARN(...)  Logger::Log("WARN",  __VA_ARGS__)
#define LOG_ERROR(...) Logger::Log("ERROR", __VA_ARGS__)
#define LOG_DEBUG(...) Logger::Log("DEBUG", __VA_ARGS__)
