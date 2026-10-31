#include "alarm.h"

#include <cJSON.h>
#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "boards/common/esp32_music.h"
#include "settings.h"

#define TAG "Alarm"

namespace {

std::string Fail(const std::string& msg) {
    return "{\"success\": false, \"message\": \"" + msg + "\"}";
}

std::string Ok(const std::string& msg) {
    return "{\"success\": true, \"message\": \"" + msg + "\"}";
}

// 没点名放什么时统一放《好运来》。这里只存歌名、不焊 CDN 直链：酷我的 url 带 hash，
// 几周后必然 404，焊进固件就是给未来的自己埋一颗哑弹。
constexpr char kDefaultRingSong[] = "好运来";
constexpr char kDefaultRingArtist[] = "祖海";

constexpr char kNvsNs[] = "alarm";
constexpr char kKeyRingUrl[] = "ring_url";
constexpr char kKeyRingSong[] = "ring_song";
constexpr char kKeyRingArtist[] = "ring_artist";
constexpr char kKeyList[] = "list";
constexpr char kKeyNextId[] = "next_id";

// 上报给 AI 的事件里必须有「几点」：只说事情它接不上时间线，反应就只能客套
std::string NowHmText() {
    time_t now = time(nullptr);
    struct tm tm_now = {};
    localtime_r(&now, &tm_now);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M", &tm_now);
    return buf;
}

// 中文 label 直接拼进 URL 会断：设备侧只能自己把 UTF-8 逐字节转义
std::string UrlEncode(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

// 提醒话交给服务端本地 TTS 合成：本机没有中文语音引擎，只有一段段 MP3 能播。
// 返回空串表示没配服务地址 —— 那就只上屏，不能假装会说话。
std::string TtsUrlFor(const std::string& label) {
#ifdef CONFIG_XZ_MUSIC_API_BASE
    std::string base = CONFIG_XZ_MUSIC_API_BASE;
    if (base.empty() || label.empty()) {
        return "";
    }
    return base + "/api/music/xz/tts?text=" + UrlEncode("提醒你，" + label);
#else
    (void)label;
    return "";
#endif
}

}  // namespace

Alarm::Alarm() : default_song_(kDefaultRingSong), default_artist_(kDefaultRingArtist) {}

Alarm& Alarm::GetInstance() {
    static Alarm instance;
    return instance;
}

Alarm::~Alarm() {
    for (auto* e : entries_) {
        if (e->timer) {
            esp_timer_stop(e->timer);
            esp_timer_delete(e->timer);
        }
        delete e;
    }
    entries_.clear();
}

bool Alarm::SystemTimeReady() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    return t.tm_year + 1900 >= 2025;
}

int64_t Alarm::UsUntil(int hour, int minute, uint8_t weekdays) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    // 逐天往后找第一个命中 weekdays 的日子；0 表示只响这一次（今天过了就明天）
    for (int day = 0; day <= 7; ++day) {
        struct tm want = t;
        want.tm_hour = hour;
        want.tm_min = minute;
        want.tm_sec = 0;
        want.tm_mday += day;
        want.tm_isdst = -1;
        time_t target = mktime(&want);  // mktime 顺手把 tm_wday 归一化好
        int64_t diff = static_cast<int64_t>(target - now);
        if (diff <= 0) {
            continue;
        }
        if (weekdays == 0 || (weekdays & (1 << want.tm_wday))) {
            return diff * 1000000LL;
        }
    }
    return 86400LL * 1000000LL;
}

std::string Alarm::WeekdaysText(uint8_t weekdays) {
    if (weekdays == 0) {
        return "只响一次";
    }
    if (weekdays == 0x7F) {
        return "每天";
    }
    if (weekdays == 0x3E) {
        return "工作日";
    }
    if (weekdays == 0x41) {
        return "周末";
    }
    static const char* kNames[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    std::string out = "每";
    for (int i = 0; i < 7; ++i) {
        if (weekdays & (1 << i)) {
            out += kNames[i];
        }
    }
    return out;
}

void Alarm::ApplyDefaultRing(std::string& audio_url, std::string& song_name,
                             std::string& artist) const {
    if (!audio_url.empty() || !song_name.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(ring_mutex_);
    audio_url = default_url_;
    song_name = default_song_;
    artist = default_artist_;
}

// 一次性闹钟响完把 timer 句柄留到下次 Set/Cancel 再回收：ESP-IDF 不允许在
// 定时器自己的回调里 delete 自己，硬删会卡死。
void Alarm::CleanupLocked() {
    auto it = entries_.begin();
    while (it != entries_.end()) {
        Entry* e = *it;
        if (!e->fired) {
            ++it;
            continue;
        }
        if (e->timer) {
            esp_timer_stop(e->timer);
            esp_timer_delete(e->timer);
        }
        delete e;
        it = entries_.erase(it);
    }
}

int Alarm::Add(uint8_t weekdays, int hour, int minute, int64_t delay_us, const std::string& audio_url,
               const std::string& song_name, const std::string& artist, const std::string& label,
               time_t fire_epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    CleanupLocked();

    auto* e = new Entry();
    e->id = next_id_++;
    e->weekdays = weekdays;
    e->hour = hour;
    e->minute = minute;
    e->fire_epoch = fire_epoch;
    e->audio_url = audio_url;
    e->song_name = song_name;
    e->artist = artist;
    e->label = label;

    // arg 只传 id 不传指针：回调线程和 Cancel 并发时不会有悬垂指针
    esp_timer_create_args_t args = {
        .callback =
            [](void* arg) {
                Alarm::GetInstance().OnFire(static_cast<int>(reinterpret_cast<intptr_t>(arg)));
            },
        .arg = reinterpret_cast<void*>(static_cast<intptr_t>(e->id)),
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&args, &e->timer);
    if (err != ESP_OK) {
        delete e;
        return 0;
    }
    err = esp_timer_start_once(e->timer, delay_us);
    if (err != ESP_OK) {
        esp_timer_delete(e->timer);
        delete e;
        return 0;
    }

    entries_.push_back(e);
    SaveEntriesLocked();
    // %lld 在这版 newlib 下打不出来（会丢成 "ld"），拆成秒数打印，别让日志误导判断
    ESP_LOGI(TAG, "alarm #%d armed, fire in %d s (weekdays=0x%02x)", e->id,
             static_cast<int>(delay_us / 1000000LL), static_cast<unsigned>(weekdays));
    return e->id;
}

void Alarm::OnFire(int id) {
    std::string audio_url;
    std::string song_name;
    std::string artist;
    std::string label;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(entries_.begin(), entries_.end(),
                               [id](const Entry* e) { return e->id == id; });
        if (it == entries_.end()) {
            return;  // 已经取消了
        }
        Entry* e = *it;
        audio_url = e->audio_url;
        song_name = e->song_name;
        artist = e->artist;
        label = e->label;

        if (e->weekdays != 0) {
            // 响完直接排到下一个命中的星期，不再兜底加一天：掩码自己含周末/工作日
            int64_t next = UsUntil(e->hour, e->minute, e->weekdays);
            esp_timer_stop(e->timer);
            esp_timer_start_once(e->timer, next);
        } else {
            e->fired = true;
        }
        SaveEntriesLocked();
    }

    // 完整事件先存下：到点的是什么事、几点。AI 随时能通过 self.alarm.list 查它。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_fired_label_ = label.empty() ? "闹钟" : label;
        last_fired_at_ = NowHmText();
    }

    ESP_LOGI(TAG, "alarm #%d fired", id);

    // 播放要回主任务：PlayDirect 会起线程、动显示和采样率，不能在 esp_timer 任务里做
    const std::string tts_url = TtsUrlFor(label);
    Application::GetInstance().Schedule([audio_url, song_name, artist, label, tts_url]() {
        bool has_song = !audio_url.empty() || !song_name.empty();
        // 会出声就别再响提示音：Alert 的音效和后面抢同一条音频通道
        bool will_speak = has_song || !tts_url.empty();
        if (!label.empty()) {
            Application::GetInstance().Alert("提醒", label.c_str(), "happy",
                                            will_speak ? "" : Lang::Sounds::P3_VIBRATION);
        }
        auto music = Board::GetInstance().GetMusic();
        if (!music) {
            ESP_LOGW(TAG, "no music player on this board");
            return;
        }
        auto* esp32_music = static_cast<Esp32Music*>(music);
        // 播之前先挂上事件上下文：播完那一刻上报的是这件事和到点时间，不是歌名。
        // plays 算法要和下面分支一致：先念再响铃是 2，只念或只响铃是 1。
        if (will_speak) {
            int plays = (tts_url.empty() ? 0 : 1) + (has_song ? 1 : 0);
            esp32_music->SetReminderContext(label.empty() ? "闹钟" : label, NowHmText(), plays);
        }

        // 先说事情再放铃声：等语音播完的几秒里不能占着主任务，
        // 一卡住按键、唤醒词、上屏全停，所以等待放在独立线程。
        if (!tts_url.empty()) {
            std::thread([esp32_music, tts_url, audio_url, song_name, artist]() {
                Application::GetInstance().Schedule([esp32_music, tts_url]() {
                    esp32_music->PlayDirect(tts_url, "", "", "提醒", "");
                });
                // 先等它真的出声（联网+合成要 1~3 秒），再等它播完；
                // 两个超时都是止损：语音拉不回来也得把铃声响掉，闹钟不能装死。
                int waited = 0;
                while (!esp32_music->IsPlaying() && waited < 10000) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                    waited += 200;
                }
                if (waited >= 10000) {
                    ESP_LOGW(TAG, "reminder voice never started, falling through to ring");
                }
                waited = 0;
                while (esp32_music->IsPlaying() && waited < 60000) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                    waited += 200;
                }
                ESP_LOGI(TAG, "reminder voice done, ringing");

                if (audio_url.empty() && song_name.empty()) {
                    return;
                }
                Application::GetInstance().Schedule([esp32_music, audio_url, song_name, artist]() {
                    if (!audio_url.empty()) {
                        esp32_music->PlayDirect(audio_url, "", "", song_name, artist);
                    } else {
                        Board::GetInstance().GetMusic()->Download(song_name, artist);
                    }
                });
            }).detach();
            return;
        }

        if (!audio_url.empty()) {
            esp32_music->PlayDirect(audio_url, "", "", song_name, artist);
        } else if (!song_name.empty()) {
            music->Download(song_name, artist);
        }
    });
}

std::string Alarm::SetClock(int hour, int minute, int weekdays, const std::string& audio_url,
                            const std::string& song_name, const std::string& artist,
                            const std::string& label) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return Fail("时间不合法，hour 用 0-23，minute 用 0-59");
    }
    if (weekdays < 0 || weekdays > 0x7F) {
        return Fail("weekdays 是星期位掩码，取 0-127：0=只响一次，127=每天，62=工作日");
    }
    if (!SystemTimeReady()) {
        return Fail("设备还没同步到网络时间，绝对时刻算不出来；先联网激活一次，或者改用倒计时");
    }

    std::string url = audio_url, song = song_name, who = artist;
    // 有 label 也要套铃声：先念事情再放歌，两段串行不会互相盖掉。
    // 只念一句就等于把用户设的铃声吞了——「提醒我起床」本来就要响音乐。
    ApplyDefaultRing(url, song, who);

    auto mask = static_cast<uint8_t>(weekdays);
    int64_t delay = UsUntil(hour, minute, mask);
    int id = Add(mask, hour, minute, delay, url, song, who, label);
    if (id == 0) {
        return Fail("定时器创建失败");
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "闹钟已设置 #%d %02d:%02d（%s），%.0f 分钟后响", id, hour, minute,
             WeekdaysText(mask).c_str(), delay / 60000000.0);
    return Ok(label.empty() ? std::string(buf) : std::string(buf) + "，提醒你：" + label);
}

std::string Alarm::SetTimer(int seconds, const std::string& audio_url,
                            const std::string& song_name, const std::string& artist,
                            const std::string& label) {
    if (seconds <= 0) {
        return Fail("seconds 必须大于 0");
    }
    if (seconds > 24 * 3600) {
        return Fail("倒计时最长 24 小时，再长就用绝对时刻");
    }
    std::string url = audio_url, song = song_name, who = artist;
    ApplyDefaultRing(url, song, who);
    // hour/minute 对倒计时没意义，存 -1 让 List 只显示秒数。
    // 同时记下到点的墙钟秒：重启后时间已同步就能接着倒，不然刷个固件就白设了。
    time_t fire_epoch = SystemTimeReady() ? time(nullptr) + seconds : 0;
    int id = Add(0, -1, -1, static_cast<int64_t>(seconds) * 1000000LL, url, song, who, label, fire_epoch);
    if (id == 0) {
        return Fail("定时器创建失败");
    }

    char buf[80];
    snprintf(buf, sizeof(buf), "倒计时已设置 #%d，%d 秒后响", id, seconds);
    return Ok(label.empty() ? std::string(buf) : std::string(buf) + "，提醒你：" + label);
}

std::string Alarm::SetRing(int id, const std::string& audio_url, const std::string& song_name,
                           const std::string& artist) {
    if (id <= 0) {
        if (audio_url.empty() && song_name.empty()) {
            return Fail("改默认铃声要说清换成什么：给 song_name（歌名）或 audio_url（直链）");
        }
        std::string song;
        std::string who;
        std::string url;
        {
            std::lock_guard<std::mutex> lock(ring_mutex_);
            default_url_ = audio_url;
            if (!song_name.empty()) {
                default_song_ = song_name;
            }
            if (!artist.empty()) {
                default_artist_ = artist;
            }
            song = default_song_;
            who = default_artist_;
            url = default_url_;
        }
        {
            Settings settings(kNvsNs, true);
            settings.SetString(kKeyRingUrl, url);
            settings.SetString(kKeyRingSong, song);
            settings.SetString(kKeyRingArtist, who);
        }
        return Ok("默认铃声已改成《" + song + "》（" + who +
                  "），以后不点名放什么的闹钟都放它");
    }

    if (audio_url.empty() && song_name.empty()) {
        return Fail("要说清换成什么歌：给 song_name 或 audio_url");
    }

    // 只换铃声，定时器一动不动：用户想改的是到点放什么，不是什么时候响
    std::lock_guard<std::mutex> lock(mutex_);
    CleanupLocked();
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [id](const Entry* e) { return e->id == id; });
    if (it == entries_.end()) {
        return Fail("没有这个闹钟");
    }
    Entry* e = *it;
    // 旧直链必须清掉：OnFire 里 PlayDirect 优先，留着它新歌永远不生效
    e->audio_url = audio_url;
    e->song_name = song_name;
    e->artist = artist;
    SaveEntriesLocked();

    ESP_LOGI(TAG, "alarm #%d ring changed to %s", id,
             e->song_name.empty() ? e->audio_url.c_str() : e->song_name.c_str());
    std::string what = e->song_name.empty()
                           ? e->audio_url
                           : e->song_name + (e->artist.empty() ? "" : "（" + e->artist + "）");
    return Ok("闹钟 #" + std::to_string(id) + " 的铃声已改成 " + what + "，响铃时间不变");
}

std::string Alarm::Cancel(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    CleanupLocked();

    int removed = 0;
    if (id <= 0) {
        for (auto* e : entries_) {
            if (e->timer) {
                esp_timer_stop(e->timer);
                esp_timer_delete(e->timer);
            }
            delete e;
            removed++;
        }
        entries_.clear();
    } else {
        auto it = std::find_if(entries_.begin(), entries_.end(),
                               [id](const Entry* e) { return e->id == id; });
        if (it != entries_.end()) {
            Entry* e = *it;
            if (e->timer) {
                esp_timer_stop(e->timer);
                esp_timer_delete(e->timer);
            }
            delete e;
            entries_.erase(it);
            removed = 1;
        }
    }

    if (removed == 0) {
        return Fail("没有这个闹钟");
    }
    SaveEntriesLocked();
    char buf[48];
    snprintf(buf, sizeof(buf), "已取消 %d 个闹钟", removed);
    return Ok(buf);
}

std::string Alarm::List() const {
    std::lock_guard<std::mutex> lock(mutex_);

    cJSON* arr = cJSON_CreateArray();
    for (auto* e : entries_) {
        if (e->fired) {
            continue;
        }
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", e->id);
        if (e->hour >= 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%02d:%02d", e->hour, e->minute);
            cJSON_AddStringToObject(o, "clock", buf);
        }
        cJSON_AddBoolToObject(o, "daily", e->weekdays == 0x7F);
        cJSON_AddNumberToObject(o, "weekdays", e->weekdays);
        cJSON_AddStringToObject(o, "repeat", WeekdaysText(e->weekdays).c_str());
        if (!e->label.empty()) {
            cJSON_AddStringToObject(o, "label", e->label.c_str());
        }
        if (!e->audio_url.empty()) {
            cJSON_AddStringToObject(o, "audio_url", e->audio_url.c_str());
        }
        if (!e->song_name.empty()) {
            cJSON_AddStringToObject(o, "song_name", e->song_name.c_str());
        }
        if (!e->artist.empty()) {
            cJSON_AddStringToObject(o, "artist", e->artist.c_str());
        }
        cJSON_AddItemToArray(arr, o);
    }

    char* text = cJSON_PrintUnformatted(arr);
    std::string out = text ? text : "[]";
    cJSON_free(text);
    cJSON_Delete(arr);
    return out;
}

std::string Alarm::LastFiredJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_fired_at_.empty()) {
        return "null";
    }
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "reminder_due");
    cJSON_AddStringToObject(o, "label", last_fired_label_.c_str());
    cJSON_AddStringToObject(o, "at", last_fired_at_.c_str());
    char* text = cJSON_PrintUnformatted(o);
    std::string out = text ? text : "null";
    cJSON_free(text);
    cJSON_Delete(o);
    return out;
}

void Alarm::SaveEntriesLocked() const {
    cJSON* arr = cJSON_CreateArray();
    for (auto* e : entries_) {
        // 响过的一次性闹钟就完事，不值得占 NVS
        if (e->fired) {
            continue;
        }
        // 倒计时没拿到墙钟基准（时间没同步）就存不了：存个 1970 的秒数只会算错
        if (e->hour < 0 && e->fire_epoch <= 0) {
            continue;
        }
        cJSON* o = cJSON_CreateObject();
        if (e->hour < 0) {
            cJSON_AddNumberToObject(o, "fire_at", static_cast<double>(e->fire_epoch));
        } else {
            cJSON_AddNumberToObject(o, "hour", e->hour);
            cJSON_AddNumberToObject(o, "minute", e->minute);
            cJSON_AddNumberToObject(o, "weekdays", e->weekdays);
        }
        if (!e->label.empty()) {
            cJSON_AddStringToObject(o, "label", e->label.c_str());
        }
        if (!e->audio_url.empty()) {
            cJSON_AddStringToObject(o, "audio_url", e->audio_url.c_str());
        }
        if (!e->song_name.empty()) {
            cJSON_AddStringToObject(o, "song_name", e->song_name.c_str());
        }
        if (!e->artist.empty()) {
            cJSON_AddStringToObject(o, "artist", e->artist.c_str());
        }
        cJSON_AddItemToArray(arr, o);
    }

    char* text = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    Settings settings(kNvsNs, true);
    settings.SetString(kKeyList, text ? text : "[]");
    settings.SetInt(kKeyNextId, next_id_);
    if (text) {
        cJSON_free(text);
    }
}

void Alarm::Restore() {
    if (restored_) {
        return;
    }

    Settings settings(kNvsNs, true);
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        std::string song = settings.GetString(kKeyRingSong);
        if (!song.empty()) {
            default_song_ = song;
        }
        std::string artist = settings.GetString(kKeyRingArtist);
        if (!artist.empty()) {
            default_artist_ = artist;
        }
        default_url_ = settings.GetString(kKeyRingUrl);
    }

    std::string list = settings.GetString(kKeyList);
    if (list.empty()) {
        return;
    }
    // 时间没同步就算不出「还有多久到七点」，硬算会响在错误的时刻；留在 NVS 等下次
    if (!SystemTimeReady()) {
        ESP_LOGW(TAG, "alarms in NVS but time not synced, retry later");
        return;
    }
    // 到这才算恢复完成：时间没就绪时允许下次再试
    restored_ = true;

    cJSON* arr = cJSON_Parse(list.c_str());
    if (!arr) {
        ESP_LOGW(TAG, "bad alarm list in NVS, ignored");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next_id_ = std::max(next_id_, static_cast<int>(settings.GetInt(kKeyNextId, 1)));
    }

    auto str_field = [](cJSON* o, const char* key) {
        cJSON* v = cJSON_GetObjectItem(o, key);
        return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
    };

    int count = 0;
    cJSON* o = nullptr;
    cJSON_ArrayForEach(o, arr) {
        // 倒计时条目只有 fire_at：按剩余秒数接着倒，已经过点就丢掉
        cJSON* jf = cJSON_GetObjectItem(o, "fire_at");
        if (cJSON_IsNumber(jf)) {
            time_t at = static_cast<time_t>(jf->valuedouble);
            time_t now = time(nullptr);
            if (at > now) {
                SetTimer(static_cast<int>(at - now), str_field(o, "audio_url"), str_field(o, "song_name"),
                         str_field(o, "artist"), str_field(o, "label"));
                count++;
            } else {
                ESP_LOGW(TAG, "countdown already due while off, dropped");
            }
            continue;
        }
        cJSON* jh = cJSON_GetObjectItem(o, "hour");
        cJSON* jm = cJSON_GetObjectItem(o, "minute");
        if (!cJSON_IsNumber(jh) || !cJSON_IsNumber(jm)) {
            continue;
        }
        // 老固件只存了 daily 布尔，没 weekdays 字段：读到它就当成每天
        cJSON* jw = cJSON_GetObjectItem(o, "weekdays");
        int wd = cJSON_IsNumber(jw)
                     ? jw->valueint
                     : (cJSON_IsTrue(cJSON_GetObjectItem(o, "daily")) ? 0x7F : 0);
        SetClock(jh->valueint, jm->valueint, wd, str_field(o, "audio_url"), str_field(o, "song_name"),
                 str_field(o, "artist"), str_field(o, "label"));
        count++;
    }
    cJSON_Delete(arr);
    ESP_LOGI(TAG, "restored %d alarm(s) from NVS", count);
}
