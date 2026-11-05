#include "audio_service.h"

#include <cmath>

#include <esp_log.h>

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#if CONFIG_USE_AFE_WAKE_WORD
#include "wake_words/afe_wake_word.h"
#elif CONFIG_USE_ESP_WAKE_WORD
#include "wake_words/esp_wake_word.h"
#elif CONFIG_USE_CUSTOM_WAKE_WORD
#include "wake_words/custom_wake_word.h"
#endif

#define TAG "AudioService"

namespace {

int RmsOf(const std::vector<int16_t>& pcm) {
    if (pcm.empty()) {
        return 0;
    }
    int64_t sum = 0;
    for (auto v : pcm) {
        sum += (int64_t)v * v;
    }
    return (int)std::sqrt((double)sum / (double)pcm.size());
}

}  // namespace


AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
}

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}


void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    /* Setup the audio codec */
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_->SetComplexity(0);

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

#if CONFIG_USE_AFE_WAKE_WORD
    wake_word_ = std::make_unique<AfeWakeWord>();
#elif CONFIG_USE_ESP_WAKE_WORD
    wake_word_ = std::make_unique<EspWakeWord>();
#elif CONFIG_USE_CUSTOM_WAKE_WORD
    wake_word_ = std::make_unique<CustomWakeWord>();
#else
    wake_word_ = nullptr;
#endif

    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 1);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 2, this, 3, &audio_output_task_handle_);
#else
    /* Start the audio input task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048, this, 3, &audio_output_task_handle_);
#endif

    /* Start the opus codec task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusCodecTask();
        vTaskDelete(NULL);
    }, "opus_codec", 2048 * 13, this, 2, &opus_codec_task_handle_);
}

void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
    service_stopped_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_encode_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        codec_->EnableInput(true);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate);
        if (!codec_->InputData(data)) {
            return false;
        }
        if (codec_->input_channels() == 2) {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples);
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;
    input_read_fail_count_ = 0;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

void AudioService::AudioInputTask() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the wake word */
        if (bits & AS_EVENT_WAKE_WORD_RUNNING) {
            std::vector<int16_t> data;
            int samples = wake_word_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    wake_word_->Feed(data);
                    continue;
                }
            }
        }

        /* Feed the audio processor */
        if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
            std::vector<int16_t> data;
            int samples = audio_processor_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    audio_processor_->Feed(std::move(data));
                    continue;
                }
            }
        }

        // 读失败绝不能 break：任务一退，之后每轮对话都收不到一个字节，屏幕却照旧显示
        // 「聆听中」，只有重启设备才恢复——用户看到的就是这个。改为退避重试；连续失败
        // 说明 RX 通道进了坏状态，重开一次再试。
        if (++input_read_fail_count_ % 10 == 1) {
            ESP_LOGW(TAG, "Audio input read failed %d times, bits: %lx", input_read_fail_count_, bits);
        }
        if (input_read_fail_count_ % 50 == 0) {
            ESP_LOGW(TAG, "Audio input keeps failing, reopening I2S RX channel");
            codec_->ResetInputChannel();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() { return !audio_playback_queue_.empty() || service_stopped_; });
        if (service_stopped_) {
            break;
        }

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (!codec_->output_enabled()) {
            codec_->EnableOutput(true);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        }
        // 先把这一帧 PCM 交给 AEC 当参考信号（R 通道），再送去喇叭；播放路径延迟在
        // AfeAudioProcessor 里按固定样本数补偿。同时记下电平和时刻，用来判断「最近有没有在出声」。
        if (audio_processor_) {
            audio_processor_->PushReference(task->pcm, codec_->output_sample_rate());
        }
        UpdateEchoReference(task->pcm);
        codec_->OutputData(task->pcm);

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;

#if CONFIG_USE_SERVER_AEC
        /* Record the timestamp for server AEC */
        if (task->timestamp > 0) {
            lock.lock();
            timestamp_queue_.push_back(task->timestamp);
        }
#endif
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_ ||
                (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) ||
                (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_) {
            break;
        }

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = packet->timestamp;

            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            if (opus_decoder_->Decode(std::move(packet->payload), task->pcm)) {
                // Resample if the sample rate is different
                if (opus_decoder_->sample_rate() != codec_->output_sample_rate()) {
                    int target_size = output_resampler_.GetOutputSamples(task->pcm.size());
                    std::vector<int16_t> resampled(target_size);
                    output_resampler_.Process(task->pcm.data(), task->pcm.size(), resampled.data());
                    task->pcm = std::move(resampled);
                }

                lock.lock();
                audio_playback_queue_.push_back(std::move(task));
                audio_queue_cv_.notify_all();
            } else {
                ESP_LOGE(TAG, "Failed to decode audio");
                lock.lock();
            }
            debug_statistics_.decode_count++;
        }
        
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;
            if (!opus_encoder_->Encode(std::move(task->pcm), packet->payload)) {
                ESP_LOGE(TAG, "Failed to encode audio");
                continue;
            }

            if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                {
                    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                    audio_send_queue_.push_back(std::move(packet));
                }
                if (callbacks_.on_send_queue_available) {
                    callbacks_.on_send_queue_available();
                }
            } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                audio_testing_queue_.push_back(std::move(packet));
            }
            debug_statistics_.encode_count++;
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) {
        return;
    }

    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(sample_rate, 1, frame_duration);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decoder_->sample_rate(), codec->output_sample_rate());
        output_resampler_.Configure(opus_decoder_->sample_rate(), codec->output_sample_rate());
    }
}

void AudioService::UpdateEchoReference(const std::vector<int16_t>& pcm) {
    echo_ref_rms_.store(RmsOf(pcm), std::memory_order_relaxed);
    echo_ref_us_.store(esp_timer_get_time(), std::memory_order_relaxed);
}

void AudioService::GateEchoFrame(std::vector<int16_t>& pcm) {
    int64_t now = esp_timer_get_time();
    int32_t ref = echo_ref_rms_.load(std::memory_order_relaxed);
    int64_t ref_us = echo_ref_us_.load(std::memory_order_relaxed);
    bool playing = (ref > 0) && (now - ref_us) <= 300000;

    // 设备端 AEC 已经接管回声：上行不再压静音（压了人声也一起没了），只报播放期的上行
    // 残余电平——AI 说话、没人插话时这个值应该贴近底噪，这是 AEC 真生效的唯一硬证据。
    if (audio_processor_ && audio_processor_->AecActive()) {
        if (playing) {
            aec_up_sum_ += RmsOf(pcm);
            aec_frames_++;
        }
        if (now - gate_log_us_ > 500000) {
            gate_log_us_ = now;
            int32_t avg = aec_frames_ ? (int32_t)(aec_up_sum_ / aec_frames_) : 0;
            ESP_LOGI(TAG, "AEC: playing=%d up_avg=%ld ref=%ld frames=%lu", (int)playing, (long)avg,
                     (long)ref, (unsigned long)aec_frames_);
            aec_up_sum_ = 0;
            aec_frames_ = 0;
        }
        return;
    }
    // 上一版把未确认帧衰减 -30dB（/32）就放上去，实测没用：服务端 VAD 是按神经网络
    // 判语音的，回声哪怕只剩 90 的电平也照样判成「有人在说话」。播放期上行必须压成
    // 真静音，服务端才没有任何东西可误判；打断改由下面的确认窗口在设备端放行。
    constexpr int32_t kFloorRatio = 3;      // 上行要盖过回声底 3 倍
    constexpr int32_t kAbsOpenRms = 900;    // 同时绝对电平得够，挡掉底噪
    // 实测：本机播放期上行 RMS 中位数 26694、峰值 32230（S16 满幅 32767）。
    // 之前封顶 2500 把门限算成 7500，回声十倍于门限，每 300ms 就确认一次「插话」。
    // 现在封顶抬到满幅：floor 跟着真实回声走，门限 >> 回声，播放期一律不放行。
    // 这不是调参能解决的问题——人声在麦克风上同样削波到 32000，和回声无法用电平分开，
    // 要恢复打断必须上设备端 AEC（喂播放 PCM 当 R 通道），那是下一步的事。
    constexpr int32_t kFloorCeil = 32000;
    constexpr int64_t kTailUs = 300000;     // 播放停 300ms 后回声尾巴已散，不再门控
    constexpr int kConfirmFrames = 5;       // 300ms 连续超阈值才算真插话
    constexpr int kSettleFrames = 8;        // 每次开口头 480ms 只标定回声底，不放行
    constexpr int64_t kHoldUs = 400000;     // 确认插话后 400ms 全放行，让人声完整上行

    // 确认插话之后的保持窗口：整帧放行
    if (now < gate_hold_until_us_) {
        return;
    }

    if (!playing) {
        // 喇叭静着，没有回声风险，原样上行
        gate_playing_ = false;
        gate_echo_floor_ = 0;
        gate_settle_frames_ = 0;
        gate_confirm_frames_ = 0;
        return;
    }

    if (!gate_playing_) {
        // 刚开口：重开一轮标定，回声底丢掉重学
        gate_playing_ = true;
        gate_echo_floor_ = 0;
        gate_settle_frames_ = kSettleFrames;
        gate_confirm_frames_ = 0;
    }

    int up = RmsOf(pcm);

    if (gate_settle_frames_ > 0) {
        // 标定期：取峰值当回声底（回音是起伏的，峰值更安全），一律压成静音
        gate_echo_floor_ = std::min<int32_t>(std::max<int32_t>(gate_echo_floor_, up), kFloorCeil);
        gate_settle_frames_--;
        gate_confirm_frames_ = 0;
        std::fill(pcm.begin(), pcm.end(), 0);
        if (now - gate_log_us_ > 200000) {
            gate_log_us_ = now;
            ESP_LOGI(TAG, "Echo gate: up=%d floor=%ld settle %d ref=%ld", up,
                     (long)gate_echo_floor_, gate_settle_frames_, (long)ref);
        }
        return;
    }

    // 标定后不跟着往上跑，只在更低时缓慢下调（回声底可能被标定期高估）
    if (up < gate_echo_floor_) {
        gate_echo_floor_ = (int32_t)(0.5f * gate_echo_floor_ + 0.5f * up);
    }

    int32_t open_thr = std::max<int32_t>((int32_t)(gate_echo_floor_ * kFloorRatio), kAbsOpenRms);
    bool candidate = (up >= open_thr);
    gate_confirm_frames_ = candidate ? gate_confirm_frames_ + 1 : 0;

    bool open = gate_confirm_frames_ >= kConfirmFrames;
    if (open) {
        gate_hold_until_us_ = now + kHoldUs;   // 确认插话：放行并保持一段
        gate_confirm_frames_ = 0;
        gate_open_count_++;
    } else {
        std::fill(pcm.begin(), pcm.end(), 0);
        gate_mute_count_++;
    }

    if (now - gate_log_us_ > 200000) {
        gate_log_us_ = now;
        ESP_LOGI(TAG, "Echo gate: up=%d floor=%ld thr=%ld ref=%ld %s muted=%lu opened=%lu", up,
                 (long)gate_echo_floor_, (long)open_thr, (long)ref, open ? "OPEN" : "mute",
                 (unsigned long)gate_mute_count_, (unsigned long)gate_open_count_);
    }
}


void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    if (type == kAudioTaskTypeEncodeToSendQueue) {
        GateEchoFrame(pcm);
    }

    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);
    
    /* Push the task to the encode queue */
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);

    /* If the task is to send queue, we need to set the timestamp */
    if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
        if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
            task->timestamp = timestamp_queue_.front();
        } else {
            ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp", timestamp_queue_.size());
        }
        timestamp_queue_.pop_front();
    }

    audio_queue_cv_.wait(lock, [this]() { return audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE; });
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this]() { return audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE; });
        } else {
            return false;
        }
    }
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (wake_word_) {
        wake_word_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    return wake_word_->GetLastDetectedWakeWord();
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (wake_word_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    if (!wake_word_) {
        return;
    }

    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                return;
            }
            wake_word_initialized_ = true;
        }
        wake_word_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        wake_word_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!audio_processor_initialized_) {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS);
            audio_processor_initialized_ = true;
        }

        /* We should make sure no audio is playing */
        ResetDecoder();
        audio_input_need_warmup_ = true;
        audio_processor_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        audio_processor_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    if (!audio_processor_initialized_) {
        audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS);
        audio_processor_initialized_ = true;
    }

    audio_processor_->EnableDeviceAec(enable);
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::PlaySound(const std::string_view& sound) {
    const char* data = sound.data();
    size_t size = sound.size();
    for (const char* p = data; p < data + size; ) {
        auto p3 = (BinaryProtocol3*)p;
        p += sizeof(BinaryProtocol3);

        auto payload_size = ntohs(p3->payload_size);
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = 16000;
        packet->frame_duration = 60;
        packet->payload.resize(payload_size);
        memcpy(packet->payload.data(), p3->payload, payload_size);
        p += payload_size;

        PushPacketToDecodeQueue(std::move(packet), true);
    }
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && audio_decode_queue_.empty() && audio_playback_queue_.empty() && audio_testing_queue_.empty();
}

void AudioService::ResetDecoder() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    opus_decoder_->ResetState();
    timestamp_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        codec_->EnableOutput(false);
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

void AudioService::UpdateOutputTimestamp() {
    last_output_time_ = std::chrono::steady_clock::now();
}
