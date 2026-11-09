#ifndef _AUDIO_CODEC_H
#define _AUDIO_CODEC_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <driver/i2s_std.h>

#include <vector>
#include <string>
#include <functional>

#include "board.h"

// RX 侧 dma_frame_num 必须是 AFE 单帧请求量（512 samples）的整除数。原来写 240，
// 512 / 240 = 2.13，i2s_channel_read 每次都要跨一个半 desc 才凑得满，_boot.log 实测
// 一半的读调用返回 ESP_ERR_TIMEOUT bytes=0（reql 3043 次失败 / 6122 帧），音频流成了
// 一半有、一半空的断续信号——AFE 拿这种输入既唤不醒也认不准，现场就是「听不进去」。
// 256 整除 512，6×256 = 1536 帧 = 96ms，缓冲深度不变。
#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 256
// 播放侧单独加厚：6×240 在 44.1kHz 下只有 32.6ms，WiFi 一次调度抖动就够把 DMA 掏空、直接断音。
// 16×320 = 5120 帧 ≈ 116ms@44.1k，给播放线程留出补数据的余量；麦克风仍用上面的原值，不拖长录音延迟。
#define AUDIO_CODEC_TX_DMA_DESC_NUM 16
#define AUDIO_CODEC_TX_DMA_FRAME_NUM 320
#define AUDIO_CODEC_DEFAULT_MIC_GAIN 30.0
// i2s 读超时：一帧（512 samples @16k）= 32ms，100ms 够容忍调度抖动，又能保证
// RX 通道真出问题时读调用会返回而不是永久挂住。
#define AUDIO_CODEC_READ_TIMEOUT_MS 100

class AudioCodec {
public:
    AudioCodec();
    virtual ~AudioCodec();
    
    virtual void SetOutputVolume(int volume);
    virtual void EnableInput(bool enable);
    virtual void EnableOutput(bool enable);
    virtual bool SetOutputSampleRate(int sample_rate);

    virtual void OutputData(std::vector<int16_t>& data);
    virtual bool InputData(std::vector<int16_t>& data);
    virtual void Start();
    // 输入通道读不到数据时由 AudioService 调用；基类不直接持有通道，默认空实现。
    virtual void ResetInputChannel() {}

    inline bool duplex() const { return duplex_; }
    inline bool input_reference() const { return input_reference_; }
    inline int input_sample_rate() const { return input_sample_rate_; }
    inline int output_sample_rate() const { return output_sample_rate_; }
    inline int original_output_sample_rate() const { return original_output_sample_rate_; }
    inline int input_channels() const { return input_channels_; }
    inline int output_channels() const { return output_channels_; }
    inline int output_volume() const { return output_volume_; }
    inline bool input_enabled() const { return input_enabled_; }
    inline bool output_enabled() const { return output_enabled_; }

protected:
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;

    bool duplex_ = false;
    bool input_reference_ = false;
    bool input_enabled_ = false;
    bool output_enabled_ = false;
    int input_sample_rate_ = 0;
    int output_sample_rate_ = 0;
    int original_output_sample_rate_ = 0;
    int input_channels_ = 1;
    int output_channels_ = 1;
    int output_volume_ = 70;

    virtual int Read(int16_t* dest, int samples) = 0;
    virtual int Write(const int16_t* data, int samples) = 0;
};

#endif // _AUDIO_CODEC_H
