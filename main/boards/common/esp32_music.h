#ifndef ESP32_MUSIC_H
#define ESP32_MUSIC_H

#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <vector>

#include "music.h"

// MP3解码器支持
extern "C" {
#include "mp3dec.h"
}

// 音频数据块结构
struct AudioChunk {
    uint8_t* data;
    size_t size;
    
    AudioChunk() : data(nullptr), size(0) {}
    AudioChunk(uint8_t* d, size_t s) : data(d), size(s) {}
};

class Esp32Music : public Music {
public:
    // 显示模式控制 - 移动到public区域
    enum DisplayMode {
        DISPLAY_MODE_SPECTRUM = 0,  // 默认显示频谱
        DISPLAY_MODE_LYRICS = 1     // 显示歌词
    };

private:
    std::string last_downloaded_data_;
    std::string current_music_url_;
    std::string current_song_name_;
    bool song_name_displayed_;
    
    // 歌词相关
    std::string current_lyric_url_;
    // 内置曲源那条路没有 lyric_url，只有酷我 rid：歌词按 rid 自己现拉
    std::string current_lyric_rid_;
    // 服务端随消息下发的 LRC 正文，暂存到这里，由 lyric 线程解析——解析吃 3KB+ 栈，
    // 在 MCP/播放线程里直接解析会把调用者栈顶穿（不一定是 12KB 的大栈线程）。
    std::string pending_lyric_lrc_;
    std::vector<std::pair<int, std::string>> lyrics_;  // 时间戳和歌词文本
    std::mutex lyrics_mutex_;  // 保护lyrics_数组的互斥锁
    std::atomic<int> current_lyric_index_;
    std::thread lyric_thread_;
    std::atomic<bool> is_lyric_running_;
    
    std::atomic<DisplayMode> display_mode_;
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_downloading_;
    std::thread play_thread_;
    std::thread download_thread_;
    int64_t current_play_time_ms_;  // 当前播放时间(毫秒)
    int64_t last_frame_time_ms_;    // 上一帧的时间戳
    int total_frames_decoded_;      // 已解码的帧数

    // 音频缓冲区
    std::queue<AudioChunk> audio_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    size_t buffer_size_;
    static constexpr size_t MAX_BUFFER_SIZE = 256 * 1024;  // 256KB缓冲区（降低以减少brownout风险）
    // 起播门槛按「够垫一秒」定，不按「起得稳」定：内置台全是 64k 流(8KB/s)，
    // 48KB 意味着连上之后还要干等 6 秒才有第一帧，加建连就是十秒静默。
    // 抗抖交给播放中的 RESUME_BUFFER_SIZE 兜，起播慢本身就是坏体验。
    static constexpr size_t MIN_BUFFER_SIZE = 16 * 1024;
    // 断流后恢复播放的门槛：实测拉流 13KB/s 略低于 128kbps(16KB/s)，等 64KB 就是每断一次静音 5 秒。
    // 8KB≈0.5 秒，既不会每次抖动都破音，也不会把一次断流拖成一条长静音。
    static constexpr size_t RESUME_BUFFER_SIZE = 8 * 1024;
    
    // MP3解码器相关
    HMP3Decoder mp3_decoder_;
    MP3FrameInfo mp3_frame_info_;
    bool mp3_decoder_initialized_;
    
    // 闹钟借播放器出声时挂上的上下文：到点上报给 AI 的是「哪件事、几点到点」，
    // 报「《xx》放完了」AI 只知道有歌放过，接不上提醒本身。
    std::string reminder_label_;
    std::string reminder_at_;
    // 这次触发还会响几次（先念再响铃=2）：只在最后一声收尾时上报，早报会撞上后面那条音频
    int reminder_remaining_ = 0;
    std::mutex reminder_mutex_;
    // 0=普通播放；1=该上报完整提醒事件（最后一次收尾）；2=后面还有声，先静默
    int NextReminderStage(std::string& label, std::string& at);

    // 电台是无限流：断了自动重连、永远没有「播完」那一刻，所以不上报 music_finished，
    // 也不参与歌单连播。换台/停止都走同一个覆盖路径。
    std::atomic<bool> is_radio_{false};

    // 起播是否真的连上：StartStreaming 只保证线程起了，MCP 却据此回「已开始播放」。
    // 实测 20ms 就回成功，HTTP 还在握手；用户等到的是「说播了但没声」。
    std::atomic<bool> first_audio_ready_{false};
    // 起播耗时分解用：PlayResolved 记起点，各阶段打差值，用来分清慢在建连还是在攒缓冲
    std::atomic<int64_t> stream_start_us_{0};
    // 线程真退出的标志。std::thread::joinable() 在 join 之前恒为 true，靠它轮询
    // 必然等满超时再 detach：按键像卡了一秒，旧线程还活着接着抢 I2S 和采样率。
    // 线程入口包一层 lambda，函数任何返回路径都置位，不用改函数内部的每个 return。
    std::atomic<bool> play_done_{true};
    std::atomic<bool> download_done_{true};

    // 私有方法
    void DownloadAudioStream(const std::string& music_url);
    // 单次连接读到结尾/断流；live_stream 为 true 时外层会自己连回来（电台用）
    void StreamSession(const std::string& music_url, size_t& round_bytes, size_t& total_downloaded, bool& live_stream);
    void PlayAudioStream();
    void ClearAudioBuffer();
    bool InitializeMp3Decoder();
    void CleanupMp3Decoder();
    void ResetSampleRate();  // 重置采样率到原始值
    
    // 直链+歌词地址都已确定后的统一开播路径（Download 自查、PlayDirect 下发都走它）
    // lyric_rid：内置曲源只拿得到 rid，歌词靠它现拉（此时 lyric_url 为空）
    bool PlayResolved(const std::string& audio_url, const std::string& lyric_url, const std::string& song_name,
                      const std::string& lyric_rid = "");

    // 设备侧播放队列：MCP 一次下发整张歌单，自然播完一首就接下一首，
    // 不依赖服务端轮询（官方云不认自定义 MCP 通知，轮询也拿不到播放进度）。
    struct QueueItem {
        std::string audio_url;
        std::string lyric_url;
        std::string lyric_text;
        std::string title;
        std::string artist;
    };
    std::deque<QueueItem> play_queue_;
    std::mutex queue_mutex_;
    std::atomic<bool> queue_active_;
    std::atomic<int> queue_total_;

    bool PlayQueueItem(const QueueItem& item);
    // 播完一首后调用：队列里还有下一首就取出来播，返回 true 表示已续播
    bool PlayNextFromQueue();
    void ClearQueue();

    // 内置曲源：设备自己按歌名联网解析出直链（不经过任何自建服务）
    bool PlayByBuiltinLookup(const std::string& song_name, const std::string& artist_name);

#ifdef XZ_MUSIC_SELFTEST
    // 上电自检：不经过语音/服务端，直接跑一次内置曲源解析，用来分辨
    // 「设备自己搜不到」和「指令压根没到设备」两种截然不同的故障
    std::thread selftest_thread_;
#endif

    // 歌词相关私有方法
    bool DownloadLyrics(const std::string& lyric_url);
    // 内置曲源：酷我按 rid 回 lrclist JSON，边读边抽，不把整块 JSON 塞进内部堆
    bool DownloadLyricsByRid(const std::string& rid);
    // 歌词正文随消息下发时的入口：直接解析，不走 HTTP
    bool StartLyricsWithContent(const std::string& lrc);
    bool ParseLyrics(const std::string& lyric_content);
    void LyricDisplayThread();
    void UpdateLyricDisplay(int64_t current_time_ms);
    
    // ID3标签处理
    size_t SkipId3Tag(uint8_t* data, size_t size);

    int16_t* final_pcm_data_fft = nullptr;

public:
    Esp32Music();
    ~Esp32Music();

    virtual bool Download(const std::string& song_name, const std::string& artist_name) override;
  
    virtual std::string GetDownloadResult() override;
    
    // 新增方法
    virtual bool StartStreaming(const std::string& music_url) override;

    // MCP 接入点直发：直链/歌词/歌名/歌手由服务端一次给全，设备不查任何服务器地址。
    // lyric_text 是随消息一起下发的 LRC 正文 —— 跨公网播时设备连不到服务端的局域网 lyric_url。
    bool PlayDirect(const std::string& audio_url, const std::string& lyric_url,
                    const std::string& lyric_text, const std::string& title,
                    const std::string& artist);

    // songs_json: [{"audio_url":..,"lyric_text":..,"title":..,"artist":..}, ...]
    // 整张歌单一次下发，设备自己按顺序连播；手动停下/换歌即清队列。
    bool PlayQueue(const std::string& songs_json);

    // 电台：station 走内置台表（支持别名/中文），url 非空则直接用它（用户自己给地址）。
    // index 是 BrowseRadio 返回的序号（1 起），传了它就不用再靠台名匹配。
    // 返回 false 表示查不到这个台，或者用户只是泛泛说了句「放广播」——后者要去先给用户挑。
    bool PlayRadio(const std::string& station, const std::string& url, int index = 0);
    // 按用户需求（「学习的时候」「开车」「睡前」）挑候选台，只返回 JSON 不播放。
    static std::string BrowseRadio(const std::string& want);
    // station 里有没有具体台名：剥掉「电台/广播/听/放」这些泛称词后什么都不剩就是没有
    static bool IsGenericRadioName(const std::string& station);
    // 内置台名清单，AI 拿它给用户挑
    static std::string RadioStationNames();
    virtual bool StopStreaming(bool take_back = false) override;  // 停止流式播放
    // 按键换歌：队列里还有下一首就接上（会覆盖当前这首）；单曲/电台没得换时返回 false
    bool SkipToNext();
    virtual size_t GetBufferSize() const override { return buffer_size_; }
    virtual bool IsDownloading() const override { return is_downloading_; }
    // 闹钟要「先说事情再放铃声」：靠它判断那段提醒语音播完了没有
    bool IsPlaying() const { return is_playing_.load(); }
    // plays=这次触发会响几次（只念=1，先念后铃=2），最后一次收尾时才上报事件
    void SetReminderContext(const std::string& label, const std::string& at, int plays);
    virtual int16_t* GetAudioData() override { return final_pcm_data_fft; }
    
    // 显示模式控制方法
    void SetDisplayMode(DisplayMode mode);
    DisplayMode GetDisplayMode() const { return display_mode_.load(); }
};

#endif // ESP32_MUSIC_H
