#pragma once

#include "common.h"
#include <unordered_map>
#include <cctype>
#include <cstdlib>

namespace json_util {

inline void append_utf8_cp(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back((char)cp);
    } else if (cp <= 0x7FFu) {
        out.push_back((char)(0xC0u | (cp >> 6)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back((char)(0xE0u | (cp >> 12)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back((char)(0xF0u | (cp >> 18)));
        out.push_back((char)(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
}

inline std::string escape_json_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    out.push_back('"');
    return out;
}

class JsonScanner {
public:
    explicit JsonScanner(const std::string& src) : s(src), pos(0) {}

    void skip_ws() {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n'))
            ++pos;
    }

    bool match(char ch) {
        skip_ws();
        if (pos < s.size() && s[pos] == ch) {
            ++pos;
            return true;
        }
        return false;
    }

    char peek() {
        skip_ws();
        return pos < s.size() ? s[pos] : '\0';
    }

    bool parse_string(std::string& out) {
        skip_ws();
        if (pos >= s.size() || s[pos] != '"') return false;
        ++pos;
        out.clear();
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos >= s.size()) return false;
                char esc = s[pos++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        auto parse_hex4 = [&](uint32_t& val) -> bool {
                            if (pos + 4 > s.size()) return false;
                            val = 0;
                            for (int i = 0; i < 4; ++i) {
                                char h = s[pos++];
                                val <<= 4;
                                if (h >= '0' && h <= '9') val |= (uint32_t)(h - '0');
                                else if (h >= 'a' && h <= 'f') val |= (uint32_t)(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') val |= (uint32_t)(h - 'A' + 10);
                                else return false;
                            }
                            return true;
                        };
                        uint32_t cp = 0;
                        if (!parse_hex4(cp)) return false;
                        if (cp >= 0xD800u && cp <= 0xDBFFu) {
                            if (pos + 6 <= s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
                                size_t saved = pos;
                                pos += 2;
                                uint32_t low = 0;
                                if (parse_hex4(low) && low >= 0xDC00u && low <= 0xDFFFu) {
                                    cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                                } else {
                                    pos = saved;
                                }
                            }
                        }
                        append_utf8_cp(out, cp);
                        break;
                    }
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool skip_value() {
        char c = peek();
        if (c == '"') {
            std::string tmp;
            return parse_string(tmp);
        }
        if (c == '{') {
            ++pos;
            if (match('}')) return true;
            while (pos < s.size()) {
                std::string k;
                if (!parse_string(k)) return false;
                if (!match(':')) return false;
                if (!skip_value()) return false;
                if (match('}')) return true;
                if (!match(',')) return false;
            }
            return false;
        }
        if (c == '[') {
            ++pos;
            if (match(']')) return true;
            while (pos < s.size()) {
                if (!skip_value()) return false;
                if (match(']')) return true;
                if (!match(',')) return false;
            }
            return false;
        }
        size_t start = pos;
        while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ']' &&
               s[pos] != ' ' && s[pos] != '\t' && s[pos] != '\r' && s[pos] != '\n') {
            ++pos;
        }
        return pos > start;
    }

    bool parse_raw_token(std::string& out) {
        skip_ws();
        size_t start = pos;
        while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ']' &&
               s[pos] != ' ' && s[pos] != '\t' && s[pos] != '\r' && s[pos] != '\n') {
            ++pos;
        }
        if (pos == start) return false;
        out = s.substr(start, pos - start);
        return true;
    }

    const std::string& s;
    size_t pos;
};

// Recursively find first string value for target_key inside any JSON object/array.
inline bool find_string_field_recursive(JsonScanner& js, const char* target_key, std::string& out) {
    char c = js.peek();
    if (c == '{') {
        ++js.pos;
        if (js.match('}')) return false;
        while (js.pos < js.s.size()) {
            std::string k;
            if (!js.parse_string(k)) return false;
            if (!js.match(':')) return false;
            if (k == target_key && js.peek() == '"') {
                return js.parse_string(out);
            }
            if (js.peek() == '{' || js.peek() == '[') {
                if (find_string_field_recursive(js, target_key, out)) return true;
            } else {
                if (!js.skip_value()) return false;
            }
            if (js.match('}')) return false;
            if (!js.match(',')) return false;
        }
        return false;
    }
    if (c == '[') {
        ++js.pos;
        if (js.match(']')) return false;
        while (js.pos < js.s.size()) {
            if (js.peek() == '{' || js.peek() == '[') {
                if (find_string_field_recursive(js, target_key, out)) return true;
            } else {
                if (!js.skip_value()) return false;
            }
            if (js.match(']')) return false;
            if (!js.match(',')) return false;
        }
        return false;
    }
    return js.skip_value();
}

inline std::string extract_json_string_field(const std::string& json_body, const char* field_name) {
    JsonScanner js(json_body);
    std::string out;
    if (find_string_field_recursive(js, field_name, out)) return out;
    return {};
}

inline AppSettings parse_settings_json(const std::string& json_body) {
    AppSettings cfg;
    JsonScanner js(json_body);
    if (!js.match('{')) return cfg;
    if (js.match('}')) return cfg;
    while (js.pos < js.s.size()) {
        std::string key;
        if (!js.parse_string(key) || !js.match(':')) break;
        char c = js.peek();
        if (c == '"') {
            std::string val;
            if (!js.parse_string(val)) break;
            if (key == "hotkey") cfg.hotkey = val;
            else if (key == "microphone_name") cfg.microphone_name = val;
            else if (key == "language") cfg.language = val;
            else if (key == "engine") cfg.engine = val;
            else if (key == "groq_api_key") cfg.groq_api_key = val;
            else if (key == "theme") cfg.theme = (val == "light") ? "light" : "dark";
        } else if (c == '{' || c == '[') {
            if (!js.skip_value()) break;
        } else {
            std::string tok;
            if (!js.parse_raw_token(tok)) break;
            if (key == "microphone_index") {
                if (tok == "null") cfg.microphone_index = -1;
                else cfg.microphone_index = atoi(tok.c_str());
            } else if (key == "auto_punctuation") {
                cfg.auto_punctuation = (tok == "true");
            } else if (key == "auto_start") {
                cfg.auto_start = (tok == "true");
                cfg.has_auto_start_pref = true;
            }
        }
        if (js.match('}')) break;
        if (!js.match(',')) break;
    }
    return cfg;
}

inline std::string serialize_settings_json(const AppSettings& cfg) {
    std::string out = "{\n";
    out += "  \"hotkey\": " + escape_json_string(cfg.hotkey) + ",\n";
    if (cfg.microphone_index >= 0) {
        out += "  \"microphone_index\": " + std::to_string(cfg.microphone_index) + ",\n";
    } else {
        out += "  \"microphone_index\": null,\n";
    }
    out += "  \"microphone_name\": " + escape_json_string(cfg.microphone_name) + ",\n";
    out += "  \"language\": " + escape_json_string(cfg.language) + ",\n";
    out += std::string("  \"auto_punctuation\": ") + (cfg.auto_punctuation ? "true" : "false") + ",\n";
    out += "  \"engine\": " + escape_json_string(cfg.engine) + ",\n";
    out += "  \"groq_api_key\": " + escape_json_string(cfg.groq_api_key) + ",\n";
    out += "  \"theme\": " + escape_json_string(cfg.theme == "light" ? "light" : "dark") + ",\n";
    out += std::string("  \"auto_start\": ") + (cfg.auto_start ? "true" : "false") + "\n";
    out += "}";
    return out;
}

inline AppSettings load_settings_file(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return AppSettings{};
    }
    DWORD sz = GetFileSize(h, nullptr);
    std::string buf;
    if (sz != INVALID_FILE_SIZE && sz > 0 && sz < 1024 * 1024) {
        buf.resize(sz);
        DWORD read_bytes = 0;
        ReadFile(h, buf.data(), sz, &read_bytes, nullptr);
        buf.resize(read_bytes);
    }
    CloseHandle(h);
    if (buf.empty()) return AppSettings{};
    return parse_settings_json(buf);
}

inline bool save_settings_file(const std::wstring& path, const AppSettings& cfg) {
    std::string body = serialize_settings_json(cfg);
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, body.data(), (DWORD)body.size(), &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != body.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

} // namespace json_util
