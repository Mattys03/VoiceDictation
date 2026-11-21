#include "settings_ui.h"
#include "audio_engine.h"
#include "json_util.h"
#include "logger.h"
#include "popup_card.h"
#include <dwmapi.h>
#include <cmath>

namespace settings_ui {

namespace {
constexpr int IDC_EDIT_KEY = 1005;
constexpr UINT WM_APP_MICS_READY = WM_APP + 50;
constexpr UINT WM_APP_HOTKEY_PREVIEW = WM_APP + 51;
constexpr UINT WM_APP_HOTKEY_DONE = WM_APP + 52;
constexpr UINT WM_APP_DEFERRED_DD = WM_APP + 53;
constexpr UINT_PTR TIMER_SAVE_APIKEY = 1;

static std::string short_api_tag(const std::string& api) {
    std::string low = ascii_lower(api);
    if (low.find("mme") != std::string::npos) return "MME";
    if (low.find("wasapi") != std::string::npos) return "WASAPI";
    if (low.find("directsound") != std::string::npos) return "DS";
    return api.empty() ? "MME" : api;
}

static std::wstring lang_display(const std::string& code) {
    if (code == "auto") return L"Autom\u00E1tico (detectar idioma)";
    if (code == "translate_en") return L"Traduzir para Ingl\u00EAs (PT \u2192 EN)";
    if (code == "translate_es") return L"Traduzir para Espanhol (PT \u2192 ES)";
    return L"Portugu\u00EAs (Brasil)";
}

static void draw_chevron(Gdiplus::Graphics& g, float cx, float cy, float s, DWORD argb) {
    Gdiplus::Pen pen(theme::gcol(argb), 1.6f * s);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&pen, cx - 4.0f * s, cy - 1.5f * s, cx, cy + 2.5f * s);
    g.DrawLine(&pen, cx, cy + 2.5f * s, cx + 4.0f * s, cy - 1.5f * s);
}

static bool pt_in(const Gdiplus::RectF& r, float x, float y) {
    return x >= r.X && x <= (r.X + r.Width) && y >= r.Y && y <= (r.Y + r.Height);
}
} // namespace

SettingsWindow* SettingsWindow::s_active_hook_owner = nullptr;
HHOOK SettingsWindow::s_kb_hook = nullptr;
UINT SettingsWindow::s_held_mods = 0;

SettingsWindow::SettingsWindow() = default;

SettingsWindow::~SettingsWindow() {
    close();
    if (m_bb_dc) DeleteDC(m_bb_dc);
    if (m_bb_bmp) DeleteObject(m_bb_bmp);
    if (m_br_bg) DeleteObject(m_br_bg);
    if (m_br_input) DeleteObject(m_br_input);
    if (m_font_edit) DeleteObject(m_font_edit);
}

void SettingsWindow::refresh_theme_brushes() {
    m_pal = theme::get_palette(m_cfg.theme);
    if (m_br_bg) DeleteObject(m_br_bg);
    if (m_br_input) DeleteObject(m_br_input);
    m_br_bg = CreateSolidBrush(m_pal.cr_window_bg);
    m_br_input = CreateSolidBrush(m_pal.cr_input_bg);

    if (m_hwnd && IsWindow(m_hwnd)) {
        BOOL dark = m_pal.is_light ? FALSE : TRUE;
        DwmSetWindowAttribute(m_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        DWORD corner = 2; /*DWMWCP_ROUND*/
        DwmSetWindowAttribute(m_hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
        COLORREF border_cr = m_pal.is_light ? RGB(0xc5, 0xcd, 0xdf) : RGB(0x3a, 0x3d, 0x55);
        DwmSetWindowAttribute(m_hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border_cr, sizeof(border_cr));
    }
}

void SettingsWindow::show(HWND parent_hwnd, const AppSettings& current_cfg, OnSaveCallback on_save) {
    m_parent = parent_hwnd;
    m_on_save = std::move(on_save);

    if (m_hwnd && IsWindow(m_hwnd)) {
        ShowWindow(m_hwnd, SW_SHOW);
        SetForegroundWindow(m_hwnd);
        return;
    }

    m_cfg = current_cfg;
    m_key_visible = false;
    m_hotkey_preview.clear();
    refresh_theme_brushes();

    HDC sdc = GetDC(nullptr);
    UINT dpi = (UINT)GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(nullptr, sdc);
    m_scale = (dpi >= 96) ? ((float)dpi / 96.0f) : 1.0f;

    static bool s_reg = false;
    if (!s_reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &SettingsWindow::wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"VoiceDictationSettingsWndV2";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        s_reg = true;
    }

    m_win_w = (int)std::lround(396.0f * m_scale);
    bool is_groq = (m_cfg.engine == "Groq (Whisper API)");
    m_win_h = (int)std::lround((is_groq ? 604.0f : 556.0f) * m_scale);

    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - m_win_w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - m_win_h) / 2;
    if (y < wa.top) y = wa.top;

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"VoiceDictationSettingsWndV2",
        L"Configura\u00E7\u00F5es - Voice Dictation",
        WS_POPUP | WS_CLIPCHILDREN,
        x, y, m_win_w, m_win_h,
        nullptr, nullptr, GetModuleHandleW(nullptr), this
    );

    if (m_hwnd) {
        refresh_theme_brushes();
        update_layout();
        ShowWindow(m_hwnd, SW_SHOW);
        UpdateWindow(m_hwnd);
        SetForegroundWindow(m_hwnd);
        start_async_mics();
    }
}

void SettingsWindow::close() {
    stop_hotkey_capture();
    popup_card::dismiss_active_popup();
    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, TIMER_SAVE_APIKEY);
        if (m_edit_key && IsWindow(m_edit_key)) {
            wchar_t buf[512] = {};
            GetWindowTextW(m_edit_key, buf, 512);
            std::string new_key = wide_to_utf8(buf);
            if (new_key != m_cfg.groq_api_key) {
                AppSettings old_cfg = m_cfg;
                m_cfg.groq_api_key = new_key;
                commit_and_notify(old_cfg);
            }
        }
        DestroyWindow(m_hwnd);
    }
    m_hwnd = nullptr;
    m_edit_key = nullptr;
}

void SettingsWindow::update_layout() {
    bool is_groq = (m_cfg.engine == "Groq (Whisper API)");
    float target_h_dp = is_groq ? 604.0f : 556.0f;
    m_win_w = (int)std::lround(396.0f * m_scale);
    m_win_h = (int)std::lround(target_h_dp * m_scale);

    m_rc_close = { 354.0f, 9.0f, 30.0f, 26.0f };

    float y = 56.0f;
    // Card 1: Hotkey (height 74)
    m_rc_btn_hk = { 286.0f, y + 32.0f, 80.0f, 32.0f };
    y += 84.0f;

    // Card 2: Mic (height 74)
    m_rc_cb_mic = { 30.0f, y + 32.0f, 336.0f, 34.0f };
    y += 84.0f;

    // Card 3: Engine & Key (height 122 if Groq else 74)
    m_rc_cb_eng = { 30.0f, y + 32.0f, 336.0f, 34.0f };
    if (is_groq) {
        m_rc_key_box = { 112.0f, y + 76.0f, 198.0f, 32.0f };
        m_rc_btn_eye = { 316.0f, y + 76.0f, 50.0f, 32.0f };
        y += 132.0f;
    } else {
        m_rc_key_box = {};
        m_rc_btn_eye = {};
        y += 84.0f;
    }

    // Card 4: Language (height 74)
    m_rc_cb_lang = { 30.0f, y + 32.0f, 336.0f, 34.0f };
    y += 84.0f;

    // Card 5: Preferences & Theme (height 132)
    m_rc_punct_row = { 30.0f, y + 10.0f, 336.0f, 34.0f };
    m_rc_autostart_row = { 30.0f, y + 51.0f, 336.0f, 34.0f };
    m_rc_theme_dark = { 196.0f, y + 93.0f, 82.0f, 28.0f };
    m_rc_theme_light = { 284.0f, y + 93.0f, 82.0f, 28.0f };

    if (m_hwnd && IsWindow(m_hwnd)) {
        RECT cur{};
        GetWindowRect(m_hwnd, &cur);
        if ((cur.bottom - cur.top) != m_win_h || (cur.right - cur.left) != m_win_w) {
            SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, m_win_w, m_win_h, SWP_NOMOVE | SWP_NOACTIVATE);
        }
        if (m_edit_key && IsWindow(m_edit_key)) {
            if (is_groq) {
                int ex = (int)std::lround((m_rc_key_box.X + 8.0f) * m_scale);
                int ey = (int)std::lround((m_rc_key_box.Y + 7.0f) * m_scale);
                int ew = (int)std::lround((m_rc_key_box.Width - 16.0f) * m_scale);
                int eh = (int)std::lround(18.0f * m_scale);
                SetWindowPos(m_edit_key, nullptr, ex, ey, ew, eh, SWP_NOZORDER | SWP_SHOWWINDOW);
            } else {
                ShowWindow(m_edit_key, SW_HIDE);
            }
        }
    }
}

SettingsWindow::HitId SettingsWindow::hit_test(int px, int py) const {
    float x = (float)px / m_scale;
    float y = (float)py / m_scale;
    if (pt_in(m_rc_close, x, y)) return HitId::CloseBtn;
    if (y < 44.0f) return HitId::TitleBar;
    if (pt_in(m_rc_btn_hk, x, y)) return HitId::RecordHotkeyBtn;
    if (pt_in(m_rc_cb_mic, x, y)) return HitId::MicDropdown;
    if (pt_in(m_rc_cb_eng, x, y)) return HitId::EngineDropdown;
    if (m_cfg.engine == "Groq (Whisper API)" && pt_in(m_rc_btn_eye, x, y)) return HitId::EyeBtn;
    if (pt_in(m_rc_cb_lang, x, y)) return HitId::LangDropdown;
    if (pt_in(m_rc_punct_row, x, y)) return HitId::PunctToggle;
    if (pt_in(m_rc_autostart_row, x, y)) return HitId::AutoStartToggle;
    if (pt_in(m_rc_theme_dark, x, y)) return HitId::ThemeDarkBtn;
    if (pt_in(m_rc_theme_light, x, y)) return HitId::ThemeLightBtn;
    return HitId::None;
}

void SettingsWindow::paint_window(HDC target_dc) {
    if (!m_hwnd) return;
    if (!m_bb_dc || m_bb_w != m_win_w || m_bb_h != m_win_h) {
        if (m_bb_dc) DeleteDC(m_bb_dc);
        if (m_bb_bmp) DeleteObject(m_bb_bmp);
        m_bb_dc = CreateCompatibleDC(target_dc);
        m_bb_bmp = CreateCompatibleBitmap(target_dc, m_win_w, m_win_h);
        SelectObject(m_bb_dc, m_bb_bmp);
        m_bb_w = m_win_w;
        m_bb_h = m_win_h;
    }

    const float s = m_scale;
    const auto& pal = m_pal;
    const bool is_groq = (m_cfg.engine == "Groq (Whisper API)");

    Gdiplus::Graphics g(m_bb_dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

    Gdiplus::SolidBrush br_win(theme::gcol(pal.window_bg));
    g.FillRectangle(&br_win, 0, 0, m_win_w, m_win_h);

    // Subtle outer border
    Gdiplus::Pen pen_outer(theme::gcol(pal.border), 1.2f * s);
    g.DrawRectangle(&pen_outer, 0.5f, 0.5f, (float)m_win_w - 1.0f, (float)m_win_h - 1.0f);

    // Header strip
    Gdiplus::SolidBrush br_hdr(theme::gcol(pal.card_bg));
    g.FillRectangle(&br_hdr, 1.0f, 1.0f, (float)m_win_w - 2.0f, 44.0f * s);
    Gdiplus::Pen pen_sep(theme::gcol(pal.border_subtle), 1.0f * s);
    g.DrawLine(&pen_sep, 0.0f, 44.0f * s, (float)m_win_w, 44.0f * s);

    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::FontFamily ff_mono(L"Consolas");
    Gdiplus::Font f_title(&ff, 11.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font f_sec(&ff, 8.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font f_norm(&ff, 9.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::Font f_bold(&ff, 9.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font f_small(&ff, 8.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::Font f_hk(&ff_mono, 10.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);

    Gdiplus::StringFormat sf_left;
    sf_left.SetAlignment(Gdiplus::StringAlignmentNear);
    sf_left.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    sf_left.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    sf_left.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    Gdiplus::StringFormat sf_center;
    sf_center.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf_center.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    Gdiplus::SolidBrush br_txt(theme::gcol(pal.text_primary));
    Gdiplus::SolidBrush br_sec(theme::gcol(pal.text_secondary));
    Gdiplus::SolidBrush br_muted(theme::gcol(pal.text_muted));
    Gdiplus::SolidBrush br_green(theme::gcol(pal.accent_green));
    Gdiplus::SolidBrush br_amber(theme::gcol(pal.accent_amber));
    Gdiplus::SolidBrush br_card(theme::gcol(pal.card_bg));
    Gdiplus::SolidBrush br_input(theme::gcol(pal.input_bg));
    Gdiplus::Pen pen_card(theme::gcol(pal.border_subtle), 1.1f * s);
    Gdiplus::Pen pen_input(theme::gcol(pal.border), 1.0f * s);

    // Header dot + title + close X
    g.FillEllipse(&br_green, 18.0f * s, 17.5f * s, 9.0f * s, 9.0f * s);
    g.DrawString(L"Configura\u00E7\u00F5es", -1, &f_title, rdp(34.0f, 1.0f, 240.0f, 43.0f), &sf_left, &br_txt);

    {
        Gdiplus::GraphicsPath cp;
        theme::add_round_rect_path(cp, sc(m_rc_close.X), sc(m_rc_close.Y), sc(m_rc_close.Width), sc(m_rc_close.Height), 7.0f * s);
        if (m_hover == HitId::CloseBtn) {
            Gdiplus::SolidBrush br_red(theme::gcol_alpha(pal.accent_danger, 45));
            g.FillPath(&br_red, &cp);
        }
        Gdiplus::SolidBrush br_x(theme::gcol(m_hover == HitId::CloseBtn ? pal.accent_danger : pal.text_muted));
        g.DrawString(L"\u2715", -1, &f_bold,
                     rdp(m_rc_close.X, m_rc_close.Y, m_rc_close.Width, m_rc_close.Height), &sf_center, &br_x);
    }

    auto draw_card = [&](float y_dp, float h_dp, const wchar_t* label) {
        Gdiplus::GraphicsPath p;
        theme::add_round_rect_path(p, 16.0f * s, y_dp * s, 364.0f * s, h_dp * s, 12.0f * s);
        g.FillPath(&br_card, &p);
        g.DrawPath(&pen_card, &p);
        g.DrawString(label, -1, &f_sec, rdp(30.0f, y_dp + 10.0f, 300.0f, 16.0f), &sf_left, &br_muted);
    };

    auto draw_pill_btn = [&](const Gdiplus::RectF& rc, const wchar_t* text, HitId id, bool active = false) {
        Gdiplus::GraphicsPath p;
        theme::add_round_rect_path(p, sc(rc.X), sc(rc.Y), sc(rc.Width), sc(rc.Height), 8.0f * s);
        DWORD bg = active ? pal.accent_green : ((m_hover == id) ? pal.btn_hover : pal.btn_bg);
        Gdiplus::SolidBrush br(theme::gcol(bg));
        g.FillPath(&br, &p);
        Gdiplus::SolidBrush txt_br(theme::gcol(active ? 0xFFFFFFFF : pal.text_primary));
        g.DrawString(text, -1, &f_bold, rdp(rc.X, rc.Y, rc.Width, rc.Height), &sf_center, &txt_br);
    };

    auto draw_dropdown = [&](const Gdiplus::RectF& rc, const std::wstring& val, HitId id) {
        Gdiplus::GraphicsPath p;
        theme::add_round_rect_path(p, sc(rc.X), sc(rc.Y), sc(rc.Width), sc(rc.Height), 8.0f * s);
        Gdiplus::SolidBrush br(theme::gcol(m_hover == id ? pal.hover_bg : pal.input_bg));
        g.FillPath(&br, &p);
        Gdiplus::Pen pen(theme::gcol(m_hover == id ? pal.accent_green : pal.border), 1.0f * s);
        g.DrawPath(&pen, &p);
        g.DrawString(val.c_str(), -1, &f_norm,
                     rdp(rc.X + 12.0f, rc.Y, rc.Width - 36.0f, rc.Height), &sf_left, &br_txt);
        draw_chevron(g, (rc.X + rc.Width - 16.0f) * s, (rc.Y + rc.Height * 0.5f) * s, s, pal.text_muted);
    };

    float y = 56.0f;

    // 1. Hotkey card
    draw_card(y, 74.0f, L"ATALHO GLOBAL");
    {
        Gdiplus::GraphicsPath kp;
        theme::add_round_rect_path(kp, 30.0f * s, (y + 32.0f) * s, 248.0f * s, 32.0f * s, 8.0f * s);
        g.FillPath(&br_input, &kp);
        g.DrawPath(&pen_input, &kp);
        std::wstring hk_txt = m_capturing_hotkey
            ? utf8_to_wide(m_hotkey_preview.empty() ? "Pressione..." : m_hotkey_preview)
            : utf8_to_wide(m_cfg.hotkey);
        g.DrawString(hk_txt.c_str(), -1, &f_hk,
                     rdp(42.0f, y + 32.0f, 224.0f, 32.0f), &sf_left,
                     m_capturing_hotkey ? &br_amber : &br_green);
        draw_pill_btn(m_rc_btn_hk, m_capturing_hotkey ? L"..." : L"Gravar", HitId::RecordHotkeyBtn, m_capturing_hotkey);
    }
    y += 84.0f;

    // 2. Microphone card
    draw_card(y, 74.0f, L"MICROFONE");
    {
        std::wstring mic_disp = m_cfg.microphone_name.empty()
            ? L"Padr\u00E3o do Sistema"
            : utf8_to_wide(m_cfg.microphone_name);
        draw_dropdown(m_rc_cb_mic, mic_disp, HitId::MicDropdown);
    }
    y += 84.0f;

    // 3. Engine & Groq key card
    draw_card(y, is_groq ? 122.0f : 74.0f, L"MOTOR DE TRANSCRI\u00C7\u00C3O");
    draw_dropdown(m_rc_cb_eng, utf8_to_wide(m_cfg.engine), HitId::EngineDropdown);
    if (is_groq) {
        g.DrawString(L"API Key:", -1, &f_norm, rdp(30.0f, y + 76.0f, 78.0f, 32.0f), &sf_left, &br_sec);
        Gdiplus::GraphicsPath ip;
        theme::add_round_rect_path(ip, sc(m_rc_key_box.X), sc(m_rc_key_box.Y), sc(m_rc_key_box.Width), sc(m_rc_key_box.Height), 8.0f * s);
        g.FillPath(&br_input, &ip);
        g.DrawPath(&pen_input, &ip);
        draw_pill_btn(m_rc_btn_eye, m_key_visible ? L"Ocultar" : L"Ver", HitId::EyeBtn);
    }
    y += (is_groq ? 132.0f : 84.0f);

    // 4. Language card
    draw_card(y, 74.0f, L"IDIOMA DA FALA");
    draw_dropdown(m_rc_cb_lang, lang_display(m_cfg.language), HitId::LangDropdown);
    y += 84.0f;

    // 5. Preferences & Appearance card
    {
        Gdiplus::GraphicsPath p;
        theme::add_round_rect_path(p, 16.0f * s, y * s, 364.0f * s, 132.0f * s, 12.0f * s);
        g.FillPath(&br_card, &p);
        g.DrawPath(&pen_card, &p);

        auto draw_switch_row = [&](float row_y, const wchar_t* label, bool checked) {
            g.DrawString(label, -1, &f_norm,
                         rdp(30.0f, row_y, 260.0f, 34.0f), &sf_left, &br_txt);
            float sw_x = 324.0f * s, sw_y = (row_y + 6.0f) * s, sw_w = 42.0f * s, sw_h = 22.0f * s;
            Gdiplus::GraphicsPath swp;
            theme::add_round_rect_path(swp, sw_x, sw_y, sw_w, sw_h, 11.0f * s);
            DWORD sw_col = checked ? pal.accent_green : pal.mic_idle_circle;
            Gdiplus::SolidBrush sw_br(theme::gcol(sw_col));
            g.FillPath(&sw_br, &swp);
            float thumb_d = 16.0f * s;
            float thumb_x = checked ? (sw_x + sw_w - thumb_d - 3.0f * s) : (sw_x + 3.0f * s);
            Gdiplus::SolidBrush thumb_br(Gdiplus::Color(255, 255, 255, 255));
            g.FillEllipse(&thumb_br, thumb_x, sw_y + 3.0f * s, thumb_d, thumb_d);
        };

        draw_switch_row(y + 10.0f, L"Pontua\u00E7\u00E3o Autom\u00E1tica", m_cfg.auto_punctuation);
        g.DrawLine(&pen_sep, 30.0f * s, (y + 47.0f) * s, 366.0f * s, (y + 47.0f) * s);

        draw_switch_row(y + 51.0f, L"Iniciar com o Windows", m_cfg.auto_start);
        g.DrawLine(&pen_sep, 30.0f * s, (y + 88.0f) * s, 366.0f * s, (y + 88.0f) * s);

        // Theme selector buttons
        g.DrawString(L"Tema da Interface", -1, &f_norm,
                     rdp(30.0f, y + 92.0f, 160.0f, 30.0f), &sf_left, &br_txt);
        draw_pill_btn(m_rc_theme_dark, L"Escuro", HitId::ThemeDarkBtn, m_cfg.theme != "light");
        draw_pill_btn(m_rc_theme_light, L"Claro", HitId::ThemeLightBtn, m_cfg.theme == "light");
    }

    // Footer hint
    g.DrawString(L"Salvo automaticamente em config.json", -1, &f_small,
                 rdp(20.0f, (float)m_win_h / s - 22.0f, 356.0f, 18.0f), &sf_center, &br_muted);

    BitBlt(target_dc, 0, 0, m_win_w, m_win_h, m_bb_dc, 0, 0, SRCCOPY);
}

void SettingsWindow::start_async_mics() {
    HWND target = m_hwnd;
    HANDLE h = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
        HWND hwnd = (HWND)param;
        auto* mics = new std::vector<MicDeviceInfo>(audio_engine::enumerate_mme_microphones());
        if (!PostMessageW(hwnd, WM_APP_MICS_READY, 0, (LPARAM)mics)) {
            delete mics;
        }
        return 0;
    }, target, 0, nullptr);
    if (h) CloseHandle(h);
}

void SettingsWindow::apply_mics_list(const std::vector<MicDeviceInfo>& mics) {
    m_mics = mics;
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SettingsWindow::open_mic_dropdown() {
    bool deferred = false;
    if (popup_card::check_toggle_or_dismiss("cfg_mic", &deferred)) {
        if (deferred && m_hwnd) PostMessageW(m_hwnd, WM_APP_DEFERRED_DD, 1, 0);
        return;
    }
    if (m_mics.empty()) {
        m_mics = audio_engine::enumerate_mme_microphones();
    }
    std::vector<popup_card::PopupItem> items;
    items.push_back({ 0, L"Padr\u00E3o do Sistema", m_cfg.microphone_name.empty(), false, false });
    for (size_t i = 0; i < m_mics.size(); ++i) {
        bool chk = !m_cfg.microphone_name.empty() && device_name_matches(m_cfg.microphone_name, m_mics[i].name);
        std::string label = m_mics[i].name + " [" + short_api_tag(m_mics[i].api) + "]";
        items.push_back({ (int)(i + 1), utf8_to_wide(label), chk, false, false });
    }
    POINT pt{ (LONG)sc(m_rc_cb_mic.X), (LONG)sc(m_rc_cb_mic.Y + m_rc_cb_mic.Height + 4.0f) };
    ClientToScreen(m_hwnd, &pt);
    int res = popup_card::show_popup_menu(m_hwnd, pt.x, pt.y, items, m_pal, 336, "cfg_mic");
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    SetForegroundWindow(m_hwnd);
    if (res < 0) return;

    AppSettings old_cfg = m_cfg;
    if (res == 0) {
        m_cfg.microphone_index = -1;
        m_cfg.microphone_name.clear();
    } else if ((size_t)(res - 1) < m_mics.size()) {
        m_cfg.microphone_index = m_mics[(size_t)(res - 1)].index;
        m_cfg.microphone_name = m_mics[(size_t)(res - 1)].name;
    }
    LOGI("[OK] Microfone salvo nas configs: %s (idx=%d)",
         m_cfg.microphone_name.empty() ? "padrao" : m_cfg.microphone_name.c_str(), m_cfg.microphone_index);
    commit_and_notify(old_cfg);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SettingsWindow::open_engine_dropdown() {
    bool deferred = false;
    if (popup_card::check_toggle_or_dismiss("cfg_eng", &deferred)) {
        if (deferred && m_hwnd) PostMessageW(m_hwnd, WM_APP_DEFERRED_DD, 2, 0);
        return;
    }
    std::vector<popup_card::PopupItem> items = {
        { 0, L"Google (Gratuito)", m_cfg.engine == "Google (Gratuito)", false, false },
        { 1, L"Groq (Whisper API)", m_cfg.engine == "Groq (Whisper API)", false, false },
    };
    POINT pt{ (LONG)sc(m_rc_cb_eng.X), (LONG)sc(m_rc_cb_eng.Y + m_rc_cb_eng.Height + 4.0f) };
    ClientToScreen(m_hwnd, &pt);
    int res = popup_card::show_popup_menu(m_hwnd, pt.x, pt.y, items, m_pal, 336, "cfg_eng");
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    SetForegroundWindow(m_hwnd);
    if (res < 0) return;

    AppSettings old_cfg = m_cfg;
    m_cfg.engine = (res == 1) ? "Groq (Whisper API)" : "Google (Gratuito)";
    LOGI("[OK] Motor salvo: %s", m_cfg.engine.c_str());
    update_layout();
    commit_and_notify(old_cfg);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SettingsWindow::open_lang_dropdown() {
    bool deferred = false;
    if (popup_card::check_toggle_or_dismiss("cfg_lang", &deferred)) {
        if (deferred && m_hwnd) PostMessageW(m_hwnd, WM_APP_DEFERRED_DD, 3, 0);
        return;
    }
    struct LOpt { const char* code; const wchar_t* label; };
    static const LOpt opts[] = {
        { "pt-BR", L"Portugu\u00EAs (Brasil)" },
        { "auto", L"Autom\u00E1tico (detectar idioma)" },
        { "translate_en", L"Traduzir para Ingl\u00EAs (PT \u2192 EN)" },
        { "translate_es", L"Traduzir para Espanhol (PT \u2192 ES)" },
    };
    std::vector<popup_card::PopupItem> items;
    for (int i = 0; i < 4; ++i) {
        items.push_back({ i, opts[i].label, m_cfg.language == opts[i].code, false, false });
    }
    POINT pt{ (LONG)sc(m_rc_cb_lang.X), (LONG)sc(m_rc_cb_lang.Y + m_rc_cb_lang.Height + 4.0f) };
    ClientToScreen(m_hwnd, &pt);
    int res = popup_card::show_popup_menu(m_hwnd, pt.x, pt.y, items, m_pal, 336, "cfg_lang");
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    SetForegroundWindow(m_hwnd);
    if (res < 0 || res >= 4) return;

    AppSettings old_cfg = m_cfg;
    m_cfg.language = opts[res].code;
    LOGI("[OK] Idioma salvo: %s", m_cfg.language.c_str());
    commit_and_notify(old_cfg);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

LRESULT CALLBACK SettingsWindow::kb_hook_proc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && s_active_hook_owner && s_active_hook_owner->m_capturing_hotkey) {
        const auto* kb = (const KBDLLHOOKSTRUCT*)lp;
        DWORD vk = kb->vkCode;
        bool is_down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        bool is_up = (wp == WM_KEYUP || wp == WM_SYSKEYUP);

        auto mod_flag_for = [](DWORD v) -> UINT {
            if (v == VK_CONTROL || v == VK_LCONTROL || v == VK_RCONTROL) return MOD_CONTROL;
            if (v == VK_MENU || v == VK_LMENU || v == VK_RMENU) return MOD_ALT;
            if (v == VK_SHIFT || v == VK_LSHIFT || v == VK_RSHIFT) return MOD_SHIFT;
            if (v == VK_LWIN || v == VK_RWIN) return MOD_WIN;
            return 0;
        };

        UINT mf = mod_flag_for(vk);
        if (mf) {
            if (is_down) s_held_mods |= mf;
            else if (is_up) s_held_mods &= ~mf;

            std::string preview;
            if (s_held_mods & MOD_ALT) preview += "Alt+";
            if (s_held_mods & MOD_CONTROL) preview += "Ctrl+";
            if (s_held_mods & MOD_SHIFT) preview += "Shift+";
            if (s_held_mods & MOD_WIN) preview += "Win+";
            preview += "...";
            auto* msg_str = new std::string(preview);
            if (!PostMessageW(s_active_hook_owner->m_hwnd, WM_APP_HOTKEY_PREVIEW, 0, (LPARAM)msg_str)) {
                delete msg_str;
            }
            return 1;
        }

        if (is_down && s_held_mods != 0) {
            std::string key_name;
            if (vk >= 'A' && vk <= 'Z') key_name = std::string(1, (char)vk);
            else if (vk >= '0' && vk <= '9') key_name = std::string(1, (char)vk);
            else if (vk >= VK_F1 && vk <= VK_F12) key_name = "F" + std::to_string((int)(vk - VK_F1 + 1));
            else if (vk == VK_SPACE) key_name = "Space";
            else if (vk == VK_TAB) key_name = "Tab";

            if (!key_name.empty()) {
                std::string combo;
                if (s_held_mods & MOD_ALT) combo += "Alt+";
                if (s_held_mods & MOD_CONTROL) combo += "Ctrl+";
                if (s_held_mods & MOD_SHIFT) combo += "Shift+";
                if (s_held_mods & MOD_WIN) combo += "Win+";
                combo += key_name;
                auto* done_str = new std::string(combo);
                if (!PostMessageW(s_active_hook_owner->m_hwnd, WM_APP_HOTKEY_DONE, 0, (LPARAM)done_str)) {
                    delete done_str;
                }
                return 1;
            }
        }
    }
    return CallNextHookEx(s_kb_hook, code, wp, lp);
}

void SettingsWindow::start_hotkey_capture() {
    stop_hotkey_capture();
    m_capturing_hotkey = true;
    m_hotkey_preview = "Pressione...";
    s_held_mods = 0;
    s_active_hook_owner = this;
    s_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, &SettingsWindow::kb_hook_proc, nullptr, 0);
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SettingsWindow::stop_hotkey_capture() {
    m_capturing_hotkey = false;
    s_held_mods = 0;
    if (s_kb_hook) {
        UnhookWindowsHookEx(s_kb_hook);
        s_kb_hook = nullptr;
    }
    if (s_active_hook_owner == this) s_active_hook_owner = nullptr;
}

void SettingsWindow::commit_and_notify(const AppSettings& old_cfg) {
    json_util::save_settings_file(get_exe_dir() + L"\\config.json", m_cfg);
    if (m_on_save) {
        m_on_save(old_cfg, m_cfg);
    }
}

LRESULT CALLBACK SettingsWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        self = (SettingsWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (SettingsWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (self) return self->handle_msg(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SettingsWindow::handle_msg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            if (m_font_edit) DeleteObject(m_font_edit);
            m_font_edit = CreateFontW(-(int)std::lround(14.0f * m_scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
            m_populating = true;
            m_edit_key = CreateWindowExW(
                0, L"EDIT", utf8_to_wide(m_cfg.groq_api_key).c_str(),
                WS_CHILD | ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP,
                0, 0, 100, 20,
                hwnd, (HMENU)(INT_PTR)IDC_EDIT_KEY, GetModuleHandleW(nullptr), nullptr
            );
            if (m_edit_key && m_font_edit) {
                SendMessageW(m_edit_key, WM_SETFONT, (WPARAM)m_font_edit, TRUE);
            }
            m_populating = false;
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && !m_capturing_hotkey) {
                close();
                return 0;
            }
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            paint_window(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_NCHITTEST: {
            POINT pt{ (int)(short)LOWORD(lp), (int)(short)HIWORD(lp) };
            ScreenToClient(hwnd, &pt);
            if (hit_test(pt.x, pt.y) == HitId::TitleBar) return HTCAPTION;
            return HTCLIENT;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, m_pal.cr_text);
            SetBkColor(hdc, m_pal.cr_input_bg);
            return (LRESULT)m_br_input;
        }

        case WM_MOUSEMOVE: {
            if (!m_tracking_mouse) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                m_tracking_mouse = true;
            }
            HitId h = hit_test((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
            if (h != m_hover) {
                m_hover = h;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            m_tracking_mouse = false;
            if (m_hover != HitId::None) {
                m_hover = HitId::None;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            HitId h = hit_test((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
            if (h == HitId::CloseBtn) {
                close();
                return 0;
            }
            if (h == HitId::RecordHotkeyBtn) {
                start_hotkey_capture();
                return 0;
            }
            if (h == HitId::MicDropdown) {
                open_mic_dropdown();
                return 0;
            }
            if (h == HitId::EngineDropdown) {
                open_engine_dropdown();
                return 0;
            }
            if (h == HitId::EyeBtn) {
                m_key_visible = !m_key_visible;
                if (m_edit_key) {
                    SendMessageW(m_edit_key, EM_SETPASSWORDCHAR, m_key_visible ? 0 : (WPARAM)L'*', 0);
                    InvalidateRect(m_edit_key, nullptr, TRUE);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (h == HitId::LangDropdown) {
                open_lang_dropdown();
                return 0;
            }
            if (h == HitId::PunctToggle) {
                AppSettings old_cfg = m_cfg;
                m_cfg.auto_punctuation = !m_cfg.auto_punctuation;
                LOGI("[OK] Pontuacao automatica: %s", m_cfg.auto_punctuation ? "Ativado" : "Desativado");
                commit_and_notify(old_cfg);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (h == HitId::AutoStartToggle) {
                AppSettings old_cfg = m_cfg;
                m_cfg.auto_start = !m_cfg.auto_start;
                m_cfg.has_auto_start_pref = true;
                bool reg_ok = set_windows_auto_start(m_cfg.auto_start);
                LOGI("[OK] Iniciar com o Windows: %s (status=%s)",
                     m_cfg.auto_start ? "Ativado" : "Desativado",
                     reg_ok ? "ok" : "falhou");
                commit_and_notify(old_cfg);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (h == HitId::ThemeDarkBtn || h == HitId::ThemeLightBtn) {
                std::string target_theme = (h == HitId::ThemeLightBtn) ? "light" : "dark";
                if (target_theme != m_cfg.theme) {
                    AppSettings old_cfg = m_cfg;
                    m_cfg.theme = target_theme;
                    refresh_theme_brushes();
                    if (m_edit_key) InvalidateRect(m_edit_key, nullptr, TRUE);
                    LOGI("[OK] Tema alterado: %s", m_cfg.theme.c_str());
                    commit_and_notify(old_cfg);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wp) == IDC_EDIT_KEY && HIWORD(wp) == EN_CHANGE && !m_populating) {
                SetTimer(hwnd, TIMER_SAVE_APIKEY, 650, nullptr);
                return 0;
            }
            break;

        case WM_TIMER:
            if (wp == TIMER_SAVE_APIKEY) {
                KillTimer(hwnd, TIMER_SAVE_APIKEY);
                if (m_edit_key) {
                    wchar_t buf[512] = {};
                    GetWindowTextW(m_edit_key, buf, 512);
                    std::string new_key = wide_to_utf8(buf);
                    if (new_key != m_cfg.groq_api_key) {
                        AppSettings old_cfg = m_cfg;
                        m_cfg.groq_api_key = new_key;
                        commit_and_notify(old_cfg);
                    }
                }
                return 0;
            }
            break;

        case WM_APP_MICS_READY: {
            auto* mics = (std::vector<MicDeviceInfo>*)lp;
            if (mics) {
                apply_mics_list(*mics);
                delete mics;
            }
            return 0;
        }

        case WM_APP_HOTKEY_PREVIEW: {
            auto* s = (std::string*)lp;
            if (s) {
                m_hotkey_preview = *s;
                delete s;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_APP_HOTKEY_DONE: {
            auto* s = (std::string*)lp;
            stop_hotkey_capture();
            if (s) {
                AppSettings old_cfg = m_cfg;
                m_cfg.hotkey = *s;
                LOGI("[OK] Novo atalho salvo: %s", m_cfg.hotkey.c_str());
                delete s;
                commit_and_notify(old_cfg);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_APP_DEFERRED_DD:
            if (wp == 1) open_mic_dropdown();
            else if (wp == 2) open_engine_dropdown();
            else if (wp == 3) open_lang_dropdown();
            return 0;

        case WM_DESTROY:
            stop_hotkey_capture();
            m_hwnd = nullptr;
            m_edit_key = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace settings_ui
