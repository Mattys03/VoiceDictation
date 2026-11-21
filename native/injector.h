#pragma once

#include "common.h"

namespace injector {

struct TargetWindow {
    HWND hwnd = nullptr;
    HWND focus = nullptr;
    std::string label;
};

std::string get_window_label(HWND hwnd);
HWND get_focused_control(HWND hwnd);

bool focus_target_window(HWND target, DWORD timeout_ms = 350);

// Internal clipboard testability & state helpers
bool get_clipboard_text_safe(std::wstring& out_text, bool& out_had_text);
bool set_clipboard_text_safe(const std::wstring& wtext, bool exclude_history);
bool paste_isolated_instant(const std::string& utf8_text, bool send_keys = true);

bool inject_unicode_keystrokes(const std::string& utf8_text);

bool type_text_into_target(
    const std::string& utf8_text,
    TargetWindow& tracked_target,
    DWORD own_pid
);

} // namespace injector
