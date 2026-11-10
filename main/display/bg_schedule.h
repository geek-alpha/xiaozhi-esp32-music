#ifndef BG_SCHEDULE_H
#define BG_SCHEDULE_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class Display;

/**
 * 屏幕背景色时间表：「9:00~12:00 黄、15:00~20:00 紫」这种，到点自己换底色。
 *
 * 规则存 NVS（display/bg_rules），后台任务每 30 秒按本地时间评估一次：命中就覆盖
 * 背景色，离开区间恢复主题默认色。只在结果变化时才下发 —— SetTheme 会重刷整屏，
 * 每 30 秒白刷一次，用户看到的就是闪。
 *
 * 时间没同步（Alarm::SystemTimeReady 为假）时整轮跳过：按 1970 年算会先按错时刻
 * 刷一遍，NTP 回来再刷一遍，比不换还乱。
 */
class BgSchedule {
public:
    struct Rule {
        int start = 0;  // 当天第几分钟，0~1439
        int end = 0;    // 开区间：走到 end 这一分钟就算离开，恢复主题色
        uint32_t rgb = 0;
    };

    static BgSchedule& GetInstance();

    // 绑定显示器、读回 NVS 规则、起轮询任务。幂等，display 为空返回 false
    bool Start(Display* display);
    // 整份替换。json 是规范化数组 [{"s":540,"e":720,"rgb":16776960}]（mcp 层已经把
    // 「上午9点」「黄色」翻译成数字了）。返回空串=成功，否则是给 AI 看的失败原因
    std::string SetRules(const std::string& json);
    std::string ClearRules();
    // 当前规则，带 HH:MM 方便 AI 直接念给人听
    std::string DumpJson() const;

private:
    BgSchedule() = default;
    BgSchedule(const BgSchedule&) = delete;
    BgSchedule& operator=(const BgSchedule&) = delete;

    static void TaskEntry(void* arg);
    void Tick();
    static std::string ParseRules(const std::string& json, std::vector<Rule>* out);
    std::string DumpLocked() const;
    void SaveLocked() const;

    mutable std::mutex mutex_;
    std::vector<Rule> rules_;
    Display* display_ = nullptr;
    bool started_ = false;
    // 上一次真正下发过的状态，用来判断「有没有变化」
    bool have_applied_ = false;
    bool applied_hit_ = false;
    uint32_t applied_rgb_ = 0;
};

#endif  // BG_SCHEDULE_H
