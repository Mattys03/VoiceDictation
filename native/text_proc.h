#pragma once

#include "common.h"
#include <algorithm>
#include <cstring>
#include <sstream>

namespace text_proc {

inline std::wstring wlower(std::wstring s) {
    if (!s.empty()) {
        CharLowerBuffW(s.data(), (DWORD)s.size());
    }
    return s;
}

inline std::wstring wupper(std::wstring s) {
    if (!s.empty()) {
        CharUpperBuffW(s.data(), (DWORD)s.size());
    }
    return s;
}

inline std::string utf8_lower(const std::string& s) {
    return wide_to_utf8(wlower(utf8_to_wide(s)));
}

inline std::string utf8_upper(const std::string& s) {
    return wide_to_utf8(wupper(utf8_to_wide(s)));
}

inline std::wstring wtrim(const std::wstring& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n')) --e;
    return s.substr(b, e - b);
}

inline std::wstring wrstrip(const std::wstring& s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n')) --e;
    return s.substr(0, e);
}

inline std::wstring wlstrip(const std::wstring& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) ++b;
    return s.substr(b);
}

inline std::wstring strip_edge_punct(const std::wstring& s) {
    auto is_strip = [](wchar_t ch) {
        return wcschr(L".,;:!?\u2014\u2013()[]\"' ", ch) != nullptr;
    };
    size_t b = 0;
    while (b < s.size() && is_strip(s[b])) ++b;
    size_t e = s.size();
    while (e > b && is_strip(s[e - 1])) --e;
    return s.substr(b, e - b);
}

inline std::string whisper_language_code(const std::string& language) {
    if (language == "pt" || language == "pt-BR" ||
        language == "translate_en" || language == "translate_es") {
        return "pt";
    }
    if (language == "en" || language == "en-US") return "en";
    if (language == "es" || language == "es-ES") return "es";
    return "";
}

inline std::wstring apply_voice_commands_w(std::wstring text) {
    struct Cmd {
        const wchar_t* phrase;
        const wchar_t* sym;
    };
    // Ordered longest-first so multi-word commands match before shorter prefixes
    static const Cmd cmds[] = {
        { L"ponto de interroga\u00E7\u00E3o", L"?" },
        { L"ponto de interrogacao", L"?" },
        { L"ponto de exclama\u00E7\u00E3o", L"!" },
        { L"ponto de exclamacao", L"!" },
        { L"fecha par\u00EAnteses", L")" },
        { L"abre par\u00EAnteses", L"(" },
        { L"fecha parenteses", L")" },
        { L"abre parenteses", L"(" },
        { L"ponto e v\u00EDrgula", L";" },
        { L"ponto e virgula", L";" },
        { L"retic\u00EAncias", L"..." },
        { L"reticencias", L"..." },
        { L"dois pontos", L":" },
        { L"ponto final", L"." },
        { L"travess\u00E3o", L" \u2014" },
        { L"travessao", L" \u2014" },
        { L"nova linha", L"\n" },
        { L"par\u00E1grafo", L"\n\n" },
        { L"paragrafo", L"\n\n" },
        { L"v\u00EDrgula", L"," },
        { L"virgula", L"," },
    };

    for (const auto& c : cmds) {
        std::wstring cmd(c.phrase);
        std::wstring sym(c.sym);
        std::wstring low = wlower(text);
        size_t idx = low.find(cmd);
        while (idx != std::wstring::npos) {
            std::wstring before = wrstrip(text.substr(0, idx));
            std::wstring after = wlstrip(text.substr(idx + cmd.size()));
            if (sym == L"\n" || sym == L"\n\n") {
                text = before + sym + after;
            } else {
                text = before + sym + L" " + after;
            }
            low = wlower(text);
            idx = low.find(cmd);
        }
    }
    return text;
}

inline std::vector<std::wstring> split_ws_w(const std::wstring& s) {
    std::vector<std::wstring> words;
    std::wstring cur;
    for (wchar_t ch : s) {
        if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n') {
            if (!cur.empty()) {
                words.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) words.push_back(std::move(cur));
    return words;
}

inline std::wstring rstrip_punct_w(const std::wstring& s) {
    size_t e = s.size();
    while (e > 0 && wcschr(L".,;:!?", s[e - 1]) != nullptr) --e;
    return s.substr(0, e);
}

inline std::wstring insert_commas_w(const std::wstring& text) {
    static const wchar_t* kCommaBefore[] = {
        L"mas", L"por\u00E9m", L"porem", L"contudo", L"entretanto", L"todavia",
        L"ou seja", L"por exemplo", L"enfim", L"ent\u00E3o", L"entao",
        L"porque", L"pois", L"portanto", L"assim", L"al\u00E9m disso",
        L"al\u00E9m do mais", L"ali\u00E1s", L"alias", L"inclusive", L"na verdade",
        L"tipo", L"sabe", L"t\u00E1 ligado", L"ta ligado", L"entendeu",
        L"no entanto", L"apesar de", L"embora"
    };
    auto is_connector = [&](const std::wstring& w) {
        for (const wchar_t* c : kCommaBefore) {
            if (w == c) return true;
        }
        return false;
    };
    auto ends_with_punct = [](const std::wstring& w) {
        return !w.empty() && wcschr(L".,;:!?\u2014(", w.back()) != nullptr;
    };

    std::vector<std::wstring> words = split_ws_w(text);
    std::vector<std::wstring> result;
    size_t i = 0;
    while (i < words.size()) {
        bool matched = false;
        for (size_t length : { (size_t)3, (size_t)2 }) {
            if (i + length <= words.size()) {
                std::wstring phrase = words[i];
                for (size_t k = 1; k < length; ++k) {
                    phrase += L" ";
                    phrase += words[i + k];
                }
                std::wstring phrase_clean = rstrip_punct_w(wlower(phrase));
                if (is_connector(phrase_clean)) {
                    if (!result.empty() && !ends_with_punct(result.back())) {
                        result.back() += L",";
                    }
                    for (size_t k = 0; k < length; ++k) {
                        result.push_back(words[i + k]);
                    }
                    i += length;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            std::wstring word_low = rstrip_punct_w(wlower(words[i]));
            if (i > 0 && is_connector(word_low)) {
                if (!result.empty() && !ends_with_punct(result.back())) {
                    result.back() += L",";
                }
            }
            result.push_back(words[i]);
            ++i;
        }
    }

    std::wstring out;
    for (size_t k = 0; k < result.size(); ++k) {
        if (k > 0) out.push_back(L' ');
        out += result[k];
    }
    return out;
}

inline std::string auto_punctuate(const std::string& utf8_text) {
    std::wstring text = wtrim(utf8_to_wide(utf8_text));
    if (text.empty()) return {};

    text = apply_voice_commands_w(text);
    text = insert_commas_w(text);

    if (!text.empty() && IsCharAlphaW(text[0])) {
        text[0] = (wchar_t)(uintptr_t)CharUpperW((LPWSTR)(uintptr_t)text[0]);
    }

    bool capitalize_next = false;
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t ch = text[i];
        if (capitalize_next && IsCharAlphaW(ch)) {
            text[i] = (wchar_t)(uintptr_t)CharUpperW((LPWSTR)(uintptr_t)ch);
            capitalize_next = false;
        }
        if (ch == L'.' || ch == L'!' || ch == L'?' || ch == L'\n') {
            capitalize_next = true;
        }
    }

    text = wrstrip(text);
    if (!text.empty() && wcschr(L".!?;:\u2026", text.back()) != nullptr) {
        return wide_to_utf8(text);
    }
    text.push_back(L'.');
    return wide_to_utf8(text);
}

inline std::string filter_and_format_transcription(
    const std::string& raw_utf8,
    size_t audio_bytes,
    uint32_t sample_rate,
    int speech_peak,
    bool use_auto_punctuation,
    bool capslock_on,
    std::string* blocked_reason = nullptr
) {
    std::wstring text = wtrim(utf8_to_wide(raw_utf8));
    if (text.empty()) return {};

    static const wchar_t* kWatermarks[] = {
        L"amara.org", L"s\u00F4nia ruberti", L"sonia ruberti",
        L"legendas:", L"legenda por", L"legendas por", L"legendado por",
        L"tradu\u00E7\u00E3o e sincroniza\u00E7\u00E3o", L"traducao e sincronizacao",
        L"obrigado por assistir", L"obrigada por assistir",
        L"thanks for watching", L"thank you for watching",
        L"subtitles by", L"subtitles created by", L"closed captions",
        L"inscreva-se no canal", L"se inscreva no canal",
        L"deixe seu like", L"deixe o seu like", L"deixe o like",
        L"curta e compartilhe", L"ative o sininho",
        L"todos os direitos reservados", L"transcribed by",
        L"translated by", L"assistir ao v\u00EDdeo", L"assistir a este v\u00EDdeo",
        L"transcribe verbatim", L"spoken language without translating",
        L"transcreva exatamente", L"sem traduzir nada"
    };

    static const wchar_t* kPhantomWords[] = {
        L"obrigado", L"obrigada", L"thank you", L"thanks", L"you",
        L"bye", L"bye bye", L"goodbye", L"tchau", L"valeu", L"falou",
        L"al\u00F4", L"alo", L"ol\u00E1", L"ola", L"ei", L"e a\u00ED", L"e ai",
        L"the end", L"fim", L"so", L"yes", L"no", L"transcreva", L"transcribe"
    };

    std::wstring low = wtrim(wlower(text));
    for (const wchar_t* wm : kWatermarks) {
        std::wstring wms(wm);
        size_t pos = low.find(wms);
        if (pos != std::wstring::npos) {
            while (pos != std::wstring::npos) {
                text.erase(pos, wms.size());
                low = wtrim(wlower(text));
                pos = low.find(wms);
            }
            text = wtrim(text);
            low = wtrim(wlower(text));
        }
    }

    std::wstring clean_word = strip_edge_punct(low);
    bool is_hallucination = false;

    if (clean_word.size() <= 1 ||
        text == L"." || text == L".." || text == L"..." ||
        text == L"?" || text == L"!" || text == L"\u2014" ||
        text == L"-" || text == L"\u2026") {
        is_hallucination = true;
    } else if (((low.rfind(L"[", 0) == 0 && !low.empty() && low.back() == L']') ||
                (low.rfind(L"(", 0) == 0 && !low.empty() && low.back() == L')'))) {
        is_hallucination = true;
    } else {
        bool in_phantom = false;
        for (const wchar_t* pw : kPhantomWords) {
            if (clean_word == pw) {
                in_phantom = true;
                break;
            }
        }
        if (in_phantom && (audio_bytes < (size_t)sample_rate * 2 || (speech_peak >= 0 && speech_peak < 800))) {
            is_hallucination = true;
        }
    }

    if (is_hallucination) {
        if (blocked_reason) *blocked_reason = wide_to_utf8(text);
        return {};
    }

    // Case-insensitive adjacent duplicate word removal
    std::vector<std::wstring> words = split_ws_w(text);
    std::vector<std::wstring> deduped;
    std::wstring prev_low;
    for (const auto& w : words) {
        std::wstring wl = wlower(w);
        if (deduped.empty() || wl != prev_low) {
            deduped.push_back(w);
            prev_low = wl;
        }
    }
    std::wstring joined;
    for (size_t i = 0; i < deduped.size(); ++i) {
        if (i > 0) joined.push_back(L' ');
        joined += deduped[i];
    }

    std::string out = wide_to_utf8(joined);
    if (use_auto_punctuation) {
        out = auto_punctuate(out);
    }
    if (capslock_on) {
        out = utf8_upper(out);
    }
    return out;
}

} // namespace text_proc
