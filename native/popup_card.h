#pragma once

#include "common.h"
#include "theme.h"
#include <vector>
#include <string>

namespace popup_card {

struct PopupItem {
    int id = 0;
    std::wstring label;
    bool checked = false;
    bool disabled = false;
    bool separator = false;
};

bool is_popup_open();
void dismiss_active_popup();

// Enforces single-popup non-reentrant toggle semantics:
// - Returns true if the caller should NOT open right now (either toggled the
//   same popup closed, clicked within the close cooldown window, or dismissed
//   a different popup and set *out_deferred = true so the caller can PostMessage
//   to reopen cleanly after the outer modal loop unwinds).
// - Returns false when safe to open immediately.
bool check_toggle_or_dismiss(const char* tag, bool* out_deferred = nullptr, DWORD cooldown_ms = 320);

int show_popup_menu(
    HWND owner_hwnd,
    int screen_x,
    int screen_y,
    const std::vector<PopupItem>& items,
    const theme::ThemePalette& pal,
    int min_width_dp = 190,
    const char* tag = ""
);

} // namespace popup_card
