#pragma once

#include "common.h"
#include <mmsystem.h>
#include <atomic>
#include <functional>

namespace audio_engine {

constexpr uint32_t kChunkSamples = 1024;
constexpr size_t kMaxRecordSeconds = 15 * 60;
constexpr int kMaxMicRecoveries = 3;
constexpr int kNumRingBuffers = 8;

constexpr int kSpeechThresholdFallback = 120;
constexpr int kSpeechAbsMinRms = 300;
constexpr double kSpeechNoiseRatio = 1.6;
constexpr int kSpeechMinStepRms = 1200;
constexpr int kSpeechThresholdMax = 4000;
constexpr double kNoiseFloorPercentile = 0.20;

struct SpeechAnalysis {
    bool has_speech = false;
    int peak = 0;
    int floor = 0;
    int threshold = kSpeechThresholdFallback;
    int chunks_above = 0;
};

struct CapturedSession {
    std::vector<int16_t> pcm16;
    uint32_t sample_rate = 16000;
    double duration_sec = 0.0;
    double trimmed_sec = 0.0;
    int peak = 0;
    int floor = 0;
    size_t trim_start = 0;
    size_t trim_end = 0;
    bool has_speech = false;
    bool aborted = false;
    std::string mic_problem;
    std::string resolved_mic_name;
    bool adopted_new_mic = false;
};

int compute_rms_pcm16(const int16_t* samples, size_t count);

SpeechAnalysis analyze_speech(const std::vector<int>& rms_values);

std::vector<int16_t> trim_silent_edges(
    const std::vector<std::vector<int16_t>>& frames,
    const std::vector<int>& rms_values,
    int threshold,
    size_t pre_chunks,
    size_t post_chunks,
    size_t* out_start = nullptr,
    size_t* out_end = nullptr
);

std::vector<int16_t> resample_pcm16_mono(
    const int16_t* input,
    size_t count,
    uint32_t in_rate,
    uint32_t out_rate
);

std::vector<uint8_t> build_wav_bytes(const int16_t* pcm, size_t sample_count, uint32_t sample_rate);
std::vector<uint8_t> build_flac_verbatim_bytes(const int16_t* pcm, size_t sample_count, uint32_t sample_rate);

std::vector<MicDeviceInfo> enumerate_mme_microphones();

class AudioRecorder {
public:
    using DoneCallback = std::function<void(uint64_t session_id, CapturedSession&&)>;
    using StatusCallback = std::function<void(uint64_t session_id, const char* status_utf8)>;

    AudioRecorder();
    ~AudioRecorder();

    void start_thread();
    void stop_thread();

    void request_prewarm(const AppSettings& settings);
    void invalidate_cache();

    bool start_capture(
        uint64_t session_id,
        const AppSettings& settings,
        DoneCallback on_done,
        StatusCallback on_status
    );
    void stop_capture();

    float current_level() const { return m_level.load(std::memory_order_relaxed); }
    bool is_capturing() const { return m_capturing.load(std::memory_order_relaxed); }

private:
    enum class CmdType { None, Prewarm, Capture };

    struct ProbeResult {
        bool valid = false;
        bool healthy = false;
        bool opened = false;
        int peak = 0;
        UINT dev_id = WAVE_MAPPER;
        uint32_t rate = 44100;
        std::string name;
        std::string problem;
    };

    static DWORD WINAPI thread_proc(LPVOID param);
    void run_loop();
    void do_prewarm(const AppSettings& cfg);
    void do_capture(uint64_t session_id, const AppSettings& cfg, DoneCallback on_done, StatusCallback on_status);

    ProbeResult probe_device(UINT dev_id, const std::string& dev_name, uint32_t rate, int chunks = 8);
    ProbeResult resolve_best_device(const AppSettings& cfg, bool* out_used_configured);

    bool open_wave_in(UINT dev_id, uint32_t rate, HWAVEIN* out_hwi, HANDLE hEvent);

    CRITICAL_SECTION m_cs{};
    HANDLE m_thread = nullptr;
    HANDLE m_cmd_event = nullptr;
    HANDLE m_stop_capture_event = nullptr;
    HANDLE m_shutdown_event = nullptr;

    CmdType m_pending_cmd = CmdType::None;
    AppSettings m_pending_settings;
    uint64_t m_pending_session = 0;
    DoneCallback m_pending_done;
    StatusCallback m_pending_status;

    bool m_has_cached = false;
    UINT m_cached_dev_id = WAVE_MAPPER;
    uint32_t m_cached_rate = 44100;
    std::string m_cached_name;
    UINT m_last_dev_count = 0;

    std::atomic<bool> m_capturing{false};
    std::atomic<bool> m_prewarm_busy{false};
    std::atomic<float> m_level{0.0f};
};

} // namespace audio_engine
