#ifndef AFE_AUDIO_PROCESSOR_H
#define AFE_AUDIO_PROCESSOR_H

#include <esp_afe_sr_models.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

#include <opus_resampler.h>

#include "audio_processor.h"
#include "audio_codec.h"

class AfeAudioProcessor : public AudioProcessor {
public:
    AfeAudioProcessor();
    ~AfeAudioProcessor();

    void Initialize(AudioCodec* codec, int frame_duration_ms) override;
    void Feed(std::vector<int16_t>&& data) override;
    void Start() override;
    void Stop() override;
    bool IsRunning() override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    void EnableDeviceAec(bool enable) override;
    void PushReference(const std::vector<int16_t>& pcm, int sample_rate) override;
    bool AecActive() const override { return aec_active_; }

private:
    EventGroupHandle_t event_group_ = nullptr;
    esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    std::function<void(std::vector<int16_t>&& data)> output_callback_;
    std::function<void(bool speaking)> vad_state_change_callback_;
    AudioCodec* codec_ = nullptr;
    int frame_samples_ = 0;
    bool is_speaking_ = false;
    std::vector<int16_t> output_buffer_;

    // 虚拟参考通道：codec 是单工时（input_reference()==false，硬件只接了 mic），
    // AFE 的 AEC 却只认交错的 R 通道。所以自己造一路——上层把即将播放的 PCM 推下来，
    // 重采样到 16k 存环形缓冲，喂 AFE 时按延迟窗口把 [M,R] 交错起来。
    bool virtual_ref_ = false;
    bool aec_active_ = false;
    OpusResampler ref_resampler_;
    std::mutex ref_mutex_;
    std::vector<int16_t> ref_ring_;   // 16k 单通道环形缓冲
    std::atomic<int64_t> stream_pos_{0};  // mic 已采集的 16k 样本数，播放参考按这条时间轴定位
    int64_t last_push_end_ = 0;       // 最后一次推参考写到的时间轴位置（受 ref_mutex_ 保护）
    int ref_delay_samples_ = 0;       // OutputData 到 mic 拾到声音的路径延迟
    std::vector<int16_t> feed_buffer_;
    std::vector<int16_t> ref_resample_buffer_;

    void AudioProcessorTask();
};

#endif 
