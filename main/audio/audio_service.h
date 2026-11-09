#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <memory>
#include <deque>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include <opus_encoder.h>
#include <opus_decoder.h>
#include <opus_resampler.h>

#include "audio_codec.h"
#include "audio_processor.h"
#include "processors/audio_debugger.h"
#include "wake_word.h"
#include "protocol.h"


/*
 * There are two types of audio data flow:
 * 1. (MIC) -> [Processors] -> {Encode Queue} -> [Opus Encoder] -> {Send Queue} -> (Server)
 * 2. (Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue} -> (Speaker)
 *
 * We use one task for MIC / Speaker / Processors, and one task for Opus Encoder / Opus Decoder.
 * 
 * Decode Queue and Send Queue are the main queues, because Opus packets are quite smaller than PCM packets.
 * 
 */

#define OPUS_FRAME_DURATION_MS 60
#define MAX_ENCODE_TASKS_IN_QUEUE 2
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
#define MAX_DECODE_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define AUDIO_TESTING_MAX_DURATION_MS 10000
#define MAX_TIMESTAMPS_IN_QUEUE 3

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000

// TTS 说话期间的打断门限。单看 AFE 的 VAD 太灵敏：环境噪声、键盘声、开门声都会被人声
// 模型判成 speech，AI 说半句就被自家杂音打断。人说话有两个可用特征——够响、够久：
// AEC 之后残余能量要过 RMS 门限，而且得连续维持住（说话不会只响一两帧就停）。
// 口径是 16bit 满幅 32767 的线性 RMS。
// 门限只用 AEC 之后的 rms，固定值，不跟播放参考 ref 走。_cap3.log 实测（19 帧）：
// 无人说话时残余 20~1304（pre 高达 14172 的纯回声 AEC 也只漏 318），人声 2025~7186；
// 两堆跟 ref 无单调关系（ref=4348 时残余 146，ref=2158 时残余 318）。
// 上一版按 1.2x ref + 2500 算，ref=4400 就把门限推到 7780、高过人声，现场表现就是
// 「怎么喊它都不停」。2500 坐在两堆中间。参考断流的帧另由 EchoRefBroken() 整帧跳过。
// TTS 期间语音打断总开关。置 0 = 只有唤醒词能开新一轮，AI 说话时插话一律不响应。
// 关掉的理由不是判据不准，是它跟「听得清」直接冲突：AI 说话期间上行按设计被 muted
// 丢掉，用户抢话时头几个字永远进不了 ASR，识别出来全是残句；关掉后用户会等 AI 说完
// 再讲，句子完整，反而好认。要恢复打断改成 1 即可，判据逻辑都还在。
#define BARGE_IN_ENABLE 0
#define BARGE_IN_THR_ABS 2500
// 300ms 严格连续（5 帧）：键盘敲击、关门这类瞬态撑不到这个长度。
#define BARGE_IN_HOLD_MS 300
// 起播头 500ms 只学本底、不判打断（AEC 收敛 + 首帧爆音都在这一段）。
#define BARGE_IN_SETTLE_MS 500
// 虚拟参考的对齐量（ms）。真实路径延迟算不出来：I2S TX DMA 6x240 帧、播放队列、
// RX DMA 积压、功放与声学传播，差上百毫秒很正常，对齐差一截 AEC 就等于没开。
#define BARGE_ECHO_REF_ALIGN_MS 70
// 允许 1 帧（≈60ms）空隙：中文连读字与字之间能量会掉一瞬，硬清零会让累计永远凑不满


#define AS_EVENT_AUDIO_TESTING_RUNNING      (1 << 0)
#define AS_EVENT_WAKE_WORD_RUNNING          (1 << 1)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING    (1 << 2)
#define AS_EVENT_PLAYBACK_NOT_EMPTY         (1 << 3)

struct AudioServiceCallbacks {
    std::function<void(void)> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
    std::function<void(void)> on_audio_testing_queue_full;
};


enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
};

struct AudioTask {
    AudioTaskType type;
    std::vector<int16_t> pcm;
    uint32_t timestamp;
};

struct DebugStatistics {
    uint32_t input_count = 0;
    uint32_t decode_count = 0;
    uint32_t encode_count = 0;
    uint32_t playback_count = 0;
};

class AudioService {
public:
    AudioService();
    ~AudioService();

    void Initialize(AudioCodec* codec);
    void Start();
    void Stop();
    void EncodeWakeWord();
    std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
    const std::string& GetLastWakeWord() const;
    bool IsVoiceDetected() const { return voice_detected_; }
    // 打断判定：不看单帧 VAD，只认「够响且持续够久」的声音，见 BARGE_IN_THR_MIN
    bool IsBargeInConfirmed() const;
    int GetBargeInRms() const { return barge_in_rms_.load(std::memory_order_relaxed); }
    // 这次连续段里的峰值：瞬时 rms 会在回落帧被打成 98，拿它看日志只会误导判断
    int GetBargeInPeakRms() const { return barge_in_peak_rms_.load(std::memory_order_relaxed); }
    int GetBargeInHeldMs() const { return barge_in_run_ms_.load(std::memory_order_relaxed); }
    // 起播时清零：上一轮残留的计数不能带进新一轮
    void ResetBargeIn();
    bool IsIdle();
    bool IsWakeWordRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING; }
    bool IsAudioProcessorRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING; }

    void EnableWakeWordDetection(bool enable);
    void EnableVoiceProcessing(bool enable);
    // 静默监听：处理器照常跑（AEC 和 VAD 都得工作），但拾到的人声一帧都不上行。
    // TTS 说话期间靠它——本地要拿 VAD 判断用户开没开口，音频发给服务端只会被当成新一轮输入。
    void SetUplinkMuted(bool muted) { uplink_muted_.store(muted, std::memory_order_relaxed); }
    bool IsUplinkMuted() const { return uplink_muted_.load(std::memory_order_relaxed); }
    void EnableAudioTesting(bool enable);
    void EnableDeviceAec(bool enable);

    void SetCallbacks(AudioServiceCallbacks& callbacks);

    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
    void PlaySound(const std::string_view& sound);
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    void ResetDecoder();
    
    void UpdateOutputTimestamp();

private:
    AudioCodec* codec_ = nullptr;
    AudioServiceCallbacks callbacks_;
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<WakeWord> wake_word_;
    std::unique_ptr<AudioDebugger> audio_debugger_;
    std::unique_ptr<OpusEncoderWrapper> opus_encoder_;
    std::unique_ptr<OpusDecoderWrapper> opus_decoder_;
    OpusResampler input_resampler_;
    OpusResampler reference_resampler_;
    OpusResampler output_resampler_;
    DebugStatistics debug_statistics_;
    int input_read_fail_count_ = 0;  // 连续读失败计数，成功后清零

    EventGroupHandle_t event_group_;

    // Audio encode / decode
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
    // For server AEC
    std::deque<uint32_t> timestamp_queue_;

    bool wake_word_initialized_ = false;
    bool audio_processor_initialized_ = false;
    bool voice_detected_ = false;
    std::atomic<bool> uplink_muted_{false};

    // 回声门控：单工编解码（NoAudioCodecSimplex）没有回采参考通道，AFE 的 AEC 一直是关的，
    // 喇叭出声时麦克风拾到的就是自己的声音；服务端把回声当用户插话，AI 刚开口就被自己打断。
    // 播放数字电平和声学泄漏差几个数量级，比值判据不可用；改为每次开口先用前几帧标定
    // 「播放中的上行基线」（回声底），之后的帧要明显盖过这个基线才当插话放行。
    void UpdateBargeInEnergy(const std::vector<int16_t>& pcm);
    std::atomic<int> barge_in_rms_{0};
    std::atomic<int> barge_in_peak_rms_{0};
    std::atomic<int> barge_in_run_ms_{0};
    std::atomic<int> barge_in_gap_frames_{0};
    // AEC 之前的原始 mic 电平：和后面的 barge rms 相减才是「回声被消掉多少」的唯一凭据
    std::atomic<int> barge_in_pre_rms_{0};
    // AEC 后环境本底（跨轮保留，慢速平滑）。旧版每轮清零再取 settle 期最大值，
    // 一次起播爆音就把门限顶到 24k：短句被自己打断、长句死活打不断，根都在这里。
    std::atomic<int> barge_echo_floor_{0};
    int barge_settle_ms_left_ = 0;
    int64_t barge_in_log_us_ = 0;
    void UpdateEchoReference(const std::vector<int16_t>& pcm);
    void GateEchoFrame(std::vector<int16_t>& pcm);
    // 非播放期上行改用 AEC 之前的原始 mic 帧。这块单工板没有硬件回采通道，AEC 的参考是
    // 软件按 70ms 对齐造出来的；参考跟真实回声对不上时残余抑制会连人声一起切掉——
    // _cap3.log 里成批出现 pre=13672 rms=208、pre=20289 rms=430，前后差 40dB 以上，
    // 稍微站远一点的人声正好落在这条被切掉的区间，服务端根本拿不到声音。
    // 喇叭静着时没有回声要消，原始信号最保真；播放期仍然走 AEC 后。
    // 存原始样本而不是帧：AFE 的 feed_chunksize 和 fetch_chunksize 本来就不相等，
    // 按「帧数相等」取对齐永远匹配不上（上一版就是这样，日志里 src 一直是 aec）。
    // 环形按样本写、按样本取最新的一段，size 天然吻合。
    void StorePreSamples(const std::vector<int16_t>& pcm);
    bool TakeLatestPreSamples(size_t n, std::vector<int16_t>& out);
    std::vector<int16_t> pre_ring_ = std::vector<int16_t>(8192, 0);
    size_t pre_ring_pos_ = 0;
    size_t pre_ring_filled_ = 0;
    std::mutex pre_frames_mutex_;
    // 上行自适应增益（Q8 定点，256 = 1.0x）。固定倍数两头不讨好：近讲削顶、远讲照旧弱。
    // 按帧 RMS 追到 ASR 舒服的区间，只抬不压，帧间平滑，软拐点代替硬削。
    void ApplyUplinkGain(std::vector<int16_t>& pcm);
    std::atomic<int> uplink_gain_q8_{256};
    int64_t uplink_log_us_ = 0;
    std::atomic<int32_t> echo_ref_rms_{0};
    std::atomic<int64_t> echo_ref_us_{0};
    bool gate_playing_ = false;
    int32_t gate_echo_floor_ = 0;     // 播放期间的上行基线（回声电平），每次开口重新标定
    int gate_settle_frames_ = 0;
    int gate_confirm_frames_ = 0;
    int64_t gate_hold_until_us_ = 0;  // 确认插话后短时间内全放行，让人声完整上行
    int64_t gate_log_us_ = 0;
    uint32_t gate_mute_count_ = 0;    // 播放期被压成静音的帧数
    uint32_t gate_open_count_ = 0;    // 确认成插话、放行的次数
    int64_t aec_up_sum_ = 0;          // 设备端 AEC 生效时，播放期上行 RMS 的累计（诊断用）
    uint32_t aec_frames_ = 0;

    bool service_stopped_ = true;
    bool audio_input_need_warmup_ = false;
    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    void AudioInputTask();
    void AudioOutputTask();
    void OpusCodecTask();
    void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm);
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckAndUpdateAudioPowerState();
};

#endif
