#pragma once

#include "common.h"
#include "theme.h"
#include <functional>

namespace settings_ui {

class SettingsWindow {
public:
    using OnSaveCallback = std::function<void(const AppSettings& old_cfg, const AppSettings& new_cfg)>;

    SettingsWindow();
    ~SettingsWindow();

    void show(HWND parent_hwnd, const AppSettings& current_cfg, OnSaveCallback on_save);
    void close();
    HWND hwnd() const { return m_hwnd; }

private:
    enum class HitId {
        None,
        TitleBar,
        CloseBtn,
        RecordHotkeyBtn,
        MicDropdown,
        EngineDropdown,
        EyeBtn,
        LangDropdown,
        PunctToggle,
        AutoStartToggle,
        ThemeDarkBtn,
        ThemeLightBtn
    };

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK kb_hook_proc(int code, WPARAM wp, LPARAM lp);

    LRESULT handle_msg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void update_layout();
    void refresh_theme_brushes();
    void paint_window(HDC target_dc);
    HitId hit_test(int px, int py) const;

    void start_async_mics();
    void apply_mics_list(const std::vector<MicDeviceInfo>& mics);
    void open_mic_dropdown();
    void open_engine_dropdown();
    void open_lang_dropdown();

    void start_hotkey_capture();
    void stop_hotkey_capture();
    void commit_and_notify(const AppSettings& old_cfg);

    float sc(float dp) const { return dp * m_scale; }
    Gdiplus::RectF rdp(float x, float y, float w, float h) const {
        return Gdiplus::RectF(x * m_scale, y * m_scale, w * m_scale, h * m_scale);
    }

    HWND m_hwnd = nullptr;
    HWND m_parent = nullptr;
    HWND m_edit_key = nullptr;

    AppSettings m_cfg;
    theme::ThemePalette m_pal;
    OnSaveCallback m_on_save;
    float m_scale = 1.0f;
    int m_win_w = 396;
    int m_win_h = 604;

    HFONT m_font_edit = nullptr;
    HBRUSH m_br_bg = nullptr;
    HBRUSH m_br_input = nullptr;

    // Backbuffer DIB for flicker-free GDI+ WM_PAINT
    HDC m_bb_dc = nullptr;
    HBITMAP m_bb_bmp = nullptr;
    int m_bb_w = 0;
    int m_bb_h = 0;

    // Hitboxes in DIPs (updated dynamically when Groq row toggles visibility)
    Gdiplus::RectF m_rc_close;
    Gdiplus::RectF m_rc_btn_hk;
    Gdiplus::RectF m_rc_cb_mic;
    Gdiplus::RectF m_rc_cb_eng;
    Gdiplus::RectF m_rc_key_box;
    Gdiplus::RectF m_rc_btn_eye;
    Gdiplus::RectF m_rc_cb_lang;
    Gdiplus::RectF m_rc_punct_row;
    Gdiplus::RectF m_rc_autostart_row;
    Gdiplus::RectF m_rc_theme_dark;
    Gdiplus::RectF m_rc_theme_light;

    HitId m_hover = HitId::None;
    bool m_tracking_mouse = false;
    bool m_key_visible = false;
    bool m_populating = false;
    bool m_capturing_hotkey = false;
    std::string m_hotkey_preview;
    std::vector<MicDeviceInfo> m_mics;

    static SettingsWindow* s_active_hook_owner;
    static HHOOK s_kb_hook;
    static UINT s_held_mods;
};

} // namespace settings_ui
