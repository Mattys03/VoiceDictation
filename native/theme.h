#pragma once

#include "common.h"
#include <objidl.h>
#include <gdiplus.h>

namespace theme {

struct ThemePalette {
    bool is_light = false;

    // ARGB colors for GDI+
    DWORD window_bg = 0xFF181B2A;
    DWORD pill_bg = 0xF61E2130;
    DWORD status_bg = 0xF61E2135;
    DWORD card_bg = 0xFF22253A;
    DWORD input_bg = 0xFF161926;
    DWORD hover_bg = 0xFF2F334F;
    DWORD btn_bg = 0xFF2D314B;
    DWORD btn_hover = 0xFF3C4162;

    DWORD border = 0xFF3A3D55;
    DWORD border_subtle = 0xFF2C2F45;
    DWORD handle = 0xFF45485C;

    DWORD text_primary = 0xFFEEF0FA;
    DWORD text_secondary = 0xFFA4A7BF;
    DWORD text_muted = 0xFF7B809A;

    DWORD icon_idle = 0xFF72768A;
    DWORD icon_hover = 0xFFFFFFFF;
    DWORD mic_idle_circle = 0xFF555870;
    DWORD mic_icon = 0xFFFFFFFF;

    DWORD accent_green = 0xFF4DD964;
    DWORD accent_amber = 0xFFD4A017;
    DWORD accent_danger = 0xFFFF5F5F;

    COLORREF cr_window_bg = RGB(0x18, 0x1b, 0x2a);
    COLORREF cr_input_bg = RGB(0x16, 0x19, 0x26);
    COLORREF cr_text = RGB(0xee, 0xf0, 0xfa);
};

inline ThemePalette get_palette(const std::string& theme_name) {
    ThemePalette p;
    if (theme_name == "light") {
        p.is_light = true;
        p.window_bg = 0xFFEEF1F7;
        p.pill_bg = 0xF8F7F9FC;
        p.status_bg = 0xF8F7F9FC;
        p.card_bg = 0xFFFFFFFF;
        p.input_bg = 0xFFE8ECF4;
        p.hover_bg = 0xFFDDE3F0;
        p.btn_bg = 0xFFE3E8F2;
        p.btn_hover = 0xFFD0D7E8;

        p.border = 0xFFC5CDDF;
        p.border_subtle = 0xFFDCE1ED;
        p.handle = 0xFFB8C0D2;

        p.text_primary = 0xFF191E2D;
        p.text_secondary = 0xFF444B60;
        p.text_muted = 0xFF6A728A;

        p.icon_idle = 0xFF646B82;
        p.icon_hover = 0xFF191E2D;
        p.mic_idle_circle = 0xFF8B93A8;
        p.mic_icon = 0xFFFFFFFF;

        p.accent_green = 0xFF22C55E;
        p.accent_amber = 0xFFD97706;
        p.accent_danger = 0xFFEF4444;

        p.cr_window_bg = RGB(0xee, 0xf1, 0xf7);
        p.cr_input_bg = RGB(0xe8, 0xec, 0xf4);
        p.cr_text = RGB(0x19, 0x1e, 0x2d);
    }
    return p;
}

inline Gdiplus::Color gcol(DWORD argb) {
    return Gdiplus::Color(argb);
}

inline Gdiplus::Color gcol_alpha(DWORD rgb_only, BYTE alpha) {
    return Gdiplus::Color(alpha, (BYTE)((rgb_only >> 16) & 0xFF),
                          (BYTE)((rgb_only >> 8) & 0xFF),
                          (BYTE)(rgb_only & 0xFF));
}

inline void add_round_rect_path(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    float max_r = (w < h ? w : h) * 0.5f;
    if (r > max_r) r = max_r;
    if (r < 0.5f) r = 0.5f;
    float d = r * 2.0f;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

} // namespace theme
