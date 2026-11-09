#ifndef VIRTUAL_ECHO_REF_H
#define VIRTUAL_ECHO_REF_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include <opus_resampler.h>

// 单工编解码（NoAudioCodecSimplex）硬件只引了 mic，input_reference() 恒为 false，
// 而 AFE 的 AEC 只认格式串里的 R 通道。这里软件造一路：上层把即将送喇叭的 PCM 推下来，
// 重采样到 16k 存环形缓冲，喂 AFE 时按「延迟窗口」把 [M,R] 交错起来。
//
// 唤醒词和语音处理各持一份：说话期间语音处理是停的，能听见用户的只有唤醒词，
// 它一样需要回声消除，否则喇叭里的 TTS 直接把它淹没。
class VirtualEchoRef {
public:
    // align_delay_ms：OutputData 到麦克风拾到声音的路径延迟
    void Initialize(int output_sample_rate, int align_delay_ms = 70);
    // 运行时改对齐量。真实路径延迟（I2S TX DMA + 播放队列 + RX DMA + 声学）没法算准，
    // 只能靠实际回声残差反推：残差最低的那一档就是对齐对了。
    void SetAlignDelayMs(int align_delay_ms);
    void Push(const std::vector<int16_t>& pcm, int sample_rate);
    // mic 一路 + 参考一路，交错成 [M,R,M,R...]；参考按 mic 采样时间轴定位
    void Interleave(const int16_t* mic, size_t n, std::vector<int16_t>& out);
    // 取走「参考窗口没数据、只能填 0」的样本数（读后清零）。播放期这个数不为 0 就意味着
    // AEC 拿不到 R 通道，那一刻它完全消不掉回声，残余会从几百直接飙到上万。
    int TakeUnderrunSamples() { return underrun_samples_.exchange(0, std::memory_order_relaxed); }

private:
    // 环容量只要盖住「延迟 + 一个 feed 块」，4096 样本 = 8KB
    static constexpr size_t kRingSamples = 4096;

    OpusResampler resampler_;
    std::mutex mutex_;
    std::vector<int16_t> ring_;
    std::vector<int16_t> resample_buf_;
    std::atomic<int64_t> stream_pos_{0};
    std::atomic<int> underrun_samples_{0};
    int64_t last_push_end_ = 0;
    int delay_samples_ = 16000 * 70 / 1000;
};

#endif  // VIRTUAL_ECHO_REF_H
