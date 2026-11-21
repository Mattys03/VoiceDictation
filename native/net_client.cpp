#include "net_client.h"
#include "json_util.h"
#include "logger.h"

namespace net_client {

HttpSession::HttpSession() {
    InitializeCriticalSection(&m_cs);
}

HttpSession::~HttpSession() {
    EnterCriticalSection(&m_cs);
    if (m_session) {
        WinHttpCloseHandle(m_session);
        m_session = nullptr;
    }
    LeaveCriticalSection(&m_cs);
    DeleteCriticalSection(&m_cs);
}

HINTERNET HttpSession::ensure_session() {
    EnterCriticalSection(&m_cs);
    if (!m_session) {
        m_session = WinHttpOpen(
            L"VoiceDictation-Native/2.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );
        if (!m_session) {
            m_session = WinHttpOpen(
                L"VoiceDictation-Native/2.0",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS,
                0
            );
        }
        if (m_session) {
            // Finite timeouts: resolve 5s, connect 8s, send 25s, receive 45s
            WinHttpSetTimeouts(m_session, 5000, 8000, 25000, 45000);
        }
    }
    HINTERNET h = m_session;
    LeaveCriticalSection(&m_cs);
    return h;
}

bool HttpSession::do_request(
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
) {
    HINTERNET hSess = ensure_session();
    if (!hSess) {
        if (out_err) *out_err = "WinHttpOpen falhou";
        return false;
    }

    HINTERNET hConn = WinHttpConnect(hSess, host, port, 0);
    if (!hConn) {
        if (out_err) *out_err = "WinHttpConnect falhou (" + std::to_string(GetLastError()) + ")";
        return false;
    }

    DWORD flags = use_tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(
        hConn, method, path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags
    );
    if (!hReq) {
        if (out_err) *out_err = "WinHttpOpenRequest falhou (" + std::to_string(GetLastError()) + ")";
        WinHttpCloseHandle(hConn);
        return false;
    }

    WinHttpSetTimeouts(hReq, 5000, 8000, 25000, 45000);

    BOOL ok = WinHttpSendRequest(
        hReq,
        extra_headers ? extra_headers : WINHTTP_NO_ADDITIONAL_HEADERS,
        extra_headers ? (DWORD)-1L : 0,
        body ? (LPVOID)body : WINHTTP_NO_REQUEST_DATA,
        body_len,
        body_len,
        0
    );

    if (!ok) {
        if (out_err) *out_err = "WinHttpSendRequest falhou (" + std::to_string(GetLastError()) + ")";
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        return false;
    }

    if (!WinHttpReceiveResponse(hReq, nullptr)) {
        if (out_err) *out_err = "WinHttpReceiveResponse falhou (" + std::to_string(GetLastError()) + ")";
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        return false;
    }

    DWORD status = 0;
    DWORD status_sz = sizeof(status);
    WinHttpQueryHeaders(
        hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX
    );
    if (out_status) *out_status = status;

    std::string resp;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
        size_t old_sz = resp.size();
        resp.resize(old_sz + avail);
        DWORD read = 0;
        if (!WinHttpReadData(hReq, resp.data() + old_sz, avail, &read)) break;
        resp.resize(old_sz + read);
    }

    if (out_body) *out_body = std::move(resp);

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    return true;
}

void HttpSession::prewarm_groq(const std::string& api_key) {
    if (api_key.empty()) return;
    std::wstring hdr = L"Authorization: Bearer " + utf8_to_wide(api_key) + L"\r\n";
    DWORD status = 0;
    std::string body, err;
    if (do_request(L"api.groq.com", INTERNET_DEFAULT_HTTPS_PORT, true,
                   L"GET", L"/openai/v1/models", hdr.c_str(),
                   nullptr, 0, &status, &body, &err)) {
        if (status >= 200 && status < 300) {
            LOGI("[OK] Groq WinHTTP e TLS pre-aquecidos (status=%lu)", status);
        } else {
            LOGI("[DEBUG] Pre-aquecimento Groq status=%lu", status);
        }
    } else {
        LOGI("[DEBUG] Pre-aquecimento Groq: %s", err.c_str());
    }
}

std::string HttpSession::transcribe_groq_wav(
    const std::string& api_key,
    const uint8_t* wav_data,
    size_t wav_size,
    const std::string& lang_code,
    std::string* err_out
) {
    if (api_key.empty() || !wav_data || wav_size == 0) return {};

    const char* boundary = "----VoiceDictationNativeBoundary9a8b7c6d";
    std::string body;
    body.reserve(wav_size + 1024);

    auto add_field = [&](const char* name, const std::string& val) {
        body += "--";
        body += boundary;
        body += "\r\nContent-Disposition: form-data; name=\"";
        body += name;
        body += "\"\r\n\r\n";
        body += val;
        body += "\r\n";
    };

    // File part
    body += "--";
    body += boundary;
    body += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
    body += "Content-Type: audio/wav\r\n\r\n";
    size_t off = body.size();
    body.resize(off + wav_size);
    memcpy(body.data() + off, wav_data, wav_size);
    body += "\r\n";

    add_field("model", "whisper-large-v3-turbo");
    add_field("temperature", "0.0");
    if (!lang_code.empty()) {
        add_field("language", lang_code);
    }
    body += "--";
    body += boundary;
    body += "--\r\n";

    std::wstring hdr = L"Authorization: Bearer " + utf8_to_wide(api_key) + L"\r\n" +
                       L"Content-Type: multipart/form-data; boundary=" + utf8_to_wide(boundary) + L"\r\n";

    DWORD status = 0;
    std::string resp, err;
    bool ok = do_request(
        L"api.groq.com", INTERNET_DEFAULT_HTTPS_PORT, true,
        L"POST", L"/openai/v1/audio/transcriptions", hdr.c_str(),
        body.data(), (DWORD)body.size(), &status, &resp, &err
    );
    body.clear();
    body.shrink_to_fit();

    if (!ok) {
        if (err_out) *err_out = err;
        return {};
    }
    if (status < 200 || status >= 300) {
        if (err_out) *err_out = "HTTP " + std::to_string(status) + ": " + resp.substr(0, 200);
        return {};
    }
    return json_util::extract_json_string_field(resp, "text");
}

std::string HttpSession::translate_groq(
    const std::string& api_key,
    const std::string& text,
    const std::string& target_language_name
) {
    if (api_key.empty() || text.empty()) return text;

    static const char* kModels[] = { "openai/gpt-oss-20b", "groq/compound-mini" };
    std::string sys_prompt =
        "Translate the user text directly to natural, fluent " + target_language_name +
        ". If already in " + target_language_name +
        ", keep it as is. Output ONLY the translated text, without quotation marks, notes, or explanations.";

    std::wstring hdr = L"Authorization: Bearer " + utf8_to_wide(api_key) + L"\r\n" +
                       L"Content-Type: application/json\r\n";

    for (const char* model : kModels) {
        std::string payload = "{";
        payload += "\"model\":" + json_util::escape_json_string(model) + ",";
        payload += "\"temperature\":0.0,";
        payload += "\"messages\":[";
        payload += "{\"role\":\"system\",\"content\":" + json_util::escape_json_string(sys_prompt) + "},";
        payload += "{\"role\":\"user\",\"content\":" + json_util::escape_json_string(text) + "}";
        payload += "]}";

        DWORD status = 0;
        std::string resp, err;
        if (do_request(L"api.groq.com", INTERNET_DEFAULT_HTTPS_PORT, true,
                       L"POST", L"/openai/v1/chat/completions", hdr.c_str(),
                       payload.data(), (DWORD)payload.size(), &status, &resp, &err)) {
            if (status >= 200 && status < 300) {
                std::string tr = json_util::extract_json_string_field(resp, "content");
                if (!tr.empty()) return tr;
            } else {
                LOGE("[!] Erro traducao (%s): HTTP %lu", model, status);
            }
        } else {
            LOGE("[!] Erro traducao (%s): %s", model, err.c_str());
        }
    }
    return text;
}

std::string HttpSession::transcribe_google_flac(
    const uint8_t* flac_data,
    size_t flac_size,
    uint32_t sample_rate,
    const std::string& lang_tag,
    std::string* err_out
) {
    if (!flac_data || flac_size == 0) return {};

    std::string lang = lang_tag.empty() ? "pt-BR" : lang_tag;
    std::wstring path = L"/speech-api/v2/recognize?client=chromium&lang=" +
                        utf8_to_wide(lang) +
                        L"&key=AIzaSyBOti4mM-6x9WDnZIjIeyEU21OpBXqWBgw&pFilter=0";
    std::wstring hdr = L"Content-Type: audio/x-flac; rate=" + std::to_wstring(sample_rate) + L"\r\n";

    DWORD status = 0;
    std::string resp, err;
    if (!do_request(L"www.google.com", INTERNET_DEFAULT_HTTPS_PORT, true,
                    L"POST", path.c_str(), hdr.c_str(),
                    flac_data, (DWORD)flac_size, &status, &resp, &err)) {
        if (err_out) *err_out = err;
        return {};
    }
    if (status < 200 || status >= 300) {
        if (err_out) *err_out = "Google HTTP " + std::to_string(status);
        return {};
    }

    // Google speech API returns line-separated JSON chunks; parse each non-empty line for "transcript"
    size_t pos = 0;
    while (pos < resp.size()) {
        size_t nl = resp.find('\n', pos);
        std::string line = (nl == std::string::npos) ? resp.substr(pos) : resp.substr(pos, nl - pos);
        if (!line.empty()) {
            std::string tr = json_util::extract_json_string_field(line, "transcript");
            if (!tr.empty()) return tr;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return {};
}

} // namespace net_client
