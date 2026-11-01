#include "afe_audio_processor.h"
#include <esp_log.h>

#define PROCESSOR_RUNNING 0x01

#define TAG "AfeAudioProcessor"

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

void AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    // 单工编解码（NoAudioCodecSimplex）硬件只引了 mic，input_reference() 恒为 false，
    // 而 AFE 的 AEC 只认格式串里的 R 通道。这里软件造一路参考通道配套。
    int mic_num = codec_->input_channels() - (codec_->input_reference() ? 1 : 0);
    int ref_num = codec_->input_reference() ? 1 : 0;
    if (ref_num == 0) {
        ref_num = 1;
        virtual_ref_ = true;
        if (codec_->output_sample_rate() != 16000) {
            ref_resampler_.Configure(codec_->output_sample_rate(), 16000);
        }
        // 环容量只要盖住「延迟 + 一个 feed 块」，4096 样本 = 8KB
        ref_ring_.assign(4096, 0);
        // 播放到出声的路径延迟：I2S DMA 6×240 帧 @24k ≈ 60ms，再加功放和声学传播，按 70ms 对齐
        ref_delay_samples_ = 16000 * 70 / 1000;
    }

    std::string input_format;
    for (int i = 0; i < mic_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }

    srmodel_list_t *models = esp_srmodel_init("model");
    char* ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    char* vad_model_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }

    if (ns_model_name != nullptr) {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    } else {
        afe_config->ns_init = false;
    }

    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

#ifdef CONFIG_USE_DEVICE_AEC
    afe_config->aec_init = true;
    afe_config->vad_init = false;
    aec_active_ = ref_num > 0;
#else
    // 有参考通道（虚拟的也算）就能开 AEC；VAD 照样留着，用来判断用户是不是在说话
    afe_config->aec_init = virtual_ref_;
    afe_config->vad_init = true;
    aec_active_ = virtual_ref_;
#endif

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);
    
    xTaskCreate([](void* arg) {
        auto this_ = (AfeAudioProcessor*)arg;
        this_->AudioProcessorTask();
        vTaskDelete(NULL);
    }, "audio_communication", 4096, this, 3, NULL);
}

AfeAudioProcessor::~AfeAudioProcessor() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    vEventGroupDelete(event_group_);
}

size_t AfeAudioProcessor::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (afe_data_ == nullptr) {
        return;
    }
    if (!virtual_ref_) {
        afe_iface_->feed(afe_data_, data.data());
        return;
    }

    // mic 一路 + 参考一路，交错成 [M,R,M,R...]。参考信号按「mic 采样时间轴」定位：
    // 每个播放样本在 PushReference 里被记在「它将被 mic 拾到的那个时刻」，这里只需
    // 取同一时刻，对齐自动成立——不必逐轮猜延迟，也不会因为播放断断续续就错位。
    size_t n = data.size();
    int64_t pos = stream_pos_.load(std::memory_order_relaxed);
    feed_buffer_.resize(n * 2);
    {
        std::lock_guard<std::mutex> lock(ref_mutex_);
        int64_t cap = (int64_t)ref_ring_.size();
        int64_t valid_lo = last_push_end_ - cap + 1;
        for (size_t i = 0; i < n; i++) {
            int64_t idx = pos + (int64_t)i;
            feed_buffer_[2 * i] = data[i];
            feed_buffer_[2 * i + 1] =
                (idx >= 0 && idx >= valid_lo && idx <= last_push_end_) ? ref_ring_[idx % cap] : 0;
        }
    }
    afe_iface_->feed(afe_data_, feed_buffer_.data());
    stream_pos_.store(pos + (int64_t)n, std::memory_order_relaxed);
}

void AfeAudioProcessor::PushReference(const std::vector<int16_t>& pcm, int sample_rate) {
    if (!virtual_ref_ || pcm.empty()) {
        return;
    }
    const int16_t* src = pcm.data();
    size_t n = pcm.size();
    if (sample_rate != 16000) {
        ref_resample_buffer_.resize(ref_resampler_.GetOutputSamples(n));
        ref_resampler_.Process(pcm.data(), n, ref_resample_buffer_.data());
        src = ref_resample_buffer_.data();
        n = ref_resample_buffer_.size();
    }
    if (n > ref_ring_.size()) {
        src += n - ref_ring_.size();
        n = ref_ring_.size();
    }

    // 这批样本马上送 I2S，会在 ref_delay_samples_ 之后被麦克风拾到，所以记在时间轴的
    // 「当前位置 + 延迟」处；等 mic 走到那里，Feed 正好读到对齐的参考。
    std::lock_guard<std::mutex> lock(ref_mutex_);
    int64_t cap = (int64_t)ref_ring_.size();
    int64_t base = stream_pos_.load(std::memory_order_relaxed) + ref_delay_samples_;
    for (size_t i = 0; i < n && (int64_t)i < cap; i++) {
        ref_ring_[(base + (int64_t)i) % cap] = src[i];
    }
    last_push_end_ = std::max(last_push_end_, base + (int64_t)n);
}

void AfeAudioProcessor::Start() {
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop() {
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
}

bool AfeAudioProcessor::IsRunning() {
    return xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING;
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

void AfeAudioProcessor::AudioProcessorTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            if (res != nullptr) {
                ESP_LOGI(TAG, "Error code: %d", res->ret_value);
            }
            continue;
        }

        // VAD state change
        if (vad_state_change_callback_) {
            if (res->vad_state == VAD_SPEECH && !is_speaking_) {
                is_speaking_ = true;
                vad_state_change_callback_(true);
            } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
                is_speaking_ = false;
                vad_state_change_callback_(false);
            }
        }

        if (output_callback_) {
            size_t samples = res->data_size / sizeof(int16_t);
            
            // Add data to buffer
            output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);
            
            // Output complete frames when buffer has enough data
            while (output_buffer_.size() >= frame_samples_) {
                if (output_buffer_.size() == frame_samples_) {
                    // If buffer size equals frame size, move the entire buffer
                    output_callback_(std::move(output_buffer_));
                    output_buffer_.clear();
                    output_buffer_.reserve(frame_samples_);
                } else {
                    // If buffer size exceeds frame size, copy one frame and remove it
                    output_callback_(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
                    output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
                }
            }
        }
    }
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    if (enable) {
#if CONFIG_USE_DEVICE_AEC
        afe_iface_->disable_vad(afe_data_);
        afe_iface_->enable_aec(afe_data_);
#else
        ESP_LOGE(TAG, "Device AEC is not supported");
#endif
    } else {
        afe_iface_->disable_aec(afe_data_);
        afe_iface_->enable_vad(afe_data_);
    }
}
