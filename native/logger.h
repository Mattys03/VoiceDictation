#pragma once

#include "common.h"
#include <cstdio>
#include <cstdarg>

class AppLogger {
public:
    static constexpr size_t kMaxBytes = 2u * 1024u * 1024u;
    static constexpr int kBackupCount = 3;

    static AppLogger& instance() {
        static AppLogger inst;
        return inst;
    }

    void init(const std::wstring& log_path) {
        EnterCriticalSection(&m_cs);
        m_path = log_path;
        LeaveCriticalSection(&m_cs);
    }

    void info(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        write_line("INFO", fmt, args);
        va_end(args);
    }

    void error(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        write_line("ERROR", fmt, args);
        va_end(args);
    }

private:
    AppLogger() { InitializeCriticalSection(&m_cs); }
    ~AppLogger() { DeleteCriticalSection(&m_cs); }

    void write_line(const char* level, const char* fmt, va_list args) {
        char msg[2048];
        vsnprintf(msg, sizeof(msg), fmt, args);

        SYSTEMTIME st;
        GetLocalTime(&st);
        char line[2200];
        int len = snprintf(
            line, sizeof(line),
            "%04d-%02d-%02d %02d:%02d:%02d [%s] %s\r\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond,
            level, msg
        );
        if (len <= 0) return;
        if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;

        EnterCriticalSection(&m_cs);
        if (!m_path.empty()) {
            rotate_if_needed();
            HANDLE h = CreateFileW(
                m_path.c_str(), FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr
            );
            if (h != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(h, line, (DWORD)len, &written, nullptr);
                CloseHandle(h);
            }
        }
        LeaveCriticalSection(&m_cs);
    }

    void rotate_if_needed() {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(m_path.c_str(), GetFileExInfoStandard, &fad)) return;
        uint64_t sz = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (sz < kMaxBytes) return;

        for (int i = kBackupCount - 1; i >= 1; --i) {
            std::wstring src = m_path + L"." + std::to_wstring(i);
            std::wstring dst = m_path + L"." + std::to_wstring(i + 1);
            MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        std::wstring first = m_path + L".1";
        MoveFileExW(m_path.c_str(), first.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    CRITICAL_SECTION m_cs{};
    std::wstring m_path;
};

#define LOGI(...) AppLogger::instance().info(__VA_ARGS__)
#define LOGE(...) AppLogger::instance().error(__VA_ARGS__)
