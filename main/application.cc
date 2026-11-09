#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "font_awesome_symbols.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "weather.h"

#include <cstring>
#include <new>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>

#define TAG "Application"

// 聆听状态静默这么久还没一句话，就是人走了：关麦回待机省电。
// 必须明显早于服务端的静默告别阈值（实测约 67 秒就播「那我先不打扰你了，拜拜」），
// 否则设备一直空转听着，服务端抢在固件前面开口，用户就听到那句尴尬的再见。
#define LISTENING_IDLE_STANDBY_SEC 45

// 一轮会话里没有唤醒词、自动接话的上限。真人闲聊几轮就到头，能连刷几十轮的
// 一定是环境音（游戏语音/电视）在喂它，那就该停下来等唤醒词。
#define AUTO_ROUND_LIMIT 12

// 续接退避：500ms 起翻倍，封顶 30 秒。服务端反复挂断时按这个节奏重连，
// 不至于把重连打成攻击，也不至于让用户觉得已经待机了。
static int64_t ResumeBackoffUs(int fails)
{
    if (fails <= 0)
    {
        return 200000;
    }
    int shift = fails > 6 ? 6 : fails;
    int64_t us = (int64_t)500000 * (1LL << (shift - 1));
    return us > 30000000 ? 30000000 : us;
}

static const char *const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"};

Application::Application()
{
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void *arg)
        {
            Application *app = (Application *)arg;
            app->OnClockTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application()
{
    if (clock_timer_handle_ != nullptr)
    {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckNewVersion(Ota &ota)
{
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    auto &board = Board::GetInstance();
    while (true)
    {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        if (!ota.CheckVersion())
        {
            retry_count++;
            if (retry_count >= MAX_RETRY)
            {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[128];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "sad", Lang::Sounds::P3_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++)
            {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle)
                {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion())
        {
            Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "happy", Lang::Sounds::P3_UPGRADE);

            vTaskDelay(pdMS_TO_TICKS(3000));

            SetDeviceState(kDeviceStateUpgrading);

            display->SetIcon(FONT_AWESOME_DOWNLOAD);
            std::string message = std::string(Lang::Strings::NEW_VERSION) + ota.GetFirmwareVersion();
            display->SetChatMessage("system", message.c_str());

            board.SetPowerSaveMode(false);
            audio_service_.Stop();
            vTaskDelay(pdMS_TO_TICKS(1000));

            bool upgrade_success = ota.StartUpgrade([display](int progress, size_t speed)
                                                    { std::thread([display, progress, speed]()
                                                                  {
                    char buffer[32];
                    snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                    display->SetChatMessage("system", buffer); })
                                                          .detach(); });

            if (!upgrade_success)
            {
                // Upgrade failed, restart audio service and continue running
                ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
                audio_service_.Start();       // Restart audio service
                board.SetPowerSaveMode(true); // Restore power save mode
                Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "sad", Lang::Sounds::P3_EXCLAMATION);
                vTaskDelay(pdMS_TO_TICKS(3000));
                // Continue to normal operation (don't break, just fall through)
            }
            else
            {
                // Upgrade success, reboot immediately
                ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
                display->SetChatMessage("system", "Upgrade successful, rebooting...");
                vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
                Reboot();
                return; // This line will never be reached after reboot
            }
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge())
        {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode())
        {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i)
        {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK)
            {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                break;
            }
            else if (err == ESP_ERR_TIMEOUT)
            {
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
            else
            {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle)
            {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string &code, const std::string &message)
{
    struct digit_sound
    {
        char digit;
        const std::string_view &sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{digit_sound{'0', Lang::Sounds::P3_0},
                                                           digit_sound{'1', Lang::Sounds::P3_1},
                                                           digit_sound{'2', Lang::Sounds::P3_2},
                                                           digit_sound{'3', Lang::Sounds::P3_3},
                                                           digit_sound{'4', Lang::Sounds::P3_4},
                                                           digit_sound{'5', Lang::Sounds::P3_5},
                                                           digit_sound{'6', Lang::Sounds::P3_6},
                                                           digit_sound{'7', Lang::Sounds::P3_7},
                                                           digit_sound{'8', Lang::Sounds::P3_8},
                                                           digit_sound{'9', Lang::Sounds::P3_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);

    for (const auto &digit : code)
    {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound &ds)
                               { return ds.digit == digit; });
        if (it != digit_sounds.end())
        {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char *status, const char *message, const char *emotion, const std::string_view &sound)
{
    ESP_LOGW(TAG, "Alert %s: %s [%s]", status, message, emotion);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty())
    {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert()
{
    if (device_state_ == kDeviceStateIdle)
    {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState()
{
    if (device_state_ == kDeviceStateActivating)
    {
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    else if (device_state_ == kDeviceStateWifiConfiguring)
    {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }
    else if (device_state_ == kDeviceStateAudioTesting)
    {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_)
    {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle)
    {
        Schedule([this]()
                 {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime); });
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        Schedule([this]()
                 { AbortSpeaking(kAbortReasonNone); });
    }
    else if (device_state_ == kDeviceStateListening)
    {
        // 手动待机：按键关掉会话后不再自动续接
        Schedule([this]()
                 {
            user_stopped_ = true;
            resume_pending_ = false;
            resume_fail_count_ = 0;
            protocol_->CloseAudioChannel(); });
    }
}

void Application::StartListening()
{
    if (device_state_ == kDeviceStateActivating)
    {
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    else if (device_state_ == kDeviceStateWifiConfiguring)
    {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_)
    {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle)
    {
        Schedule([this]()
                 {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop); });
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        Schedule([this]()
                 {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop); });
    }
}

void Application::StopListening()
{
    if (device_state_ == kDeviceStateAudioTesting)
    {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end())
    {
        return;
    }

    Schedule([this]()
             {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        } });
}

void Application::Start()
{
    auto &board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]()
    {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string &wake_word)
    {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking)
    {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();

    // 待机屏的天气：自己拉公网接口，不等服务端
    Weather::GetInstance().Start();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new firmware version or get the MQTT broker address
    Ota ota;
    CheckNewVersion(ota);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
    McpServer::GetInstance().AddCommonTools();

    if (ota.HasMqttConfig())
    {
        protocol_ = std::make_unique<MqttProtocol>();
    }
    else if (ota.HasWebsocketConfig())
    {
        protocol_ = std::make_unique<WebsocketProtocol>();
    }
    else
    {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnNetworkError([this](const std::string &message)
                              {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR); });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet)
                               {
        if (device_state_ == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        } });
    protocol_->OnAudioChannelOpened([this, codec, &board]()
                                    {
        board.SetPowerSaveMode(false);
        // 新会话开始：解除上次手动待机的拦截
        user_stopped_ = false;
        resume_pending_ = false;
        channel_opened_us_ = esp_timer_get_time();
        last_activity_us_ = esp_timer_get_time();
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        } });
    protocol_->OnAudioChannelClosed([this, &board]()
                                    {
        // 播放期间不能开 modem sleep：电台是持续收流，省电会把收包攒到 beacon，
        // 几十毫秒的接收空档正好撞上 I2S 只有 32ms 的硬件缓冲，听感就是一直卡。
        if (!audio_owns_channel_) {
            board.SetPowerSaveMode(true);
        }
        // 服务端 goodbye 只是它收尾这一轮，不等于用户要待机。
        // user_stopped_ 要留到下个会话开始再清：手动待机时服务端会回一条 goodbye，
        bool stopped = user_stopped_;
        // 是音乐/提醒把通道借走了，不是会话真断：别当成断连去续接。
        // audio_owns_channel_ 覆盖整段播放：goodbye 会引来第二次 close，脉冲挡不住它。
        bool for_audio = audio_suspend_closing_.exchange(false) || audio_owns_channel_;
        // 通道只活了几秒就被服务端收尾：算一次抖动，退避拉长，别把重连打成攻击
        bool quick = (esp_timer_get_time() - channel_opened_us_) < 3000000;
        Schedule([this, stopped, for_audio, quick]() {
            if (for_audio) {
                resume_pending_ = false;
                return;
            }
            if (stopped) {
                resume_pending_ = false;
                resume_fail_count_ = 0;
                auto display = Board::GetInstance().GetDisplay();
                display->SetChatMessage("system", "");
                SetDeviceState(kDeviceStateIdle);
                return;
            }
            if (quick) {
                int fails = ++resume_fail_count_;
                if (fails >= 8) {
                    ESP_LOGW(TAG, "Server keeps hanging up (%d times), entering standby", fails);
                    resume_pending_ = false;
                    resume_fail_count_ = 0;
                    SetDeviceState(kDeviceStateIdle);
                    return;
                }
            }
            resume_pending_ = true;
            resume_due_us_ = esp_timer_get_time() + ResumeBackoffUs(resume_fail_count_);
        }); });
    protocol_->OnIncomingJson([this, display](const cJSON *root)
                              {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        // 服务端还在下发东西就算有活动，看门狗只管“真没人说话”
        last_activity_us_ = esp_timer_get_time();
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this, display]() {
                    // 服务器说这轮 tts 结束了：字幕从这里才开始回顶慢滚
                    display->NotifySpeechFinished();
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else if (++auto_rounds_ > AUTO_ROUND_LIMIT) {
                            // 一轮里连续这么多次都没等唤醒词就自己接话：房间里一直有声音
                            // （游戏/电视/别人聊天），它每轮都抢着答，用户真开口时它正忙着答别人，
                            // 体感就是「说了没反应」。fix23.log 实测 60 轮全程 wake=0，每轮只隔 2~3 秒。
                            // 回到待机、关麦、重开唤醒词，把主动权交回用户。
                            auto_rounds_ = 0;
                            ESP_LOGI(TAG, "Auto rounds over %d, standby until wake word", AUTO_ROUND_LIMIT);
                            user_stopped_ = true;
                            resume_pending_ = false;
                            resume_fail_count_ = 0;
                            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                                protocol_->CloseAudioChannel();
                            } else {
                                SetDeviceState(kDeviceStateIdle);
                            }
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                last_activity_us_ = esp_timer_get_time();
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::P3_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        } });
    bool protocol_started = protocol_->Start();

    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota.HasServerTime();
    if (protocol_started)
    {
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::P3_SUCCESS);
    }

    // Print heap stats
    SystemInfo::PrintHeapStats();
}

void Application::OnClockTimer()
{
    clock_ticks_++;

    // 取消了一切自动待机：会话断了（服务端 goodbye / 网络抖动）就自己接回来，
    // 只有用户手动按键才算待机。接不上按指数退避重试，别把服务端打冒烟。
    if (resume_pending_ && !user_stopped_)
    {
        int64_t now = esp_timer_get_time();
        if (now >= resume_due_us_)
        {
            resume_due_us_ = now + 3000000;  // 单次重连卡住也不超过 3 秒
            last_activity_us_ = now;
            Schedule([this]()
                     { ResumeSession(); });
        }
    }

    // 聆听就是在等一句话，等满 45 秒一句没来就回待机（早于服务端告别，别让它先开口）。
    // clock_ticks_ 在每次状态切换时归零，等于「在本状态待了多少秒」；
    // 再加一道 last_activity_us_ 是为了把服务端刚下发过的内容算成活动，别掐在人说话中间。
    if (device_state_ == kDeviceStateListening && clock_ticks_ >= LISTENING_IDLE_STANDBY_SEC &&
        (esp_timer_get_time() - last_activity_us_) >= (int64_t)LISTENING_IDLE_STANDBY_SEC * 1000000)
    {
        ESP_LOGI(TAG, "Listening idle for %d seconds, entering standby to save power", LISTENING_IDLE_STANDBY_SEC);
        // 走手动待机同一条路：user_stopped_ 不设的话，关通道的回调会把会话自动接回来，白切。
        Schedule([this]()
                 {
            user_stopped_ = true;
            resume_pending_ = false;
            resume_fail_count_ = 0;
            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                protocol_->CloseAudioChannel();
            } else {
                SetDeviceState(kDeviceStateIdle);
            } });
    }

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar();

    // 天气只占待机屏：一进会话就把字幕位让给对话
    if (device_state_ == kDeviceStateIdle)
    {
        display->SetWeatherText(Weather::GetInstance().Detail().c_str());
    }
    else
    {
        display->SetWeatherText("");
    }

    // Print the debug info every 10 seconds
    if (clock_ticks_ % 10 == 0)
    {
        // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
        // SystemInfo::PrintTaskList();
        SystemInfo::PrintHeapStats();
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop()
{
    // Raise the priority of the main event loop to avoid being interrupted by background tasks (which has priority 2)
    vTaskPrioritySet(NULL, 3);

    while (true)
    {
        // 50ms 超时不是等事件用，是给「打断判定」留轮询节拍：说话期间上行静音、唤醒词也
        // 关着，事件位一个都不会来，portMAX_DELAY 会让这个循环彻底睡死，打断永远判不到。
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED | MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_ERROR, pdTRUE, pdFALSE, pdMS_TO_TICKS(50));
        if (bits & MAIN_EVENT_ERROR)
        {
            // 网络报错不等于用户要待机：没手动按过就让它自己接回来
            if (!user_stopped_)
            {
                resume_pending_ = true;
                resume_due_us_ = esp_timer_get_time() + ResumeBackoffUs(resume_fail_count_);
            }
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "sad", Lang::Sounds::P3_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO)
        {
            while (auto packet = audio_service_.PopPacketFromSendQueue())
            {
                if (!protocol_->SendAudio(std::move(packet)))
                {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED)
        {
            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE)
        {
            if (device_state_ == kDeviceStateListening)
            {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }

            // 音乐播放中用户开口 = 要打断。这是唯一能落到「用户意图」的信号：
            // 唤醒词在音频处理器跑起来后是关着的，想打断只能直接说话。
            // 但设备自己 TTS 说话时不行：这块板 AEC 参考通道没接上（日志 AEC: ref=109 frames=0），
            // 扬声器声音直接灌回麦克风，VAD 必然把尾音判成人声 —— AI 一说完「已开播」就把刚起的台停掉，
            // 实测 radio14.log 里 61 次误停，每次都是 total played 0 bytes。
            // 必须确认音乐真占着通道才动手：StopStreaming(true) 会走 ResumeAfterAudio(true)，
            // 把 resume_pending_ 重新挂上、把 resume 时间推到 500ms 后——在没有音乐的正常对话里
            // 调它，等于自己把刚建立的会话重启一遍。fix21.log 实测：唤醒后 330ms，唤醒提示音的
            // 回声被判成人声，第一次触发就把会话搅成 connecting→goodbye→resumed 死循环，
            // 服务端每轮 1 秒挂断，用户看到屏幕挂着「聆听中」却说什么都没反应。
            if (device_state_ != kDeviceStateSpeaking && audio_owns_channel_ && audio_service_.IsVoiceDetected())
            {
                auto music = Board::GetInstance().GetMusic();
                if (music)
                {
                    ESP_LOGI(TAG, "Stopping music streaming: user started speaking");
                    // true：这条路径是用户开口要说话，停完必须把音频通道重开接回
                    music->StopStreaming(true);
                }
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto &task : tasks)
            {
                task();
            }
        }

        // TTS 说话中用户直接开口 = 打断，不必先喊唤醒词。这里必须无条件轮询：说话期间
        // 上行静音、唤醒词也关着，事件位一个都不来，原来挂在 VAD_CHANGE 分支里等于只在
        // VAD 首次报人声时查一次（那时 held 才 60ms，必然跳过），之后没人复查，
        // 现场表现就是「怎么喊都不停」。节拍由上面 WaitBits 的 50ms 超时给。
        // 判据只认「够响 + 够久」，见 AudioService::IsBargeInConfirmed：单看 VAD 太灵敏，
        // 环境噪声、键盘声都会被判成人声，AI 说半句就被杂音打断。
        // 起播 800ms 内不判——首帧爆音和提示音尾巴会被当成开口（TTS 首句前面还有一段
        // 「叮咚」和降噪 warmup，400ms 盖不住）；aborted_ 由 tts start 复位，保证一轮只发一次 abort。
        if (BARGE_IN_ENABLE && device_state_ == kDeviceStateSpeaking && !aborted_ &&
            audio_service_.IsBargeInConfirmed() &&
            esp_timer_get_time() - speaking_started_us_ > 1500000)
        {
            ESP_LOGI(TAG, "Voice barge-in: peak=%d held=%dms, aborting",
                     audio_service_.GetBargeInPeakRms(), audio_service_.GetBargeInHeldMs());
            AbortSpeaking(kAbortReasonNone);
        }
    }
}

void Application::OnWakeWordDetected()
{
    if (!protocol_)
    {
        return;
    }

    if (device_state_ == kDeviceStateIdle)
    {
        if (!protocol_->IsAudioChannelOpened())
        {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel())
            {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
        // 用户主动喊了，连续对话的轮次重新从零数
        auto_rounds_ = 0;
        // 唤醒词只负责唤醒。上行唤醒词音频或 wake_word_detected 事件，都会让服务端
        // 把「你好小智」当成用户说的第一句话去回答，所以这里只切状态进入聆听。
        // 提示音必须排在 SetListeningMode 之后：切 listening 会 EnableVoiceProcessing(true)，
        // 里面的 ResetDecoder() 把播放队列整个倒掉，先播的叮咚一声都出不来。
        // 反过来的风险是开麦后播会被拾取——但 input 有 120ms warmup 不读麦克风，起始段盖得住。
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        audio_service_.PlaySound(Lang::Sounds::P3_POPUP);
        ESP_LOGI(TAG, "Popup sound queued (%d bytes)", (int)Lang::Sounds::P3_POPUP.size());
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    }
    else if (device_state_ == kDeviceStateActivating)
    {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeaking(AbortReason reason)
{
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    protocol_->SendAbortSpeaking(reason);
}

void Application::SetListeningMode(ListeningMode mode)
{
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

// 音乐/提醒要独占音频通道，会话得先让出来。这只是借用，不是用户要待机——
// 所以绝不设 user_stopped_，而且记下这笔账，播完原样接回。
void Application::SuspendSessionForAudio()
{
    // 播放线程只认 idle 这一个状态：只要不落到 idle 它就一直等，闹钟就永远不出声。
    // 所以这里不能因为 protocol_ 为空、或状态是 connecting 就少走一步。
    bool was_alive = (device_state_ != kDeviceStateIdle);
    audio_owns_channel_ = true;
    resume_pending_ = false;
    // 独占通道这段时间网络要持续供数据，modem sleep 一开就是周期性断供
    Board::GetInstance().SetPowerSaveMode(false);
    ESP_LOGI(TAG, "Audio needs the channel, session suspended (resume after: %d)", was_alive);
    if (protocol_)
    {
        // CloseAudioChannel 会同步回调 OnAudioChannelClosed，压这个标记让那一次别去续接
        audio_suspend_closing_ = true;
        protocol_->CloseAudioChannel();
    }
    SetDeviceState(kDeviceStateIdle);
}

void Application::ResumeAfterAudio(bool take_back)
{
    // 不看 audio_suspend_wants_resume_：设备在 Idle 起播时它是 false，会把这个信号吞掉。
    // take_back=true 就是调用方明确说「用户要说话」，跟起播前在什么状态无关。
    bool wanted = take_back;
    audio_owns_channel_ = false;
    audio_suspend_closing_ = false;
    if (!take_back)
    {
        // 用户主动掐断：播放线程尾可能已经排上了 500ms 后的自动续接，得一并取消
        resume_pending_ = false;
    }
    if (!wanted || user_stopped_)
    {
        // 播放彻底结束、也不接回会话：把 modem sleep 还给系统
        Board::GetInstance().SetPowerSaveMode(true);
        return;
    }
    resume_pending_ = true;
    resume_due_us_ = esp_timer_get_time() + 500000;
    ESP_LOGI(TAG, "Audio finished, taking the session back");
}

// 服务端结束了这一轮会话，但用户没按待机：重开通道接着听，不用再喊唤醒词。
// 失败就退避再试（1s/2s/4s/8s/16s），连试五轮都不行才老实待机——那时候唤醒词还能用。
void Application::ResumeSession()
{
    if (!protocol_ || user_stopped_)
    {
        resume_pending_ = false;
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    auto mode = aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateConnecting);
    if (!protocol_->OpenAudioChannel())
    {
        int fails = ++resume_fail_count_;
        if (fails >= 5)
        {
            ESP_LOGW(TAG, "Resume session failed %d times, giving up and going idle", fails);
            resume_pending_ = false;
            resume_fail_count_ = 0;
            SetDeviceState(kDeviceStateIdle);
            return;
        }
        int64_t backoff_us = ResumeBackoffUs(fails);
        resume_due_us_ = esp_timer_get_time() + backoff_us;
        ESP_LOGW(TAG, "Resume session failed (%d/%d), retry in %d ms", fails, 4, (int)(backoff_us / 1000));
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    // listen start 必须在这里无条件发：EnableVoiceProcessing(true) 已经把处理器拉起来了，
    // 下面 SetDeviceState 里那次 !IsAudioProcessorRunning() 必然是 false，它永远不会补发。
    // 音乐/电台播完的场景尤其致命——处理器刚被 Stop 过，服务端整轮都收不到 start listening，
    // 设备却在 listening 状态挂着，用户说话等于说给空气。
    audio_service_.EnableVoiceProcessing(true);
    // 唤醒词必须让位：AudioInputTask 优先把读到的音频喂给唤醒词然后 continue，两个同时开着
    // voice processing 一个字节都拿不到。这条路径下 processor 已在跑，下面 SetDeviceState 里那句
    // false 会被 if 跳过，所以必须在这里补。
    audio_service_.EnableWakeWordDetection(false);
    protocol_->SendStartListening(mode);
    SetDeviceState(kDeviceStateListening);
    last_activity_us_ = esp_timer_get_time();
    ESP_LOGI(TAG, "Session resumed without wake word");
}

void Application::SetDeviceState(DeviceState state)
{
    if (device_state_ == state)
    {
        return;
    }

    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto &board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    // 停音乐只认「用户真开口」这一个信号，见 VAD 事件与 OnWakeWordDetected。
    // 这里不能按状态停：tts 播完服务端会自己把状态打回 listening，那是系统行为，
    // 跟着停就等于每句「已经开播啦」都亲手把刚起的歌掐掉。

    switch (state)
    {
    case kDeviceStateUnknown:
    case kDeviceStateIdle:
        display->SetStatus(Lang::Strings::STANDBY);
        // 表情不跟着状态回中性：一张表情要留到下一句情绪事件，说话完就切回来等于白演
        audio_service_.SetUplinkMuted(false);
        passive_listen_ = false;
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(true);
        break;
    case kDeviceStateConnecting:
        display->SetStatus(Lang::Strings::CONNECTING);
        display->SetChatMessage("system", "");
        break;
    case kDeviceStateListening:
        display->SetStatus(Lang::Strings::LISTENING);
        // 唤醒词先让位，绝不能放进下面的 if：AudioInputTask 优先把读到的音频喂给唤醒词然后
        // continue，两个同时开着 voice processing 就一个字节都拿不到——AFE fetch 空转刷
        // "Ringbuffer of AFE is empty"，屏幕挂「聆听中」说话毫无反应。音乐停止走 ResumeSession
        // 时 processor 已经在跑，原来这句 false 正好被这个 if 跳掉，就是这个 bug。
        audio_service_.EnableWakeWordDetection(false);
        audio_service_.SetUplinkMuted(false);
        // Make sure the audio processor is running
        if (!audio_service_.IsAudioProcessorRunning() || passive_listen_)
        {
            // Send the start listening command
            protocol_->SendStartListening(listening_mode_);
            audio_service_.EnableVoiceProcessing(true);
            passive_listen_ = false;
        }
        break;
    case kDeviceStateSpeaking:
        display->SetStatus(Lang::Strings::SPEAKING);

        // 说话期间语音处理器保持运行，但上行静音：本地要靠它的 VAD 判断用户开没开口，
        // 这才是「直接说话就能打断」，不必先喊唤醒词。回声由虚拟参考喂给 AFE 的 AEC 消掉。
        speaking_started_us_ = esp_timer_get_time();
        audio_service_.EnableWakeWordDetection(false);
        audio_service_.SetUplinkMuted(true);
        audio_service_.ResetBargeIn();
        // 无条件置上：处理器不再随状态停，回到 listening 时只能靠这个标志补发 start listening
        passive_listen_ = true;
        if (!audio_service_.IsAudioProcessorRunning())
        {
            audio_service_.EnableVoiceProcessing(true);
        }
        audio_service_.ResetDecoder();
        break;
    default:
        // Do nothing
        break;
    }
}

void Application::Reboot()
{
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

void Application::WakeWordInvoke(const std::string &wake_word)
{
    if (device_state_ == kDeviceStateIdle)
    {
        ToggleChatState();
        Schedule([this, wake_word]()
                 {
            if (protocol_) {
                protocol_->SendWakeWordDetected(wake_word); 
            } });
    }
    else if (device_state_ == kDeviceStateSpeaking)
    {
        Schedule([this]()
                 { AbortSpeaking(kAbortReasonNone); });
    }
    else if (device_state_ == kDeviceStateListening)
    {
        Schedule([this]()
                 {
            user_stopped_ = true;
            resume_pending_ = false;
            resume_fail_count_ = 0;
            if (protocol_) {
                protocol_->CloseAudioChannel();
            } });
    }
}

bool Application::CanEnterSleepMode()
{
    if (device_state_ != kDeviceStateIdle)
    {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened())
    {
        return false;
    }

    if (!audio_service_.IsIdle())
    {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string &payload)
{
    Schedule([this, payload]()
             {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        } });
}

void Application::SetAecMode(AecMode mode)
{
    aec_mode_ = mode;
    Schedule([this]()
             {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        } });
}
// 新增：接收外部音频数据（如音乐播放）
void Application::AddAudioData(AudioStreamPacket &&packet)
{
    if (packet.payload.size() >= sizeof(int16_t))
    {
        AddAudioData(reinterpret_cast<const int16_t *>(packet.payload.data()),
                     packet.payload.size() / sizeof(int16_t), packet.sample_rate);
    }
}

// 传裸指针 + 复用成员缓冲，播放线程每帧零分配。
// 原先每帧现建 vector：内部 RAM 只有 30KB 出头，每秒要抖 76 次 2.3KB 的分配
// （音乐侧 payload 一次 + 这里一次），碎片到 operator new 抛 bad_alloc，
// 没人接就是 std::terminate → abort → 整机重启，实测连播 51 秒必现。
void Application::AddAudioData(const int16_t *samples, size_t num_samples, int sample_rate)
{
    if (samples == nullptr || num_samples == 0)
    {
        return;
    }

    try
    {
        auto codec = Board::GetInstance().GetAudioCodec();
        if (device_state_ != kDeviceStateIdle)
        {
            return;
        }

        // resize 到同一容量不再分配，这才是消除碎片的关键
        std::vector<int16_t> &pcm_data = music_pcm_buf_;
        pcm_data.resize(num_samples);
        memcpy(pcm_data.data(), samples, num_samples * sizeof(int16_t));

        // 采样率不一致才重采样：一致时这条路径完全不该有分配
        if (sample_rate != codec->output_sample_rate())
        {
            if (sample_rate <= 0 || codec->output_sample_rate() <= 0)
            {
                ESP_LOGE(TAG, "Invalid sample rates: %d -> %d",
                         sample_rate, codec->output_sample_rate());
                return;
            }

            if (sample_rate > codec->output_sample_rate())
            {
                // 能直接改硬件采样率就不用插值，重采样只做兜底
                ESP_LOGI(TAG, "Music Player: Adjust the sampling rate from %d Hz to %d Hz",
                         codec->output_sample_rate(), sample_rate);
                if (codec->SetOutputSampleRate(sample_rate))
                {
                    ESP_LOGI(TAG, "Successfully switched to music playback sampling rate: %d Hz", sample_rate);
                }
                else
                {
                    ESP_LOGW(TAG, "Unable to switch sampling rate, continue using current sampling rate: %d Hz", codec->output_sample_rate());
                }
            }
            else
            {
                // 上采样：线性插值
                float upsample_ratio = codec->output_sample_rate() / static_cast<float>(sample_rate);
                size_t expected_size = static_cast<size_t>(pcm_data.size() * upsample_ratio + 0.5f);
                std::vector<int16_t> resampled;
                resampled.reserve(expected_size);

                for (size_t i = 0; i < pcm_data.size(); ++i)
                {
                    resampled.push_back(pcm_data[i]);

                    int interpolation_count = static_cast<int>(upsample_ratio) - 1;
                    if (interpolation_count > 0 && i + 1 < pcm_data.size())
                    {
                        int16_t current = pcm_data[i];
                        int16_t next = pcm_data[i + 1];
                        for (int j = 1; j <= interpolation_count; ++j)
                        {
                            float t = static_cast<float>(j) / (interpolation_count + 1);
                            int16_t interpolated = static_cast<int16_t>(current + (next - current) * t);
                            resampled.push_back(interpolated);
                        }
                    }
                    else if (interpolation_count > 0)
                    {
                        for (int j = 1; j <= interpolation_count; ++j)
                        {
                            resampled.push_back(pcm_data[i]);
                        }
                    }
                }

                ESP_LOGI(TAG, "Upsampled %d -> %d samples (ratio: %.2f)",
                         (int)pcm_data.size(), (int)resampled.size(), upsample_ratio);
                pcm_data = std::move(resampled);
            }
        }

        // 确保音频输出已启用
        if (!codec->output_enabled())
        {
            codec->EnableOutput(true);
        }

        codec->OutputData(pcm_data);
        audio_service_.UpdateOutputTimestamp();
    }
    catch (const std::bad_alloc &)
    {
        // 每帧都刷日志会让情况更糟（日志本身也要内存），只记前几次现场
        static int drops = 0;
        if (drops++ < 5)
        {
            ESP_LOGW(TAG, "AddAudioData drop: n=%u cap=%u int_free=%u int_largest=%u psram_free=%u",
                     (unsigned)num_samples, (unsigned)music_pcm_buf_.capacity(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    }
}

void Application::PlaySound(const std::string_view &sound)
{
    audio_service_.PlaySound(sound);
}
