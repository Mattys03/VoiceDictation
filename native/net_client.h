#pragma once

#include "common.h"
#include <winhttp.h>

namespace net_client {

class HttpSession {
public:
    HttpSession();
    ~HttpSession();

    void prewarm_groq(const std::string& api_key);

    std::string transcribe_groq_wav(
        const std::string& api_key,
        const uint8_t* wav_data,
        size_t wav_size,
        const std::string& lang_code,
        std::string* err_out = nullptr
    );

    std::string translate_groq(
        const std::string& api_key,
        const std::string& text,
        const std::string& target_language_name
    );

    std::string transcribe_google_flac(
        const uint8_t* flac_data,
        size_t flac_size,
        uint32_t sample_rate,
        const std::string& lang_tag,
        std::string* err_out = nullptr
    );

private:
    HINTERNET ensure_session();
    bool do_request(
        const wchar_t* host,
        INTERNET_PORT port,
        bool use_tls,
        const wchar_t* method,
        const wchar_t* path,
        const wchar_t* extra_headers,
        const void* body,
        DWORD body_len,
        DWORD* out_status,
        std::string* out_body,
        std::string* out_err
    );

    CRITICAL_SECTION m_cs{};
    HINTERNET m_session = nullptr;
};

} // namespace net_client
