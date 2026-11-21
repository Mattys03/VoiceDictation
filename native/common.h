#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <cstdint>

inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out((size_t)needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), needed);
    return out;
}

inline std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out((size_t)needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), needed, nullptr, nullptr);
    return out;
}

inline std::wstring get_exe_dir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf, len);
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : path.substr(0, pos);
}

struct AppSettings {
    std::string hotkey = "Alt+H";
    int microphone_index = -1; // -1 represents JSON null
    std::string microphone_name;
    std::string language = "auto";
    bool auto_punctuation = true;
    std::string engine = "Google (Gratuito)";
    std::string groq_api_key;
    std::string theme = "dark"; // "dark" | "light"
    bool auto_start = false;
    bool has_auto_start_pref = false;
};

inline std::wstring get_exe_path_w() {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, len);
}

inline std::wstring get_startup_shortcut_path() {
    wchar_t dir[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, SHGFP_TYPE_CURRENT, dir))) {
        return std::wstring(dir) + L"\\Voice Dictation.lnk";
    }
    return {};
}

inline void run_hidden_cmd(const std::wstring& cmd_line) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring mut = cmd_line;
    if (CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 2500);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

inline bool is_windows_auto_start_enabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = 0, sz = 0;
        LONG q = RegQueryValueExW(hKey, L"VoiceDictation", nullptr, &type, nullptr, &sz);
        RegCloseKey(hKey);
        if (q == ERROR_SUCCESS && sz > 2) return true;
    }
    std::wstring lnk = get_startup_shortcut_path();
    if (!lnk.empty() && GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    return false;
}

inline bool set_windows_auto_start(bool enabled) {
    HKEY hKey = nullptr;
    LONG rc = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr
    );
    bool reg_ok = false;
    if (rc == ERROR_SUCCESS && hKey) {
        if (enabled) {
            std::wstring val = L"\"" + get_exe_path_w() + L"\" --silent";
            DWORD bytes = (DWORD)((val.size() + 1) * sizeof(wchar_t));
            reg_ok = (RegSetValueExW(hKey, L"VoiceDictation", 0, REG_SZ,
                                     (const BYTE*)val.c_str(), bytes) == ERROR_SUCCESS);
        } else {
            LONG del_rc = RegDeleteValueW(hKey, L"VoiceDictation");
            reg_ok = (del_rc == ERROR_SUCCESS || del_rc == ERROR_FILE_NOT_FOUND);
        }
        RegCloseKey(hKey);
    }

    if (enabled) {
        run_hidden_cmd(L"schtasks.exe /Change /TN \"VoiceDictationLive\" /ENABLE");
    } else {
        std::wstring lnk = get_startup_shortcut_path();
        if (!lnk.empty()) DeleteFileW(lnk.c_str());
        run_hidden_cmd(L"schtasks.exe /Change /TN \"VoiceDictationLive\" /DISABLE");
    }
    return reg_ok;
}

struct MicDeviceInfo {
    int index = -1;
    std::string name;
    std::string api = "MME";
    uint32_t default_rate = 44100;
};

inline std::string ascii_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

inline bool is_loopback_device_name(const std::string& name) {
    std::string low = ascii_lower(name);
    const char* needles[] = {
        "stereo mix", "mixagem estereo", "what u hear",
        "wave out", "loopback", "vb-audio", "cable output",
        "voicemeeter"
    };
    for (const char* n : needles) {
        if (low.find(n) != std::string::npos) return true;
    }
    return false;
}

inline std::string strip_right_paren(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == ')')) {
        s.pop_back();
    }
    while (!s.empty() && s.front() == ' ') {
        s.erase(s.begin());
    }
    return ascii_lower(s);
}

inline bool device_name_matches(const std::string& saved, const std::string& candidate) {
    if (saved.empty() || candidate.empty()) return false;
    std::string a = strip_right_paren(saved);
    std::string b = strip_right_paren(candidate);
    if (a.empty() || b.empty()) return false;
    return a == b || a.rfind(b, 0) == 0 || b.rfind(a, 0) == 0;
}

struct ParsedHotkey {
    UINT mods = 0; // MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN
    UINT vk = 0;
    bool valid = false;
};

inline ParsedHotkey parse_hotkey_win(const std::string& hotkey_str) {
    ParsedHotkey res;
    std::string cur;
    auto handle_part = [&](std::string part) {
        while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.erase(part.begin());
        while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.pop_back();
        part = ascii_lower(part);
        if (part.empty()) return;
        if (part == "alt") res.mods |= MOD_ALT;
        else if (part == "ctrl" || part == "control") res.mods |= MOD_CONTROL;
        else if (part == "shift") res.mods |= MOD_SHIFT;
        else if (part == "win" || part == "cmd") res.mods |= MOD_WIN;
        else if (part == "space") res.vk = VK_SPACE;
        else if (part == "enter" || part == "return") res.vk = VK_RETURN;
        else if (part == "tab") res.vk = VK_TAB;
        else if (part.size() == 1) {
            char c = part[0];
            if (c >= 'a' && c <= 'z') res.vk = (UINT)('A' + (c - 'a'));
            else if (c >= '0' && c <= '9') res.vk = (UINT)c;
        } else if (part[0] == 'f') {
            int fnum = atoi(part.c_str() + 1);
            if (fnum >= 1 && fnum <= 12) res.vk = (UINT)(VK_F1 + (fnum - 1));
        }
    };
    for (char c : hotkey_str) {
        if (c == '+') {
            handle_part(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    handle_part(cur);
    res.valid = (res.vk != 0);
    return res;
}
