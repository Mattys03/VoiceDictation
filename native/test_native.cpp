#include "common.h"
#include "json_util.h"
#include "text_proc.h"
#include "audio_engine.h"
#include "injector.h"
#include "theme.h"
#include "popup_card.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

static int g_passed = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
    ++g_passed; \
} while (0)

static int test_json_and_config() {
    AppSettings def_s;
    CHECK(def_s.engine == "Google (Gratuito)");
    CHECK(def_s.language == "auto");
    CHECK(def_s.groq_api_key.empty());
    CHECK(def_s.theme == "dark");
    CHECK(def_s.auto_start == false);
    CHECK(def_s.has_auto_start_pref == false);
    CHECK(def_s.allow_ellipsis == true);
    CHECK(def_s.smart_review == false);

    AppSettings s;
    s.hotkey = "Ctrl+Shift+H";
    s.microphone_index = -1;
    s.microphone_name = "Microfone (4- fifine Microphone";
    s.language = "pt-BR";
    s.auto_punctuation = true;
    s.allow_ellipsis = false;
    s.smart_review = true;
    s.engine = "Groq (Whisper API)";
    s.groq_api_key = "gsk_test_123\"quote";
    s.theme = "light";
    s.auto_start = true;
    s.has_auto_start_pref = true;

    std::string dumped = json_util::serialize_settings_json(s);
    CHECK(dumped.find("\"microphone_index\": null") != std::string::npos);
    CHECK(dumped.find("\"theme\": \"light\"") != std::string::npos);
    CHECK(dumped.find("\"auto_start\": true") != std::string::npos);
    CHECK(dumped.find("\"allow_ellipsis\": false") != std::string::npos);
    CHECK(dumped.find("\"smart_review\": true") != std::string::npos);

    AppSettings parsed = json_util::parse_settings_json(dumped);
    CHECK(parsed.hotkey == s.hotkey);
    CHECK(parsed.microphone_index == -1);
    CHECK(parsed.microphone_name == s.microphone_name);
    CHECK(parsed.language == "pt-BR");
    CHECK(parsed.auto_punctuation == true);
    CHECK(parsed.allow_ellipsis == false);
    CHECK(parsed.smart_review == true);
    CHECK(parsed.engine == "Groq (Whisper API)");
    CHECK(parsed.groq_api_key == s.groq_api_key);
    CHECK(parsed.theme == "light");
    CHECK(parsed.auto_start == true);
    CHECK(parsed.has_auto_start_pref == true);

    // Unknown or missing theme falls back safely to dark; omitted auto_start leaves pref false
    AppSettings parsed_def = json_util::parse_settings_json("{\"theme\":\"weird\"}");
    CHECK(parsed_def.theme == "dark");
    CHECK(parsed_def.auto_start == false);
    CHECK(parsed_def.has_auto_start_pref == false);
    CHECK(parsed_def.engine == "Google (Gratuito)");
    CHECK(parsed_def.language == "auto");
    CHECK(!theme::get_palette("dark").is_light);
    CHECK(theme::get_palette("light").is_light);

    // Unicode surrogate pair emoji & accented text extraction from Groq JSON
    std::string groq_resp = "{\"text\":\"Ol\\u00e1 a\\u00ed! \\ud83d\\ude00\"}";
    std::string extracted = json_util::extract_json_string_field(groq_resp, "text");
    CHECK(extracted == "Ol\xC3\xA1 a\xC3\xAD! \xF0\x9F\x98\x80");
    return 0;
}

static int test_speech_detection_and_trim() {
    using namespace audio_engine;

    // 1. Real dictation from log: floor ~10000, peak ~20000
    std::vector<int> real_rms(20, 10000);
    real_rms[10] = 20000;
    auto sa = analyze_speech(real_rms);
    CHECK(sa.has_speech == true);

    // 2. Digital silence
    std::vector<int> silence(10, 0);
    CHECK(analyze_speech(silence).has_speech == false);

    // 3. Low level 162-RMS hiss
    std::vector<int> hiss(15, 162);
    CHECK(analyze_speech(hiss).has_speech == false);

    // 4. Hot hiss constant noise without speech
    std::vector<int> hot_hiss(20, 1500);
    CHECK(analyze_speech(hot_hiss).has_speech == false);

    // 5. Speech from chunk 0
    std::vector<int> first_chunk_speech = { 5000, 4800, 200, 180, 190, 200, 195, 185, 190, 190 };
    CHECK(analyze_speech(first_chunk_speech).has_speech == true);

    // 6. Half recording paused, half talking
    std::vector<int> pauses = { 100, 100, 100, 100, 100, 3500, 3800, 4000, 3200, 3500 };
    CHECK(analyze_speech(pauses).has_speech == true);

    // 7. Trim only edges, keep middle
    std::vector<std::vector<int16_t>> frames = {
        {1, 1}, {2, 2}, {90, 90}, {3, 3}, {80, 80}, {4, 4}, {5, 5}
    };
    std::vector<int> levels = { 10, 20, 900, 50, 800, 15, 12 };
    size_t st = 0, en = 0;
    auto trimmed = trim_silent_edges(frames, levels, 300, 1, 1, &st, &en);
    CHECK(st == 1 && en == 6);
    CHECK(trimmed.size() == 10); // frames 1..5 preserved including middle gap frame 3
    return 0;
}

static int test_pcm_wav_flac_and_resample() {
    using namespace audio_engine;

    std::vector<int16_t> in44(44100, 1000);
    int rms = compute_rms_pcm16(in44.data(), in44.size());
    CHECK(rms == 1000);

    auto out16 = resample_pcm16_mono(in44.data(), in44.size(), 44100, 16000);
    CHECK(out16.size() == 16000);
    CHECK(compute_rms_pcm16(out16.data(), out16.size()) == 1000);

    auto wav = build_wav_bytes(out16.data(), out16.size(), 16000);
    CHECK(wav.size() == 44 + 16000 * 2);
    CHECK(memcmp(wav.data(), "RIFF", 4) == 0);
    CHECK(memcmp(wav.data() + 8, "WAVEfmt ", 8) == 0);

    auto flac = build_flac_verbatim_bytes(out16.data(), out16.size(), 16000);
    CHECK(flac.size() > 42 + 16000 * 2);
    CHECK(memcmp(flac.data(), "fLaC", 4) == 0);
    return 0;
}

static int test_text_processing() {
    using namespace text_proc;

    CHECK(whisper_language_code("pt-BR") == "pt");
    CHECK(whisper_language_code("translate_en") == "pt");
    CHECK(whisper_language_code("auto") == "");
    CHECK(whisper_language_code("en") == "en");

    // Punctuation commands + connector commas + accents
    std::string p1 = auto_punctuate("ol\xC3\xA1 tudo bem v\xC3\xADrgula mas eu fui l\xC3\xA1");
    CHECK(p1 == "Ol\xC3\xA1 tudo bem, mas eu fui l\xC3\xA1.");

    // Watermark & phantom blockers
    std::string blocked;
    std::string r_wm = filter_and_format_transcription(
        "Amara.org... obrigado por assistir",
        32000, 16000, 500, true, false, &blocked
    );
    CHECK(!blocked.empty() && r_wm.empty());

    blocked.clear();
    std::string r_ph = filter_and_format_transcription(
        "Obrigado.", 16000, 16000, 500, true, false, &blocked
    );
    CHECK(!blocked.empty() && r_ph.empty());

    // Real phrase with adjacent word dedupe
    blocked.clear();
    std::string r_ok = filter_and_format_transcription(
        "eu eu quero testar isso", 64000, 16000, 5000, true, false, &blocked
    );
    CHECK(blocked.empty());
    CHECK(r_ok == "Eu quero testar isso.");

    // Ellipsis disabled: strips ... and Unicode ellipsis cleanly while preserving decimals
    blocked.clear();
    std::string r_no_dots = filter_and_format_transcription(
        "eu... eu quero v2.0 sem isso... tudo bem\xE2\x80\xA6",
        64000, 16000, 5000, true, false, &blocked, false, false
    );
    CHECK(blocked.empty());
    CHECK(r_no_dots.find("...") == std::string::npos);
    CHECK(r_no_dots.find("\xE2\x80\xA6") == std::string::npos);
    CHECK(r_no_dots == "Eu quero v2.0 sem isso tudo bem.");

    // Mid-sentence ellipsis + exclamation + no auto_punctuation
    std::string r_no_dots_raw = finalize_transcription("oi... tudo bem...!", false, false, false, false);
    CHECK(r_no_dots_raw == "oi tudo bem!");

    // Smart review path avoids regex comma insertion around "sabe" and strips ellipsis if disabled
    std::string r_rev = finalize_transcription("Ele sabe como fazer...", true, false, false, true);
    CHECK(r_rev == "Ele sabe como fazer.");
    return 0;
}

static int test_hotkey_and_device_helpers() {
    auto h1 = parse_hotkey_win("Alt+H");
    CHECK(h1.valid && h1.mods == MOD_ALT && h1.vk == 'H');

    auto h2 = parse_hotkey_win("Ctrl+Shift+F9");
    CHECK(h2.valid && h2.mods == (MOD_CONTROL | MOD_SHIFT) && h2.vk == VK_F9);

    auto h3 = parse_hotkey_win("InvalidOnly");
    CHECK(!h3.valid);

    CHECK(device_name_matches("Microfone (4- fifine Microphone", "Microfone (4- fifine Microphone)"));
    CHECK(!device_name_matches("Microfone (Realtek", "Microfone (4- fifine Microphone"));
    CHECK(is_loopback_device_name("CABLE Output (VB-Audio Virtual)"));
    CHECK(!is_loopback_device_name("Microfone (4- fifine Microphone"));
    return 0;
}

static int test_clipboard_paste_timing_and_restore() {
    using namespace injector;

    // Backup user's real live clipboard so test leaves zero side effects
    std::wstring real_orig;
    bool real_had = false;
    get_clipboard_text_safe(real_orig, real_had);

    // 1. Dictated text stays on clipboard >= 300ms before restoring old Ctrl+V
    CHECK(set_clipboard_text_safe(L"OLD_CTRL_V_ITEM", false));
    CHECK(paste_isolated_instant("Frase transcrita teste ", false));

    std::wstring cur;
    bool has = false;
    CHECK(get_clipboard_text_safe(cur, has) && has);
    CHECK(cur == L"Frase transcrita teste ");

    Sleep(300); // far past old 70ms race window!
    CHECK(get_clipboard_text_safe(cur, has) && has);
    CHECK(cur == L"Frase transcrita teste ");

    Sleep(550); // past 700ms delayed restore window
    CHECK(get_clipboard_text_safe(cur, has) && has);
    CHECK(cur == L"OLD_CTRL_V_ITEM");

    // 2. Back-to-back pastes within 150ms must STILL preserve OLD_CTRL_V_ITEM
    CHECK(paste_isolated_instant("Primeira ", false));
    Sleep(150);
    CHECK(paste_isolated_instant("Segunda ", false));
    CHECK(get_clipboard_text_safe(cur, has) && has && cur == L"Segunda ");
    Sleep(850);
    CHECK(get_clipboard_text_safe(cur, has) && has && cur == L"OLD_CTRL_V_ITEM");

    // 3. External Ctrl+C during window must NOT be clobbered by delayed restore
    CHECK(paste_isolated_instant("Temporaria ", false));
    Sleep(100);
    CHECK(set_clipboard_text_safe(L"USER_MANUAL_COPY", false));
    Sleep(750);
    CHECK(get_clipboard_text_safe(cur, has) && has && cur == L"USER_MANUAL_COPY");

    // Restore real clipboard cleanly
    set_clipboard_text_safe(real_had ? real_orig : L"", true);
    return 0;
}

static int test_popup_toggle_and_cooldown() {
    Gdiplus::GdiplusStartupInput gi;
    ULONG_PTR gt = 0;
    Gdiplus::GdiplusStartup(&gt, &gi, nullptr);

    bool deferred = false;
    CHECK(!popup_card::is_popup_open());
    CHECK(!popup_card::check_toggle_or_dismiss("history", &deferred));
    CHECK(!deferred);

    // Simulate opening real popup menu and closing via dismiss on same thread timer
    UINT_PTR tid = SetTimer(nullptr, 0, 40, [](HWND, UINT, UINT_PTR, DWORD) {
        if (popup_card::is_popup_open()) {
            popup_card::dismiss_active_popup();
        }
    });
    std::vector<popup_card::PopupItem> items = { { 1, L"Item 1", false, false, false } };
    int res = popup_card::show_popup_menu(nullptr, 100, 100, items, theme::get_palette("dark"), 180, "history");
    KillTimer(nullptr, tid);
    CHECK(res == -1);
    CHECK(!popup_card::is_popup_open());

    // Immediate 2nd click on same button ("history") within 260ms must be suppressed (toggle closed)
    CHECK(popup_card::check_toggle_or_dismiss("history", &deferred));
    CHECK(!deferred);

    // Different tag ("lang") right after closing "history" is allowed cleanly
    CHECK(!popup_card::check_toggle_or_dismiss("lang", &deferred));

    // Clicking popup item 0 ("Google (Gratuito)") must return id 0 (never -1 during DestroyWindow WM_ACTIVATE!)
    UINT_PTR click_tid = SetTimer(nullptr, 0, 50, [](HWND, UINT, UINT_PTR, DWORD) {
        HWND pw = FindWindowW(L"VoiceDictationCustomMenu", nullptr);
        if (pw) {
            SendMessageW(pw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(60, 24));
            SendMessageW(pw, WM_LBUTTONUP, 0, MAKELPARAM(60, 24));
        }
    });
    std::vector<popup_card::PopupItem> engines = {
        { 0, L"Google (Gratuito)", false, false, false },
        { 1, L"Groq (Whisper API)", true, false, false },
    };
    int eng_res = popup_card::show_popup_menu(nullptr, 120, 120, engines, theme::get_palette("dark"), 240, "test_eng");
    KillTimer(nullptr, click_tid);
    CHECK(eng_res == 0);
    CHECK(!popup_card::is_popup_open());

    Gdiplus::GdiplusShutdown(gt);
    return 0;
}

int main() {
    if (test_json_and_config() != 0) return 1;
    if (test_speech_detection_and_trim() != 0) return 1;
    if (test_pcm_wav_flac_and_resample() != 0) return 1;
    if (test_text_processing() != 0) return 1;
    if (test_hotkey_and_device_helpers() != 0) return 1;
    if (test_clipboard_paste_timing_and_restore() != 0) return 1;
    if (test_popup_toggle_and_cooldown() != 0) return 1;
    printf("[OK] test_native: %d checks passed!\n", g_passed);
    return 0;
}
