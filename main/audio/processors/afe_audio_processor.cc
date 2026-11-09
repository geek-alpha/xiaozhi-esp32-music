#include "afe_audio_processor.h"
#include <esp_log.h>

#define PROCESSOR_RUNNING 0x01
// reset 必须由持有 afe_data_ 的本任务来做：Stop() 以前在自己线程里直接调
// reset_buffer，而 AudioProcessorTask 此时正卡在 fetch_with_delay 里读同一个对象，
// 两线程同时动 AFE 内部 ringbuffer 与状态索引 → 索引错乱，之后 fetch 永远返回
// ESP_FAIL。fix20.log 实测：第一次 state=speaking（走 Stop）之后 107999ms 起
// "Error code: -1" + "Ringbuffer of AFE is empty" 每 200ms 一条刷到日志末尾，
// 屏幕挂着「聆听中」，用户说话完全没反应。
#define PROCESSOR_RESET   0x02

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
        // 播放到出声的路径延迟：I2S DMA 6×240 帧 @24k ≈ 60ms，再加功放和声学传播，按 70ms 对齐
        ref_.Initialize(codec_->output_sample_rate(), 70);
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
    ref_.Interleave(data.data(), data.size(), feed_buffer_);
    if (ref_.TakeUnderrunSamples() > 0) {
        ref_underrun_us_.store(esp_timer_get_time(), std::memory_order_relaxed);
    }
    afe_iface_->feed(afe_data_, feed_buffer_.data());
}

bool AfeAudioProcessor::EchoRefBroken() const {
    int64_t t = ref_underrun_us_.load(std::memory_order_relaxed);
    // 120ms 盖住 AEC 的帧延迟：某帧喂进去的参考是 0，它的后果要到后面几帧的输出里才显现。
    return t != 0 && (esp_timer_get_time() - t) < 120000;
}

void AfeAudioProcessor::PushReference(const std::vector<int16_t>& pcm, int sample_rate) {
    if (!virtual_ref_) {
        return;
    }
    ref_.Push(pcm, sample_rate);
}

void AfeAudioProcessor::SetEchoRefAlignMs(int ms) {
    if (virtual_ref_) {
        ref_.SetAlignDelayMs(ms);
    }
}

void AfeAudioProcessor::Start() {
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop() {
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);
    if (afe_data_ != nullptr) {
        // 只登记请求，实际 reset 由 AudioProcessorTask 在 fetch 之外执行
        xEventGroupSetBits(event_group_, PROCESSOR_RESET);
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
        // 等「可运行」或「待重置」任一：Stop 之后若用 portMAX_DELAY 只等 RUNNING，
        // 任务会永久睡死，挂上的 reset 也没人执行。
        EventBits_t bits = xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING | PROCESSOR_RESET,
                                               pdFALSE, pdFALSE, portMAX_DELAY);

        // 此刻本任务不在 fetch 中，动 afe_data_ 才是安全的
        if (bits & PROCESSOR_RESET) {
            xEventGroupClearBits(event_group_, PROCESSOR_RESET);
            afe_iface_->reset_buffer(afe_data_);
            is_speaking_ = false;
            output_buffer_.clear();
        }
        if ((bits & PROCESSOR_RUNNING) == 0) {
            continue;
        }

        // 绝不能无限等：Stop() 只是清位 + reset_buffer，唤不醒已经卡在 fetch 里的本任务；
        // Start() 又只置位。任务一旦困在 portMAX_DELAY 就再不出不来，feed 那边继续灌，
        // AFE(FEED) ringbuffer 每秒刷 31 条 full，麦克风数据全丢——屏幕挂着「聆听中」，
        // 用户说话却没反应（wire 日志 watch2.log 实测）。有限超时让每轮都能回到循环顶部
        // 重新检查 PROCESSOR_RUNNING，Stop/Start 才真正生效。
        auto res = afe_iface_->fetch_with_delay(afe_data_, pdMS_TO_TICKS(200));
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
