#include "esp32_music.h"
#include "board.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"
#include "http_client.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>
#include <cJSON.h>
#include <cstring>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype> // 为isdigit函数
#include <thread> // 为线程ID比较
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <utility>

#define TAG "Esp32Music"

// 内部 RAM 见底时 std::thread 构造会抛 system_error，没人接就是 terminate→abort→整机重启
// （radio7.log:346：播放中说完一句「换个电台」就重启了）。这些后台线程都是锦上添花，
// 栈优先落 PSRAM，再兜一层 catch：建不起来就跳过，绝不拖整机重启。
// 栈必须显式给足：CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT 只有 3072，而 istringstream/string/
// stoi 这类 libstdc++ 代码一块就要吃掉几 KB（live_play.log:294 的 task lyric 栈溢出就是这么炸的）。
template <typename Fn>
static bool StartBgThread(std::thread &slot, const char *name, Fn &&fn, size_t stack_size = 8 * 1024)
{
    if (slot.joinable())
    {
        ESP_LOGW(TAG, "bg thread %s: previous still joinable, skipped", name);
        return false;
    }

    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = name;
    cfg.prio = 5;
    cfg.stack_size = stack_size;
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= 64 * 1024)
    {
        cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }
    ESP_LOGI(TAG, "bg thread %s: stack=%u caps=%s", name, (unsigned)stack_size,
             (cfg.stack_alloc_caps & MALLOC_CAP_SPIRAM) ? "PSRAM" : "IRAM");
    esp_pthread_set_cfg(&cfg);
    // cfg 是线程局部的，会污染本线程后面建的所有线程，用完立刻还原
    esp_pthread_cfg_t restore = esp_pthread_get_default_config();

    bool ok = false;
    try
    {
        slot = std::thread(std::forward<Fn>(fn));
        ok = true;
    }
    catch (const std::exception &e)
    {
        ESP_LOGE(TAG, "bg thread %s failed: %s", name, e.what());
    }
    esp_pthread_set_cfg(&restore);
    return ok;
}

// 自查接口基地址：默认留空 —— 设备不再绑任何服务器 IP，播歌靠 MCP 接入点下发直链。
// 菜单里手动填了 XZ_MUSIC_API_BASE 才会启用旧的自查路径（保留给离线调试）。
// 注意：设备侧没有 CA 证书包，大白主服务的 8008 是自签 HTTPS，进不了 TLS，所以真要填就填明文端口。
#ifndef CONFIG_XZ_MUSIC_API_BASE
#define CONFIG_XZ_MUSIC_API_BASE ""
#endif

// ========== 简单的ESP32认证函数 ==========

/**
 * @brief 获取设备MAC地址
 * @return MAC地址字符串
 */
static std::string get_device_mac()
{
    return SystemInfo::GetMacAddress();
}

/**
 * @brief 获取设备芯片ID
 * @return 芯片ID字符串
 */
static std::string get_device_chip_id()
{
    // 使用MAC地址作为芯片ID，去除冒号分隔符
    std::string mac = SystemInfo::GetMacAddress();
    // 去除所有冒号
    mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
    return mac;
}

/**
 * @brief 生成动态密钥
 * @param timestamp 时间戳
 * @return 动态密钥字符串
 */
static std::string generate_dynamic_key(int64_t timestamp)
{
    // 密钥（请修改为与服务端一致）
    const std::string secret_key = "your-esp32-secret-key-2024";

    // 获取设备信息
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();

    // 组合数据：MAC:芯片ID:时间戳:密钥
    std::string data = mac + ":" + chip_id + ":" + std::to_string(timestamp) + ":" + secret_key;

    // SHA256哈希
    unsigned char hash[32];
    mbedtls_sha256((unsigned char *)data.c_str(), data.length(), hash, 0);

    // 转换为十六进制字符串（前16字节）
    std::string key;
    for (int i = 0; i < 16; i++)
    {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", hash[i]);
        key += hex;
    }

    return key;
}

/**
 * @brief 为HTTP请求添加认证头
 * @param http HTTP客户端指针
 */
static void add_auth_headers(Http *http)
{
    // 获取当前时间戳
    int64_t timestamp = esp_timer_get_time() / 1000000; // 转换为秒

    // 生成动态密钥
    std::string dynamic_key = generate_dynamic_key(timestamp);

    // 获取设备信息
    std::string mac = get_device_mac();
    std::string chip_id = get_device_chip_id();

    // 添加认证头
    if (http)
    {
        http->SetHeader("X-MAC-Address", mac);
        http->SetHeader("X-Chip-ID", chip_id);
        http->SetHeader("X-Timestamp", std::to_string(timestamp));
        http->SetHeader("X-Dynamic-Key", dynamic_key);

        ESP_LOGI(TAG, "Added auth headers - MAC: %s, ChipID: %s, Timestamp: %s",
                 mac.c_str(), chip_id.c_str(), std::to_string(timestamp).c_str());
    }
}

// URL编码函数
static std::string url_encode(const std::string &str)
{
    std::string encoded;
    char hex[4];

    for (size_t i = 0; i < str.length(); i++)
    {
        unsigned char c = str[i];

        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            encoded += c;
        }
        else if (c == ' ')
        {
            encoded += '+'; // 空格编码为'+'或'%20'
        }
        else
        {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

// ========== 内置曲源（酷我公开接口）==========
// MCP 接入点不给直链时，设备自己把歌名换成可播直链，全程只依赖公网 http。
// 搜索接口返回的是 GBK 编码的单引号伪 JSON，设备端没有码表做转码 ——
// 歌名歌手只用调用方传来的 UTF-8，返回里只抽数字字段（rid/时长），这是能塞进单片机的关键。
static std::string http_get_text(const std::string &url)
{
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http)
    {
        return "";
    }
    // 酷我 r.s 只对浏览器 UA + kuwo Referer 吐结果：短 UA（Mozilla/5.0）实测 HIT=0，
    // 拿到的是空 abslist，内置自搜等于形同虚设。这条 UA 是能稳定出结果的最低要求。
    http->SetHeader("User-Agent",
                    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
    http->SetHeader("Referer", "http://www.kuwo.cn/");
    http->SetHeader("Accept", "*/*");
    if (!http->Open("GET", url))
    {
        return "";
    }
    if (http->GetStatusCode() != 200)
    {
        http->Close();
        return "";
    }
    std::string body = http->ReadAll();
    http->Close();
    return body;
}

static bool all_digits(const std::string &s)
{
    if (s.empty())
    {
        return false;
    }
    for (char c : s)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
    }
    return true;
}

// 噪声词按 UTF-8 字面量做字节子串比对：源站返回 UTF-8，本文件也是 UTF-8，
// 设备端没有码表，这样不转码也能认出「伴奏」「翻唱」。分两档性质不同：
// 硬剔除 = 放出来不是听歌（人声被抽掉、根本不是这首）；软降分 = 只是不是原版。
static const char *const kNoiseHard[] = {
    "伴奏", "伴唱", "KTV", "卡拉OK", "纯音乐", "清唱", "铃声", "试听", "抢听",
    "搞笑", "恶搞", "鬼畜", "彩铃", "相声", "小品", "instrumental", "karaoke"};
static const char *const kNoiseSoft[] = {
    "DJ", "dj", "remix", "Remix", "混音", "抖音", "翻唱", "cover", "Cover",
    "女声版", "男声版", "钢琴版", "吉他版", "童声", "live", "Live", "现场",
    "演唱会", "串烧", "改编", "片段", "版)", "版）"};

static const size_t kNoiseHardN = sizeof(kNoiseHard) / sizeof(kNoiseHard[0]);
static const size_t kNoiseSoftN = sizeof(kNoiseSoft) / sizeof(kNoiseSoft[0]);

static bool contains_any(const std::string &hay, const char *const *terms, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (hay.find(terms[i]) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

static int count_any(const std::string &hay, const char *const *terms, size_t n)
{
    int hits = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (hay.find(terms[i]) != std::string::npos)
        {
            hits++;
        }
    }
    return hits;
}

// 设备没装 CA 证书包，https 握手必失败（stream 端只认 200/206）；
// 酷我 CDN 同一路径 http 也回 206，所以统一降到 http。
static std::string force_http(std::string url)
{
    if (url.rfind("https://", 0) == 0)
    {
        url.replace(0, 8, "http://");
    }
    return url;
}

bool Esp32Music::PlayByBuiltinLookup(const std::string &song_name, const std::string &artist_name)
{
    if (song_name.empty())
    {
        return false;
    }

    // 只拿歌名搜：拼上歌手会让结果偏向「该歌手其他歌」——搜「晴天 周杰伦」前八条
    // 里一条原唱都没有，全是伴奏/串烧/别人的歌。歌手名只用来给候选打分。
    std::string search_url = "http://search.kuwo.cn/r.s?all=" + url_encode(song_name) +
                             "&ft=music&client=kt&itemset=web_2013&pn=0&rn=8&rformat=json&encoding=utf8";
    ESP_LOGI(TAG, "Builtin lookup search: %s", search_url.c_str());

    std::string body = http_get_text(search_url);
    if (body.empty())
    {
        ESP_LOGE(TAG, "Builtin lookup: search returned nothing");
        return false;
    }

    // 记录以 DC_TARGETID 分界，但 ARTIST 排在它前面，切片得从上一条的
    // DC_TARGETID 起算，否则歌手字段永远落在片区之外。
    std::vector<size_t> marks;
    for (size_t p = body.find("'DC_TARGETID':'"); p != std::string::npos;
         p = body.find("'DC_TARGETID':'", p + 1))
    {
        marks.push_back(p);
    }

    std::string best_rid;
    int best_duration = 0;
    int best_score = -1000;

    // strict=false 时不再硬剔除：八条全是伴奏也得挑一条，空手而归等于「放不了歌」，
    // 比放个伴奏更糟。
    auto scan = [&](bool strict) {
        best_rid.clear();
        best_score = -1000;
        for (size_t i = 0; i < marks.size(); i++)
        {
            size_t seg_end = (i + 1 < marks.size()) ? marks[i + 1] : body.size();
            size_t rid_start = marks[i] + 15;
            size_t rid_end = body.find('\'', rid_start);
            if (rid_end == std::string::npos || rid_end >= seg_end)
            {
                continue;
            }
            std::string rid = body.substr(rid_start, rid_end - rid_start);
            if (!all_digits(rid))
            {
                continue;
            }

            size_t seg_start = (i == 0) ? 0 : marks[i - 1];
            std::string seg = body.substr(seg_start, seg_end - seg_start);
            if (strict && contains_any(seg, kNoiseHard, kNoiseHardN))
            {
                continue;
            }

            int duration = 0;
            size_t dpos = seg.find("'DURATION':'");
            if (dpos != std::string::npos)
            {
                size_t dstart = dpos + 12;
                size_t dend = seg.find('\'', dstart);
                if (dend != std::string::npos && dend > dstart && dend - dstart <= 5)
                {
                    duration = atoi(seg.substr(dstart, dend - dstart).c_str());
                }
            }

            int score = 0;
            if (seg.find(song_name) != std::string::npos)
            {
                score += 8;
            }
            if (!artist_name.empty() && seg.find(artist_name) != std::string::npos)
            {
                score += 6;
            }
            score -= 3 * count_any(seg, kNoiseSoft, kNoiseSoftN);
            if (duration >= 90 && duration <= 420)
            {
                score += 3;
            }
            else if (duration > 420)
            {
                score -= 4;
            }
            else
            {
                score -= 6;
            }

            if (score > best_score)
            {
                best_score = score;
                best_rid = rid;
                best_duration = duration;
            }
        }
    };

    scan(true);
    if (best_rid.empty())
    {
        scan(false);
    }
    if (best_rid.empty())
    {
        ESP_LOGE(TAG, "Builtin lookup: no usable track for '%s'", song_name.c_str());
        return false;
    }

    std::string audio_url = http_get_text(
        "http://antiserver.kuwo.cn/anti.s?type=convert_url&format=mp3&response=url&rid=MUSIC_" + best_rid);
    while (!audio_url.empty() &&
           (audio_url.back() == '\r' || audio_url.back() == '\n' || audio_url.back() == ' ' ||
            audio_url.back() == '"'))
    {
        audio_url.pop_back();
    }
    if (audio_url.rfind("http", 0) != 0)
    {
        ESP_LOGE(TAG, "Builtin lookup: no direct url for rid %s", best_rid.c_str());
        return false;
    }
    audio_url = force_http(audio_url);

    ESP_LOGI(TAG, "Builtin lookup: %s -> rid %s, %ds, score %d, %s",
             song_name.c_str(), best_rid.c_str(), best_duration, best_score, audio_url.c_str());
    last_downloaded_data_ = audio_url;
    return PlayResolved(audio_url, "", song_name, best_rid);
}

Esp32Music::Esp32Music() : last_downloaded_data_(), current_music_url_(), current_song_name_(),
                           song_name_displayed_(false), current_lyric_url_(), lyrics_(),
                           current_lyric_index_(-1), lyric_thread_(), is_lyric_running_(false),
                           display_mode_(DISPLAY_MODE_LYRICS), is_playing_(false), is_downloading_(false),
                           play_thread_(), download_thread_(), audio_buffer_(), buffer_mutex_(),
                           buffer_cv_(), buffer_size_(0), mp3_decoder_(nullptr), mp3_frame_info_(),
                           mp3_decoder_initialized_(false), queue_active_(false), queue_total_(0)
{
    ESP_LOGI(TAG, "Music player initialized with default spectrum display mode");
    InitializeMp3Decoder();

#ifdef XZ_MUSIC_SELFTEST
    // 诊断用：真机到底能不能自己搜到，只能靠设备自己跑一次回答
    StartBgThread(selftest_thread_, "selftest", [this]() {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        ESP_LOGI(TAG, "SELFTEST: start builtin lookup");
        bool ok = PlayByBuiltinLookup("晴天", "周杰伦");
        ESP_LOGI(TAG, "SELFTEST: done ok=%d", (int)ok);
    }, 16 * 1024);
#endif
}

Esp32Music::~Esp32Music()
{
    ESP_LOGI(TAG, "Destroying music player - stopping all operations");

    // 停止所有操作
    is_downloading_ = false;
    is_playing_ = false;
    is_lyric_running_ = false;

    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    // 等待下载线程结束，设置5秒超时
    if (download_thread_.joinable())
    {
        ESP_LOGI(TAG, "Waiting for download thread to finish (timeout: 5s)");
        auto start_time = std::chrono::steady_clock::now();

        // 等待线程结束
        bool thread_finished = false;
        while (!thread_finished)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - start_time)
                               .count();

            if (elapsed >= 5)
            {
                ESP_LOGW(TAG, "Download thread join timeout after 5 seconds");
                break;
            }

            // 再次设置停止标志，确保线程能够检测到
            is_downloading_ = false;

            // 通知条件变量
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }

            // 检查线程是否已经结束
            if (!download_thread_.joinable())
            {
                thread_finished = true;
            }

            // 定期打印等待信息
            if (elapsed > 0 && elapsed % 1 == 0)
            {
                ESP_LOGI(TAG, "Still waiting for download thread to finish... (%ds)", (int)elapsed);
            }
        }

        if (download_thread_.joinable())
        {
            download_thread_.join();
        }
        ESP_LOGI(TAG, "Download thread finished");
    }

    // 等待播放线程结束，设置3秒超时
    if (play_thread_.joinable())
    {
        ESP_LOGI(TAG, "Waiting for playback thread to finish (timeout: 3s)");
        auto start_time = std::chrono::steady_clock::now();

        bool thread_finished = false;
        while (!thread_finished)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - start_time)
                               .count();

            if (elapsed >= 3)
            {
                ESP_LOGW(TAG, "Playback thread join timeout after 3 seconds");
                break;
            }

            // 再次设置停止标志
            is_playing_ = false;

            // 通知条件变量
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }

            // 检查线程是否已经结束
            if (!play_thread_.joinable())
            {
                thread_finished = true;
            }
        }

        if (play_thread_.joinable())
        {
            play_thread_.join();
        }
        ESP_LOGI(TAG, "Playback thread finished");
    }

    // 等待歌词线程结束
    if (lyric_thread_.joinable())
    {
        ESP_LOGI(TAG, "Waiting for lyric thread to finish");
        lyric_thread_.join();
        ESP_LOGI(TAG, "Lyric thread finished");
    }

    // 清理缓冲区和MP3解码器
    ClearAudioBuffer();
    CleanupMp3Decoder();

    ESP_LOGI(TAG, "Music player destroyed successfully");
}

bool Esp32Music::Download(const std::string &song_name, const std::string &artist_name)
{
    ESP_LOGI(TAG, "Starting to get music details for: %s", song_name.c_str());

    last_downloaded_data_.clear();
    current_song_name_ = song_name;

    // 主路径：大白兼容接口（质量最好，能拿到原唱直链 + 歌词）。
    // 但它一挂设备就彻底放不出歌——所以下面每条失败分支都必须落到内置自搜。
    // 内置那条只依赖公网酷我，和电台同级，不需要任何自建服务活着。
    std::string base_url = CONFIG_XZ_MUSIC_API_BASE;
    if (base_url.empty())
    {
        ESP_LOGW(TAG, "No self-lookup base configured, use builtin music source");
        return PlayByBuiltinLookup(song_name, artist_name);
    }

    std::string full_url = base_url + "/api/music/xz/song?song=" + url_encode(song_name) + "&artist=" + url_encode(artist_name);
    ESP_LOGI(TAG, "Request URL: %s", full_url.c_str());

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "application/json");
    add_auth_headers(http.get());

    if (!http->Open("GET", full_url))
    {
        ESP_LOGE(TAG, "Failed to connect to music API, falling back to builtin");
        return PlayByBuiltinLookup(song_name, artist_name);
    }

    int status_code = http->GetStatusCode();
    if (status_code != 200)
    {
        ESP_LOGE(TAG, "Music API HTTP %d, falling back to builtin", status_code);
        http->Close();
        return PlayByBuiltinLookup(song_name, artist_name);
    }

    std::string payload = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "Music API status %d, content_length %d", status_code, (int)payload.length());

    if (payload.find("ESP32动态密钥验证失败") != std::string::npos)
    {
        ESP_LOGE(TAG, "Auth failed for song: %s, falling back to builtin", song_name.c_str());
        return PlayByBuiltinLookup(song_name, artist_name);
    }

    cJSON *response_json = payload.empty() ? nullptr : cJSON_Parse(payload.c_str());
    if (response_json == nullptr)
    {
        ESP_LOGE(TAG, "Music API gave no parsable JSON, falling back to builtin");
        return PlayByBuiltinLookup(song_name, artist_name);
    }

    cJSON *audio_item = cJSON_GetObjectItem(response_json, "audio_url");
    cJSON *lyric_item = cJSON_GetObjectItem(response_json, "lyric_url");
    std::string audio = (cJSON_IsString(audio_item) && audio_item->valuestring) ? audio_item->valuestring : "";
    std::string lyric = (cJSON_IsString(lyric_item) && lyric_item->valuestring) ? lyric_item->valuestring : "";
    cJSON_Delete(response_json);

    if (audio.empty())
    {
        ESP_LOGE(TAG, "Music API returned no audio_url for '%s', falling back to builtin", song_name.c_str());
        return PlayByBuiltinLookup(song_name, artist_name);
    }
    audio = force_http(audio);

    if (!PlayResolved(audio, lyric, song_name))
    {
        ESP_LOGE(TAG, "Primary audio failed to start, falling back to builtin");
        return PlayByBuiltinLookup(song_name, artist_name);
    }

    last_downloaded_data_ = audio;
    return true;
}

std::string Esp32Music::GetDownloadResult()
{
    return last_downloaded_data_;
}

bool Esp32Music::PlayResolved(const std::string &audio_url, const std::string &lyric_url, const std::string &song_name,
                                const std::string &lyric_rid)
{
    if (audio_url.empty())
    {
        ESP_LOGE(TAG, "Audio URL not found or empty for song: %s", song_name.c_str());
        return false;
    }

    current_music_url_ = audio_url;
    stream_start_us_.store(esp_timer_get_time());
    ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
    ESP_LOGI(TAG, "Starting streaming playback for: %s", song_name.c_str());
    song_name_displayed_ = false; // 重置歌名显示标志
    first_audio_ready_ = false;
    if (!StartStreaming(current_music_url_))
    {
        // 线程起不来（内部 RAM 耗尽）时以前照样往上报成功，界面显示在播、耳朵里一点声音都没有
        ESP_LOGE(TAG, "StartStreaming failed for: %s", audio_url.c_str());
        current_song_name_.clear();
        is_radio_ = false;
        return false;
    }

    // 线程起来不等于有声音：HTTP 可能还在握手、DNS 不通、被墙、服务器 5xx。
    // 以前这里直接 return true，MCP 回「已开始播放」，用户面对的是一片静默——
    // 播放失败的唯一可靠判据是「首块音频真的进了缓冲区」。
    const int kFirstAudioTimeoutMs = 4000;
    int waited_ms = 0;
    while (!first_audio_ready_ && is_downloading_ && waited_ms < kFirstAudioTimeoutMs)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited_ms += 50;
    }
    if (!first_audio_ready_)
    {
        ESP_LOGE(TAG, "No audio within %d ms, start failed: %s", kFirstAudioTimeoutMs, audio_url.c_str());
        StopStreaming();
        current_song_name_.clear();
        is_radio_ = false;
        return false;
    }

    current_lyric_url_ = lyric_url;
    current_lyric_rid_ = lyric_rid;
    // 两条路只要有一条拿得到词源就继续：MCP 下发的是 URL，内置自搜只有 rid
    if (lyric_url.empty() && lyric_rid.empty())
    {
        ESP_LOGW(TAG, "No lyric source for this song");
        return true;
    }

    if (display_mode_ != DISPLAY_MODE_LYRICS)
    {
        ESP_LOGI(TAG, "Lyric source ready but spectrum display mode is active, skipping lyrics");
        return true;
    }

    ESP_LOGI(TAG, "Loading lyrics for: %s (lyrics display mode)", song_name.c_str());
    if (is_lyric_running_)
    {
        is_lyric_running_ = false;
        if (lyric_thread_.joinable())
        {
            lyric_thread_.join();
        }
    }

    is_lyric_running_ = true;
    current_lyric_index_ = -1;
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        lyrics_.clear();
        pending_lyric_lrc_.clear();
    }
    StartBgThread(lyric_thread_, "lyric", [this]() { LyricDisplayThread(); }, 12 * 1024);
    return true;
}

void Esp32Music::SetReminderContext(const std::string &label, const std::string &at, int plays)
{
    std::lock_guard<std::mutex> lock(reminder_mutex_);
    reminder_label_ = label;
    reminder_at_ = at;
    reminder_remaining_ = plays > 0 ? plays : 1;
}

int Esp32Music::NextReminderStage(std::string &label, std::string &at)
{
    std::lock_guard<std::mutex> lock(reminder_mutex_);
    if (reminder_label_.empty() && reminder_at_.empty())
    {
        return 0;
    }
    if (--reminder_remaining_ > 0)
    { // 后面还有铃声/歌要放，先不报：报了 AI 开口就会抢掉那条音频
        return 2;
    }
    label = reminder_label_;
    at = reminder_at_;
    reminder_label_.clear();
    reminder_at_.clear();
    reminder_remaining_ = 0;
    return 1;
}

bool Esp32Music::PlayDirect(const std::string &audio_url, const std::string &lyric_url,
                            const std::string &lyric_text, const std::string &title,
                            const std::string &artist)
{
    // 点播就不是电台了：先清掉电台语义，否则这条歌放完会被当成直播流不上报
    is_radio_ = false;

    if (audio_url.empty())
    {
        ESP_LOGE(TAG, "PlayDirect called without audio_url");
        return false;
    }

    // 单曲点播就打断当前歌单，不然后在队列里排着的歌把点播顶掉
    ClearQueue();

    std::string name = title.empty() ? current_song_name_ : title;
    current_song_name_ = name;
    last_downloaded_data_ = audio_url;
    ESP_LOGI(TAG, "PlayDirect: %s - %s", name.c_str(), artist.c_str());

    if (!lyric_text.empty())
    {
        bool started = PlayResolved(audio_url, "", name);
        StartLyricsWithContent(lyric_text);
        return started;
    }
    return PlayResolved(audio_url, lyric_url, name);
}

// 内置电台表：全是 http 明文 MP3 流。解码链路只认 MP3，HLS/AAC 的台放不了，别往里加。
struct RadioStation
{
    const char *aliases; // 用 | 分隔：用户和大模型可能喊的各种叫法
    const char *name;
    const char *url;
    // scene：品类/使用场景标签，用 | 分隔。AI 拿用户的需求（「学习」「开车」「睡前」）跟它对齐，
    // 所以这里写的是「人话」而不是台名里出现的字。
    const char *scene;
    // desc：一句话风格，候选清单直接念给用户听，别让大模型自己编。
    const char *desc;
};

// 全部国内源、全部 MP3 直链（固件只解 MP3，不支持 HLS/AAC）。
// 实测每家 14~16KB/s、首字节 0.2~1.8s，64k 流留了一倍余量；换源前先量，别凭台名挑。
// 下表里 2026-11 新增的 26 个台，都经 tools/probe_all.py 实测：audio/mpeg、首字节 <1s、瞬时 ≥30KB/s。
// 顺序有语义：同分时排前面的赢，音乐台故意排在交通/新闻台之前。
// 想加台：先用 tools/probe_all.py 实测（audio/mpeg、首字节 <1s、瞬时 ≥12KB/s），过了再进表。
static const RadioStation RADIO_STATIONS[] = {
    {"华语|金曲|经典|老歌|中文歌", "华语金曲500", "https://lhttp.qtfm.cn/live/5022308/64k.mp3",
     "华语|金曲|老歌|怀旧|流行|随便听|背景音|无脑听", "华语金曲一直连播，不用挑歌，当背景音最省心"},
    {"中国之声|新闻|cnr|中央台", "中国之声", "https://lhttp.qtfm.cn/live/15318317/64k.mp3",
     "新闻|时事|资讯|央媒|严肃|了解大事", "央广中国之声，全天新闻和评论，想知道发生了什么就听它"},
    {"广东|粤语|gd", "广东音乐之声", "https://lhttp.qtfm.cn/live/1260/64k.mp3",
     "粤语|广东|流行|音乐", "粤语流行歌为主，广东本地音乐台"},
    {"江苏|南京|js", "江苏经典流行音乐广播", "https://lhttp.qtfm.cn/live/4938/64k.mp3",
     "江苏|南京|流行|经典|音乐", "江苏本地流行金曲频道"},
    {"两广|广西", "两广之声音乐台", "https://lhttp.qtfm.cn/live/20500149/64k.mp3",
     "两广|广西|音乐|方言", "两广地区音乐台，带点南方式口味"},
    {"清晨|早晨|早安", "清晨音乐台", "http://lhttp.qingting.fm/live/4915/64k.mp3",
     "清晨|早晨|起床|通勤|轻快|上班路上", "早晨轻快音乐，起床和上班路上听刚好"},
    {"怀集", "怀集音乐之声", "https://lhttp.qingting.fm/live/4804/64k.mp3",
     "怀集|小城|本地|音乐", "怀集本地音乐广播"},
    {"动听", "动听音乐台", "https://lhttp-hw.qtfm.cn/live/5022107/64k.mp3",
     "流行|音乐|动听|好歌", "流行好歌连播"},
    {"星空|star", "星空电台", "https://lhttp-hw.qtfm.cn/live/5022379/64k.mp3",
     "夜间|放松|助眠|安静|星空|一个人", "夜里慢慢播的音乐，适合放松和睡前"},
    {"国际新闻|环球新闻|国际台", "国际新闻", "https://lhttp.qtfm.cn/live/20500172/64k.mp3",
     "国际|环球|新闻|时事|国外", "国际新闻资讯，关注外面的世界"},
    {"粤语台|港澳|亚洲粤语|asia", "亚洲粤语台", "https://lhttp.qtfm.cn/live/15318569/64k.mp3",
     "粤语|港澳|老歌|亚洲", "粤语歌和港味内容"},
    {"浙江音乐|浙江音乐调频|浙江", "浙江音乐调频", "https://lhttp.qtfm.cn/live/4866/64k.mp3",
     "浙江|杭州|音乐|流行", "浙江音乐调频，本地收听率很高"},
    {"山东音乐|山东经典|山东", "山东经典音乐广播", "https://lhttp.qtfm.cn/live/20240/64k.mp3",
     "山东|经典|音乐|老歌", "山东经典音乐广播"},
    {"辽宁音乐|辽宁经典|辽宁", "辽宁经典音乐广播", "https://lhttp.qtfm.cn/live/20021/64k.mp3",
     "辽宁|经典|音乐|老歌", "辽宁经典音乐广播"},
    {"重庆音乐|重庆", "重庆音乐广播", "https://lhttp.qtfm.cn/live/647/64k.mp3",
     "重庆|音乐|流行", "重庆音乐广播"},
    {"海南音乐|海南", "海南音乐广播", "https://lhttp.qtfm.cn/live/4878/64k.mp3",
     "海南|音乐|轻松|海边", "海南音乐广播，调子轻快"},
    {"上海音乐|沸点|上海", "上海沸点100音乐广播", "https://lhttp.qtfm.cn/live/5022341/64k.mp3",
     "上海|流行|音乐|潮流|新歌", "上海流行音乐台，新歌跟得紧"},
    {"城市音乐|cityfm|city", "CityFM城市音乐台", "https://lhttp.qtfm.cn/live/20500153/64k.mp3",
     "城市|流行|音乐|通勤", "城市流行音乐，适合通勤时听"},
    {"怀旧|怀旧金曲|964", "怀旧金曲964", "https://lhttp.qtfm.cn/live/5021555/64k.mp3",
     "怀旧|老歌|回忆|年代|经典", "怀旧老歌，听的都是以前的旋律"},
    {"年代音乐|年代|1022", "年代音乐1022", "https://lhttp.qtfm.cn/live/20500066/64k.mp3",
     "年代|老歌|经典|回忆", "年代金曲，按年代排歌"},
    {"芒果|芒果音乐|湖南音乐|湖南", "芒果音乐台", "https://lhttp.qtfm.cn/live/4979/64k.mp3",
     "湖南|芒果|流行|音乐|综艺", "湖南芒果音乐台，流行和综艺感"},
    {"欧美音乐|欧美|英文歌|外文", "欧美音乐88.7", "https://lhttp.qtfm.cn/live/15318703/64k.mp3",
     "欧美|英文|外文|外语|西式", "欧美流行音乐，练耳朵也能听"},
    {"经典905|经典音乐|905", "经典音乐FM90.5", "https://lhttp.qtfm.cn/live/20212269/64k.mp3",
     "古典|纯音乐|轻音乐|学习|专注|工作|安静|不吵", "纯音乐和古典为主，学习工作时听它不抢注意力"},
    {"太原音乐|太原", "太原音乐广播", "https://lhttp.qtfm.cn/live/1185/64k.mp3",
     "太原|山西|音乐", "太原音乐广播"},
    {"南通音乐|南通", "南通音乐广播", "https://lhttp.qtfm.cn/live/21275/64k.mp3",
     "南通|江苏|音乐", "南通音乐广播"},
    {"宜昌音乐|宜昌", "宜昌音乐广播", "https://lhttp.qtfm.cn/live/20567/64k.mp3",
     "宜昌|湖北|音乐", "宜昌音乐广播"},
    {"安阳音乐|安阳", "安阳音乐广播", "https://lhttp.qtfm.cn/live/2123/64k.mp3",
     "安阳|河南|音乐", "安阳音乐广播"},
    {"好音乐|濮阳", "濮阳好音乐1023", "https://lhttp.qtfm.cn/live/5021461/64k.mp3",
     "濮阳|河南|音乐", "濮阳好音乐频道"},
    {"珠江|珠江之声", "珠江之声", "https://lhttp.qtfm.cn/live/470/64k.mp3",
     "珠江|广东|粤语|综合|新闻", "珠江经济台，粤语综合内容加新闻"},
    {"四川交通|四川", "四川交通广播FM101.7", "https://lhttp.qtfm.cn/live/4886/64k.mp3",
     "四川|成都|交通|路况|开车|堵车", "四川路况和交通资讯，开车时听"},
    {"广东交通|广东交通之声", "广东交通之声", "https://lhttp.qtfm.cn/live/1262/64k.mp3",
     "广东|交通|路况|开车|堵车", "广东路况和交通资讯"},
    {"深圳交通|深圳", "深圳交通频率", "https://lhttp.qtfm.cn/live/1272/64k.mp3",
     "深圳|交通|路况|开车|堵车", "深圳路况和交通资讯"},
    {"杭州交通|杭州", "杭州交通91.8", "https://lhttp.qtfm.cn/live/1133/64k.mp3",
     "杭州|交通|路况|开车|堵车", "杭州路况，开车通勤用得上"},
    {"无锡交通|无锡", "无锡交通广播", "https://lhttp.qtfm.cn/live/2780/64k.mp3",
     "无锡|交通|路况|开车|堵车", "无锡路况和交通资讯"},
    {"重庆之声|重庆新闻", "重庆之声", "https://lhttp.qtfm.cn/live/1498/64k.mp3",
     "重庆|新闻|综合|民生", "重庆本地新闻和民生话题"},
    {"北京新闻|北京", "北京新闻广播", "https://lhttp.qtfm.cn/live/339/64k.mp3",
     "北京|新闻|民生|本地", "北京本地新闻，早晚高峰有路况"},
    {"广州新闻|广州", "广州新闻电台", "https://lhttp.qtfm.cn/live/4848/64k.mp3",
     "广州|新闻|民生|粤语", "广州本地新闻台"},
};

static std::string ToLowerAscii(const std::string &in)
{
    std::string out = in;
    for (char &c : out)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = (char)(c - 'A' + 'a');
        }
    }
    return out;
}

// 打分制：谁跟用户说的话重合得更具体，谁赢。
// 不能用「命中就返回」：中国之声的别名是「新闻」，会把「北京新闻广播」这种点名抢走。
static int StationScore(const std::string &want, const RadioStation &station)
{
    std::string keyword = ToLowerAscii(want);
    if (keyword.size() < 2)
    { // 单字符会把所有台都匹配上
        return 0;
    }

    std::string name = ToLowerAscii(station.name);
    if (name == keyword)
    {
        return 1000;
    }

    int best = 0;
    if (name.find(keyword) != std::string::npos)
    {
        best = (int)keyword.size(); // 用户说的词完整落在台名里
    }
    if (keyword.find(name) != std::string::npos)
    {
        best = std::max(best, (int)name.size()); // 用户说了台名的超集（「放广州新闻电台」）
    }

    std::string aliases = station.aliases;
    size_t start = 0;
    while (start < aliases.size())
    {
        size_t bar = aliases.find('|', start);
        std::string alias = ToLowerAscii(aliases.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
        if (alias.size() >= 2 && keyword.find(alias) != std::string::npos)
        {
            // 别名一模一样（用户就喊了这个名字）高分，仅仅包含则按长度比
            best = std::max(best, (int)(alias == keyword ? 900 : alias.size()));
        }
        if (bar == std::string::npos)
        {
            break;
        }
        start = bar + 1;
    }
    return best;
}

// 「放广播」「听电台」和「北京新闻广播」都带"电台/广播"，前者是泛称、后者是点名。
// 把泛称词剥干再判长度：剥空了才是泛称，否则拿剩下的实义词去匹配，避免泛称被名字长的台抢走。
static bool IsGenericRadioRequest(const std::string &station)
{
    static const char *GENERIC_WORDS[] = {
        "广播电台", "收音机", "电台", "广播", "radio", "listen", "music",
        "音乐", "听", "放", "点", "个", "首", "一", "下", "来", "我", "想", "要", "台",
        "随便", "什么", "啥", "好听", "帮我", "给我", "现在", "就", "吧", "啊", "呢", "了", "的", "些",
    };
    std::string rest = ToLowerAscii(station);
    for (bool changed = true; changed;)
    {
        changed = false;
        for (const char *w : GENERIC_WORDS)
        {
            size_t pos = rest.find(w);
            if (pos != std::string::npos)
            {
                rest.erase(pos, strlen(w));
                changed = true;
            }
        }
    }
    return rest.size() < 2; // 剥完只剩空或单字节标点，说明用户没点名
}


// 用户说的是需求而不是台名（「想学习的时候听点」「开车路上」）时用这个跟 scene 对齐。
// 设备端只做字面兜底，真正的语义判断交给云端大模型——它拿到候选清单自己挑。
static int StationWantScore(const std::string &want, const RadioStation &station)
{
    int best = StationScore(want, station) * 10;
    std::string list = ToLowerAscii(station.scene);
    size_t start = 0;
    while (start < list.size())
    {
        size_t bar = list.find('|', start);
        std::string tag = list.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        // UTF-8 中文词至少 6 字节，短于 3 字节的单字会把什么都匹配上
        if (tag.size() >= 3 && ToLowerAscii(want).find(tag) != std::string::npos)
        {
            best = std::max(best, (int)tag.size() * 10);
        }
        if (bar == std::string::npos)
        {
            break;
        }
        start = bar + 1;
    }
    return best;
}

// 把用户的需求变成候选台清单（JSON），只查不播。
// 用户选哪个，再由大模型回传 index 或台名——选择权在用户手上，设备不替他定。
std::string Esp32Music::BrowseRadio(const std::string &want)
{
    const int kCount = (int)(sizeof(RADIO_STATIONS) / sizeof(RADIO_STATIONS[0]));
    const int kMax = 6;
    int hit[kMax];
    int hit_score[kMax];
    int hits = 0;

    for (int i = 0; i < kCount; i++)
    {
        int score = want.empty() ? 1 : StationWantScore(want, RADIO_STATIONS[i]);
        if (score <= 0)
        {
            continue;
        }
        // 插入排序维护前 kMax 名，比全部排一遍便宜，也不用 STL 容器
        int pos = (hits < kMax) ? hits : kMax - 1;
        if (hits < kMax)
        {
            hits++;
        }
        else if (score <= hit_score[kMax - 1])
        {
            continue;
        }
        while (pos > 0 && hit_score[pos - 1] < score)
        {
            hit_score[pos] = hit_score[pos - 1];
            hit[pos] = hit[pos - 1];
            pos--;
        }
        hit_score[pos] = score;
        hit[pos] = i;
    }

    // 字面一个都没对上：退回前几个通用台，清单绝不能空。
    // 空清单等于把「哪种台合适」全推给大模型猜，它会自己编台名出来。
    bool fallback = false;
    if (hits == 0)
    {
        fallback = true;
        for (int i = 0; i < kMax && i < kCount; i++)
        {
            hit[hits++] = i;
        }
    }

    std::string json = "{\"count\":" + std::to_string(hits) + ",\"fallback\":" + (fallback ? "true" : "false") +
                       ",\"stations\":[";
    for (int i = 0; i < hits; i++)
    {
        const RadioStation &s = RADIO_STATIONS[hit[i]];
        if (i)
        {
            json += ",";
        }
        json += "{\"index\":" + std::to_string(hit[i] + 1) + ",\"name\":\"" + s.name + "\",\"desc\":\"" +
                s.desc + "\",\"scene\":\"" + s.scene + "\"}";
    }
    json += "]}";
    return json;
}

bool Esp32Music::IsGenericRadioName(const std::string &station)
{
    return IsGenericRadioRequest(station);
}

// 电台和歌是两套语义：点播会紧接着排下一首，电台是无限流，绝不能让它接上歌单
bool Esp32Music::PlayRadio(const std::string &station, const std::string &url, int index)
{
    std::string stream_url = url;
    std::string name = station;

    const int kCount = (int)(sizeof(RADIO_STATIONS) / sizeof(RADIO_STATIONS[0]));

    // 按序号播：多字台名（「上海沸点100音乐广播」）让大模型复述容易念错，序号最稳
    if (stream_url.empty() && index > 0)
    {
        if (index > kCount)
        {
            ESP_LOGW(TAG, "Radio index out of range: %d", index);
            return false;
        }
        stream_url = RADIO_STATIONS[index - 1].url;
        name = RADIO_STATIONS[index - 1].name;
    }

    if (stream_url.empty())
    {
        // 泛称必须先生效，否则「放广播」会被名字里带「广播」的台（江苏/山东/辽宁…）抢先命中
        bool generic = IsGenericRadioRequest(station);
        if (!generic)
        {
            int best_score = 0;
            const RadioStation *best = nullptr;
            for (const auto &candidate : RADIO_STATIONS)
            {
                int score = StationScore(station, candidate);
                if (score > best_score)
                {
                    best_score = score;
                    best = &candidate;
                }
            }
            if (best)
            {
                stream_url = best->url;
                name = best->name;
            }
        }
        if (stream_url.empty())
        {
            // 泛称不再默认播第一个台：那样等于替用户做了选择。
            // 交给大模型先调 browse_radio 把候选念给用户听，用户选完再回来播。
            if (!generic)
            {
                ESP_LOGW(TAG, "Radio station not found: %s", station.c_str());
            }
            else
            {
                ESP_LOGI(TAG, "Generic radio request '%s' -> let user choose", station.c_str());
            }
            return false;
        }
    }
    else if (name.empty())
    {
        name = "电台";
    }

    ClearQueue();
    is_radio_ = true;
    current_song_name_ = name;
    last_downloaded_data_ = stream_url;
    ESP_LOGI(TAG, "PlayRadio: %s -> %s", name.c_str(), stream_url.c_str());
    return PlayResolved(stream_url, "", name);
}

std::string Esp32Music::RadioStationNames()
{
    std::string out;
    for (const auto &station : RADIO_STATIONS)
    {
        if (!out.empty())
        {
            out += "、";
        }
        out += station.name;
    }
    return out;
}

// 播放队列：一次下发整张歌单，设备自己按顺序连播
bool Esp32Music::PlayQueueItem(const QueueItem &item)
{
    is_radio_ = false;

    if (item.audio_url.empty())
    {
        ESP_LOGE(TAG, "Queue item without audio_url: %s", item.title.c_str());
        return false;
    }

    std::string title = item.title.empty() ? item.artist : item.title;
    current_song_name_ = title;
    last_downloaded_data_ = item.audio_url;
    ESP_LOGI(TAG, "Queue playing: %s (total queued %d)", title.c_str(), queue_total_.load());

    if (!item.lyric_text.empty())
    {
        bool started = PlayResolved(item.audio_url, "", title);
        StartLyricsWithContent(item.lyric_text);
        return started;
    }
    return PlayResolved(item.audio_url, item.lyric_url, title);
}

bool Esp32Music::PlayQueue(const std::string &songs_json)
{
    if (songs_json.empty())
    {
        ESP_LOGE(TAG, "PlayQueue: empty songs json");
        return false;
    }

    cJSON *root = cJSON_Parse(songs_json.c_str());
    if (root == nullptr || !cJSON_IsArray(root))
    {
        ESP_LOGE(TAG, "PlayQueue: songs json is not an array");
        if (root != nullptr)
        {
            cJSON_Delete(root);
        }
        return false;
    }

    auto read_field = [](cJSON *node, const char *key) -> std::string {
        cJSON *field = cJSON_GetObjectItem(node, key);
        if (field != nullptr && cJSON_IsString(field) && field->valuestring != nullptr)
        {
            return std::string(field->valuestring);
        }
        return std::string();
    };

    std::deque<QueueItem> items;
    int total = cJSON_GetArraySize(root);
    for (int i = 0; i < total; i++)
    {
        cJSON *node = cJSON_GetArrayItem(root, i);
        if (node == nullptr || !cJSON_IsObject(node))
        {
            continue;
        }

        QueueItem item;
        item.audio_url = read_field(node, "audio_url");
        item.lyric_url = read_field(node, "lyric_url");
        item.lyric_text = read_field(node, "lyric_text");
        item.title = read_field(node, "title");
        item.artist = read_field(node, "artist");
        if (item.title.empty())
        {
            item.title = read_field(node, "song_name");
        }
        if (item.audio_url.empty())
        {
            ESP_LOGW(TAG, "PlayQueue: skip entry %d (no audio_url)", i);
            continue;
        }
        items.push_back(item);
    }
    cJSON_Delete(root);

    if (items.empty())
    {
        ESP_LOGE(TAG, "PlayQueue: none of %d entries is playable", total);
        return false;
    }

    QueueItem first;
    int queued = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        play_queue_.swap(items);
        first = play_queue_.front();
        play_queue_.pop_front();
        queue_total_ = (int)play_queue_.size() + 1;
        queue_active_ = true;
        queued = (int)play_queue_.size();
    }

    ESP_LOGI(TAG, "PlayQueue: %d songs accepted, %d left after first", queue_total_.load(), queued);
    return PlayQueueItem(first);
}

bool Esp32Music::PlayNextFromQueue()
{
    QueueItem next;
    int left = 0;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (play_queue_.empty())
        {
            queue_active_ = false;
            queue_total_ = 0;
            return false;
        }
        next = play_queue_.front();
        play_queue_.pop_front();
        left = (int)play_queue_.size();
    }

    ESP_LOGI(TAG, "Queue: continue with %s (%d left)", next.title.c_str(), left);

    // 不能在播放线程里直接起下一首：StartStreaming 会 join play_thread_，那就是当前线程自己。
    // 用独立线程接力，旧线程 return 后这次 join 立即返回。
    std::thread next_thread;
    if (StartBgThread(next_thread, "next_track", [this, next]() {
            vTaskDelay(pdMS_TO_TICKS(300));
            if (!queue_active_.load())
            {
                ESP_LOGI(TAG, "Queue: cancelled before %s started", next.title.c_str());
                return;
            }
            if (!PlayQueueItem(next))
            {
                ESP_LOGW(TAG, "Queue: failed to start %s", next.title.c_str());
            }
        }))
    {
        next_thread.detach();
    }
    else
    {
        return false;
    }

    return true;
}

bool Esp32Music::SkipToNext()
{
    // 队列只在「整张歌单」下发时存在；单曲/电台没得换，由调用方提示用户
    if (!queue_active_.load())
    {
        return false;
    }
    return PlayNextFromQueue();
}

void Esp32Music::ClearQueue()
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!play_queue_.empty())
    {
        ESP_LOGI(TAG, "Queue cleared: %d pending songs dropped", (int)play_queue_.size());
    }
    play_queue_.clear();
    queue_active_ = false;
    queue_total_ = 0;
}

// 开始流式播放
bool Esp32Music::StartStreaming(const std::string &music_url)
{
    if (music_url.empty())
    {
        ESP_LOGE(TAG, "Music URL is empty");
        return false;
    }

    ESP_LOGI(TAG, "Starting streaming for URL: %s", music_url.c_str());

    // 省电模式会把射频周期性停掉，实测拉流 5~6KB/s 且忽高忽低，64k 流(8KB/s)扛不住。
    // 只在流播放期间关，待机仍按默认省电。
    esp_wifi_set_ps(WIFI_PS_NONE);
  try {

    // 停止之前的播放和下载
    is_downloading_ = false;
    is_playing_ = false;

    // 等待之前的线程完全结束
    if (download_thread_.joinable())
    {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all(); // 通知线程退出
        }
        try {
            download_thread_.join();
        } catch (const std::exception& e) {
            ESP_LOGW(TAG, "join download failed: %s", e.what());
        }
    }
    if (play_thread_.joinable())
    {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all(); // 通知线程退出
        }
        try {
            play_thread_.join();
        } catch (const std::exception& e) {
            ESP_LOGW(TAG, "join play failed: %s", e.what());
        }
    }

    // 清空缓冲区
    ClearAudioBuffer();

    // 栈必须放 PSRAM：内部 RAM 常态只剩 10~20KB（AEC+显示+opus 占着），
    // 线程栈从这里要 8KB 就会 ENOMEM；而 ENOMEM 后 libstdc++ 抛异常自己也要内存，
    // 抛不出来就 std::terminate → abort → 整机重启（radio6.log:325）。
    // 有 CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y，栈可以直接落 PSRAM。
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 128 * 1024)
    {
        ESP_LOGE(TAG, "PSRAM too low for stream threads: %d bytes",
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return false;
    }

    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 12288; // pcm_buffer 一个局部就 4.5KB，12KB 留足余量
    cfg.prio = 5;           // 中等优先级
    cfg.thread_name = "audio_stream";
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_err_t cfg_ret = esp_pthread_set_cfg(&cfg);
    if (cfg_ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_pthread_set_cfg failed: %d", (int)cfg_ret);
    }

    // 先置两个标志再起线程：下载线程的 while 看的就是这两个，晚一步就会自己退出
    is_downloading_ = true;
    is_playing_ = true;
    download_thread_ = std::thread(&Esp32Music::DownloadAudioStream, this, music_url);

    // 开始播放线程（会等待缓冲区有足够数据）
    play_thread_ = std::thread(&Esp32Music::PlayAudioStream, this);

    // cfg 挂在调用线程上，用完立刻恢复默认，别让后面别的线程也继承 PSRAM 栈
    esp_pthread_cfg_t restore = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&restore);

    ESP_LOGI(TAG, "Streaming threads started successfully");

    return true;
  } catch (const std::exception &e) {
    ESP_LOGE(TAG, "StartStreaming failed: %s", e.what());
    return false;
  }
}

// 停止流式播放
bool Esp32Music::StopStreaming()
{
    ESP_LOGI(TAG, "Stopping music streaming - current state: downloading=%d, playing=%d",
             is_downloading_.load(), is_playing_.load());

    // 手动停止就是不要歌单了，否则下一首会在 300ms 后自己冒出来
    ClearQueue();

    // 重置采样率到原始值
    ResetSampleRate();

    // 检查是否有流式播放正在进行
    if (!is_playing_ && !is_downloading_)
    {
        ESP_LOGW(TAG, "No streaming in progress");
        return true;
    }

    // 停止下载和播放标志
    is_downloading_ = false;
    is_playing_ = false;

    // 清空歌名显示
    auto &board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display)
    {
        display->SetMusicInfo(""); // 清空歌名显示
        display->SetChatMessage("lyric", ""); // 歌词是走 chat 通道画的，不清就留在屏幕上盖住待机画面
        ESP_LOGI(TAG, "Cleared song name display");
    }

    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    // 等待线程结束（避免重复代码，让StopStreaming也能等待线程完全停止）
    if (download_thread_.joinable())
    {
        download_thread_.join();
        ESP_LOGI(TAG, "Download thread joined in StopStreaming");
    }

    // 等待播放线程结束，使用更安全的方式
    if (play_thread_.joinable())
    {
        // 先设置停止标志
        is_playing_ = false;

        // 通知条件变量，确保线程能够退出
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }

        // 使用超时机制等待线程结束，避免死锁
        bool thread_finished = false;
        int wait_count = 0;
        const int max_wait = 100; // 最多等待1秒

        while (!thread_finished && wait_count < max_wait)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;

            // 检查线程是否仍然可join
            if (!play_thread_.joinable())
            {
                thread_finished = true;
                break;
            }
        }

        if (play_thread_.joinable())
        {
            if (wait_count >= max_wait)
            {
                ESP_LOGW(TAG, "Play thread join timeout, detaching thread");
                play_thread_.detach();
            }
            else
            {
                play_thread_.join();
                ESP_LOGI(TAG, "Play thread joined in StopStreaming");
            }
        }
    }

    // 在线程完全结束后，只在频谱模式下停止FFT显示
    if (display && display_mode_ == DISPLAY_MODE_SPECTRUM)
    {
        display->stopFft();
        ESP_LOGI(TAG, "Stopped FFT display in StopStreaming (spectrum mode)");
    }
    else if (display)
    {
        ESP_LOGI(TAG, "Not in spectrum mode, skipping FFT stop in StopStreaming");
    }

    ESP_LOGI(TAG, "Music streaming stop signal sent");
    return true;
}

// 流式下载音频数据
// 电台这类无尽流被掐断要自己接回来；点播文件读到 EOF 就是正常结束，不重连
void Esp32Music::DownloadAudioStream(const std::string &music_url)
{
    ESP_LOGD(TAG, "Starting audio stream download from: %s", music_url.c_str());

    // 验证URL有效性
    if (music_url.empty() || music_url.find("http") != 0)
    {
        ESP_LOGE(TAG, "Invalid URL format: %s", music_url.c_str());
        is_downloading_ = false;
        return;
    }

    const size_t RECONNECT_RESET_BYTES = 512 * 1024; // 一轮拿到这么多数据说明链路通着，失败计数清零
    const int MAX_RECONNECT_FAILS = 20;              // 断开后重连：20 轮是给“已经出过声”的容忍度
    // 首连一个字节都没拿到就别用 20 轮：1s→2s→4s→8s 退 20 次 ≈ 2 分钟静音屏。
    // 连不上就 2 次内认输，让 MCP 把失败回给 AI，用户才知道该换台。
    const int MAX_FIRST_CONNECT_FAILS = 2;
    size_t total_downloaded = 0;
    bool live_stream = false;
    int reconnect_fails = 0;

    while (is_downloading_ && is_playing_)
    {
        size_t round_bytes = 0;
        StreamSession(music_url, round_bytes, total_downloaded, live_stream);

        if (!is_downloading_ || !is_playing_ || !live_stream)
        {
            break;
        }
        if (round_bytes >= RECONNECT_RESET_BYTES)
        {
            reconnect_fails = 0;
        }
        int limit = (total_downloaded == 0) ? MAX_FIRST_CONNECT_FAILS : MAX_RECONNECT_FAILS;
        if (++reconnect_fails > limit)
        {
            ESP_LOGE(TAG, "Live stream unreachable after %d retries, total %u bytes, giving up",
                     limit, (unsigned)total_downloaded);
            break;
        }

        int delay_ms = (total_downloaded == 0)
                           ? 1000
                           : (1000 << (reconnect_fails > 3 ? 3 : reconnect_fails - 1)); // 1s→2s→4s→8s封顶
        ESP_LOGW(TAG, "Live stream dropped after %u bytes, reconnect #%d in %d ms",
                 (unsigned)round_bytes, reconnect_fails, delay_ms);
        // 分片睡：用户一按停/切歌，立刻就能退出
        for (int slept = 0; slept < delay_ms && is_downloading_ && is_playing_; slept += 100)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    is_downloading_ = false;

    // 通知播放线程下载完成
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    ESP_LOGI(TAG, "Audio stream download thread finished");
}

// 单次连接读到结尾或断流都算这一轮结束，要不要重连由外层决定
void Esp32Music::StreamSession(const std::string &music_url, size_t &round_bytes, size_t &total_downloaded, bool &live_stream)
{
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);

    // 设置基本请求头
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Range", "bytes=0-"); // 支持断点续传

    // 添加ESP32认证头
    add_auth_headers(http.get());

    int64_t t_open = esp_timer_get_time();
    if (!http->Open("GET", music_url))
    {
        ESP_LOGE(TAG, "Failed to connect to music stream URL");
        return;
    }
    // newlib-nano 不支持 %lld（CONFIG_NEWLIB_NANO_FORMAT=y）：会打出「ld ms」并把后面的参数读错位
    ESP_LOGI(TAG, "起播计时: HTTP 建连(DNS+TCP+TLS) %d ms",
             (int)((esp_timer_get_time() - t_open) / 1000));

    int status_code = http->GetStatusCode();
    if (status_code != 200 && status_code != 206)
    { // 206 for partial content
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http->Close();
        return;
    }

    ESP_LOGI(TAG, "Started downloading audio stream, status: %d", status_code);

    // 电台（icecast 无限流）不报 Content-Length，读到 EOF 只是被掐断；点播文件报了长度，EOF 就是真的放完了
    if (http->GetResponseHeader("Content-Length").empty())
    {
        live_stream = true;
        ESP_LOGI(TAG, "No Content-Length, treat as live stream");
    }

    // 分块读取音频数据
    const size_t chunk_size = 4096; // 4KB每块
    char buffer[chunk_size];
    bool fatal = false;

    while (is_downloading_ && is_playing_)
    {
        int bytes_read = http->Read(buffer, chunk_size);
        static int dbg_read = 0;
        if (dbg_read++ < 8)
        {
            ESP_LOGI(TAG, "DBG read #%d: %d bytes", dbg_read, bytes_read);
        }
        if (bytes_read < 0)
        {
            ESP_LOGE(TAG, "Failed to read audio data: error code %d", bytes_read);
            break;
        }
        if (bytes_read == 0)
        {
            if (live_stream)
            {
                ESP_LOGI(TAG, "Live stream closed by server, total: %d bytes", (int)total_downloaded);
            }
            else
            {
                ESP_LOGI(TAG, "Audio stream download completed, total: %d bytes", (int)total_downloaded);
            }
            break;
        }

        // 首块数据落地才算这条流真的通了：PlayResolved 在等这个标志决定回真成功还是真失败
        first_audio_ready_ = true;

        // 拉流速率 vs 码率才是电台卡不卡的第一判据：128kbps 需要 16KB/s，供不上就必然周期性断流
        {
            static int64_t net_t0 = esp_timer_get_time();
            static size_t net_bytes = 0;
            net_bytes += bytes_read;
            int64_t net_now = esp_timer_get_time();
            if (net_now - net_t0 >= 2000000)
            {
                float kbs = net_bytes / 1024.0f * 1e6f / (net_now - net_t0);
                wifi_ap_record_t ap = {};
                int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
                ESP_LOGW(TAG, "DIAG net: %.1f KB/s (128kbps needs 16.0) buf=%dKB rssi=%d",
                         kbs, (int)(buffer_size_ / 1024), rssi);
                net_t0 = net_now;
                net_bytes = 0;
            }
        }

        // 打印数据块信息
        // ESP_LOGI(TAG, "Downloaded chunk: %d bytes at offset %d", bytes_read, total_downloaded);

        // 安全地打印数据块的十六进制内容（前16字节）
        if (bytes_read >= 16)
        {
            // ESP_LOGI(TAG, "Data: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X ...",
            //         (unsigned char)buffer[0], (unsigned char)buffer[1], (unsigned char)buffer[2], (unsigned char)buffer[3],
            //         (unsigned char)buffer[4], (unsigned char)buffer[5], (unsigned char)buffer[6], (unsigned char)buffer[7],
            //         (unsigned char)buffer[8], (unsigned char)buffer[9], (unsigned char)buffer[10], (unsigned char)buffer[11],
            //         (unsigned char)buffer[12], (unsigned char)buffer[13], (unsigned char)buffer[14], (unsigned char)buffer[15]);
        }
        else
        {
            ESP_LOGI(TAG, "Data chunk too small: %d bytes", bytes_read);
        }

        // 尝试检测文件格式（检查文件头）
        if (round_bytes == 0 && bytes_read >= 4)
        {
            if (memcmp(buffer, "ID3", 3) == 0)
            {
                ESP_LOGI(TAG, "Detected MP3 file with ID3 tag");
            }
            else if (buffer[0] == 0xFF && (buffer[1] & 0xE0) == 0xE0)
            {
                ESP_LOGI(TAG, "Detected MP3 file header");
            }
            else if (memcmp(buffer, "RIFF", 4) == 0)
            {
                ESP_LOGI(TAG, "Detected WAV file");
            }
            else if (memcmp(buffer, "fLaC", 4) == 0)
            {
                ESP_LOGI(TAG, "Detected FLAC file");
            }
            else if (memcmp(buffer, "OggS", 4) == 0)
            {
                ESP_LOGI(TAG, "Detected OGG file");
            }
            else
            {
                ESP_LOGI(TAG, "Unknown audio format, first 4 bytes: %02X %02X %02X %02X",
                         (unsigned char)buffer[0], (unsigned char)buffer[1],
                         (unsigned char)buffer[2], (unsigned char)buffer[3]);
            }
        }

        // 创建音频数据块
        uint8_t *chunk_data = (uint8_t *)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
        if (!chunk_data)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for audio chunk");
            fatal = true;
            break;
        }
        memcpy(chunk_data, buffer, bytes_read);

        // 等待缓冲区有空间
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            buffer_cv_.wait(lock, [this]
                            { return buffer_size_ < MAX_BUFFER_SIZE || !is_downloading_; });

            if (is_downloading_)
            {
                audio_buffer_.push(AudioChunk(chunk_data, bytes_read));
                buffer_size_ += bytes_read;
                total_downloaded += bytes_read;
                round_bytes += bytes_read;

                // 通知播放线程有新数据
                buffer_cv_.notify_one();

                if (total_downloaded % (256 * 1024) == 0)
                { // 每256KB打印一次进度
                    ESP_LOGI(TAG, "Downloaded %d bytes, buffer size: %d", total_downloaded, buffer_size_);
                }
            }
            else
            {
                heap_caps_free(chunk_data);
                break;
            }
        }
    }

    http->Close();

    if (fatal)
    {
        is_downloading_ = false;
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    ESP_LOGI(TAG, "Stream session ended, this round: %u bytes, total: %u bytes",
             (unsigned)round_bytes, (unsigned)total_downloaded);
}

// 流式播放音频数据
void Esp32Music::PlayAudioStream()
{
    ESP_LOGI(TAG, "Starting audio stream playback");

    // 初始化时间跟踪变量
    current_play_time_ms_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec)
    {
        ESP_LOGE(TAG, "Audio codec not available");
        is_playing_ = false;
        return;
    }

    if (!mp3_decoder_initialized_)
    {
        ESP_LOGE(TAG, "MP3 decoder not initialized");
        is_playing_ = false;
        return;
    }

    // 等缓冲区攒够数据再开播。谓词必须带 is_playing_：不然停播时 notify 也叫不醒它，
    // StopStreaming 只能等 1 秒后 detach —— 旧播放线程还活着，下次 StartStreaming 又把
    // is_playing_ 置回 true，它就跟新线程一起抢 buffer 和解码器。
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        buffer_cv_.wait(lock, [this]
                        { return buffer_size_ >= MIN_BUFFER_SIZE || !is_playing_ || (!is_downloading_ && !audio_buffer_.empty()); });
    }


    ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
    ESP_LOGI(TAG, "起播计时: 攒够缓冲 %d ms (buffer=%d)",
             (int)((esp_timer_get_time() - stream_start_us_.load()) / 1000), (int)buffer_size_);

    size_t total_played = 0;
    uint8_t *mp3_input_buffer = nullptr;
    int bytes_left = 0;
    uint8_t *read_ptr = nullptr;
    // 等 device 回 idle 的计时。会话续接、服务端握手都可能把它长时间占在非 idle，
    // 闹钟不能被这件事卡死，所以等够就强制抢一次通道。
    int state_wait_ticks = 0;

    // TEMP-DIAG：卡顿到底卡在网络还是卡在解码/写 I2S，靠这三项分得清
    std::vector<int16_t> mono_buffer;
    int diag_underruns = 0;
    int64_t diag_last_write_us = 0;
    int64_t diag_max_cycle_us = 0;
    int64_t diag_report_us = esp_timer_get_time();

    // 分配MP3输入缓冲区
    mp3_input_buffer = (uint8_t *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!mp3_input_buffer)
    {
        ESP_LOGE(TAG, "Failed to allocate MP3 input buffer");
        is_playing_ = false;
        return;
    }

    // 标记是否已经处理过ID3标签
    bool id3_processed = false;

    while (is_playing_)
    {
        // 检查设备状态，只有在空闲状态才播放音乐
        auto &app = Application::GetInstance();
        DeviceState current_state = app.GetDeviceState();

        // 状态转换：说话中-》聆听中-》待机状态-》播放音乐
        if (current_state == kDeviceStateListening || current_state == kDeviceStateSpeaking)
        {
            if (current_state == kDeviceStateSpeaking)
            {
                ESP_LOGI(TAG, "Device is in speaking state, switching to listening state for music playback");
            }
            if (current_state == kDeviceStateListening)
            {
                ESP_LOGI(TAG, "Device is in listening state, switching to idle state for music playback");
            }
            // 让出通道但不设 user_stopped_：这是播放要独占音频，不是用户按了待机
            app.SuspendSessionForAudio();
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        else if (current_state != kDeviceStateIdle)
        { // 不是待机状态，先等一下；等够了就抢通道，别无限等
            ESP_LOGD(TAG, "Device state is %d, pausing music playback", current_state);
            if (++state_wait_ticks > 120)
            {
                ESP_LOGW(TAG, "Device stuck in state %d, taking the channel for playback", current_state);
                app.SuspendSessionForAudio();
                state_wait_ticks = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        state_wait_ticks = 0;

        // 输出通道会被 AudioService 的空闲省电逻辑关掉，进到这里先重新打开
        if (!codec->output_enabled())
        {
            ESP_LOGI(TAG, "Re-enabling audio output for music playback");
            codec->EnableOutput(true);
        }

        // 设备状态检查通过，显示当前播放的歌名
        if (!song_name_displayed_ && !current_song_name_.empty())
        {
            auto &board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display)
            {
                // 电台不是「歌」：没有《》也不该写「播放中」，它是没有尽头的一条流
                std::string formatted_song_name = is_radio_.load()
                                                      ? current_song_name_ + " · 直播中"
                                                      : "《" + current_song_name_ + "》播放中...";
                display->SetMusicInfo(formatted_song_name.c_str());
                ESP_LOGI(TAG, "Displaying song name: %s", formatted_song_name.c_str());
                song_name_displayed_ = true;
            }

            // 根据显示模式启动相应的显示功能
            if (display)
            {
                if (display_mode_ == DISPLAY_MODE_SPECTRUM)
                {
                    display->start();
                    ESP_LOGI(TAG, "Display start() called for spectrum visualization");
                }
                else
                {
                    ESP_LOGI(TAG, "Lyrics display mode active, FFT visualization disabled");
                }
            }
        }

        // 如果需要更多MP3数据，从缓冲区读取
        if (bytes_left < 4096)
        { // 保持至少4KB数据用于解码
            AudioChunk chunk;

            // 从缓冲区获取音频数据
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty())
                {
                    if (!is_downloading_)
                    {
                        // 下载完成且缓冲区为空，播放结束
                        ESP_LOGI(TAG, "Playback finished, total played: %d bytes", total_played);
                        break;
                    }
                    // 断流恢复：等回一大段再继续。只等 4KB(0.25s) 就接着放的话，
                    // 每次网络抖动都变成一次破音，电台听感就是一直在卡。
                    diag_underruns++;
                    buffer_cv_.wait(lock, [this]
                                    { return buffer_size_ >= RESUME_BUFFER_SIZE || !is_downloading_; });
                    if (audio_buffer_.empty())
                    {
                        continue;
                    }
                }

                chunk = audio_buffer_.front();
                audio_buffer_.pop();
                buffer_size_ -= chunk.size;

                // 通知下载线程缓冲区有空间
                buffer_cv_.notify_one();
            }

            // 将新数据添加到MP3输入缓冲区
            if (chunk.data && chunk.size > 0)
            {
                // 移动剩余数据到缓冲区开头
                if (bytes_left > 0 && read_ptr != mp3_input_buffer)
                {
                    memmove(mp3_input_buffer, read_ptr, bytes_left);
                }

                // 检查缓冲区空间
                size_t space_available = 8192 - bytes_left;
                size_t copy_size = std::min(chunk.size, space_available);

                // 复制新数据
                memcpy(mp3_input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += copy_size;
                read_ptr = mp3_input_buffer;

                // 检查并跳过ID3标签（仅在开始时处理一次）
                if (!id3_processed && bytes_left >= 10)
                {
                    size_t id3_skip = SkipId3Tag(read_ptr, bytes_left);
                    if (id3_skip > 0)
                    {
                        read_ptr += id3_skip;
                        bytes_left -= id3_skip;
                        ESP_LOGI(TAG, "Skipped ID3 tag: %u bytes", (unsigned int)id3_skip);
                    }
                    id3_processed = true;
                }

                // 释放chunk内存
                heap_caps_free(chunk.data);
            }
        }

        // 尝试找到MP3帧同步
        int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync_offset < 0)
        {
            ESP_LOGW(TAG, "No MP3 sync word found, skipping %d bytes", bytes_left);
            bytes_left = 0;
            continue;
        }

        // 跳过到同步位置
        if (sync_offset > 0)
        {
            read_ptr += sync_offset;
            bytes_left -= sync_offset;
        }

        // 解码MP3帧
        int16_t pcm_buffer[2304];
        int decode_result = MP3Decode(mp3_decoder_, &read_ptr, &bytes_left, pcm_buffer, 0);

        if (decode_result == 0)
        {
            // 解码成功，获取帧信息
            MP3GetLastFrameInfo(mp3_decoder_, &mp3_frame_info_);
            total_frames_decoded_++;

            // 基本的帧信息有效性检查，防止除零错误
            if (mp3_frame_info_.samprate == 0 || mp3_frame_info_.nChans == 0)
            {
                ESP_LOGW(TAG, "Invalid frame info: rate=%d, channels=%d, skipping",
                         mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                continue;
            }

            // 计算当前帧的持续时间(毫秒)
            int frame_duration_ms = (mp3_frame_info_.outputSamps * 1000) /
                                    (mp3_frame_info_.samprate * mp3_frame_info_.nChans);

            // 更新当前播放时间
            current_play_time_ms_ += frame_duration_ms;

            ESP_LOGD(TAG, "Frame %d: time=%dms, duration=%dms, rate=%d, ch=%d",
                     total_frames_decoded_, (int)current_play_time_ms_, frame_duration_ms,
                     mp3_frame_info_.samprate, mp3_frame_info_.nChans);

            // 更新歌词显示
            int buffer_latency_ms = 600; // 实测调整值
            UpdateLyricDisplay(current_play_time_ms_ + buffer_latency_ms);

            // 将PCM数据发送到Application的音频解码队列
            if (mp3_frame_info_.outputSamps > 0)
            {
                int16_t *final_pcm_data = pcm_buffer;
                int final_sample_count = mp3_frame_info_.outputSamps;

                // 如果是双通道，转换为单通道混合
                if (mp3_frame_info_.nChans == 2)
                {
                    // 双通道转单通道：将左右声道混合
                    int stereo_samples = mp3_frame_info_.outputSamps; // 包含左右声道的总样本数
                    int mono_samples = stereo_samples / 2;            // 实际的单声道样本数

                    mono_buffer.resize(mono_samples);

                    for (int i = 0; i < mono_samples; ++i)
                    {
                        // 混合左右声道 (L + R) / 2
                        int left = pcm_buffer[i * 2];      // 左声道
                        int right = pcm_buffer[i * 2 + 1]; // 右声道
                        mono_buffer[i] = (int16_t)((left + right) / 2);
                    }

                    final_pcm_data = mono_buffer.data();
                    final_sample_count = mono_samples;

                    ESP_LOGD(TAG, "Converted stereo to mono: %d -> %d samples",
                             stereo_samples, mono_samples);
                }
                else if (mp3_frame_info_.nChans == 1)
                {
                    // 已经是单声道，无需转换
                    ESP_LOGD(TAG, "Already mono audio: %d samples", final_sample_count);
                }
                else
                {
                    ESP_LOGW(TAG, "Unsupported channel count: %d, treating as mono",
                             mp3_frame_info_.nChans);
                }

                size_t pcm_size_bytes = final_sample_count * sizeof(int16_t);

                if (final_pcm_data_fft == nullptr)
                {
                    final_pcm_data_fft = (int16_t *)heap_caps_malloc(
                        final_sample_count * sizeof(int16_t),
                        MALLOC_CAP_SPIRAM);
                }

                // 只有频谱模式才有人读这份拷贝：歌词模式下每帧白拷 2KB 进 PSRAM，
                // 播放线程不该为没人看的画面多干一次活。
                if (display_mode_ == DISPLAY_MODE_SPECTRUM)
                {
                    memcpy(
                        final_pcm_data_fft,
                        final_pcm_data,
                        final_sample_count * sizeof(int16_t));
                }

                ESP_LOGD(TAG, "Sending %d PCM samples (%d bytes, rate=%d, channels=%d->1) to Application",
                         final_sample_count, pcm_size_bytes, mp3_frame_info_.samprate, mp3_frame_info_.nChans);

                // 发送到Application的音频解码队列
                if (total_frames_decoded_ <= 6)
                {
                    ESP_LOGI(TAG, "DBG push in #%d: samples=%d rate=%d state=%d free=%u",
                             (int)total_frames_decoded_, final_sample_count, mp3_frame_info_.samprate,
                             (int)app.GetDeviceState(),
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                }
                // 直接给指针：AudioStreamPacket 的 payload 每帧都要现分配 2.3KB，
                // 和 Application 里那次分配叠在一起，内部 RAM 每秒抖 76 次 ——
                // 碎片到 operator new 抛 bad_alloc，整机就 abort 重启
                app.AddAudioData(final_pcm_data, (size_t)final_sample_count, mp3_frame_info_.samprate);
                if (total_frames_decoded_ == 0)
                {
                    ESP_LOGI(TAG, "起播计时: 首帧 PCM 送出 %d ms",
                             (int)((esp_timer_get_time() - stream_start_us_.load()) / 1000));
                }
                if (total_frames_decoded_ <= 6)
                {
                    ESP_LOGI(TAG, "DBG push out #%d: total=%d", (int)total_frames_decoded_, (int)total_played);
                }
                total_played += pcm_size_bytes;

                // 一次循环 = 一帧(约26ms)：正常应该贴着音频时钟走，
                // 明显超过一帧就说明这一帧里被别的事挡住了
                int64_t now_us = esp_timer_get_time();
                if (diag_last_write_us > 0)
                {
                    int64_t cycle = now_us - diag_last_write_us;
                    if (cycle > diag_max_cycle_us)
                    {
                        diag_max_cycle_us = cycle;
                    }
                }
                diag_last_write_us = now_us;
                if (now_us - diag_report_us > 2000000)
                {
                    ESP_LOGI(TAG, "DIAG play: underruns=%d buf=%uKB maxcycle=%dms",
                             diag_underruns, (unsigned)(buffer_size_ / 1024),
                             (int)(diag_max_cycle_us / 1000));
                    diag_report_us = now_us;
                    diag_max_cycle_us = 0;
                }

                // 打印播放进度
                if (total_played % (128 * 1024) == 0)
                {
                    ESP_LOGI(TAG, "Played %d bytes, buffer size: %d", total_played, buffer_size_);
                }
            }
        }
        else
        {
            // 解码失败
            static int dbg_fail = 0;
            if (dbg_fail++ < 6)
            {
                ESP_LOGW(TAG, "DBG decode fail %d: err=%d bytes_left=%d off=%d",
                         dbg_fail, decode_result, bytes_left,
                         (int)(read_ptr - mp3_input_buffer));
            }
            ESP_LOGW(TAG, "MP3 decode failed with error: %d", decode_result);

            // 跳过一些字节继续尝试
            if (bytes_left > 1)
            {
                read_ptr++;
                bytes_left--;
            }
            else
            {
                bytes_left = 0;
            }
        }
    }

    // 清理
    if (mp3_input_buffer)
    {
        heap_caps_free(mp3_input_buffer);
    }

    // 播放结束时进行基本清理，但不调用StopStreaming避免线程自我等待
    ESP_LOGI(TAG, "Audio stream playback finished, total played: %d bytes", total_played);
    ESP_LOGI(TAG, "Performing basic cleanup from play thread");

    // 停止播放标志
    is_playing_ = false;
    is_lyric_running_ = false;

    // 自然播完这条路上以前什么都没收：采样率留在音乐的 44.1k、屏幕停在「《歌名》播放中…」，
    // 于是看起来整个卡死在播放态，下一句语音也变调。这三件事必须和 StopStreaming 做齐。
    ResetSampleRate();

    auto &finish_board = Board::GetInstance();
    auto finish_display = finish_board.GetDisplay();
    if (finish_display)
    {
        finish_display->SetMusicInfo("");
        finish_display->SetChatMessage("lyric", "");
    }
    song_name_displayed_ = false;

    // 只在频谱显示模式下才停止FFT显示
    if (display_mode_ == DISPLAY_MODE_SPECTRUM)
    {
        if (finish_display)
        {
            finish_display->stopFft();
            ESP_LOGI(TAG, "Stopped FFT display from play thread (spectrum mode)");
        }
    }
    else
    {
        ESP_LOGI(TAG, "Not in spectrum mode, skipping FFT stop");
    }

    // 歌单连播：还有下一首就直接接上，且不上报 music_finished ——
    // 一报 AI 就插话「《xx》放完了」，连播中间不该有人声。
    if (queue_active_.load() && PlayNextFromQueue())
    {
        return;
    }

    // 上报给 AI 的必须是一次完整事件：哪件事、几点到点，而不是「《xx》放完了」。
    // 只报歌名时 AI 只能客套一句「歌放完了」，它并不知道刚才提醒的是什么。
    std::string reminder_label;
    std::string reminder_at;
    int reminder_stage = NextReminderStage(reminder_label, reminder_at);

    auto json_escape = [](const std::string &in)
    {
        std::string out;
        for (char c : in)
        {
            if (c == '"' || c == '\\')
            {
                out += '\\';
            }
            if ((unsigned char)c >= 0x20)
            {
                out += c;
            }
        }
        return out;
    };

    if (reminder_stage == 1)
    {
        char payload[384];
        snprintf(payload, sizeof(payload),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/alarm_fired\",\"params\":"
                 "{\"event\":\"reminder_due\",\"label\":\"%s\",\"at\":\"%s\",\"sound\":\"%s\"}}",
                 json_escape(reminder_label).c_str(), json_escape(reminder_at).c_str(),
                 json_escape(current_song_name_).c_str());
        Application::GetInstance().SendMcpMessage(payload);
        ESP_LOGI(TAG, "Reported reminder_due: %s at %s", reminder_label.c_str(), reminder_at.c_str());

        // 不能再用 detect 把这句话塞给 AI：服务端会直接回
        // "detect is only for wake, don't send long texts"——按协议 detect 的 text 只能是唤醒词。
        // 事情已经由本地 TTS 念出来了，AI 想知道细节就查 self.alarm.list 的 last_fired。
    }
    // 电台结束是被掐断/彻底连不上，不是「放完了」：报上去 AI 会开口说《台名》放完了
    else if (reminder_stage == 0 && !is_radio_.load())
    {
        // 设备是 MCP server，AI 侧没有轮询设备的能力，只能靠上行通知告诉它这首放完了。
        std::string title = json_escape(current_song_name_);
        char payload[192];
        snprintf(payload, sizeof(payload),
                 "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/music_finished\",\"params\":{\"title\":\"%s\"}}",
                 title.c_str());
        Application::GetInstance().SendMcpMessage(payload);
        ESP_LOGI(TAG, "Reported music_finished to server: %s", title.c_str());
    }
    // reminder_stage == 2：这次闹钟的铃声也放完了，事件已经报过，不再打扰 AI

    // 真正播完了（stage 2 是“后面还有铃声”，得等它响完），把借出去的会话接回来
    if (reminder_stage != 2)
    {
        Application::GetInstance().Schedule([]()
                                            { Application::GetInstance().ResumeAfterAudio(); });
    }
}

// 清空音频缓冲区
void Esp32Music::ClearAudioBuffer()
{
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    while (!audio_buffer_.empty())
    {
        AudioChunk chunk = audio_buffer_.front();
        audio_buffer_.pop();
        if (chunk.data)
        {
            heap_caps_free(chunk.data);
        }
    }

    buffer_size_ = 0;
    ESP_LOGI(TAG, "Audio buffer cleared");
}

// 初始化MP3解码器
bool Esp32Music::InitializeMp3Decoder()
{
    mp3_decoder_ = MP3InitDecoder();
    if (mp3_decoder_ == nullptr)
    {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        mp3_decoder_initialized_ = false;
        return false;
    }

    mp3_decoder_initialized_ = true;
    ESP_LOGI(TAG, "MP3 decoder initialized successfully");
    return true;
}

// 清理MP3解码器
void Esp32Music::CleanupMp3Decoder()
{
    if (mp3_decoder_ != nullptr)
    {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    mp3_decoder_initialized_ = false;
    ESP_LOGI(TAG, "MP3 decoder cleaned up");
}

// 重置采样率到原始值
void Esp32Music::ResetSampleRate()
{
    auto &board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    if (codec && codec->original_output_sample_rate() > 0 &&
        codec->output_sample_rate() != codec->original_output_sample_rate())
    {
        ESP_LOGI(TAG, "重置采样率：从 %d Hz 重置到原始值 %d Hz",
                 codec->output_sample_rate(), codec->original_output_sample_rate());
        if (codec->SetOutputSampleRate(-1))
        { // -1 表示重置到原始值
            ESP_LOGI(TAG, "成功重置采样率到原始值: %d Hz", codec->output_sample_rate());
        }
        else
        {
            ESP_LOGW(TAG, "无法重置采样率到原始值");
        }
    }
}

// 跳过MP3文件开头的ID3标签
size_t Esp32Music::SkipId3Tag(uint8_t *data, size_t size)
{
    if (!data || size < 10)
    {
        return 0;
    }

    // 检查ID3v2标签头 "ID3"
    if (memcmp(data, "ID3", 3) != 0)
    {
        return 0;
    }

    // 计算标签大小（synchsafe integer格式）
    uint32_t tag_size = ((uint32_t)(data[6] & 0x7F) << 21) |
                        ((uint32_t)(data[7] & 0x7F) << 14) |
                        ((uint32_t)(data[8] & 0x7F) << 7) |
                        ((uint32_t)(data[9] & 0x7F));

    // ID3v2头部(10字节) + 标签内容
    size_t total_skip = 10 + tag_size;

    // 确保不超过可用数据大小
    if (total_skip > size)
    {
        total_skip = size;
    }

    ESP_LOGI(TAG, "Found ID3v2 tag, skipping %u bytes", (unsigned int)total_skip);
    return total_skip;
}

// 内置曲源的歌词：MCP 下发那条路有 lyric_url，自搜只拿到酷我 rid，词得自己拉。
// 接口回 15KB 左右 JSON（内部 RAM 常态只剩 30~40KB，整块读进 std::string 会和 PCM 抢到 bad_alloc），
// 所以 body 落 PSRAM，只把 lineLyric/time 两个字段抽成歌词表。
bool Esp32Music::DownloadLyricsByRid(const std::string &rid)
{
    if (rid.empty())
    {
        return false;
    }

    try
    {
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (!http)
        {
            ESP_LOGE(TAG, "Lyric fetch: no http client");
            return false;
        }
        http->SetTimeout(15000);
        http->SetHeader("User-Agent",
                        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                        "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
        http->SetHeader("Referer", "http://www.kuwo.cn/");
        http->SetHeader("Accept", "*/*");

        std::string url = "http://m.kuwo.cn/newh5/singles/songinfoandlrc?musicId=" + rid + "&httpsStatus=1";
        if (!http->Open("GET", url))
        {
            ESP_LOGE(TAG, "Lyric fetch: connect failed");
            return false;
        }
        if (http->GetStatusCode() != 200)
        {
            ESP_LOGE(TAG, "Lyric fetch: HTTP %d", http->GetStatusCode());
            http->Close();
            return false;
        }

        PsramString body;
        char chunk[512];
        int n;
        while ((n = http->Read(chunk, sizeof(chunk))) > 0)
        {
            if (body.size() + n > 96 * 1024)
            {
                ESP_LOGW(TAG, "Lyric fetch: body over 96KB, stopped");
                break;
            }
            body.append(chunk, n);
        }
        http->Close();

        if (body.empty())
        {
            ESP_LOGW(TAG, "Lyric fetch: empty body for rid %s", rid.c_str());
            return false;
        }

        const size_t kMaxLines = 260;   // 一首歌实际 40~190 行，封顶只为兜住异常源
        const size_t kMaxChars = 96;    // 屏幕一行就能显下这么多，再长只浪费内部堆
        std::vector<std::pair<int, std::string>> parsed;
        parsed.reserve(96);

        const char *const kTextKey = "\"lineLyric\":\"";
        const char *const kTimeKey = "\"time\":\"";
        size_t pos = 0;
        while (parsed.size() < kMaxLines)
        {
            size_t tp = body.find(kTextKey, pos);
            if (tp == PsramString::npos)
            {
                break;
            }
            size_t i = tp + strlen(kTextKey);
            std::string line;
            line.reserve(32);
            while (i < body.size() && body[i] != '"')
            {
                if (body[i] == '\\' && i + 1 < body.size())
                {
                    char e = body[i + 1];
                    i += 2;
                    if (e == 'u')
                    {
                        i += 4; // \uXXXX 这类转义设备用不上，跳过
                        continue;
                    }
                    if (line.size() < kMaxChars)
                    {
                        line.push_back(e);
                    }
                    continue;
                }
                if (line.size() < kMaxChars)
                {
                    line.push_back(body[i]);
                }
                i++;
            }

            size_t tsp = body.find(kTimeKey, i);
            if (tsp == PsramString::npos || tsp > i + 64)
            {
                pos = i + 1;
                continue;
            }
            size_t j = tsp + strlen(kTimeKey);
            int seconds = 0;
            int frac = 0;
            int frac_digits = 0;
            bool dot = false;
            while (j < body.size() && body[j] != '"')
            {
                char c = body[j++];
                if (c == '.')
                {
                    dot = true;
                }
                else if (c >= '0' && c <= '9')
                {
                    if (!dot)
                    {
                        seconds = seconds * 10 + (c - '0');
                    }
                    else if (frac_digits < 2)
                    {
                        frac = frac * 10 + (c - '0');
                        frac_digits++;
                    }
                }
            }
            pos = j;
            if (line.empty())
            {
                continue;
            }
            int ms = seconds * 1000 + (frac_digits == 0 ? 0 : (frac_digits == 1 ? frac * 100 : frac * 10));
            parsed.push_back({ms, line});
        }

        if (parsed.empty())
        {
            ESP_LOGW(TAG, "Lyric fetch: no timed lines for rid %s", rid.c_str());
            return false;
        }

        int count = (int)parsed.size();
        {
            std::lock_guard<std::mutex> lock(lyrics_mutex_);
            lyrics_.swap(parsed);
        }
        ESP_LOGI(TAG, "Lyrics ready: %d lines from rid %s", count, rid.c_str());
        return true;
    }
    catch (const std::exception &e)
    {
        ESP_LOGE(TAG, "Lyric fetch failed: %s", e.what());
        return false;
    }
}

// 下载歌词
bool Esp32Music::DownloadLyrics(const std::string &lyric_url)
{
    ESP_LOGI(TAG, "Downloading lyrics from: %s", lyric_url.c_str());

    // 检查URL是否为空
    if (lyric_url.empty())
    {
        ESP_LOGE(TAG, "Lyric URL is empty!");
        return false;
    }

    // 添加重试逻辑
    const int max_retries = 3;
    int retry_count = 0;
    bool success = false;
    std::string lyric_content;
    std::string current_url = lyric_url;
    int redirect_count = 0;
    const int max_redirects = 5; // 最多允许5次重定向

    while (retry_count < max_retries && !success && redirect_count < max_redirects)
    {
        if (retry_count > 0)
        {
            ESP_LOGI(TAG, "Retrying lyric download (attempt %d of %d)", retry_count + 1, max_retries);
            // 重试前暂停一下
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // 使用Board提供的HTTP客户端
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(0);
        if (!http)
        {
            ESP_LOGE(TAG, "Failed to create HTTP client for lyric download");
            retry_count++;
            continue;
        }

        // 设置基本请求头
        http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
        http->SetHeader("Accept", "text/plain");

        // 添加ESP32认证头
        add_auth_headers(http.get());

        // 打开GET连接
        ESP_LOGI(TAG, "小智开源音乐固件qq交流群:826072986");
        if (!http->Open("GET", current_url))
        {
            ESP_LOGE(TAG, "Failed to open HTTP connection for lyrics");
            // 移除delete http; 因为unique_ptr会自动管理内存
            retry_count++;
            continue;
        }

        // 检查HTTP状态码
        int status_code = http->GetStatusCode();
        ESP_LOGI(TAG, "Lyric download HTTP status code: %d", status_code);

        // 处理重定向 - 由于Http类没有GetHeader方法，我们只能根据状态码判断
        if (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308)
        {
            // 由于无法获取Location头，只能报告重定向但无法继续
            ESP_LOGW(TAG, "Received redirect status %d but cannot follow redirect (no GetHeader method)", status_code);
            http->Close();
            retry_count++;
            continue;
        }

        // 非200系列状态码视为错误
        if (status_code < 200 || status_code >= 300)
        {
            ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
            http->Close();
            retry_count++;
            continue;
        }

        // 读取响应
        lyric_content.clear();
        char buffer[1024];
        int bytes_read;
        bool read_error = false;
        int total_read = 0;

        // 由于无法获取Content-Length和Content-Type头，我们不知道预期大小和内容类型
        ESP_LOGD(TAG, "Starting to read lyric content");

        while (true)
        {
            bytes_read = http->Read(buffer, sizeof(buffer) - 1);
            // ESP_LOGD(TAG, "Lyric HTTP read returned %d bytes", bytes_read); // 注释掉以减少日志输出

            if (bytes_read > 0)
            {
                buffer[bytes_read] = '\0';
                lyric_content += buffer;
                total_read += bytes_read;

                // 定期打印下载进度 - 改为DEBUG级别减少输出
                if (total_read % 4096 == 0)
                {
                    ESP_LOGD(TAG, "Downloaded %d bytes so far", total_read);
                }
            }
            else if (bytes_read == 0)
            {
                // 正常结束，没有更多数据
                ESP_LOGD(TAG, "Lyric download completed, total bytes: %d", total_read);
                success = true;
                break;
            }
            else
            {
                // bytes_read < 0，可能是ESP-IDF的已知问题
                // 如果已经读取到了一些数据，则认为下载成功
                if (!lyric_content.empty())
                {
                    ESP_LOGW(TAG, "HTTP read returned %d, but we have data (%d bytes), continuing", bytes_read, lyric_content.length());
                    success = true;
                    break;
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to read lyric data: error code %d", bytes_read);
                    read_error = true;
                    break;
                }
            }
        }

        http->Close();

        if (read_error)
        {
            retry_count++;
            continue;
        }

        // 如果成功读取数据，跳出重试循环
        if (success)
        {
            break;
        }
    }

    // 检查是否超过了最大重试次数
    if (retry_count >= max_retries)
    {
        ESP_LOGE(TAG, "Failed to download lyrics after %d attempts", max_retries);
        return false;
    }

    // 记录前几个字节的数据，帮助调试
    if (!lyric_content.empty())
    {
        size_t preview_size = std::min(lyric_content.size(), size_t(50));
        std::string preview = lyric_content.substr(0, preview_size);
        ESP_LOGD(TAG, "Lyric content preview (%d bytes): %s", lyric_content.length(), preview.c_str());
    }
    else
    {
        ESP_LOGE(TAG, "Failed to download lyrics or lyrics are empty");
        return false;
    }

    ESP_LOGI(TAG, "Lyrics downloaded successfully, size: %d bytes", lyric_content.length());
    return ParseLyrics(lyric_content);
}

// 解析歌词
bool Esp32Music::ParseLyrics(const std::string &lyric_content)
{
    ESP_LOGI(TAG, "Parsing lyrics content");

    // 使用锁保护lyrics_数组访问
    std::lock_guard<std::mutex> lock(lyrics_mutex_);

    lyrics_.clear();

    // 按行分割歌词内容
    std::istringstream stream(lyric_content);
    std::string line;

    while (std::getline(stream, line))
    {
        // 去除行尾的回车符
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        // 跳过空行
        if (line.empty())
        {
            continue;
        }

        // 解析LRC格式: [mm:ss.xx]歌词文本
        if (line.length() > 10 && line[0] == '[')
        {
            size_t close_bracket = line.find(']');
            if (close_bracket != std::string::npos)
            {
                std::string tag_or_time = line.substr(1, close_bracket - 1);
                std::string content = line.substr(close_bracket + 1);

                // 检查是否是元数据标签而不是时间戳
                // 元数据标签通常是 [ti:标题], [ar:艺术家], [al:专辑] 等
                size_t colon_pos = tag_or_time.find(':');
                if (colon_pos != std::string::npos)
                {
                    std::string left_part = tag_or_time.substr(0, colon_pos);

                    // 检查冒号左边是否是时间（数字）
                    bool is_time_format = true;
                    for (char c : left_part)
                    {
                        if (!isdigit(c))
                        {
                            is_time_format = false;
                            break;
                        }
                    }

                    // 如果不是时间格式，跳过这一行（元数据标签）
                    if (!is_time_format)
                    {
                        // 可以在这里处理元数据，例如提取标题、艺术家等信息
                        ESP_LOGD(TAG, "Skipping metadata tag: [%s]", tag_or_time.c_str());
                        continue;
                    }

                    // 是时间格式，解析时间戳
                    try
                    {
                        int minutes = std::stoi(tag_or_time.substr(0, colon_pos));
                        float seconds = std::stof(tag_or_time.substr(colon_pos + 1));
                        int timestamp_ms = minutes * 60 * 1000 + (int)(seconds * 1000);

                        // 安全处理歌词文本，确保UTF-8编码正确
                        std::string safe_lyric_text;
                        if (!content.empty())
                        {
                            // 创建安全副本并验证字符串
                            safe_lyric_text = content;
                            // 确保字符串以null结尾
                            safe_lyric_text.shrink_to_fit();
                        }

                        lyrics_.push_back(std::make_pair(timestamp_ms, safe_lyric_text));

                        if (!safe_lyric_text.empty())
                        {
                            // 限制日志输出长度，避免中文字符截断问题
                            size_t log_len = std::min(safe_lyric_text.length(), size_t(50));
                            std::string log_text = safe_lyric_text.substr(0, log_len);
                            ESP_LOGD(TAG, "Parsed lyric: [%d ms] %s", timestamp_ms, log_text.c_str());
                        }
                        else
                        {
                            ESP_LOGD(TAG, "Parsed lyric: [%d ms] (empty)", timestamp_ms);
                        }
                    }
                    catch (const std::exception &e)
                    {
                        ESP_LOGW(TAG, "Failed to parse time: %s", tag_or_time.c_str());
                    }
                }
            }
        }
    }

    // 按时间戳排序
    std::sort(lyrics_.begin(), lyrics_.end());

    ESP_LOGI(TAG, "Parsed %d lyric lines (stack high-water mark left %u bytes)",
             (int)lyrics_.size(), (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return !lyrics_.empty();
}

bool Esp32Music::StartLyricsWithContent(const std::string &lrc)
{
    if (lrc.empty())
    {
        return false;
    }
    if (display_mode_ != DISPLAY_MODE_LYRICS)
    {
        ESP_LOGI(TAG, "Inline lyrics given but spectrum display mode is active, skipping");
        return true;
    }

    if (is_lyric_running_)
    {
        is_lyric_running_ = false;
        if (lyric_thread_.joinable())
        {
            lyric_thread_.join();
        }
    }

    // 不在调用者线程解析：ParseLyrics 吃掉 3KB+ 栈，MCP handler 线程不保证有这么多
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        pending_lyric_lrc_ = lrc;
        lyrics_.clear();
    }

    is_lyric_running_ = true;
    current_lyric_index_ = -1;
    StartBgThread(lyric_thread_, "lyric", [this]() { LyricDisplayThread(); }, 12 * 1024);
    return true;
}

// 歌词显示线程
void Esp32Music::LyricDisplayThread()
{
    ESP_LOGI(TAG, "Lyric display thread started");

    // 歌词正文已随 MCP 消息下发并解析过（lyrics_ 非空）时不必再拉 URL —— 跨公网场景下
    // current_lyric_url_ 是服务端的局域网地址，设备根本连不上。
    std::string pending;
    bool need_download;
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        pending.swap(pending_lyric_lrc_);
        need_download = lyrics_.empty();
    }
    if (!pending.empty())
    {
        if (!ParseLyrics(pending))
        {
            ESP_LOGW(TAG, "Inline lyrics have no timestamped lines, ignored");
            is_lyric_running_ = false;
            return;
        }
    }
    else if (need_download)
    {
        bool ok = current_lyric_rid_.empty() ? DownloadLyrics(current_lyric_url_)
                                             : DownloadLyricsByRid(current_lyric_rid_);
        if (!ok)
        {
            ESP_LOGE(TAG, "Failed to download or parse lyrics");
            is_lyric_running_ = false;
            return;
        }
    }

    // 定期检查是否需要更新显示(频率可以降低)
    while (is_lyric_running_ && is_playing_)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ESP_LOGI(TAG, "Lyric display thread finished");
}

void Esp32Music::UpdateLyricDisplay(int64_t current_time_ms)
{
    std::lock_guard<std::mutex> lock(lyrics_mutex_);

    if (lyrics_.empty())
    {
        return;
    }

    // 查找当前应该显示的歌词
    int new_lyric_index = -1;

    // 从当前歌词索引开始查找，提高效率
    int start_index = (current_lyric_index_.load() >= 0) ? current_lyric_index_.load() : 0;

    // 正向查找：找到最后一个时间戳小于等于当前时间的歌词
    for (int i = start_index; i < (int)lyrics_.size(); i++)
    {
        if (lyrics_[i].first <= current_time_ms)
        {
            new_lyric_index = i;
        }
        else
        {
            break; // 时间戳已超过当前时间
        }
    }

    // 如果没有找到(可能当前时间比第一句歌词还早)，显示空
    if (new_lyric_index == -1)
    {
        new_lyric_index = -1;
    }

    // 如果歌词索引发生变化，更新显示
    if (new_lyric_index != current_lyric_index_)
    {
        const int prev_index = current_lyric_index_.load();
        current_lyric_index_ = new_lyric_index;

        auto &board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display)
        {
            std::string lyric_text;

            if (current_lyric_index_ >= 0 && current_lyric_index_ < (int)lyrics_.size())
            {
                lyric_text = lyrics_[current_lyric_index_].second;
            }

            // 显示歌词
            display->SetChatMessage("lyric", lyric_text.c_str());

            // 第一句上屏记一次 INFO：否则「歌词到底有没有真画出来」只能靠盯屏幕
            if (prev_index < 0 && !lyric_text.empty())
            {
                ESP_LOGI(TAG, "Lyric on screen [%d/%d]: %s", current_lyric_index_.load(),
                         (int)lyrics_.size(), lyric_text.c_str());
            }

            ESP_LOGD(TAG, "Lyric update at %dms: %s",
                     (int)current_time_ms,
                     lyric_text.empty() ? "(no lyric)" : lyric_text.c_str());
        }
    }
}

// 删除复杂的认证初始化方法，使用简单的静态函数

// 删除复杂的类方法，使用简单的静态函数

/**
 * @brief 添加认证头到HTTP请求
 * @param http_client HTTP客户端指针
 *
 * 添加的认证头包括：
 * - X-MAC-Address: 设备MAC地址
 * - X-Chip-ID: 设备芯片ID
 * - X-Timestamp: 当前时间戳
 * - X-Dynamic-Key: 动态生成的密钥
 */
// 删除复杂的AddAuthHeaders方法，使用简单的静态函数

// 删除复杂的认证验证和配置方法，使用简单的静态函数

// 显示模式控制方法实现
void Esp32Music::SetDisplayMode(DisplayMode mode)
{
    DisplayMode old_mode = display_mode_.load();
    display_mode_ = mode;

    ESP_LOGI(TAG, "Display mode changed from %s to %s",
             (old_mode == DISPLAY_MODE_SPECTRUM) ? "SPECTRUM" : "LYRICS",
             (mode == DISPLAY_MODE_SPECTRUM) ? "SPECTRUM" : "LYRICS");
}
