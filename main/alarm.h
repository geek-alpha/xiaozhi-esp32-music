#ifndef ALARM_H
#define ALARM_H

#include <esp_timer.h>

#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

/**
 * 板子本地闹钟：到点由固件自己播歌，不依赖服务端在线、不依赖 WS 连接。
 *
 * 时间基准是 ota.cc 里从服务器下发的 server_time（settimeofday），
 * 所以「早上七点」这种绝对时刻要求设备至少成功激活过一次；没同步过时间时
 * 只允许用倒计时（SetTimer），绝对时刻会直接报错而不是算错一个钟头。
 *
 * 闹钟与默认铃声会存进 NVS，重启后由 Restore() 补回来；倒计时也存，
 * 存的是「到点那一刻的墙钟秒」——而且只在时间已同步时才存，否则算不出还有多久。
 */
class Alarm {
public:
    static Alarm& GetInstance();

    // 绝对时刻。weekdays 是星期位掩码：bit0=周日、bit1=周一 … bit6=周六，
    // 0x7F=每天、0x3E=工作日、0x41=周末、0=只在下一个到点响一次。
    std::string SetClock(int hour, int minute, int weekdays, const std::string& audio_url,
                         const std::string& song_name, const std::string& artist,
                         const std::string& label = "");
    // 倒计时。
    std::string SetTimer(int seconds, const std::string& audio_url,
                         const std::string& song_name, const std::string& artist,
                         const std::string& label = "");
    // id <= 0 表示取消全部
    std::string Cancel(int id);
    // 换铃声。id <= 0 绕组全局默认铃声（以后新建的闹钟都用它）；id > 0 只改那一个，
    // 定时器一动不动。
    std::string SetRing(int id, const std::string& audio_url, const std::string& song_name,
                        const std::string& artist);
    std::string List() const;
    // 最近一次到点的提醒，JSON 对象或 null。设备把「哪件事、几点到点」记在这里，
    // AI 随时能查到；比在播完那一刻用 detect 硬塞长文本可靠（服务端只收唤醒词）。
    std::string LastFiredJson() const;

    // 从 NVS 恢复上次的默认铃声与闹钟（含尚未到点的倒计时）。时间还没同步时
    // 整体跳过，数据留在 NVS 等下次联网。幂等。
    void Restore();

    // time(NULL) 年份 >= 2025 才算时间可信
    static bool SystemTimeReady();

private:
    Alarm();
    ~Alarm();
    Alarm(const Alarm&) = delete;
    Alarm& operator=(const Alarm&) = delete;

    struct Entry {
        int id = 0;
        bool fired = false;
        int hour = 0;
        int minute = 0;
        // bit0=周日（对齐 tm_wday）… bit6=周六；0 表示只响这一次
        uint8_t weekdays = 0;
        std::string audio_url;
        std::string song_name;
        std::string artist;
        // 到点要提醒的那件事（「吃药」「开会」），可为空
        std::string label;
        // 倒计时的到点墙钟秒；绝对时刻类存 0。重启后靠它算还剩多久
        time_t fire_epoch = 0;
        esp_timer_handle_t timer = nullptr;
    };

    int Add(uint8_t weekdays, int hour, int minute, int64_t delay_us, const std::string& audio_url,
            const std::string& song_name, const std::string& artist,
            const std::string& label, time_t fire_epoch = 0);
    void OnFire(int id);
    void CleanupLocked();
    // 调用方须已持 mutex_
    void SaveEntriesLocked() const;
    // 下一个命中 weekdays 的 hour:minute 距今多少微秒；weekdays=0 只看今天/明天
    static int64_t UsUntil(int hour, int minute, uint8_t weekdays);
    // 「每天」「工作日」「周一三五」这类说法，用在回复里
    static std::string WeekdaysText(uint8_t weekdays);
    // 没指定放什么就套用全局默认铃声
    void ApplyDefaultRing(std::string& audio_url, std::string& song_name,
                          std::string& artist) const;

    int next_id_ = 1;
    std::vector<Entry*> entries_;
    std::string last_fired_label_;
    std::string last_fired_at_;
    mutable std::mutex mutex_;
    bool restored_ = false;

    std::string default_url_;
    std::string default_song_;
    std::string default_artist_;
    mutable std::mutex ring_mutex_;
};

#endif
