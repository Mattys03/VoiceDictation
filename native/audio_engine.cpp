#include "audio_engine.h"
#include "logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace audio_engine {

int compute_rms_pcm16(const int16_t* samples, size_t count) {
    if (!samples || count == 0) return 0;
    uint64_t sum_sq = 0;
    for (size_t i = 0; i < count; ++i) {
        int64_t s = samples[i];
        sum_sq += (uint64_t)(s * s);
    }
    return (int)std::sqrt((double)sum_sq / (double)count);
}

SpeechAnalysis analyze_speech(const std::vector<int>& rms_values) {
    SpeechAnalysis res;
    if (rms_values.empty()) return res;

    int peak = *std::max_element(rms_values.begin(), rms_values.end());
    std::vector<int> ordered = rms_values;
    size_t pos = 0;
    size_t p_raw = (size_t)((double)ordered.size() * kNoiseFloorPercentile);
    if (p_raw > 0) pos = p_raw - 1;
    std::nth_element(ordered.begin(), ordered.begin() + pos, ordered.end());
    int floor = ordered[pos];

    res.peak = peak;
    res.floor = floor;

    if (peak < kSpeechAbsMinRms) {
        res.has_speech = false;
        res.threshold = kSpeechAbsMinRms;
        res.chunks_above = 0;
        return res;
    }

    int ratio_thr = (int)((double)floor * kSpeechNoiseRatio);
    int threshold = std::max(kSpeechAbsMinRms, std::min(ratio_thr, kSpeechThresholdMax));
    int above = 0;
    for (int v : rms_values) {
        if (v >= threshold) ++above;
    }

    bool louder_than_room = (double)peak >= (double)floor * kSpeechNoiseRatio;
    bool clear_step = (peak - floor) >= kSpeechMinStepRms;
    res.threshold = threshold;
    res.chunks_above = above;
    res.has_speech = (louder_than_room || clear_step);
    return res;
}

std::vector<int16_t> trim_silent_edges(
    const std::vector<std::vector<int16_t>>& frames,
    const std::vector<int>& rms_values,
    int threshold,
    size_t pre_chunks,
    size_t post_chunks,
    size_t* out_start,
    size_t* out_end
) {
    size_t count = std::min(frames.size(), rms_values.size());
    size_t first_active = count;
    size_t last_active = 0;
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        if (rms_values[i] >= threshold) {
            if (!found) first_active = i;
            last_active = i;
            found = true;
        }
    }
    if (!found) {
        if (out_start) *out_start = 0;
        if (out_end) *out_end = 0;
        return {};
    }

    size_t start = (first_active > pre_chunks) ? (first_active - pre_chunks) : 0;
    size_t end = std::min(count, last_active + post_chunks + 1);
    if (out_start) *out_start = start;
    if (out_end) *out_end = end;

    size_t total_samples = 0;
    for (size_t i = start; i < end; ++i) total_samples += frames[i].size();

    std::vector<int16_t> out;
    out.reserve(total_samples);
    for (size_t i = start; i < end; ++i) {
        out.insert(out.end(), frames[i].begin(), frames[i].end());
    }
    return out;
}

std::vector<int16_t> resample_pcm16_mono(
    const int16_t* input,
    size_t count,
    uint32_t in_rate,
    uint32_t out_rate
) {
    if (!input || count == 0) return {};
    if (in_rate == 0 || out_rate == 0 || in_rate == out_rate) {
        return std::vector<int16_t>(input, input + count);
    }

    size_t out_count = (size_t)(((uint64_t)count * out_rate) / in_rate);
    if (out_count == 0) return {};
    std::vector<int16_t> out(out_count);

    double ratio = (double)in_rate / (double)out_rate;
    for (size_t i = 0; i < out_count; ++i) {
        double src_pos = (double)i * ratio;
        size_t idx0 = (size_t)src_pos;
        if (idx0 >= count - 1) {
            out[i] = input[count - 1];
        } else {
            double frac = src_pos - (double)idx0;
            double s = (double)input[idx0] * (1.0 - frac) + (double)input[idx0 + 1] * frac;
            if (s > 32767.0) s = 32767.0;
            if (s < -32768.0) s = -32768.0;
            out[i] = (int16_t)s;
        }
    }
    return out;
}

std::vector<uint8_t> build_wav_bytes(const int16_t* pcm, size_t sample_count, uint32_t sample_rate) {
    uint32_t data_bytes = (uint32_t)(sample_count * sizeof(int16_t));
    uint32_t riff_size = 36 + data_bytes;
    std::vector<uint8_t> wav(44 + data_bytes);
    uint8_t* p = wav.data();

    auto write_u32 = [&](size_t off, uint32_t v) { memcpy(p + off, &v, 4); };
    auto write_u16 = [&](size_t off, uint16_t v) { memcpy(p + off, &v, 2); };

    memcpy(p + 0, "RIFF", 4);
    write_u32(4, riff_size);
    memcpy(p + 8, "WAVEfmt ", 8);
    write_u32(16, 16);                  // PCM fmt chunk size
    write_u16(20, 1);                   // PCM format
    write_u16(22, 1);                   // Mono
    write_u32(24, sample_rate);
    write_u32(28, sample_rate * 2);     // Byte rate
    write_u16(32, 2);                   // Block align
    write_u16(34, 16);                  // Bits per sample
    memcpy(p + 36, "data", 4);
    write_u32(40, data_bytes);
    if (data_bytes > 0) {
        memcpy(p + 44, pcm, data_bytes);
    }
    return wav;
}

static uint8_t flac_crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x80) crc = (uint8_t)((crc << 1) ^ 0x07);
            else crc <<= 1;
        }
    }
    return crc;
}

static uint16_t flac_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x8005);
            else crc <<= 1;
        }
    }
    return crc;
}

static void write_utf8_uint32_be(std::vector<uint8_t>& out, uint32_t val) {
    if (val < 0x80) {
        out.push_back((uint8_t)val);
    } else if (val < 0x800) {
        out.push_back((uint8_t)(0xC0 | (val >> 6)));
        out.push_back((uint8_t)(0x80 | (val & 0x3F)));
    } else if (val < 0x10000) {
        out.push_back((uint8_t)(0xE0 | (val >> 12)));
        out.push_back((uint8_t)(0x80 | ((val >> 6) & 0x3F)));
        out.push_back((uint8_t)(0x80 | (val & 0x3F)));
    } else if (val < 0x200000) {
        out.push_back((uint8_t)(0xF0 | (val >> 18)));
        out.push_back((uint8_t)(0x80 | ((val >> 12) & 0x3F)));
        out.push_back((uint8_t)(0x80 | ((val >> 6) & 0x3F)));
        out.push_back((uint8_t)(0x80 | (val & 0x3F)));
    } else {
        out.push_back((uint8_t)(0xF8 | (val >> 24)));
        out.push_back((uint8_t)(0x80 | ((val >> 18) & 0x3F)));
        out.push_back((uint8_t)(0x80 | ((val >> 12) & 0x3F)));
        out.push_back((uint8_t)(0x80 | ((val >> 6) & 0x3F)));
        out.push_back((uint8_t)(0x80 | (val & 0x3F)));
    }
}

std::vector<uint8_t> build_flac_verbatim_bytes(const int16_t* pcm, size_t sample_count, uint32_t sample_rate) {
    // RFC 9639 lossless VERBATIM FLAC container (zero external binaries needed)
    const uint32_t kBlockSize = 4096;
    std::vector<uint8_t> out;
    out.reserve(64 + sample_count * 2 + (sample_count / kBlockSize + 2) * 16);

    // 1. "fLaC" marker + STREAMINFO (last metadata block, type=0, length=34)
    out.insert(out.end(), {'f', 'L', 'a', 'C', 0x80, 0x00, 0x00, 0x22});

    uint16_t min_bs = (uint16_t)std::min<size_t>(kBlockSize, std::max<size_t>(16, sample_count));
    uint16_t max_bs = (uint16_t)kBlockSize;
    uint8_t si[34] = {};
    si[0] = (uint8_t)(min_bs >> 8);
    si[1] = (uint8_t)(min_bs & 0xFF);
    si[2] = (uint8_t)(max_bs >> 8);
    si[3] = (uint8_t)(max_bs & 0xFF);
    // min/max frame size = 0 (unknown)
    // sample_rate (20 bits) | (channels-1 = 0, 3 bits) | (bps-1 = 15, 5 bits) | total_samples (36 bits)
    uint64_t total_s = sample_count;
    si[10] = (uint8_t)((sample_rate >> 12) & 0xFF);
    si[11] = (uint8_t)((sample_rate >> 4) & 0xFF);
    si[12] = (uint8_t)(((sample_rate & 0x0F) << 4) | 0x00 | (15 >> 1));
    si[13] = (uint8_t)(((15 & 1) << 7) | ((total_s >> 32) & 0x0F));
    si[14] = (uint8_t)((total_s >> 24) & 0xFF);
    si[15] = (uint8_t)((total_s >> 16) & 0xFF);
    si[16] = (uint8_t)((total_s >> 8) & 0xFF);
    si[17] = (uint8_t)(total_s & 0xFF);
    // MD5 left 0 (valid per spec)
    out.insert(out.end(), si, si + 34);

    // 2. Frames
    uint32_t frame_num = 0;
    size_t pos = 0;
    while (pos < sample_count) {
        uint32_t bs = (uint32_t)std::min<size_t>(kBlockSize, sample_count - pos);
        size_t frame_start = out.size();

        uint8_t bs_code = 0x07; // 16-bit blocksize - 1 at end of header
        if (bs == 4096) bs_code = 0x0C;

        // Byte 0-1: sync code 0xFFF8 (fixed blocking strategy)
        out.push_back(0xFF);
        out.push_back(0xF8);
        // Byte 2: block size (upper 4) | sample rate code = 0 (get from STREAMINFO -> safe for any rate!)
        out.push_back((uint8_t)(bs_code << 4));
        // Byte 3: channels = 0 (mono, 1ch) | sample size code = 4 (16-bit) -> (0<<4)|(4<<1) = 0x08
        out.push_back(0x08);
        write_utf8_uint32_be(out, frame_num);
        if (bs_code == 0x07) {
            uint16_t bs_m1 = (uint16_t)(bs - 1);
            out.push_back((uint8_t)(bs_m1 >> 8));
            out.push_back((uint8_t)(bs_m1 & 0xFF));
        }
        uint8_t hdr_crc = flac_crc8(out.data() + frame_start, out.size() - frame_start);
        out.push_back(hdr_crc);

        // Subframe header (VERBATIM = type 1 -> (0 << 7) | (1 << 1) | 0 = 0x02)
        out.push_back(0x02);
        // Samples big-endian 16-bit signed
        size_t old_sz = out.size();
        out.resize(old_sz + (size_t)bs * 2);
        uint8_t* dst = out.data() + old_sz;
        for (uint32_t k = 0; k < bs; ++k) {
            uint16_t u = (uint16_t)pcm[pos + k];
            dst[k * 2 + 0] = (uint8_t)(u >> 8);
            dst[k * 2 + 1] = (uint8_t)(u & 0xFF);
        }

        uint16_t frame_crc = flac_crc16(out.data() + frame_start, out.size() - frame_start);
        out.push_back((uint8_t)(frame_crc >> 8));
        out.push_back((uint8_t)(frame_crc & 0xFF));

        pos += bs;
        ++frame_num;
    }
    return out;
}

std::vector<MicDeviceInfo> enumerate_mme_microphones() {
    std::vector<MicDeviceInfo> list;
    UINT count = waveInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        WAVEINCAPSW caps{};
        if (waveInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        MicDeviceInfo info;
        info.index = (int)i;
        info.name = wide_to_utf8(caps.szPname);
        info.api = "MME";
        if (caps.dwFormats & (WAVE_FORMAT_4S16 | WAVE_FORMAT_4M16 | WAVE_FORMAT_4S08 | WAVE_FORMAT_4M08)) {
            info.default_rate = 44100;
        } else {
            info.default_rate = 48000;
        }
        list.push_back(std::move(info));
    }
    return list;
}

AudioRecorder::AudioRecorder() {
    InitializeCriticalSection(&m_cs);
    m_cmd_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_stop_capture_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_shutdown_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

AudioRecorder::~AudioRecorder() {
    stop_thread();
    if (m_cmd_event) CloseHandle(m_cmd_event);
    if (m_stop_capture_event) CloseHandle(m_stop_capture_event);
    if (m_shutdown_event) CloseHandle(m_shutdown_event);
    DeleteCriticalSection(&m_cs);
}

void AudioRecorder::start_thread() {
    if (m_thread) return;
    ResetEvent(m_shutdown_event);
    m_thread = CreateThread(nullptr, 0, &AudioRecorder::thread_proc, this, 0, nullptr);
    if (m_thread) {
        SetThreadPriority(m_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    }
}

void AudioRecorder::stop_thread() {
    if (!m_thread) return;
    SetEvent(m_stop_capture_event);
    SetEvent(m_shutdown_event);
    SetEvent(m_cmd_event);
    WaitForSingleObject(m_thread, 3000);
    CloseHandle(m_thread);
    m_thread = nullptr;
}

void AudioRecorder::invalidate_cache() {
    EnterCriticalSection(&m_cs);
    m_has_cached = false;
    LeaveCriticalSection(&m_cs);
}

void AudioRecorder::request_prewarm(const AppSettings& settings) {
    if (m_capturing.load(std::memory_order_relaxed) || m_prewarm_busy.load(std::memory_order_relaxed)) {
        return;
    }
    EnterCriticalSection(&m_cs);
    if (m_pending_cmd == CmdType::None) {
        m_pending_cmd = CmdType::Prewarm;
        m_pending_settings = settings;
        SetEvent(m_cmd_event);
    }
    LeaveCriticalSection(&m_cs);
}

bool AudioRecorder::start_capture(
    uint64_t session_id,
    const AppSettings& settings,
    DoneCallback on_done,
    StatusCallback on_status
) {
    if (m_capturing.load(std::memory_order_relaxed)) return false;
    ResetEvent(m_stop_capture_event);
    m_capturing.store(true, std::memory_order_relaxed);
    m_level.store(0.0f, std::memory_order_relaxed);

    EnterCriticalSection(&m_cs);
    m_pending_cmd = CmdType::Capture;
    m_pending_session = session_id;
    m_pending_settings = settings;
    m_pending_done = std::move(on_done);
    m_pending_status = std::move(on_status);
    SetEvent(m_cmd_event);
    LeaveCriticalSection(&m_cs);
    return true;
}

void AudioRecorder::stop_capture() {
    SetEvent(m_stop_capture_event);
}

DWORD WINAPI AudioRecorder::thread_proc(LPVOID param) {
    static_cast<AudioRecorder*>(param)->run_loop();
    return 0;
}

void AudioRecorder::run_loop() {
    HANDLE waits[2] = { m_cmd_event, m_shutdown_event };
    while (true) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 + 1) break;

        CmdType cmd = CmdType::None;
        AppSettings cfg;
        uint64_t sid = 0;
        DoneCallback done_cb;
        StatusCallback status_cb;

        EnterCriticalSection(&m_cs);
        cmd = m_pending_cmd;
        m_pending_cmd = CmdType::None;
        cfg = m_pending_settings;
        sid = m_pending_session;
        done_cb = std::move(m_pending_done);
        status_cb = std::move(m_pending_status);
        LeaveCriticalSection(&m_cs);

        if (cmd == CmdType::Prewarm) {
            do_prewarm(cfg);
        } else if (cmd == CmdType::Capture) {
            do_capture(sid, cfg, std::move(done_cb), std::move(status_cb));
        }
    }
}

bool AudioRecorder::open_wave_in(UINT dev_id, uint32_t rate, HWAVEIN* out_hwi, HANDLE hEvent) {
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = rate * 2;
    wfx.cbSize = 0;

    MMRESULT res = waveInOpen(
        out_hwi, dev_id, &wfx,
        (DWORD_PTR)hEvent, 0, CALLBACK_EVENT
    );
    return res == MMSYSERR_NOERROR && *out_hwi != nullptr;
}

AudioRecorder::ProbeResult AudioRecorder::probe_device(
    UINT dev_id, const std::string& dev_name, uint32_t rate, int chunks
) {
    ProbeResult pr;
    pr.dev_id = dev_id;
    pr.rate = rate;
    pr.name = dev_name;

    HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent) {
        pr.problem = "falha ao criar evento de audio";
        return pr;
    }

    HWAVEIN hwi = nullptr;
    if (!open_wave_in(dev_id, rate, &hwi, hEvent)) {
        CloseHandle(hEvent);
        pr.problem = "nao pode ser aberto";
        return pr;
    }
    pr.opened = true;

    std::vector<int16_t> buf(kChunkSamples * 2);
    WAVEHDR hdrs[2]{};
    for (int i = 0; i < 2; ++i) {
        hdrs[i].lpData = (LPSTR)&buf[i * kChunkSamples];
        hdrs[i].dwBufferLength = kChunkSamples * sizeof(int16_t);
        waveInPrepareHeader(hwi, &hdrs[i], sizeof(WAVEHDR));
        waveInAddBuffer(hwi, &hdrs[i], sizeof(WAVEHDR));
    }

    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    waveInStart(hwi);
    int peak = 0;
    bool all_zero = true;
    int collected = 0;
    bool failed = false;

    while (collected < chunks) {
        DWORD w = WaitForSingleObject(hEvent, 1200);
        if (w != WAIT_OBJECT_0) {
            pr.problem = "timeout na leitura do microfone";
            failed = true;
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (hdrs[i].dwFlags & WHDR_DONE) {
                size_t n = hdrs[i].dwBytesRecorded / sizeof(int16_t);
                const int16_t* s = (const int16_t*)hdrs[i].lpData;
                int rms = compute_rms_pcm16(s, n);
                if (rms > peak) peak = rms;
                if (rms > 0) all_zero = false;
                ++collected;
                hdrs[i].dwFlags &= ~WHDR_DONE;
                hdrs[i].dwBytesRecorded = 0;
                if (collected < chunks) {
                    waveInAddBuffer(hwi, &hdrs[i], sizeof(WAVEHDR));
                }
            }
        }
    }

    QueryPerformanceCounter(&t1);
    waveInReset(hwi);
    for (int i = 0; i < 2; ++i) {
        waveInUnprepareHeader(hwi, &hdrs[i], sizeof(WAVEHDR));
    }
    waveInClose(hwi);
    CloseHandle(hEvent);

    if (failed) return pr;

    double elapsed = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    double expected = (double)(chunks * kChunkSamples) / (double)rate;
    if (elapsed < expected * 0.45) {
        pr.problem = "nao captura em tempo real";
        return pr;
    }
    if (all_zero && peak == 0) {
        pr.problem = "entrega silencio digital constante";
        return pr;
    }
    pr.peak = peak;
    pr.healthy = true;
    pr.valid = true;
    return pr;
}

AudioRecorder::ProbeResult AudioRecorder::resolve_best_device(const AppSettings& cfg, bool* out_used_fallback_change) {
    if (out_used_fallback_change) *out_used_fallback_change = false;
    auto mics = enumerate_mme_microphones();
    if (mics.empty()) {
        ProbeResult empty;
        empty.dev_id = WAVE_MAPPER;
        empty.rate = 44100;
        empty.problem = "nenhum dispositivo de entrada encontrado";
        return empty;
    }

    bool was_configured = !cfg.microphone_name.empty() || cfg.microphone_index >= 0;
    std::vector<MicDeviceInfo> ordered;
    std::vector<bool> used(mics.size(), false);

    // 1. Exact name matches
    for (size_t i = 0; i < mics.size(); ++i) {
        if (!cfg.microphone_name.empty() && device_name_matches(cfg.microphone_name, mics[i].name)) {
            ordered.push_back(mics[i]);
            used[i] = true;
        }
    }
    // 2. Numeric index match
    for (size_t i = 0; i < mics.size(); ++i) {
        if (!used[i] && cfg.microphone_index >= 0 && mics[i].index == cfg.microphone_index) {
            ordered.push_back(mics[i]);
            used[i] = true;
        }
    }
    // 3. Remaining MME devices
    for (size_t i = 0; i < mics.size(); ++i) {
        if (!used[i]) ordered.push_back(mics[i]);
    }

    ProbeResult first_opened;
    for (const auto& m : ordered) {
        uint32_t rates[4] = { m.default_rate, 48000, 44100, 16000 };
        for (int r = 0; r < 4; ++r) {
            uint32_t rate = rates[r];
            bool dup = false;
            for (int prev = 0; prev < r; ++prev) {
                if (rates[prev] == rate) dup = true;
            }
            if (dup) continue;

            ProbeResult pr = probe_device((UINT)m.index, m.name, rate, 6);
            if (!first_opened.opened && pr.opened) first_opened = pr;
            if (pr.healthy) {
                if (was_configured && !device_name_matches(cfg.microphone_name, m.name)) {
                    if (out_used_fallback_change) *out_used_fallback_change = true;
                }
                return pr;
            }
            if (pr.opened) break; // per-device failure
        }
    }
    return first_opened;
}

void AudioRecorder::do_prewarm(const AppSettings& cfg) {
    m_prewarm_busy.store(true, std::memory_order_relaxed);
    UINT count = waveInGetNumDevs();
    bool adopted = false;
    ProbeResult pr = resolve_best_device(cfg, &adopted);
    if (pr.healthy) {
        EnterCriticalSection(&m_cs);
        m_has_cached = true;
        m_cached_dev_id = pr.dev_id;
        m_cached_rate = pr.rate;
        m_cached_name = pr.name;
        m_last_dev_count = count;
        LeaveCriticalSection(&m_cs);
        LOGI("[OK] Audio nativo MME pre-aquecido (%uHz, dev_idx=%u, %s, peak=%d)",
             pr.rate, pr.dev_id, pr.name.c_str(), pr.peak);
    } else {
        LOGE("[!] Microfone indisponivel no pre-aquecimento: %s", pr.problem.c_str());
    }
    m_prewarm_busy.store(false, std::memory_order_relaxed);
}

void AudioRecorder::do_capture(
    uint64_t session_id,
    const AppSettings& cfg,
    DoneCallback on_done,
    StatusCallback on_status
) {
    // Boost audio thread via Windows MMCSS Pro Audio during active recording so
    // CPU=100% never starves microphone hardware buffers.
    HANDLE hMmcss = nullptr;
    typedef HANDLE (WINAPI *PFN_AvSetMm)(LPCWSTR, LPDWORD);
    typedef BOOL (WINAPI *PFN_AvRevert)(HANDLE);
    HMODULE hAvrt = LoadLibraryW(L"avrt.dll");
    PFN_AvRevert pAvRevert = nullptr;
    if (hAvrt) {
        auto pAvSet = (PFN_AvSetMm)GetProcAddress(hAvrt, "AvSetMmThreadCharacteristicsW");
        pAvRevert = (PFN_AvRevert)GetProcAddress(hAvrt, "AvRevertMmThreadCharacteristics");
        if (pAvSet) {
            DWORD task_idx = 0;
            hMmcss = pAvSet(L"Pro Audio", &task_idx);
        }
    }

    CapturedSession out;
    bool used_cache = false;
    UINT dev_id = WAVE_MAPPER;
    uint32_t rate = 44100;
    std::string mic_name;

    UINT os_devs = waveInGetNumDevs();
    EnterCriticalSection(&m_cs);
    if (m_has_cached && os_devs == m_last_dev_count) {
        dev_id = m_cached_dev_id;
        rate = m_cached_rate;
        mic_name = m_cached_name;
        used_cache = true;
    }
    LeaveCriticalSection(&m_cs);

    HANDLE hBufEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWAVEIN hwi = nullptr;

    if (used_cache && hBufEvent) {
        if (open_wave_in(dev_id, rate, &hwi, hBufEvent)) {
            LOGI("[OK] Microfone aberto instantaneamente (MME cache: %uHz, idx=%u)", rate, dev_id);
        } else {
            LOGI("[!] Cache MME falhou; re-resolvendo dispositivo...");
            invalidate_cache();
        }
    }

    if (!hwi && hBufEvent && WaitForSingleObject(m_stop_capture_event, 0) == WAIT_TIMEOUT) {
        bool adopted = false;
        ProbeResult pr = resolve_best_device(cfg, &adopted);
        if (WaitForSingleObject(m_stop_capture_event, 0) == WAIT_OBJECT_0) {
            // Cancelled while resolving
        } else if (pr.valid && open_wave_in(pr.dev_id, pr.rate, &hwi, hBufEvent)) {
            dev_id = pr.dev_id;
            rate = pr.rate;
            mic_name = pr.name;
            EnterCriticalSection(&m_cs);
            m_has_cached = true;
            m_cached_dev_id = dev_id;
            m_cached_rate = rate;
            m_cached_name = mic_name;
            m_last_dev_count = waveInGetNumDevs();
            LeaveCriticalSection(&m_cs);
            out.adopted_new_mic = adopted;
            out.resolved_mic_name = mic_name;
            LOGI("[OK] Microfone selecionado e aberto (%uHz, idx=%u, %s)", rate, dev_id, mic_name.c_str());
        } else {
            // Last resort: WAVE_MAPPER system default
            const uint32_t fallback_rates[3] = { 44100, 48000, 16000 };
            for (uint32_t fr : fallback_rates) {
                if (open_wave_in(WAVE_MAPPER, fr, &hwi, hBufEvent)) {
                    dev_id = WAVE_MAPPER;
                    rate = fr;
                    LOGI("[OK] Fallback mic padrao sistema aberto (%uHz)", rate);
                    break;
                }
            }
        }
    }

    if (!hwi) {
        if (hBufEvent) CloseHandle(hBufEvent);
        if (hMmcss && pAvRevert) pAvRevert(hMmcss);
        if (hAvrt) FreeLibrary(hAvrt);
        m_capturing.store(false, std::memory_order_relaxed);
        out.aborted = true;
        out.mic_problem = "nenhum dispositivo de audio pode ser aberto";
        LOGE("[✗] Erro fatal: nenhum dispositivo waveIn aberto.");
        if (on_status) on_status(session_id, "Erro no mic");
        if (on_done) on_done(session_id, std::move(out));
        return;
    }

    if (on_status) on_status(session_id, "Ouvindo  00:00");

    std::vector<std::vector<int16_t>> frames;
    std::vector<int> rms_values;
    frames.reserve(512);
    rms_values.reserve(512);

    const size_t max_chunks = (kMaxRecordSeconds * (size_t)rate) / kChunkSamples;
    int recoveries = 0;

    auto run_stream = [&](HWAVEIN cur_hwi, uint32_t) -> bool {
        std::vector<int16_t> ring_storage(kNumRingBuffers * kChunkSamples);
        WAVEHDR hdrs[kNumRingBuffers]{};
        for (int i = 0; i < kNumRingBuffers; ++i) {
            hdrs[i].lpData = (LPSTR)&ring_storage[(size_t)i * kChunkSamples];
            hdrs[i].dwBufferLength = kChunkSamples * sizeof(int16_t);
            waveInPrepareHeader(cur_hwi, &hdrs[i], sizeof(WAVEHDR));
            waveInAddBuffer(cur_hwi, &hdrs[i], sizeof(WAVEHDR));
        }

        waveInStart(cur_hwi);
        HANDLE waits[2] = { hBufEvent, m_stop_capture_event };
        bool driver_error = false;

        while (frames.size() < max_chunks) {
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, 2500);
            if (w == WAIT_OBJECT_0 + 1) {
                // Stop signaled
                break;
            }
            if (w == WAIT_TIMEOUT) {
                LOGE("[!] Timeout aguardando buffer do microfone (driver travado)");
                driver_error = true;
                break;
            }
            for (int i = 0; i < kNumRingBuffers; ++i) {
                if (hdrs[i].dwFlags & WHDR_DONE) {
                    size_t n = hdrs[i].dwBytesRecorded / sizeof(int16_t);
                    if (n > 0) {
                        const int16_t* s = (const int16_t*)hdrs[i].lpData;
                        int rms = compute_rms_pcm16(s, n);
                        float lvl = (float)(rms - 40) / 1000.0f;
                        if (lvl < 0.0f) lvl = 0.0f;
                        if (lvl > 1.0f) lvl = 1.0f;
                        m_level.store(lvl, std::memory_order_relaxed);
                        rms_values.push_back(rms);
                        frames.emplace_back(s, s + n);
                    }
                    hdrs[i].dwFlags &= ~WHDR_DONE;
                    hdrs[i].dwBytesRecorded = 0;
                    if (WaitForSingleObject(m_stop_capture_event, 0) == WAIT_TIMEOUT) {
                        if (waveInAddBuffer(cur_hwi, &hdrs[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                            driver_error = true;
                            break;
                        }
                    }
                }
            }
            if (driver_error) break;
        }

        waveInReset(cur_hwi);
        // Drain any remaining samples captured right before stop
        for (int i = 0; i < kNumRingBuffers; ++i) {
            if ((hdrs[i].dwFlags & WHDR_DONE) && hdrs[i].dwBytesRecorded > 0) {
                size_t n = hdrs[i].dwBytesRecorded / sizeof(int16_t);
                const int16_t* s = (const int16_t*)hdrs[i].lpData;
                int rms = compute_rms_pcm16(s, n);
                rms_values.push_back(rms);
                frames.emplace_back(s, s + n);
            }
            waveInUnprepareHeader(cur_hwi, &hdrs[i], sizeof(WAVEHDR));
        }
        waveInClose(cur_hwi);
        return !driver_error;
    };

    while (true) {
        bool clean = run_stream(hwi, rate);
        hwi = nullptr;
        if (clean || WaitForSingleObject(m_stop_capture_event, 0) == WAIT_OBJECT_0) {
            break;
        }
        if (recoveries >= kMaxMicRecoveries) {
            out.aborted = true;
            out.mic_problem = "falha repetida no driver de audio";
            break;
        }
        ++recoveries;
        LOGI("[!] Tentando recuperar microfone MME (%d/%d)...", recoveries, kMaxMicRecoveries);
        Sleep(150);
        invalidate_cache();
        bool adopted = false;
        ProbeResult pr = resolve_best_device(cfg, &adopted);
        if (pr.valid && pr.rate == rate && open_wave_in(pr.dev_id, pr.rate, &hwi, hBufEvent)) {
            continue;
        }
        // Rate changed or could not reopen at same rate: finish with what we already captured
        break;
    }

    if (hBufEvent) CloseHandle(hBufEvent);
    if (hMmcss && pAvRevert) pAvRevert(hMmcss);
    if (hAvrt) FreeLibrary(hAvrt);

    m_level.store(0.0f, std::memory_order_relaxed);
    m_capturing.store(false, std::memory_order_relaxed);

    if (out.aborted || frames.empty()) {
        out.aborted = true;
        if (on_done) on_done(session_id, std::move(out));
        return;
    }

    SpeechAnalysis sa = analyze_speech(rms_values);
    out.peak = sa.peak;
    out.floor = sa.floor;
    out.has_speech = sa.has_speech;
    out.duration_sec = (double)(rms_values.size() * kChunkSamples) / (double)(rate ? rate : 1);

    if (sa.has_speech) {
        size_t pre = std::max<size_t>(1, (size_t)(0.35 * (double)rate / (double)kChunkSamples));
        size_t post = std::max<size_t>(2, (size_t)(0.60 * (double)rate / (double)kChunkSamples));
        std::vector<int16_t> trimmed = trim_silent_edges(
            frames, rms_values, sa.threshold, pre, post, &out.trim_start, &out.trim_end
        );
        frames.clear();
        frames.shrink_to_fit();
        rms_values.clear();
        rms_values.shrink_to_fit();

        out.trimmed_sec = (double)trimmed.size() / (double)(rate ? rate : 1);
        if (trimmed.size() < 600) {
            out.has_speech = false;
        } else {
            if (rate != 16000) {
                out.pcm16 = resample_pcm16_mono(trimmed.data(), trimmed.size(), rate, 16000);
                out.sample_rate = 16000;
            } else {
                out.pcm16 = std::move(trimmed);
                out.sample_rate = rate;
            }
        }
    } else {
        frames.clear();
        frames.shrink_to_fit();
        rms_values.clear();
        rms_values.shrink_to_fit();
    }

    if (on_done) on_done(session_id, std::move(out));
}

} // namespace audio_engine
