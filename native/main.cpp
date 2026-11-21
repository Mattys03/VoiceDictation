#include "common.h"
#include "logger.h"
#include "json_util.h"
#include "text_proc.h"
#include "audio_engine.h"
#include "net_client.h"
#include "injector.h"
#include "settings_ui.h"
#include "theme.h"
#include "popup_card.h"

#include <objidl.h>
#include <gdiplus.h>
#include <dbt.h>
#include <shellapi.h>
#include <deque>
#include <cmath>
#include <memory>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

constexpr int kBaseW = 240;
constexpr int kBaseH = 130;
constexpr int kBaseSW = 190;
constexpr int kBaseSH = 38;
constexpr int kHotkeyId = 42069;

constexpr UINT WM_APP_TOGGLE_IPC = WM_APP + 1;
constexpr UINT WM_APP_AUDIO_STATUS = WM_APP + 2;
constexpr UINT WM_APP_AUDIO_DONE = WM_APP + 3;
constexpr UINT WM_APP_JOB_DONE = WM_APP + 4;
constexpr UINT WM_APP_PASTE_DONE = WM_APP + 5;
constexpr UINT WM_APP_HOTKEY_FALLBACK = WM_APP + 6;
constexpr UINT WM_APP_ADOPT_MIC = WM_APP + 7;
constexpr UINT WM_APP_DEFERRED_HISTORY = WM_APP + 8;
constexpr UINT WM_APP_DEFERRED_LANG = WM_APP + 9;
constexpr UINT WM_APP_CLOSE_SETTINGS = WM_APP + 10;

constexpr UINT_PTR TIMER_ANIM = 10;
constexpr UINT_PTR TIMER_FOCUS_STATUS = 11;
constexpr UINT_PTR TIMER_STATUS_RESET = 12;

constexpr int kMenuLangBase = 2000;
constexpr int kMenuHistBase = 3000;

struct CachedDib {
    int w = 0;
    int h = 0;
    HBITMAP hbm = nullptr;
    HDC hdc = nullptr;
    void* bits = nullptr;

    void release() {
        if (hdc) { DeleteDC(hdc); hdc = nullptr; }
        if (hbm) { DeleteObject(hbm); hbm = nullptr; }
        bits = nullptr;
        w = h = 0;
    }

    bool ensure(int nw, int nh) {
        if (w == nw && h == nh && hbm && hdc) return true;
        release();
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = nw;
        bmi.bmiHeader.biHeight = -nh; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        hdc = CreateCompatibleDC(screen);
        hbm = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!hdc || !hbm || !bits) {
            release();
            return false;
        }
        SelectObject(hdc, hbm);
        w = nw;
        h = nh;
        return true;
    }
    ~CachedDib() { release(); }
};

struct WorkerJob {
    enum class Type { PrewarmGroq, Transcribe, PasteHistory };
    Type type = Type::Transcribe;
    uint64_t session_id = 0;
    audio_engine::CapturedSession capture;
    AppSettings cfg;
    injector::TargetWindow target;
    std::string text;
};

struct AudioDoneMsg {
    uint64_t session_id = 0;
    audio_engine::CapturedSession session;
};

struct JobDoneMsg {
    uint64_t session_id = 0;
    bool had_text = false;
    std::string text;
    std::string status_override;
};

enum class HoverZone { None, Close, Gear, Help, Mic, Lang };

static void add_round_rect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2.0f;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

static bool pt_in_rectf(float px, float py, float x, float y, float w, float h) {
    return px >= x && px <= (x + w) && py >= y && py <= (y + h);
}

class NativeApp {
public:
    NativeApp() {
        m_own_pid = GetCurrentProcessId();
        InitializeCriticalSection(&m_hist_cs);
        InitializeCriticalSection(&m_job_cs);
        m_job_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_job_shutdown = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~NativeApp() {
        cleanup();
        DeleteCriticalSection(&m_hist_cs);
        DeleteCriticalSection(&m_job_cs);
        if (m_job_event) CloseHandle(m_job_event);
        if (m_job_shutdown) CloseHandle(m_job_shutdown);
    }

    bool init(bool start_silent) {
        m_dir = get_exe_dir();
        AppLogger::instance().init(m_dir + L"\\voice_dictation.log");
        m_cfg = json_util::load_settings_file(m_dir + L"\\config.json");
        if (!m_cfg.has_auto_start_pref) {
            m_cfg.auto_start = is_windows_auto_start_enabled();
        } else if (m_cfg.auto_start && !is_windows_auto_start_enabled()) {
            set_windows_auto_start(true);
        }

        register_classes();

        m_dpi = get_primary_dpi();
        float s = dpi_scale();
        int mw = (int)std::lround(kBaseW * s);
        int mh = (int)std::lround(kBaseH * s);
        int sw = (int)std::lround(kBaseSW * s);
        int sh = (int)std::lround(kBaseSH * s);

        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        m_x = wa.left + ((wa.right - wa.left) - mw) / 2;
        m_y = wa.bottom - mh - (int)std::lround(40 * s);
        m_positioned = true;

        HINSTANCE hInst = GetModuleHandleW(nullptr);
        m_hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"VoiceDictationMainPill", L"Voice Dictation",
            WS_POPUP,
            m_x, m_y, mw, mh,
            nullptr, nullptr, hInst, this
        );

        m_status_hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"VoiceDictationStatusPill", L"",
            WS_POPUP,
            m_x + (mw - sw) / 2, m_y - sh - (int)std::lround(10 * s), sw, sh,
            nullptr, nullptr, hInst, this
        );

        if (!m_hwnd || !m_status_hwnd) return false;

        m_recorder.start_thread();
        m_job_thread = CreateThread(nullptr, 0, &NativeApp::job_thread_proc, this, 0, nullptr);
        start_ipc_thread();

        setup_foreground_hook();
        setup_hotkey();

        // Warm audio & Groq TLS silently in background
        m_recorder.request_prewarm(m_cfg);
        if (m_cfg.engine == "Groq (Whisper API)" && !m_cfg.groq_api_key.empty()) {
            WorkerJob j;
            j.type = WorkerJob::Type::PrewarmGroq;
            j.cfg = m_cfg;
            enqueue_job(std::move(j));
        }

        LOGI("[OK] Voice Dictation Nativo (C++/Win32) ativo! Pressione %s para ditar.", m_cfg.hotkey.c_str());
        LOGI("     Usando: %s | Idioma: %s | Mic: %s",
             m_cfg.engine.c_str(), m_cfg.language.c_str(),
             m_cfg.microphone_name.empty() ? "padrao" : m_cfg.microphone_name.c_str());

        render_main(true);
        render_status(true);

        if (!start_silent) {
            show_popup();
        }
        return true;
    }

    int run() {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return (int)msg.wParam;
    }

    static NativeApp* s_instance;

private:
    float dpi_scale() const { return (float)m_dpi / 96.0f; }

    UINT get_primary_dpi() {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        typedef UINT (WINAPI *PFN_GetDpiForSystem)();
        if (u32) {
            auto fn = (PFN_GetDpiForSystem)GetProcAddress(u32, "GetDpiForSystem");
            if (fn) {
                UINT d = fn();
                if (d >= 96 && d <= 384) return d;
            }
        }
        HDC hdc = GetDC(nullptr);
        int d = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(nullptr, hdc);
        return d >= 96 ? (UINT)d : 96;
    }

    void cleanup() {
        m_closing = true;
        teardown_hotkey();
        if (m_fg_hook) { UnhookWinEvent(m_fg_hook); m_fg_hook = nullptr; }

        if (m_ipc_stop) SetEvent(m_ipc_stop);
        if (m_ipc_thread) {
            WaitForSingleObject(m_ipc_thread, 1500);
            CloseHandle(m_ipc_thread);
            m_ipc_thread = nullptr;
        }
        if (m_ipc_stop) { CloseHandle(m_ipc_stop); m_ipc_stop = nullptr; }

        m_recorder.stop_thread();

        SetEvent(m_job_shutdown);
        SetEvent(m_job_event);
        if (m_job_thread) {
            WaitForSingleObject(m_job_thread, 3000);
            CloseHandle(m_job_thread);
            m_job_thread = nullptr;
        }

        m_settings_win.close();
        if (m_status_hwnd && IsWindow(m_status_hwnd)) { DestroyWindow(m_status_hwnd); m_status_hwnd = nullptr; }
        if (m_hwnd && IsWindow(m_hwnd)) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    }

    void register_classes() {
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc1{};
        wc1.cbSize = sizeof(wc1);
        wc1.lpfnWndProc = &NativeApp::main_wnd_proc;
        wc1.hInstance = hInst;
        wc1.lpszClassName = L"VoiceDictationMainPill";
        wc1.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc1);

        WNDCLASSEXW wc2{};
        wc2.cbSize = sizeof(wc2);
        wc2.lpfnWndProc = &NativeApp::status_wnd_proc;
        wc2.hInstance = hInst;
        wc2.lpszClassName = L"VoiceDictationStatusPill";
        wc2.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc2);
    }

    // ─── Target Window Tracking ──────────────────────────────────────────

    static void CALLBACK win_event_proc(
        HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD
    ) {
        if (event == EVENT_SYSTEM_FOREGROUND && idObject == OBJID_WINDOW && s_instance && hwnd) {
            s_instance->capture_target_hwnd(hwnd, false);
        }
    }

    void setup_foreground_hook() {
        m_fg_hook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, &NativeApp::win_event_proc,
            0, 0, WINEVENT_OUTOFCONTEXT
        );
        HWND fg = GetForegroundWindow();
        if (fg) capture_target_hwnd(fg, true);
    }

    void capture_target_hwnd(HWND hwnd, bool refresh_label) {
        if (!hwnd) return;
        if (hwnd == m_hwnd || hwnd == m_status_hwnd || hwnd == m_settings_win.hwnd()) return;
        if (hwnd == m_target.hwnd) {
            m_target.focus = injector::get_focused_control(hwnd);
            if (refresh_label) m_target.label = injector::get_window_label(hwnd);
            return;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == m_own_pid || !IsWindow(hwnd)) return;
        m_target.hwnd = hwnd;
        m_target.focus = injector::get_focused_control(hwnd);
        m_target.label = injector::get_window_label(hwnd);
    }

    // ─── IPC Toggle Event ────────────────────────────────────────────────

    void start_ipc_thread() {
        m_ipc_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_ipc_thread = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            auto* self = (NativeApp*)p;
            HANDLE hEvt = CreateEventW(nullptr, FALSE, FALSE, L"VoiceDictation_Toggle_Event");
            if (!hEvt) return 0;
            HANDLE waits[2] = { hEvt, self->m_ipc_stop };
            while (true) {
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (w != WAIT_OBJECT_0) break;
                if (self->m_hwnd) PostMessageW(self->m_hwnd, WM_APP_TOGGLE_IPC, 0, 0);
            }
            CloseHandle(hEvt);
            return 0;
        }, this, 0, nullptr);
    }

    // ─── Global Hotkey (Kernel RegisterHotKey, WH_KEYBOARD_LL fallback only) ──

    static LRESULT CALLBACK fallback_kb_proc(int code, WPARAM wp, LPARAM lp) {
        if (code >= 0 && s_instance && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
            const auto* kb = (const KBDLLHOOKSTRUCT*)lp;
            if ((kb->flags & LLKHF_INJECTED) == 0 && kb->vkCode == s_instance->m_fallback_hk.vk) {
                UINT mods = 0;
                if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
                if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) mods |= MOD_WIN;
                if ((mods & s_instance->m_fallback_hk.mods) == s_instance->m_fallback_hk.mods) {
                    PostMessageW(s_instance->m_hwnd, WM_APP_HOTKEY_FALLBACK, 0, 0);
                }
            }
        }
        return CallNextHookEx(s_instance ? s_instance->m_fallback_hook : nullptr, code, wp, lp);
    }

    void teardown_hotkey() {
        if (m_hotkey_registered && m_hwnd) {
            UnregisterHotKey(m_hwnd, kHotkeyId);
            m_hotkey_registered = false;
        }
        if (m_fallback_hook) {
            UnhookWindowsHookEx(m_fallback_hook);
            m_fallback_hook = nullptr;
        }
    }

    void setup_hotkey() {
        teardown_hotkey();
        ParsedHotkey ph = parse_hotkey_win(m_cfg.hotkey);
        if (!ph.valid) {
            LOGE("[!] Hotkey invalida: %s", m_cfg.hotkey.c_str());
            return;
        }
        m_fallback_hk = ph;
        if (RegisterHotKey(m_hwnd, kHotkeyId, ph.mods | MOD_NOREPEAT, ph.vk)) {
            m_hotkey_registered = true;
            LOGI("[OK] Hotkey %s registrada (RegisterHotKey, kernel, sem hook)", m_cfg.hotkey.c_str());
        } else {
            m_fallback_hook = SetWindowsHookExW(WH_KEYBOARD_LL, &NativeApp::fallback_kb_proc, nullptr, 0);
            LOGI("[!] RegisterHotKey falhou (%lu); fallback WH_KEYBOARD_LL ativado (%s)",
                 GetLastError(), m_cfg.hotkey.c_str());
        }
    }

    // ─── Core Dictation Flow (Zero UI Blocking) ──────────────────────────

    void on_hotkey_toggle() {
        ULONGLONG now = GetTickCount64();
        if (now - m_last_hotkey_tick < 250) return;
        m_last_hotkey_tick = now;

        if (m_is_listening) {
            stop_listening();
            return;
        }

        HWND fg = GetForegroundWindow();
        if (fg) capture_target_hwnd(fg, true);

        if (m_worker_active || m_is_processing) {
            m_start_requested = true;
            show_popup();
            set_status("Aguarde...");
            LOGI("[!] Transcricao/sessao anterior finalizando; novo ditado enfileirado.");
            return;
        }
        start_listening();
    }

    void on_mic_click() {
        if (m_is_listening) {
            stop_listening();
        } else if (m_worker_active || m_is_processing) {
            m_start_requested = true;
            set_status("Aguarde...");
        } else {
            start_listening();
        }
    }

    void start_listening() {
        if (m_is_listening || m_worker_active || m_is_processing) return;

        HWND fg = GetForegroundWindow();
        if (fg) capture_target_hwnd(fg, true);
        else if (m_target.hwnd && IsWindow(m_target.hwnd)) {
            m_target.label = injector::get_window_label(m_target.hwnd);
        }

        ++m_session_id;
        uint64_t sid = m_session_id;
        m_is_listening = true;
        m_worker_active = true;
        m_record_start_tick = GetTickCount64();
        m_last_timer_sec = -1;

        if (!m_target.label.empty()) {
            LOGI("[OK] Deste ditado: %s", m_target.label.c_str());
        }

        show_popup();
        set_status("Ouvindo  00:00");
        start_timers();
        render_main(true);

        bool started = m_recorder.start_capture(
            sid, m_cfg,
            [this](uint64_t done_sid, audio_engine::CapturedSession&& sess) {
                auto* msg = new AudioDoneMsg{ done_sid, std::move(sess) };
                if (!PostMessageW(m_hwnd, WM_APP_AUDIO_DONE, 0, (LPARAM)msg)) {
                    delete msg;
                }
            },
            [this](uint64_t st_sid, const char* txt) {
                if (st_sid == m_session_id && txt) {
                    auto* copy = new std::string(txt);
                    if (!PostMessageW(m_hwnd, WM_APP_AUDIO_STATUS, (WPARAM)st_sid, (LPARAM)copy)) {
                        delete copy;
                    }
                }
            }
        );

        if (!started) {
            m_is_listening = false;
            m_worker_active = false;
            stop_timers();
            render_main(true);
        }
    }

    void stop_listening() {
        if (!m_is_listening) return;
        m_is_listening = false;
        m_recorder.stop_capture();
        set_status("Processando...");
        render_main(true);
    }

    void on_audio_done(std::unique_ptr<AudioDoneMsg> msg) {
        m_is_listening = false;
        if (!msg || msg->session_id != m_session_id) {
            m_worker_active = false;
            return;
        }

        auto& s = msg->session;
        if (s.adopted_new_mic && !s.resolved_mic_name.empty()) {
            m_cfg.microphone_name = s.resolved_mic_name;
            m_cfg.microphone_index = -1;
            json_util::save_settings_file(m_dir + L"\\config.json", m_cfg);
            LOGI("[OK] Microfone corrigido automaticamente para: %s", m_cfg.microphone_name.c_str());
        }

        if (s.aborted || (s.pcm16.empty() && !s.has_speech)) {
            if (s.aborted) {
                LOGE("[✗] Gravacao descartada: %s", s.mic_problem.empty() ? "nenhum audio capturado" : s.mic_problem.c_str());
                set_status("Sem \u00E1udio!");
            } else {
                LOGI("[!] Sem fala detectada (%.1fs, pico=%d, piso=%d).", s.duration_sec, s.peak, s.floor);
                set_status("Sem fala detectada");
            }
            m_worker_active = false;
            m_is_processing = false;
            stop_timers();
            render_main(true);
            maybe_launch_queued();
            return;
        }

        LOGI("[OK] Fala detectada (%.1fs, recorte=%.1fs, blocos=%zu..%zu, pico=%d, piso=%d); enviando para transcricao.",
             s.duration_sec, s.trimmed_sec, s.trim_start, s.trim_end, s.peak, s.floor);

        m_is_processing = true;
        set_status("Transcrevendo...");
        render_main(true);

        WorkerJob job;
        job.type = WorkerJob::Type::Transcribe;
        job.session_id = msg->session_id;
        job.capture = std::move(s);
        job.cfg = m_cfg;
        job.target = m_target;
        enqueue_job(std::move(job));
    }

    void on_job_done(std::unique_ptr<JobDoneMsg> msg) {
        m_is_processing = false;
        m_worker_active = false;
        stop_timers();

        if (msg && !msg->status_override.empty()) {
            set_status(msg->status_override);
        } else if (m_is_visible) {
            set_status("Pausado");
        }
        render_main(true);
        maybe_launch_queued();
    }

    void maybe_launch_queued() {
        if (m_start_requested) {
            m_start_requested = false;
            LOGI("[OK] Iniciando ditado que estava na fila.");
            start_listening();
        }
    }

    // ─── Worker Thread (HTTP Groq/Google + Text Post-Proc + Paste) ──────

    void enqueue_job(WorkerJob&& job) {
        EnterCriticalSection(&m_job_cs);
        m_job_queue.push_back(std::move(job));
        SetEvent(m_job_event);
        LeaveCriticalSection(&m_job_cs);
    }

    static DWORD WINAPI job_thread_proc(LPVOID p) {
        ((NativeApp*)p)->job_loop();
        return 0;
    }

    void job_loop() {
        HANDLE waits[2] = { m_job_event, m_job_shutdown };
        while (true) {
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 + 1) break;

            while (true) {
                WorkerJob job;
                bool has_job = false;
                EnterCriticalSection(&m_job_cs);
                if (!m_job_queue.empty()) {
                    job = std::move(m_job_queue.front());
                    m_job_queue.pop_front();
                    has_job = true;
                }
                LeaveCriticalSection(&m_job_cs);
                if (!has_job) break;

                if (job.type == WorkerJob::Type::PrewarmGroq) {
                    m_http.prewarm_groq(job.cfg.groq_api_key);
                } else if (job.type == WorkerJob::Type::PasteHistory) {
                    injector::TargetWindow tgt = job.target;
                    injector::type_text_into_target(job.text, tgt, m_own_pid);
                    PostMessageW(m_hwnd, WM_APP_PASTE_DONE, 0, 0);
                } else if (job.type == WorkerJob::Type::Transcribe) {
                    handle_transcribe_job(job);
                }
            }
        }
    }

    void handle_transcribe_job(WorkerJob& job) {
        auto& cap = job.capture;
        std::string raw_text;
        const size_t total_samples = cap.pcm16.size();
        const size_t pcm_bytes = total_samples * sizeof(int16_t);
        std::string lang_code = text_proc::whisper_language_code(job.cfg.language);

        if (job.cfg.engine == "Groq (Whisper API)" && !job.cfg.groq_api_key.empty()) {
            // Chunk at ~20MB (~10.4 min mono 16kHz) cleanly under Groq 25MB limit
            constexpr size_t kMaxChunkSamples = 10u * 1024u * 1024u;
            std::string full;
            for (size_t off = 0; off < total_samples; off += kMaxChunkSamples) {
                size_t cnt = std::min(kMaxChunkSamples, total_samples - off);
                std::vector<uint8_t> wav = audio_engine::build_wav_bytes(
                    cap.pcm16.data() + off, cnt, cap.sample_rate
                );
                std::string err;
                std::string part = m_http.transcribe_groq_wav(
                    job.cfg.groq_api_key, wav.data(), wav.size(), lang_code, &err
                );
                wav.clear();
                wav.shrink_to_fit();

                if (part.empty() && !err.empty()) {
                    LOGE("[!] Groq Whisper erro: %s", err.c_str());
                } else if (!part.empty()) {
                    if (job.cfg.language == "translate_en") {
                        part = m_http.translate_groq(job.cfg.groq_api_key, part, "English");
                    } else if (job.cfg.language == "translate_es") {
                        part = m_http.translate_groq(job.cfg.groq_api_key, part, "Spanish");
                    }
                    if (!full.empty()) full += " ";
                    full += part;
                }
            }
            raw_text = std::move(full);
        }

        // Google Free fallback if Groq disabled, missing key, or failed
        if (raw_text.empty()) {
            std::vector<uint8_t> flac = audio_engine::build_flac_verbatim_bytes(
                cap.pcm16.data(), total_samples, cap.sample_rate
            );
            std::string g_lang = "pt-BR";
            if (job.cfg.language != "auto" &&
                job.cfg.language != "translate_en" &&
                job.cfg.language != "translate_es" &&
                !job.cfg.language.empty()) {
                g_lang = job.cfg.language;
            }
            std::string gerr;
            raw_text = m_http.transcribe_google_flac(
                flac.data(), flac.size(), cap.sample_rate, g_lang, &gerr
            );
            if (raw_text.empty() && !gerr.empty()) {
                LOGE("[!] Google fallback erro: %s", gerr.c_str());
            }
        }

        // Release PCM audio immediately before paste
        cap.pcm16.clear();
        cap.pcm16.shrink_to_fit();

        bool caps_on = (GetKeyState(VK_CAPITAL) & 1) != 0;
        std::string blocked;
        std::string final_text = text_proc::filter_and_format_transcription(
            raw_text, pcm_bytes, cap.sample_rate, cap.peak,
            job.cfg.auto_punctuation, caps_on, &blocked
        );

        auto* res = new JobDoneMsg();
        res->session_id = job.session_id;

        if (!blocked.empty()) {
            LOGI("[!] Alucinacao de silencio bloqueada: '%s'", blocked.c_str());
        } else if (!final_text.empty()) {
            LOGI("  >> \"%s\"", final_text.c_str());
            EnterCriticalSection(&m_hist_cs);
            m_history.push_back(final_text);
            if (m_history.size() > 5) m_history.pop_front();
            LeaveCriticalSection(&m_hist_cs);

            injector::type_text_into_target(final_text, job.target, m_own_pid);
            res->had_text = true;
            res->text = std::move(final_text);
        } else {
            LOGI("[✗] Nenhum texto retornado.");
        }

        if (!PostMessageW(m_hwnd, WM_APP_JOB_DONE, 0, (LPARAM)res)) {
            delete res;
        }
    }

    // ─── Popup Display, Timers, Menus & Layered GDI+ Drawing ────────────

    void show_popup() {
        m_is_visible = true;
        float s = dpi_scale();
        int mw = (int)std::lround(kBaseW * s);
        int mh = (int)std::lround(kBaseH * s);
        int sw = (int)std::lround(kBaseSW * s);
        int sh = (int)std::lround(kBaseSH * s);

        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        if (!m_positioned || m_x + mw < wa.left || m_x > wa.right || m_y + mh < wa.top || m_y > wa.bottom) {
            m_x = wa.left + ((wa.right - wa.left) - mw) / 2;
            m_y = wa.bottom - mh - (int)std::lround(40 * s);
            m_positioned = true;
        }

        int sx = m_x + (mw - sw) / 2;
        int sy = m_y - sh - (int)std::lround(10 * s);

        render_main(true);
        render_status(true);

        SetWindowPos(m_hwnd, HWND_TOPMOST, m_x, m_y, mw, mh,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowPos(m_status_hwnd, HWND_TOPMOST, sx, sy, sw, sh,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetTimer(m_hwnd, TIMER_FOCUS_STATUS, 200, nullptr);
    }

    void hide_popup() {
        m_is_visible = false;
        KillTimer(m_hwnd, TIMER_ANIM);
        KillTimer(m_hwnd, TIMER_FOCUS_STATUS);
        ShowWindow(m_status_hwnd, SW_HIDE);
        ShowWindow(m_hwnd, SW_HIDE);
    }

    void set_status(const std::string& utf8_status) {
        if (m_status_text != utf8_status) {
            m_status_text = utf8_status;
            render_status(true);
        }
    }

    void start_timers() {
        m_pulse_phase = 0.0f;
        SetTimer(m_hwnd, TIMER_ANIM, 45, nullptr);
        SetTimer(m_hwnd, TIMER_FOCUS_STATUS, 200, nullptr);
    }

    void stop_timers() {
        KillTimer(m_hwnd, TIMER_ANIM);
        if (!m_is_visible) KillTimer(m_hwnd, TIMER_FOCUS_STATUS);
        m_last_timer_sec = -1;
    }

    void render_main(bool force = false) {
        if (!m_hwnd) return;
        float s = dpi_scale();
        int w = (int)std::lround(kBaseW * s);
        int h = (int)std::lround(kBaseH * s);
        if (!m_main_dib.ensure(w, h)) return;

        float lvl = m_is_listening ? m_recorder.current_level() : 0.0f;
        if (!force &&
            std::fabs(lvl - m_last_drawn_lvl) < 0.02f &&
            m_last_hover == m_hover &&
            !m_is_processing && !m_is_listening) {
            return;
        }
        m_last_drawn_lvl = lvl;
        m_last_hover = m_hover;

        Gdiplus::Graphics g(m_main_dib.hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        const theme::ThemePalette pal = theme::get_palette(m_cfg.theme);

        // Main rounded card
        Gdiplus::GraphicsPath bg_path;
        add_round_rect(bg_path, 2.0f * s, 2.0f * s, (kBaseW - 4.0f) * s, (kBaseH - 4.0f) * s, 18.0f * s);
        Gdiplus::SolidBrush bg_br(theme::gcol(pal.pill_bg));
        g.FillPath(&bg_br, &bg_path);
        Gdiplus::Pen border_pen(theme::gcol(pal.border), 1.4f * s);
        g.DrawPath(&border_pen, &bg_path);

        // Top drag handle bar
        Gdiplus::GraphicsPath h_path;
        add_round_rect(h_path, (kBaseW * 0.5f - 20.0f) * s, 10.0f * s, 40.0f * s, 4.0f * s, 2.0f * s);
        Gdiplus::SolidBrush h_br(theme::gcol(pal.handle));
        g.FillPath(&h_br, &h_path);

        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        // Close X
        Gdiplus::Font close_f(&ff, 11.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
        Gdiplus::SolidBrush close_br(theme::gcol(m_hover == HoverZone::Close ? pal.icon_hover : pal.icon_idle));
        Gdiplus::RectF close_rc(206.0f * s, 4.0f * s, 28.0f * s, 24.0f * s);
        g.DrawString(L"\u2715", -1, &close_f, close_rc, &sf, &close_br);

        // Gear icon (vector cog)
        {
            float gx = 65.0f * s, gy = 62.0f * s;
            Gdiplus::Color gc = theme::gcol(m_hover == HoverZone::Gear ? pal.icon_hover : pal.icon_idle);
            Gdiplus::Pen gp(gc, 2.0f * s);
            g.DrawEllipse(&gp, gx - 5.2f * s, gy - 5.2f * s, 10.4f * s, 10.4f * s);
            for (int t = 0; t < 8; ++t) {
                float a = (float)t * (3.14159265f / 4.0f);
                g.DrawLine(&gp,
                           gx + std::cos(a) * 5.8f * s, gy + std::sin(a) * 5.8f * s,
                           gx + std::cos(a) * 8.6f * s, gy + std::sin(a) * 8.6f * s);
            }
        }

        // ? Help / History icon
        {
            Gdiplus::Font q_f(&ff, 14.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
            Gdiplus::SolidBrush q_br(theme::gcol(m_hover == HoverZone::Help ? pal.icon_hover : pal.icon_idle));
            Gdiplus::RectF q_rc(158.0f * s, 44.0f * s, 34.0f * s, 36.0f * s);
            g.DrawString(L"?", -1, &q_f, q_rc, &sf, &q_br);
        }

        // Center Mic Glow + Circle + Vector Microphone
        float cx = 120.0f * s, cy = 62.0f * s;
        float r_mic = 26.0f * s;
        Gdiplus::Color mic_col = theme::gcol(pal.mic_idle_circle);

        if (m_is_processing) {
            mic_col = theme::gcol(pal.accent_amber);
            float alpha_f = 0.28f + 0.22f * std::sin(m_pulse_phase);
            float gr = (34.0f + (0.5f + 0.3f * std::sin(m_pulse_phase * 1.5f)) * 9.0f) * s;
            Gdiplus::SolidBrush glow_br(theme::gcol_alpha(pal.accent_amber, (BYTE)(alpha_f * 255)));
            g.FillEllipse(&glow_br, cx - gr, cy - gr, gr * 2.0f, gr * 2.0f);
        } else if (m_is_listening) {
            mic_col = theme::gcol(pal.accent_green);
            float alpha_f = 0.26f + 0.20f * std::sin(m_pulse_phase);
            float gr = (33.0f + lvl * 17.0f) * s;
            Gdiplus::SolidBrush glow_br(theme::gcol_alpha(pal.accent_green, (BYTE)(alpha_f * 255)));
            g.FillEllipse(&glow_br, cx - gr, cy - gr, gr * 2.0f, gr * 2.0f);
        }

        Gdiplus::SolidBrush mic_br(mic_col);
        g.FillEllipse(&mic_br, cx - r_mic, cy - r_mic, r_mic * 2.0f, r_mic * 2.0f);

        // Crisp vector microphone inside center circle
        {
            Gdiplus::SolidBrush w_br(theme::gcol(pal.mic_icon));
            Gdiplus::Pen w_pen(theme::gcol(pal.mic_icon), 2.1f * s);
            w_pen.SetStartCap(Gdiplus::LineCapRound);
            w_pen.SetEndCap(Gdiplus::LineCapRound);

            Gdiplus::GraphicsPath cap;
            add_round_rect(cap, cx - 4.5f * s, cy - 11.0f * s, 9.0f * s, 14.0f * s, 4.5f * s);
            g.FillPath(&w_br, &cap);
            g.DrawArc(&w_pen, cx - 8.0f * s, cy - 5.0f * s, 16.0f * s, 12.0f * s, 0.0f, 180.0f);
            g.DrawLine(&w_pen, cx, cy + 7.0f * s, cx, cy + 11.5f * s);
            g.DrawLine(&w_pen, cx - 5.0f * s, cy + 11.5f * s, cx + 5.0f * s, cy + 11.5f * s);
        }

        // Bottom-right quick language pill
        {
            Gdiplus::GraphicsPath lp_path;
            add_round_rect(lp_path, 152.0f * s, 103.0f * s, 80.0f * s, 21.0f * s, 8.0f * s);
            Gdiplus::SolidBrush lp_br(theme::gcol(pal.input_bg));
            g.FillPath(&lp_br, &lp_path);
            bool hl = (m_hover == HoverZone::Lang);
            Gdiplus::Pen lp_pen(theme::gcol(hl ? pal.accent_green : pal.border), 1.1f * s);
            g.DrawPath(&lp_pen, &lp_path);

            DWORD lcol = hl ? pal.accent_green : pal.text_secondary;
            Gdiplus::Font lf(&ff, 8.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
            Gdiplus::SolidBrush ltxt_br(theme::gcol(lcol));

            if (m_cfg.language == "translate_en") {
                Gdiplus::RectF lrc(152.0f * s, 103.0f * s, 80.0f * s, 21.0f * s);
                g.DrawString(L"PT \u2192 EN", -1, &lf, lrc, &sf, &ltxt_br);
            } else if (m_cfg.language == "translate_es") {
                Gdiplus::RectF lrc(152.0f * s, 103.0f * s, 80.0f * s, 21.0f * s);
                g.DrawString(L"PT \u2192 ES", -1, &lf, lrc, &sf, &ltxt_br);
            } else if (m_cfg.language == "pt-BR" || m_cfg.language == "pt") {
                Gdiplus::RectF lrc(152.0f * s, 103.0f * s, 80.0f * s, 21.0f * s);
                g.DrawString(L"PT-BR", -1, &lf, lrc, &sf, &ltxt_br);
            } else {
                // Crisp vector planet/globe icon + "AUTO"
                float gx = 172.0f * s;
                float gy = 113.5f * s;
                float gr = 5.4f * s;
                Gdiplus::Pen gpen(theme::gcol(lcol), 1.15f * s);
                g.DrawEllipse(&gpen, gx - gr, gy - gr, 2.0f * gr, 2.0f * gr);
                g.DrawEllipse(&gpen, gx - gr * 0.45f, gy - gr, gr * 0.90f, 2.0f * gr);
                g.DrawLine(&gpen, gx - gr, gy, gx + gr, gy);
                g.DrawLine(&gpen, gx - gr * 0.85f, gy - gr * 0.48f, gx + gr * 0.85f, gy - gr * 0.48f);
                g.DrawLine(&gpen, gx - gr * 0.85f, gy + gr * 0.48f, gx + gr * 0.85f, gy + gr * 0.48f);

                Gdiplus::StringFormat sf_left;
                sf_left.SetAlignment(Gdiplus::StringAlignmentNear);
                sf_left.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                Gdiplus::RectF lrc(181.0f * s, 103.0f * s, 47.0f * s, 21.0f * s);
                g.DrawString(L"AUTO", -1, &lf, lrc, &sf_left, &ltxt_br);
            }
        }

        POINT pt_src{ 0, 0 };
        SIZE sz{ w, h };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        HDC screen = GetDC(nullptr);
        UpdateLayeredWindow(m_hwnd, screen, nullptr, &sz, m_main_dib.hdc, &pt_src, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
    }

    void render_status(bool force = false) {
        if (!m_status_hwnd) return;
        if (!force) return;
        float s = dpi_scale();
        int sw = (int)std::lround(kBaseSW * s);
        int sh = (int)std::lround(kBaseSH * s);
        if (!m_status_dib.ensure(sw, sh)) return;

        const theme::ThemePalette pal = theme::get_palette(m_cfg.theme);

        Gdiplus::Graphics g(m_status_dib.hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        Gdiplus::GraphicsPath p;
        add_round_rect(p, 2.0f * s, 2.0f * s, (kBaseSW - 4.0f) * s, (kBaseSH - 4.0f) * s, 11.0f * s);
        Gdiplus::SolidBrush br(theme::gcol(pal.status_bg));
        g.FillPath(&br, &p);
        Gdiplus::Pen pen(theme::gcol(pal.border), 1.3f * s);
        g.DrawPath(&pen, &p);

        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::Font f(&ff, 10.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
        Gdiplus::SolidBrush txt_br(theme::gcol(pal.text_primary));
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF rc(4.0f * s, 2.0f * s, (kBaseSW - 8.0f) * s, (kBaseSH - 4.0f) * s);
        std::wstring wt = utf8_to_wide(m_status_text);
        g.DrawString(wt.c_str(), -1, &f, rc, &sf, &txt_br);

        POINT pt_src{ 0, 0 };
        SIZE sz{ sw, sh };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        HDC screen = GetDC(nullptr);
        UpdateLayeredWindow(m_status_hwnd, screen, nullptr, &sz, m_status_dib.hdc, &pt_src, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
    }

    HoverZone hit_test_zone(int x_px, int y_px) const {
        float s = dpi_scale();
        float x = (float)x_px / s;
        float y = (float)y_px / s;
        if (pt_in_rectf(x, y, 206.0f, 4.0f, 28.0f, 24.0f)) return HoverZone::Close;
        if (pt_in_rectf(x, y, 152.0f, 103.0f, 80.0f, 21.0f)) return HoverZone::Lang;
        float dx_mic = x - 120.0f, dy_mic = y - 62.0f;
        if (dx_mic * dx_mic + dy_mic * dy_mic <= 27.0f * 27.0f) return HoverZone::Mic;
        if (pt_in_rectf(x, y, 47.0f, 44.0f, 36.0f, 36.0f)) return HoverZone::Gear;
        if (pt_in_rectf(x, y, 157.0f, 44.0f, 36.0f, 36.0f)) return HoverZone::Help;
        return HoverZone::None;
    }

    void show_history_menu(int sx, int sy) {
        bool deferred = false;
        if (popup_card::check_toggle_or_dismiss("history", &deferred)) {
            if (deferred && m_hwnd) {
                PostMessageW(m_hwnd, WM_APP_DEFERRED_HISTORY, (WPARAM)sx, (LPARAM)sy);
            }
            return;
        }

        std::vector<std::string> snap;
        EnterCriticalSection(&m_hist_cs);
        snap.assign(m_history.rbegin(), m_history.rend());
        LeaveCriticalSection(&m_hist_cs);

        std::vector<popup_card::PopupItem> items;
        if (snap.empty()) {
            items.push_back({ 0, L"Nenhum hist\u00F3rico salvo ainda.", false, true, false });
        } else {
            items.push_back({ 0, L"HIST\u00D3RICO (Clique para colar no chat)", false, true, false });
            items.push_back({ 0, L"", false, true, true });
            for (size_t i = 0; i < snap.size(); ++i) {
                std::wstring disp = utf8_to_wide(snap[i]);
                if (disp.size() > 52) disp = disp.substr(0, 49) + L"...";
                items.push_back({ kMenuHistBase + (int)i, disp, false, false, false });
            }
        }

        const theme::ThemePalette pal = theme::get_palette(m_cfg.theme);
        int cmd = popup_card::show_popup_menu(m_hwnd, sx, sy, items, pal, 280, "history");

        if (cmd >= kMenuHistBase && (size_t)(cmd - kMenuHistBase) < snap.size()) {
            set_status("Colando...");
            WorkerJob job;
            job.type = WorkerJob::Type::PasteHistory;
            job.text = snap[(size_t)(cmd - kMenuHistBase)];
            job.target = m_target;
            enqueue_job(std::move(job));
        } else if (m_target.hwnd && IsWindow(m_target.hwnd)) {
            SetForegroundWindow(m_target.hwnd);
        }
    }

    void show_lang_menu(int sx, int sy) {
        bool deferred = false;
        if (popup_card::check_toggle_or_dismiss("lang", &deferred)) {
            if (deferred && m_hwnd) {
                PostMessageW(m_hwnd, WM_APP_DEFERRED_LANG, (WPARAM)sx, (LPARAM)sy);
            }
            return;
        }

        struct Opt { const wchar_t* label; const char* code; };
        static const Opt opts[] = {
            { L"Autom\u00E1tico (detectar idioma)", "auto" },
            { L"Traduzir para Ingl\u00EAs (PT \u2192 EN)", "translate_en" },
            { L"Traduzir para Espanhol (PT \u2192 ES)", "translate_es" },
            { L"Portugu\u00EAs (Brasil)", "pt-BR" },
        };
        std::vector<popup_card::PopupItem> items;
        items.push_back({ 0, L"IDIOMA / TRADU\u00C7\u00C3O", false, true, false });
        items.push_back({ 0, L"", false, true, true });
        for (int i = 0; i < 4; ++i) {
            items.push_back({ kMenuLangBase + i, opts[i].label, m_cfg.language == opts[i].code, false, false });
        }

        const theme::ThemePalette pal = theme::get_palette(m_cfg.theme);
        int cmd = popup_card::show_popup_menu(m_hwnd, sx, sy, items, pal, 250, "lang");

        if (cmd >= kMenuLangBase && cmd < kMenuLangBase + 4) {
            m_cfg.language = opts[cmd - kMenuLangBase].code;
            json_util::save_settings_file(m_dir + L"\\config.json", m_cfg);
            render_main(true);
            LOGI("[OK] Idioma rapido alterado: %s", m_cfg.language.c_str());
            set_status(std::string("Idioma: ") + m_cfg.language);
            SetTimer(m_hwnd, TIMER_STATUS_RESET, 1500, nullptr);
        }
        if (m_target.hwnd && IsWindow(m_target.hwnd)) {
            SetForegroundWindow(m_target.hwnd);
        }
    }

    void open_settings_dialog() {
        if (m_settings_win.hwnd() && IsWindow(m_settings_win.hwnd())) {
            if (popup_card::is_popup_open()) {
                popup_card::dismiss_active_popup();
                PostMessageW(m_hwnd, WM_APP_CLOSE_SETTINGS, 0, 0);
            } else {
                m_settings_win.close();
                if (m_target.hwnd && IsWindow(m_target.hwnd)) {
                    SetForegroundWindow(m_target.hwnd);
                }
            }
            return;
        }
        if (popup_card::is_popup_open()) {
            popup_card::dismiss_active_popup();
        }
        m_settings_win.show(m_hwnd, m_cfg, [this](const AppSettings& old_cfg, const AppSettings& new_cfg) {
            bool hk_changed = (old_cfg.hotkey != new_cfg.hotkey);
            bool mic_changed = (old_cfg.microphone_name != new_cfg.microphone_name ||
                                old_cfg.microphone_index != new_cfg.microphone_index);
            bool key_changed = (old_cfg.groq_api_key != new_cfg.groq_api_key);

            m_cfg = new_cfg;
            render_main(true);
            render_status(true);

            if (hk_changed) setup_hotkey();
            if (mic_changed) {
                m_recorder.invalidate_cache();
                m_recorder.request_prewarm(m_cfg);
            }
            if (key_changed && m_cfg.engine == "Groq (Whisper API)" && !m_cfg.groq_api_key.empty()) {
                WorkerJob j;
                j.type = WorkerJob::Type::PrewarmGroq;
                j.cfg = m_cfg;
                enqueue_job(std::move(j));
            }
        });
    }

    static LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        NativeApp* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = (CREATESTRUCTW*)lp;
            self = (NativeApp*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
            self->m_hwnd = hwnd;
        } else {
            self = (NativeApp*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        }
        if (self) return self->handle_main_msg(hwnd, msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK status_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT handle_main_msg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;

            case WM_HOTKEY:
                if (wp == kHotkeyId) {
                    on_hotkey_toggle();
                }
                return 0;

            case WM_APP_HOTKEY_FALLBACK:
            case WM_APP_TOGGLE_IPC:
                on_hotkey_toggle();
                return 0;

            case WM_DEVICECHANGE:
                if (wp == DBT_DEVNODES_CHANGED || wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
                    m_recorder.invalidate_cache();
                    if (!m_is_listening && !m_is_processing) {
                        m_recorder.request_prewarm(m_cfg);
                    }
                }
                break;

            case WM_TIMER:
                if (wp == TIMER_ANIM) {
                    if (m_is_visible && (m_is_listening || m_is_processing)) {
                        m_pulse_phase += 0.14f;
                        if (m_pulse_phase > 6.283185f) m_pulse_phase -= 6.283185f;
                        render_main(true);
                    }
                    return 0;
                }
                if (wp == TIMER_FOCUS_STATUS) {
                    if (m_is_listening && !m_is_processing) {
                        int elapsed = (int)((GetTickCount64() - m_record_start_tick) / 1000ULL);
                        if (elapsed != m_last_timer_sec) {
                            m_last_timer_sec = elapsed;
                            char buf[32];
                            snprintf(buf, sizeof(buf), "Ouvindo  %02d:%02d", elapsed / 60, elapsed % 60);
                            set_status(buf);
                        }
                    }
                    if (m_is_visible || m_is_listening) {
                        HWND fg = GetForegroundWindow();
                        if (fg) capture_target_hwnd(fg, false);
                    }
                    return 0;
                }
                if (wp == TIMER_STATUS_RESET) {
                    KillTimer(hwnd, TIMER_STATUS_RESET);
                    if (!m_is_listening && !m_is_processing) {
                        set_status("Pausado");
                    }
                    return 0;
                }
                break;

            case WM_APP_AUDIO_STATUS: {
                std::unique_ptr<std::string> st((std::string*)lp);
                if (st && (uint64_t)wp == m_session_id && m_is_listening) {
                    set_status(*st);
                }
                return 0;
            }

            case WM_APP_AUDIO_DONE: {
                std::unique_ptr<AudioDoneMsg> amsg((AudioDoneMsg*)lp);
                on_audio_done(std::move(amsg));
                return 0;
            }

            case WM_APP_JOB_DONE: {
                std::unique_ptr<JobDoneMsg> jmsg((JobDoneMsg*)lp);
                on_job_done(std::move(jmsg));
                return 0;
            }

            case WM_APP_PASTE_DONE:
                set_status("Texto colado!");
                SetTimer(hwnd, TIMER_STATUS_RESET, 2000, nullptr);
                return 0;

            case WM_APP_DEFERRED_HISTORY:
                show_history_menu((int)wp, (int)lp);
                return 0;

            case WM_APP_DEFERRED_LANG:
                show_lang_menu((int)wp, (int)lp);
                return 0;

            case WM_APP_CLOSE_SETTINGS:
                if (m_settings_win.hwnd() && IsWindow(m_settings_win.hwnd())) {
                    m_settings_win.close();
                }
                return 0;

            case WM_MOUSEMOVE: {
                int x = (int)(short)LOWORD(lp);
                int y = (int)(short)HIWORD(lp);
                if (m_dragging) {
                    POINT pt{};
                    GetCursorPos(&pt);
                    m_x = pt.x - m_drag_off_x;
                    m_y = pt.y - m_drag_off_y;
                    float s = dpi_scale();
                    int mw = (int)std::lround(kBaseW * s);
                    int sw = (int)std::lround(kBaseSW * s);
                    int sh = (int)std::lround(kBaseSH * s);
                    int sx = m_x + (mw - sw) / 2;
                    int sy = m_y - sh - (int)std::lround(10 * s);
                    HDWP dwp = BeginDeferWindowPos(2);
                    if (dwp) {
                        dwp = DeferWindowPos(dwp, m_hwnd, HWND_TOPMOST, m_x, m_y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                        if (dwp) dwp = DeferWindowPos(dwp, m_status_hwnd, HWND_TOPMOST, sx, sy, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                        if (dwp) EndDeferWindowPos(dwp);
                    }
                    return 0;
                }
                if (!m_tracking_mouse) {
                    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                    TrackMouseEvent(&tme);
                    m_tracking_mouse = true;
                }
                HoverZone hz = hit_test_zone(x, y);
                if (hz != m_hover) {
                    m_hover = hz;
                    render_main(true);
                }
                return 0;
            }

            case WM_MOUSELEAVE:
                m_tracking_mouse = false;
                if (m_hover != HoverZone::None) {
                    m_hover = HoverZone::None;
                    render_main(true);
                }
                return 0;

            case WM_LBUTTONDOWN: {
                int x = (int)(short)LOWORD(lp);
                int y = (int)(short)HIWORD(lp);
                HoverZone hz = hit_test_zone(x, y);
                if (hz == HoverZone::Close) {
                    popup_card::dismiss_active_popup();
                    stop_listening();
                    hide_popup();
                    return 0;
                }
                if (hz == HoverZone::Mic) {
                    on_mic_click();
                    return 0;
                }
                if (hz == HoverZone::Gear) {
                    open_settings_dialog();
                    return 0;
                }
                if (hz == HoverZone::Help) {
                    POINT pt{}; GetCursorPos(&pt);
                    show_history_menu(pt.x, pt.y);
                    return 0;
                }
                if (hz == HoverZone::Lang) {
                    POINT pt{}; GetCursorPos(&pt);
                    show_lang_menu(pt.x, pt.y);
                    return 0;
                }
                // Drag background
                POINT pt{};
                GetCursorPos(&pt);
                m_drag_off_x = pt.x - m_x;
                m_drag_off_y = pt.y - m_y;
                m_dragging = true;
                SetCapture(hwnd);
                return 0;
            }

            case WM_LBUTTONUP:
                if (m_dragging) {
                    m_dragging = false;
                    ReleaseCapture();
                }
                return 0;

            case WM_DPICHANGED: {
                m_dpi = HIWORD(wp);
                const RECT* rc = (const RECT*)lp;
                m_x = rc->left;
                m_y = rc->top;
                show_popup();
                return 0;
            }

            case WM_DESTROY:
                LOGI("Voice Dictation Nativo encerrando (pid=%lu)", m_own_pid);
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    std::wstring m_dir;
    AppSettings m_cfg;
    DWORD m_own_pid = 0;
    UINT m_dpi = 96;
    bool m_closing = false;

    HWND m_hwnd = nullptr;
    HWND m_status_hwnd = nullptr;
    CachedDib m_main_dib;
    CachedDib m_status_dib;

    int m_x = 0;
    int m_y = 0;
    bool m_positioned = false;
    bool m_dragging = false;
    int m_drag_off_x = 0;
    int m_drag_off_y = 0;
    bool m_tracking_mouse = false;
    HoverZone m_hover = HoverZone::None;
    HoverZone m_last_hover = HoverZone::None;
    float m_last_drawn_lvl = -1.0f;
    float m_pulse_phase = 0.0f;

    bool m_is_visible = false;
    bool m_is_listening = false;
    bool m_worker_active = false;
    bool m_is_processing = false;
    bool m_start_requested = false;
    uint64_t m_session_id = 0;
    ULONGLONG m_record_start_tick = 0;
    int m_last_timer_sec = -1;
    std::string m_status_text = "Pausado";

    ULONGLONG m_last_hotkey_tick = 0;
    bool m_hotkey_registered = false;
    HHOOK m_fallback_hook = nullptr;
    ParsedHotkey m_fallback_hk;

    HWINEVENTHOOK m_fg_hook = nullptr;
    injector::TargetWindow m_target;

    HANDLE m_ipc_thread = nullptr;
    HANDLE m_ipc_stop = nullptr;

    CRITICAL_SECTION m_hist_cs{};
    std::deque<std::string> m_history;

    CRITICAL_SECTION m_job_cs{};
    std::deque<WorkerJob> m_job_queue;
    HANDLE m_job_event = nullptr;
    HANDLE m_job_shutdown = nullptr;
    HANDLE m_job_thread = nullptr;

    audio_engine::AudioRecorder m_recorder;
    net_client::HttpSession m_http;
    settings_ui::SettingsWindow m_settings_win;
};

NativeApp* NativeApp::s_instance = nullptr;

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd_line, int) {
    // Check --silent / --background flag
    bool is_silent = false;
    if (cmd_line && (wcsstr(cmd_line, L"--silent") || wcsstr(cmd_line, L"--background"))) {
        is_silent = true;
    }

    // Single-instance Mutex + IPC signal
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"Local\\VoiceDictation_Native_SingleInstance");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!is_silent) {
            HANDLE hEvt = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"VoiceDictation_Toggle_Event");
            if (hEvt) {
                SetEvent(hEvt);
                CloseHandle(hEvt);
            }
        }
        CloseHandle(hMutex);
        return 0;
    }

    // Boost scheduling class so 100% CPU background tasks cannot starve hotkey/popup/audio
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    // Enable Per-Monitor DPI v2 dynamically if supported
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL (WINAPI *PFN_SetDpiCtx)(DPI_AWARENESS_CONTEXT);
        auto fn = (PFN_SetDpiCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (fn) fn((DPI_AWARENESS_CONTEXT)-4);
    }

    Gdiplus::GdiplusStartupInput gdi_in;
    ULONG_PTR gdi_token = 0;
    Gdiplus::GdiplusStartup(&gdi_token, &gdi_in, nullptr);

    NativeApp app;
    NativeApp::s_instance = &app;
    int rc = 1;
    if (app.init(is_silent)) {
        rc = app.run();
    }
    NativeApp::s_instance = nullptr;

    Gdiplus::GdiplusShutdown(gdi_token);
    if (hMutex) CloseHandle(hMutex);
    return rc;
}
