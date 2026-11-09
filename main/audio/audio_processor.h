#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <string>
#include <vector>
#include <functional>

#include "audio_codec.h"

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    
    virtual void Initialize(AudioCodec* codec, int frame_duration_ms) = 0;
    virtual void Feed(std::vector<int16_t>&& data) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() = 0;
    virtual void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) = 0;
    virtual void OnVadStateChange(std::function<void(bool speaking)> callback) = 0;
    virtual size_t GetFeedSize() = 0;
    virtual void EnableDeviceAec(bool enable) = 0;

    // 单工编解码没有回采通道时，由上层把即将送喇叭的 PCM 推下来，当 AEC 的参考信号（R 通道）。
    // 只有真正开了设备端 AEC 的处理器才会用到它。
    virtual void PushReference(const std::vector<int16_t>& pcm, int sample_rate) {}
    // 虚拟参考的对齐延迟（ms）。只有软件造参考通道的处理器才需要，其余忽略。
    virtual void SetEchoRefAlignMs(int ms) {}
    // 回声参考最近是否断过流。断流的那几帧 AEC 等于没开，上行里全是自己的回声。
    virtual bool EchoRefBroken() const { return false; }
    virtual bool AecActive() const { return false; }
};

#endif
