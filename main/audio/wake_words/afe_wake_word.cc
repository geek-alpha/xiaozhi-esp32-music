#include "afe_wake_word.h"
#include "audio_service.h"

#include <esp_log.h>
#include <sstream>
#include <cstring>

#define DETECTION_RUNNING_EVENT 1

#define TAG "AfeWakeWord"
// 唤醒词走虚拟回声参考：开启后 AFE 格式变 MR、AEC 打开。2.0.30 里唤醒是 M 单通道、无 AEC，
// 打开这版后待机喊「你好小智」完全不应，先关掉定位原因，别带进主干。
static constexpr bool kWakeVirtualEchoRef = false;

AfeWakeWord::AfeWakeWord()
    : afe_data_(nullptr),
      wake_word_pcm_(),
      wake_word_opus_() {

    event_group_ = xEventGroupCreate();
}

AfeWakeWord::~AfeWakeWord() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }

    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }

    if (models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }

    vEventGroupDelete(event_group_);
}

bool AfeWakeWord::Initialize(AudioCodec* codec) {
    codec_ = codec;
    // mic 只算硬件真通道：虚拟参考是额外加的一路，不能从 input_channels() 里扣
    int mic_num = codec_->input_channels() - (codec_->input_reference() ? 1 : 0);
    int ref_num = codec_->input_reference() ? 1 : 0;
    if (ref_num == 0 && kWakeVirtualEchoRef) {
        // 单工板（NoAudioCodecSimplex）硬件只接了 mic。不造参考的话 AFE 的 AEC 是关的，
        // AI 说话期间喇叭里的 TTS 直接灌进 WakeNet，喊唤醒词永远喊不应，
        // 彻底没有语音打断——打断只能靠唤醒词，而它是唯一在说话期跑着的引擎。
        ref_num = 1;
        virtual_ref_ = true;
        ref_.Initialize(codec_->output_sample_rate(), 70);
    }

    models_ = esp_srmodel_init("model");
    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }
    for (int i = 0; i < models_->num; i++) {
        ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
        if (strstr(models_->model_name[i], ESP_WN_PREFIX) != NULL) {
            wakenet_model_ = models_->model_name[i];
            auto words = esp_srmodel_get_wake_words(models_, wakenet_model_);
            // split by ";" to get all wake words
            std::stringstream ss(words);
            std::string word;
            while (std::getline(ss, word, ';')) {
                wake_words_.push_back(word);
            }
        }
    }

    std::string input_format;
    for (int i = 0; i < mic_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), models_, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->aec_init = codec_->input_reference() || virtual_ref_;
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    // wakenet9 自带阈值 0.631，轻声/远场喊多次唤不醒；DET_MODE_95 走更激进的召回档
    afe_config->wakenet_mode = DET_MODE_95;

    afe_config_print(afe_config);
    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);

    // 阈值再压一档 0.631 -> 0.5：近距离正常说话稳定触发，仍留抗误唤醒余量
    int threshold_ret = afe_iface_->set_wakenet_threshold(afe_data_, 1, 0.5);
    ESP_LOGI(TAG, "WakeNet det threshold -> 0.5, ret=%d", threshold_ret);

    xTaskCreate([](void* arg) {
        auto this_ = (AfeWakeWord*)arg;
        this_->AudioDetectionTask();
        vTaskDelete(NULL);
    }, "audio_detection", 4096, this, 3, nullptr);

    return true;
}

void AfeWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void AfeWakeWord::Start() {
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void AfeWakeWord::Stop() {
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
}

void AfeWakeWord::Feed(const std::vector<int16_t>& data) {
    if (afe_data_ == nullptr) {
        return;
    }
    if (!virtual_ref_) {
        afe_iface_->feed(afe_data_, data.data());
        return;
    }
    // 单通道 mic + 虚拟参考交错成 [M,R,M,R...] 后再喂
    ref_.Interleave(data.data(), data.size(), feed_buffer_);
    afe_iface_->feed(afe_data_, feed_buffer_.data());
}

void AfeWakeWord::PushReference(const std::vector<int16_t>& pcm, int sample_rate) {
    if (!virtual_ref_) {
        return;
    }
    ref_.Push(pcm, sample_rate);
}

size_t AfeWakeWord::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
}

void AfeWakeWord::AudioDetectionTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio detection task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, DETECTION_RUNNING_EVENT, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            continue;;
        }

        // 唤醒词数据丢了只是这次识别少一截，但异常没人接就是整机重启，所以兜住
        try {
            StoreWakeWordData(res->data, res->data_size / sizeof(int16_t));
        } catch (const std::bad_alloc&) {
            ESP_LOGW(TAG, "StoreWakeWordData: out of memory, dropped one chunk");
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            Stop();
            last_detected_wake_word_ = wake_words_[res->wakenet_model_index - 1];

            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(last_detected_wake_word_);
            }
        }
    }
}

void AfeWakeWord::StoreWakeWordData(const int16_t* data, size_t samples) {
    // 窗口 2 秒、每块 30ms = 66 块。原来是每 30ms 新建一个 vector，而这块板子
    // 播放音乐时 PSRAM 只剩 380KB，反复 malloc/free 1KB 把堆打到碎片，
    // AudioDetectionTask 上没人接 bad_alloc —— terminate → abort（真机 113 秒实测）。
    // 改成槽位复用：块数到上限后把最老的一块搬到末尾接着用，稳态零分配。
    constexpr size_t kMaxChunks = 2000 / 30;
    if (wake_word_pcm_.size() < kMaxChunks) {
        wake_word_pcm_.emplace_back(samples, 0);
    } else {
        wake_word_pcm_.push_back(std::move(wake_word_pcm_.front()));
        wake_word_pcm_.pop_front();
    }
    auto& chunk = wake_word_pcm_.back();
    if (chunk.size() != samples) {
        chunk.resize(samples);
    }
    memcpy(chunk.data(), data, samples * sizeof(int16_t));
}

void AfeWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 7;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (AfeWakeWord*)arg;
        {
            auto start_time = esp_timer_get_time();
            auto encoder = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
            encoder->SetComplexity(0); // 0 is the fastest

            int packets = 0;
            for (auto& pcm: this_->wake_word_pcm_) {
                encoder->Encode(std::move(pcm), [this_](std::vector<uint8_t>&& opus) {
                    std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                    this_->wake_word_opus_.emplace_back(std::move(opus));
                    this_->wake_word_cv_.notify_all();
                });
                packets++;
            }
            this_->wake_word_pcm_.clear();

            auto end_time = esp_timer_get_time();
            ESP_LOGI(TAG, "Encode wake word opus %d packets in %ld ms", packets, (long)((end_time - start_time) / 1000));

            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();
        }
        vTaskDelete(NULL);
    }, "encode_wake_word", stack_size, this, 2, wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool AfeWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() {
        return !wake_word_opus_.empty();
    });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
