#include "injector.h"
#include "logger.h"
#include <atomic>

namespace injector {

namespace {

constexpr DWORD kClipboardRestoreDelayMs = 700;

CRITICAL_SECTION g_clip_cs{};
bool g_clip_cs_init = false;
struct ClipCsInit {
    ClipCsInit() { InitializeCriticalSection(&g_clip_cs); g_clip_cs_init = true; }
    ~ClipCsInit() { DeleteCriticalSection(&g_clip_cs); g_clip_cs_init = false; }
} g_clip_cs_guard;

struct PendingRestore {
    bool active = false;
    uint64_t gen = 0;
    DWORD expected_seq = 0;
    bool had_text = false;
    std::wstring saved_text;
};

PendingRestore g_pending_restore;
std::atomic<uint64_t> g_restore_gen{0};

static bool write_clipboard_raw(const std::wstring& wtext, bool exclude_history) {
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(nullptr)) {
            if (!EmptyClipboard()) {
                CloseClipboard();
                Sleep(10);
                continue;
            }
            bool ok = true;
            if (!wtext.empty()) {
                size_t bytes = (wtext.size() + 1) * sizeof(wchar_t);
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (hMem) {
                    void* p = GlobalLock(hMem);
                    if (p) {
                        memcpy(p, wtext.c_str(), bytes);
                        GlobalUnlock(hMem);
                        if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
                            GlobalFree(hMem);
                            ok = false;
                        }
                    } else {
                        GlobalFree(hMem);
                        ok = false;
                    }
                } else {
                    ok = false;
                }

                if (ok && exclude_history) {
                    static const wchar_t* kExclFmts[] = {
                        L"CanIncludeInClipboardHistory",
                        L"CanUploadToCloudClipboard",
                        L"ExcludeClipboardContentFromMonitorProcessing"
                    };
                    for (const wchar_t* fmt_name : kExclFmts) {
                        UINT fmt = RegisterClipboardFormatW(fmt_name);
                        if (fmt) {
                            HGLOBAL hf = GlobalAlloc(GMEM_MOVEABLE, 4);
                            if (hf) {
                                void* pf = GlobalLock(hf);
                                if (pf) {
                                    memset(pf, 0, 4);
                                    GlobalUnlock(hf);
                                    if (!SetClipboardData(fmt, hf)) {
                                        GlobalFree(hf);
                                    }
                                } else {
                                    GlobalFree(hf);
                                }
                            }
                        }
                    }
                }
            }
            CloseClipboard();
            return ok;
        }
        Sleep(10);
    }
    return false;
}

static DWORD WINAPI delayed_restore_proc(LPVOID param) {
    uint64_t my_gen = (uint64_t)(uintptr_t)param;
    Sleep(kClipboardRestoreDelayMs);

    if (g_restore_gen.load(std::memory_order_acquire) != my_gen) {
        return 0;
    }

    bool do_restore = false;
    bool had_text = false;
    std::wstring saved;
    DWORD expected_seq = 0;

    EnterCriticalSection(&g_clip_cs);
    if (g_pending_restore.active && g_pending_restore.gen == my_gen) {
        expected_seq = g_pending_restore.expected_seq;
        // Only restore if the clipboard still holds our own paste (the user did
        // not press Ctrl+C on something else in the meantime).
        if (GetClipboardSequenceNumber() == expected_seq) {
            do_restore = true;
            had_text = g_pending_restore.had_text;
            saved = g_pending_restore.saved_text;
        }
        g_pending_restore.active = false;
        g_pending_restore.saved_text.clear();
    }
    LeaveCriticalSection(&g_clip_cs);

    if (do_restore) {
        write_clipboard_raw(had_text ? saved : L"", true);
    }
    return 0;
}

static void release_stuck_modifiers() {
    static const WORD kMods[] = {
        VK_MENU, VK_LMENU, VK_RMENU,
        VK_CONTROL, VK_LCONTROL, VK_RCONTROL,
        VK_SHIFT, VK_LSHIFT, VK_RSHIFT,
        VK_LWIN, VK_RWIN
    };
    std::vector<INPUT> ups;
    for (WORD vk : kMods) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            INPUT inp{};
            inp.type = INPUT_KEYBOARD;
            inp.ki.wVk = vk;
            inp.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            inp.ki.dwFlags = KEYEVENTF_KEYUP;
            ups.push_back(inp);
        }
    }
    if (!ups.empty()) {
        SendInput((UINT)ups.size(), ups.data(), sizeof(INPUT));
        Sleep(8);
    }
}

} // namespace

std::string get_window_label(HWND hwnd) {
    if (!hwnd) return {};
    int len = GetWindowTextLengthW(hwnd);
    std::wstring title;
    if (len > 0 && len < 512) {
        title.resize((size_t)len + 1);
        int got = GetWindowTextW(hwnd, title.data(), len + 1);
        title.resize((got > 0) ? (size_t)got : 0);
    }
    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);

    while (!title.empty() && (title.front() == L' ' || title.front() == L'\t')) title.erase(title.begin());
    while (!title.empty() && (title.back() == L' ' || title.back() == L'\t')) title.pop_back();
    std::string utf8_title = title.empty() ? "sem titulo" : wide_to_utf8(title);
    return utf8_title + " [" + wide_to_utf8(cls) + "]";
}

HWND get_focused_control(HWND hwnd) {
    if (!hwnd) return nullptr;
    DWORD tid = GetWindowThreadProcessId(hwnd, nullptr);
    if (!tid) return nullptr;
    GUITHREADINFO gti{};
    gti.cbSize = sizeof(GUITHREADINFO);
    if (GetGUIThreadInfo(tid, &gti) && gti.hwndFocus) {
        return gti.hwndFocus;
    }
    return nullptr;
}

bool focus_target_window(HWND target, DWORD timeout_ms) {
    if (!target || !IsWindow(target)) {
        LOGI("[!] A janela de destino nao existe mais.");
        return false;
    }
    if (GetForegroundWindow() == target) return true;

    if (IsIconic(target)) {
        ShowWindow(target, SW_RESTORE);
    } else if (!IsWindowVisible(target)) {
        ShowWindow(target, SW_SHOW);
    }

    SetForegroundWindow(target);
    BringWindowToTop(target);
    if (GetForegroundWindow() == target) return true;

    // Probe target responsiveness before AttachThreadInput so a hung window at
    // 100% CPU can never freeze our worker thread.
    DWORD_PTR dummy = 0;
    if (!SendMessageTimeoutW(target, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 80, &dummy)) {
        LOGE("[!] Janela destino nao responde; evitando AttachThreadInput para nao travar.");
        return false;
    }

    HWND cur_fg = GetForegroundWindow();
    DWORD app_tid = GetCurrentThreadId();
    DWORD cur_tid = cur_fg ? GetWindowThreadProcessId(cur_fg, nullptr) : 0;
    DWORD tgt_tid = GetWindowThreadProcessId(target, nullptr);

    bool att_cur = false;
    bool att_tgt = false;
    if (cur_tid && cur_tid != app_tid) {
        att_cur = (AttachThreadInput(app_tid, cur_tid, TRUE) != FALSE);
    }
    if (tgt_tid && tgt_tid != app_tid && tgt_tid != cur_tid) {
        att_tgt = (AttachThreadInput(app_tid, tgt_tid, TRUE) != FALSE);
    }

    SetForegroundWindow(target);
    BringWindowToTop(target);

    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    bool focused = (GetForegroundWindow() == target);
    while (!focused && GetTickCount64() < deadline) {
        Sleep(15);
        focused = (GetForegroundWindow() == target);
    }

    if (att_tgt) AttachThreadInput(app_tid, tgt_tid, FALSE);
    if (att_cur) AttachThreadInput(app_tid, cur_tid, FALSE);

    if (!focused) {
        LOGI("[!] Nao foi possivel focar a janela de destino (0x%p).", (void*)target);
    }
    return focused;
}

bool get_clipboard_text_safe(std::wstring& out_text, bool& out_had_text) {
    out_had_text = false;
    out_text.clear();
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(nullptr)) {
            if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                HANDLE h = GetClipboardData(CF_UNICODETEXT);
                if (h) {
                    const wchar_t* ptr = (const wchar_t*)GlobalLock(h);
                    if (ptr) {
                        out_text.assign(ptr);
                        out_had_text = true;
                        GlobalUnlock(h);
                    }
                }
            }
            CloseClipboard();
            return true;
        }
        Sleep(10);
    }
    return false;
}

bool set_clipboard_text_safe(const std::wstring& wtext, bool exclude_history) {
    return write_clipboard_raw(wtext, exclude_history);
}

bool paste_isolated_instant(const std::string& utf8_text, bool send_keys) {
    std::wstring payload = utf8_to_wide(utf8_text);
    if (payload.empty()) return false;

    std::wstring saved;
    bool had_saved = false;

    EnterCriticalSection(&g_clip_cs);
    // Cancel any previous delayed restore timer and inherit the ORIGINAL user
    // clipboard if a previous dictation paste is still sitting on the clipboard.
    uint64_t new_gen = g_restore_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (g_pending_restore.active && GetClipboardSequenceNumber() == g_pending_restore.expected_seq) {
        had_saved = g_pending_restore.had_text;
        saved = std::move(g_pending_restore.saved_text);
        g_pending_restore.active = false;
    } else {
        g_pending_restore.active = false;
        g_pending_restore.saved_text.clear();
        get_clipboard_text_safe(saved, had_saved);
    }

    bool wrote = write_clipboard_raw(payload, true);
    DWORD seq_after_write = wrote ? GetClipboardSequenceNumber() : 0;
    if (wrote) {
        g_pending_restore.active = true;
        g_pending_restore.gen = new_gen;
        g_pending_restore.expected_seq = seq_after_write;
        g_pending_restore.had_text = had_saved;
        g_pending_restore.saved_text = std::move(saved);
    }
    LeaveCriticalSection(&g_clip_cs);

    if (!wrote) {
        LOGE("[!] Falha ao gravar texto na area de transferencia.");
        return false;
    }

    // Give Windows / Chromium clipboard observers time to observe the sequence update
    Sleep(25);

    if (send_keys) {
        release_stuck_modifiers();

        WORD vk_ctrl = VK_CONTROL;
        WORD vk_v = 'V';
        WORD sc_ctrl = (WORD)MapVirtualKeyW(vk_ctrl, MAPVK_VK_TO_VSC);
        WORD sc_v = (WORD)MapVirtualKeyW(vk_v, MAPVK_VK_TO_VSC);

        INPUT ctrl_down{};
        ctrl_down.type = INPUT_KEYBOARD;
        ctrl_down.ki.wVk = vk_ctrl;
        ctrl_down.ki.wScan = sc_ctrl;

        INPUT v_down{};
        v_down.type = INPUT_KEYBOARD;
        v_down.ki.wVk = vk_v;
        v_down.ki.wScan = sc_v;

        INPUT v_up{};
        v_up.type = INPUT_KEYBOARD;
        v_up.ki.wVk = vk_v;
        v_up.ki.wScan = sc_v;
        v_up.ki.dwFlags = KEYEVENTF_KEYUP;

        INPUT ctrl_up{};
        ctrl_up.type = INPUT_KEYBOARD;
        ctrl_up.ki.wVk = vk_ctrl;
        ctrl_up.ki.wScan = sc_ctrl;
        ctrl_up.ki.dwFlags = KEYEVENTF_KEYUP;

        UINT s1 = SendInput(1, &ctrl_down, sizeof(INPUT));
        Sleep(8);
        UINT s2 = SendInput(1, &v_down, sizeof(INPUT));
        Sleep(20);
        INPUT ups[2] = { v_up, ctrl_up };
        UINT s3 = SendInput(2, ups, sizeof(INPUT));

        if (s1 != 1 || s2 != 1 || s3 != 2) {
            LOGE("[!] SendInput Ctrl+V parcial (%u,%u,%u) - janela destino elevada?", s1, s2, s3);
        } else {
            LOGI("[OK] Texto colado a seco instantaneamente via Ctrl+V (%zu chars)", utf8_text.size());
        }
    }

    // Keep the dictated text live on clipboard for 700ms asynchronously so even
    // busy Chromium/Electron/WhatsApp processes read it cleanly before we restore
    // the user's previous Ctrl+V.
    HANDLE ht = CreateThread(nullptr, 0, &delayed_restore_proc, (LPVOID)(uintptr_t)new_gen, 0, nullptr);
    if (ht) CloseHandle(ht);
    return true;
}

bool inject_unicode_keystrokes(const std::string& utf8_text) {
    std::wstring w = utf8_to_wide(utf8_text);
    if (w.empty()) return false;
    release_stuck_modifiers();

    std::vector<INPUT> inputs;
    inputs.reserve(w.size() * 2);
    for (wchar_t ch : w) {
        if (ch == L'\r') continue;
        if (ch == L'\n') {
            INPUT d{}, u{};
            d.type = INPUT_KEYBOARD; d.ki.wVk = VK_RETURN;
            u.type = INPUT_KEYBOARD; u.ki.wVk = VK_RETURN; u.ki.dwFlags = KEYEVENTF_KEYUP;
            inputs.push_back(d);
            inputs.push_back(u);
        } else {
            INPUT d{}, u{};
            d.type = INPUT_KEYBOARD; d.ki.wScan = (WORD)ch; d.ki.dwFlags = KEYEVENTF_UNICODE;
            u.type = INPUT_KEYBOARD; u.ki.wScan = (WORD)ch; u.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            inputs.push_back(d);
            inputs.push_back(u);
        }
    }
    if (inputs.empty()) return false;
    UINT sent = SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
    return sent == (UINT)inputs.size();
}

bool type_text_into_target(
    const std::string& utf8_text,
    TargetWindow& tracked_target,
    DWORD own_pid
) {
    if (utf8_text.empty()) return false;
    std::string text_to_inject = utf8_text + " ";

    HWND cur_fg = GetForegroundWindow();
    DWORD cur_pid = 0;
    if (cur_fg) GetWindowThreadProcessId(cur_fg, &cur_pid);

    if (cur_fg && cur_pid != own_pid && IsWindow(cur_fg)) {
        tracked_target.hwnd = cur_fg;
        tracked_target.focus = get_focused_control(cur_fg);
        tracked_target.label = get_window_label(cur_fg);
    } else if (tracked_target.hwnd) {
        focus_target_window(tracked_target.hwnd, 350);
    }

    Sleep(30);

    if (!tracked_target.label.empty()) {
        LOGI("[OK] Inserindo texto em: %s (campo 0x%p)",
             tracked_target.label.c_str(), (void*)tracked_target.focus);
    }

    if (paste_isolated_instant(text_to_inject, true)) return true;
    if (inject_unicode_keystrokes(text_to_inject)) {
        LOGI("[OK] Texto digitado via SendInput Unicode fallback (%zu chars)", utf8_text.size());
        return true;
    }
    return false;
}

} // namespace injector
