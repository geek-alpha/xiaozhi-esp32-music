#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#include <string>
#include <vector>
#include <functional>

#include "audio_codec.h"

class WakeWord {
public:
    virtual ~WakeWord() = default;
    
    virtual bool Initialize(AudioCodec* codec) = 0;
    virtual void Feed(const std::vector<int16_t>& data) = 0;
    virtual void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual size_t GetFeedSize() = 0;
    virtual void EncodeWakeWordData() = 0;
    virtual bool GetWakeWordOpus(std::vector<uint8_t>& opus) = 0;
    virtual const std::string& GetLastDetectedWakeWord() const = 0;

    // 单工编解码没有回采通道时，由上层把即将送喇叭的 PCM 推下来当 AEC 的参考信号（R 通道）。
    // 说话期间语音处理是停的，能听见用户的只有唤醒词，它一样需要消回声。
    virtual void PushReference(const std::vector<int16_t>& pcm, int sample_rate) {}
};

#endif
