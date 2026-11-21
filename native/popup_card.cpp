#include "popup_card.h"
#include <algorithm>
#include <cmath>

namespace popup_card {

namespace {

struct PopupState {
    HWND hwnd = nullptr;
    const theme::ThemePalette* pal = nullptr;
    std::string tag;
    std::vector<PopupItem> items;
    std::vector<RECT> item_rects;
    float scale = 1.0f;
    int w = 220;
    int h = 100;
    int hover_idx = -1;
    int pressed_idx = -1;
    int scroll_y = 0;
    int content_h = 0;
    int max_scroll = 0;
    int chosen_id = -1;
    bool done = false;

    HBITMAP hbm = nullptr;
    HDC hdc = nullptr;

    void release_dib() {
        if (hdc) { DeleteDC(hdc); hdc = nullptr; }
        if (hbm) { DeleteObject(hbm); hbm = nullptr; }
    }
    ~PopupState() { release_dib(); }
};

static PopupState* s_active_popup = nullptr;
static std::string s_last_closed_tag;
static ULONGLONG s_last_closed_tick = 0;

static void mark_closed(PopupState* st) {
    if (!st) return;
    if (!st->tag.empty()) {
        s_last_closed_tag = st->tag;
        s_last_closed_tick = GetTickCount64();
    }
    st->done = true;
}

static UINT get_dpi_at_point(int x, int y) {
    POINT pt{ x, y };
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    UINT dpi = 96;
    if (shcore) {
        typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
        auto fn = (PFN_GetDpiForMonitor)GetProcAddress(shcore, "GetDpiForMonitor");
        UINT dx = 96, dy = 96;
        if (fn && SUCCEEDED(fn(hMon, 0, &dx, &dy))) {
            dpi = dx;
        }
        FreeLibrary(shcore);
    }
    if (dpi < 96) {
        HDC sdc = GetDC(nullptr);
        dpi = (UINT)GetDeviceCaps(sdc, LOGPIXELSX);
        ReleaseDC(nullptr, sdc);
    }
    return dpi >= 96 ? dpi : 96;
}

static void render_popup(PopupState* st) {
    if (!st || !st->hwnd) return;
    if (!st->hdc) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = st->w;
        bmi.bmiHeader.biHeight = -st->h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        HDC sdc = GetDC(nullptr);
        st->hdc = CreateCompatibleDC(sdc);
        void* bits = nullptr;
        st->hbm = CreateDIBSection(sdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, sdc);
        if (!st->hdc || !st->hbm) return;
        SelectObject(st->hdc, st->hbm);
    }

    const float s = st->scale;
    const auto& pal = *st->pal;

    Gdiplus::Graphics g(st->hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    Gdiplus::GraphicsPath bg;
    theme::add_round_rect_path(bg, 1.5f * s, 1.5f * s, (float)st->w - 3.0f * s, (float)st->h - 3.0f * s, 12.0f * s);
    Gdiplus::SolidBrush bg_br(theme::gcol(pal.card_bg));
    g.FillPath(&bg_br, &bg);
    Gdiplus::Pen border_pen(theme::gcol(pal.border), 1.3f * s);
    g.DrawPath(&border_pen, &bg);

    g.SetClip(&bg);

    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font_hdr(&ff, 8.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font font_item(&ff, 9.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::Font font_item_bold(&ff, 9.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);

    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentNear);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    Gdiplus::SolidBrush br_txt(theme::gcol(pal.text_primary));
    Gdiplus::SolidBrush br_muted(theme::gcol(pal.text_muted));
    Gdiplus::SolidBrush br_green(theme::gcol(pal.accent_green));
    Gdiplus::SolidBrush br_hover(theme::gcol(pal.hover_bg));
    Gdiplus::Pen sep_pen(theme::gcol(pal.border_subtle), 1.0f * s);

    for (size_t i = 0; i < st->items.size(); ++i) {
        const auto& it = st->items[i];
        RECT rc = st->item_rects[i];
        OffsetRect(&rc, 0, -st->scroll_y);
        if (rc.bottom < 0 || rc.top > st->h) continue;

        if (it.separator) {
            float y_mid = (float)(rc.top + rc.bottom) * 0.5f;
            g.DrawLine(&sep_pen, 10.0f * s, y_mid, (float)st->w - 10.0f * s, y_mid);
            continue;
        }

        if ((int)i == st->hover_idx && !it.disabled) {
            Gdiplus::GraphicsPath hp;
            theme::add_round_rect_path(
                hp, (float)rc.left + 6.0f * s, (float)rc.top + 2.0f * s,
                (float)(rc.right - rc.left) - 12.0f * s, (float)(rc.bottom - rc.top) - 4.0f * s, 7.0f * s
            );
            g.FillPath(&br_hover, &hp);
        }

        if (it.checked) {
            Gdiplus::SolidBrush dot_br(theme::gcol(pal.accent_green));
            float dr = 3.5f * s;
            float dx = 16.0f * s;
            float dy = (float)(rc.top + rc.bottom) * 0.5f;
            g.FillEllipse(&dot_br, dx - dr, dy - dr, dr * 2.0f, dr * 2.0f);
        }

        float text_x = it.disabled ? 14.0f * s : 28.0f * s;
        Gdiplus::RectF trc(
            text_x, (float)rc.top,
            (float)st->w - text_x - 14.0f * s, (float)(rc.bottom - rc.top)
        );
        if (it.disabled) {
            g.DrawString(it.label.c_str(), -1, &font_hdr, trc, &sf, &br_muted);
        } else if (it.checked) {
            g.DrawString(it.label.c_str(), -1, &font_item_bold, trc, &sf, &br_green);
        } else {
            g.DrawString(it.label.c_str(), -1, &font_item, trc, &sf, &br_txt);
        }
    }

    POINT pt_src{ 0, 0 };
    SIZE sz{ st->w, st->h };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC sdc = GetDC(nullptr);
    UpdateLayeredWindow(st->hwnd, sdc, nullptr, &sz, st->hdc, &pt_src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, sdc);
}

static LRESULT CALLBACK popup_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PopupState* st = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        st = (PopupState*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        st->hwnd = hwnd;
    } else {
        st = (PopupState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!st) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && !st->done) {
                st->chosen_id = -1;
                mark_closed(st);
            }
            return 0;

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && !st->done) {
                st->chosen_id = -1;
                mark_closed(st);
                return 0;
            }
            break;

        case WM_MOUSEWHEEL:
            if (st->max_scroll > 0) {
                int delta = GET_WHEEL_DELTA_WPARAM(wp);
                int step = (int)std::lround(36.0f * st->scale);
                st->scroll_y -= (delta > 0 ? step : -step);
                if (st->scroll_y < 0) st->scroll_y = 0;
                if (st->scroll_y > st->max_scroll) st->scroll_y = st->max_scroll;
                render_popup(st);
            }
            return 0;

        case WM_MOUSEMOVE: {
            int y = (int)(short)HIWORD(lp) + st->scroll_y;
            int hit = -1;
            for (size_t i = 0; i < st->items.size(); ++i) {
                if (!st->items[i].separator && !st->items[i].disabled) {
                    if (y >= st->item_rects[i].top && y < st->item_rects[i].bottom) {
                        hit = (int)i;
                        break;
                    }
                }
            }
            if (hit != st->hover_idx) {
                st->hover_idx = hit;
                render_popup(st);
            }
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            if (st->hover_idx != -1) {
                st->hover_idx = -1;
                render_popup(st);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            int y = (int)(short)HIWORD(lp) + st->scroll_y;
            st->pressed_idx = -1;
            for (size_t i = 0; i < st->items.size(); ++i) {
                if (!st->items[i].separator && !st->items[i].disabled &&
                    y >= st->item_rects[i].top && y < st->item_rects[i].bottom) {
                    st->pressed_idx = (int)i;
                    break;
                }
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (st->done) return 0;
            int x = (int)(short)LOWORD(lp);
            int y = (int)(short)HIWORD(lp);
            if (x < 0 || x >= st->w || y < 0 || y >= st->h) {
                st->chosen_id = -1;
                mark_closed(st);
                return 0;
            }
            int cy = y + st->scroll_y;
            int hit = -1;
            for (size_t i = 0; i < st->items.size(); ++i) {
                if (!st->items[i].separator && !st->items[i].disabled &&
                    cy >= st->item_rects[i].top && cy < st->item_rects[i].bottom) {
                    hit = (int)i;
                    break;
                }
            }
            if (hit >= 0 && hit == st->pressed_idx) {
                st->chosen_id = st->items[(size_t)hit].id;
                mark_closed(st);
            }
            st->pressed_idx = -1;
            return 0;
        }

        case WM_RBUTTONDOWN:
            if (!st->done) {
                st->chosen_id = -1;
                mark_closed(st);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

bool is_popup_open() {
    return s_active_popup != nullptr;
}

void dismiss_active_popup() {
    if (s_active_popup) {
        s_active_popup->chosen_id = -1;
        mark_closed(s_active_popup);
        if (s_active_popup->hwnd) {
            PostMessageW(s_active_popup->hwnd, WM_NULL, 0, 0);
        }
    }
}

bool check_toggle_or_dismiss(const char* tag, bool* out_deferred, DWORD cooldown_ms) {
    if (out_deferred) *out_deferred = false;
    std::string req = tag ? tag : "";

    if (s_active_popup) {
        bool same = (!req.empty() && s_active_popup->tag == req);
        dismiss_active_popup();
        if (!same && out_deferred) {
            *out_deferred = true;
        }
        return true;
    }

    if (!req.empty() && s_last_closed_tag == req) {
        if ((GetTickCount64() - s_last_closed_tick) < cooldown_ms) {
            s_last_closed_tag.clear();
            return true;
        }
    }
    return false;
}

int show_popup_menu(
    HWND owner_hwnd,
    int screen_x,
    int screen_y,
    const std::vector<PopupItem>& items,
    const theme::ThemePalette& pal,
    int min_width_dp,
    const char* tag
) {
    if (items.empty()) return -1;
    if (s_active_popup != nullptr) {
        // Hard guard: never nest two modal popup loops on the same UI thread.
        dismiss_active_popup();
        return -1;
    }

    static bool s_reg = false;
    if (!s_reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &popup_wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"VoiceDictationCustomMenu";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        s_reg = true;
    }

    PopupState st;
    st.pal = &pal;
    st.tag = tag ? tag : "";
    st.items = items;
    UINT dpi = get_dpi_at_point(screen_x, screen_y);
    st.scale = (float)dpi / 96.0f;
    const float s = st.scale;

    // Measure text width cleanly
    HDC sdc = GetDC(nullptr);
    Gdiplus::Graphics mg(sdc);
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font_item(&ff, 9.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    float max_tw = (float)min_width_dp * s;
    for (const auto& it : items) {
        if (it.separator) continue;
        Gdiplus::RectF bound;
        mg.MeasureString(it.label.c_str(), -1, &font_item, Gdiplus::PointF(0, 0), &bound);
        float need = bound.Width + 52.0f * s;
        if (need > max_tw) max_tw = need;
    }
    ReleaseDC(nullptr, sdc);

    int max_w = (int)std::lround(420.0f * s);
    st.w = std::min((int)std::ceil(max_tw), max_w);

    int pad_y = (int)std::lround(8.0f * s);
    int cur_y = pad_y;
    st.item_rects.resize(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        int ih = items[i].separator
                 ? (int)std::lround(9.0f * s)
                 : (items[i].disabled ? (int)std::lround(26.0f * s) : (int)std::lround(34.0f * s));
        st.item_rects[i] = RECT{ 0, cur_y, st.w, cur_y + ih };
        cur_y += ih;
    }
    cur_y += pad_y;
    st.content_h = cur_y;

    HMONITOR hMon = MonitorFromPoint(POINT{ screen_x, screen_y }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);
    RECT wa = mi.rcWork;

    int max_h = (int)((wa.bottom - wa.top) * 0.72);
    st.h = std::min(st.content_h, max_h);
    st.max_scroll = std::max(0, st.content_h - st.h);

    int px = screen_x;
    int py = screen_y;
    if (px + st.w > wa.right - 8) px = wa.right - st.w - 8;
    if (px < wa.left + 8) px = wa.left + 8;
    if (py + st.h > wa.bottom - 8) py = screen_y - st.h;
    if (py < wa.top + 8) py = wa.top + 8;

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"VoiceDictationCustomMenu", L"",
        WS_POPUP,
        px, py, st.w, st.h,
        owner_hwnd, nullptr, GetModuleHandleW(nullptr), &st
    );
    if (!hwnd) return -1;

    s_active_popup = &st;
    render_popup(&st);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(hwnd);
    if (s_active_popup == &st) {
        mark_closed(&st);
        s_active_popup = nullptr;
    }
    return st.chosen_id;
}

} // namespace popup_card
