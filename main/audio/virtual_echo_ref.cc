#include "virtual_echo_ref.h"

#include <algorithm>

void VirtualEchoRef::Initialize(int output_sample_rate, int align_delay_ms) {
    if (output_sample_rate != 16000) {
        resampler_.Configure(output_sample_rate, 16000);
    }
    ring_.assign(kRingSamples, 0);
    delay_samples_ = 16000 * align_delay_ms / 1000;
}

void VirtualEchoRef::SetAlignDelayMs(int align_delay_ms) {
    int64_t d = (int64_t)16000 * align_delay_ms / 1000;
    std::lock_guard<std::mutex> lock(mutex_);
    delay_samples_ = d;
}

void VirtualEchoRef::Push(const std::vector<int16_t>& pcm, int sample_rate) {
    if (pcm.empty()) {
        return;
    }
    const int16_t* src = pcm.data();
    size_t n = pcm.size();
    if (sample_rate != 16000) {
        resample_buf_.resize(resampler_.GetOutputSamples(n));
        resampler_.Process(pcm.data(), n, resample_buf_.data());
        src = resample_buf_.data();
        n = resample_buf_.size();
    }
    if (n > ring_.size()) {
        src += n - ring_.size();
        n = ring_.size();
    }

    // 这批样本马上送 I2S，会在 delay_samples_ 之后被麦克风拾到，所以记在时间轴的
    // 「当前位置 + 延迟」处；等 mic 走到那里，Interleave 正好读到对齐的参考。
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t cap = (int64_t)ring_.size();
    int64_t base = stream_pos_.load(std::memory_order_relaxed) + delay_samples_;
    for (size_t i = 0; i < n && (int64_t)i < cap; i++) {
        ring_[(base + (int64_t)i) % cap] = src[i];
    }
    last_push_end_ = std::max(last_push_end_, base + (int64_t)n);
}

void VirtualEchoRef::Interleave(const int16_t* mic, size_t n, std::vector<int16_t>& out) {
    int64_t pos = stream_pos_.load(std::memory_order_relaxed);
    out.resize(n * 2);
    int missing = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int64_t cap = (int64_t)ring_.size();
        int64_t valid_lo = last_push_end_ - cap + 1;
        for (size_t i = 0; i < n; i++) {
            int64_t idx = pos + (int64_t)i;
            out[2 * i] = mic[i];
            if (idx >= valid_lo && idx <= last_push_end_) {
                out[2 * i + 1] = ring_[idx % cap];
            } else {
                // 参考窗口还没写到 / 已经被覆盖：只能给 0。AFE 收到的是「此刻没回声」，
                // 但喇叭其实还在响，AEC 就会把回声当新人声放过去。
                out[2 * i + 1] = 0;
                missing++;
            }
        }
    }
    if (missing > 0) {
        underrun_samples_.fetch_add(missing, std::memory_order_relaxed);
    }
    stream_pos_.store(pos + (int64_t)n, std::memory_order_relaxed);
}
