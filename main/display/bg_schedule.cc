#include "bg_schedule.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstdio>
#include <ctime>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "alarm.h"
#include "display.h"
#include "settings.h"

#define TAG "BgSchedule"

namespace {

constexpr char kNvsNs[] = "display";
constexpr char kKeyRules[] = "bg_rules";
constexpr size_t kMaxRules = 8;
constexpr int kPollMs = 30000;
constexpr int kMinutesPerDay = 24 * 60;

int NowMinuteOfDay() {
    time_t now = time(nullptr);
    struct tm t = {};
    localtime_r(&now, &t);
    return t.tm_hour * 60 + t.tm_min;
}

// 分钟数除以一天的余数，负的也能归到 0~1439
int WrapMinute(int m) {
    int r = m % kMinutesPerDay;
    return r < 0 ? r + kMinutesPerDay : r;
}

std::string HhMm(int minutes) {
    if (minutes == kMinutesPerDay) {
        return "24:00";  // end 写 24:00 时保留原值，wrap 成 00:00 会让人看成全天
    }
    const int m = WrapMinute(minutes);
    // 手写两位数：snprintf 的 %02d 让 -Werror=format-truncation 猜不出范围，
    // 它只看到 int 就当成可能有 9 位数，直接编译失败
    char buf[8];
    buf[0] = static_cast<char>('0' + (m / 60) / 10);
    buf[1] = static_cast<char>('0' + (m / 60) % 10);
    buf[2] = ':';
    buf[3] = static_cast<char>('0' + (m % 60) / 10);
    buf[4] = static_cast<char>('0' + (m % 60) % 10);
    buf[5] = '\0';
    return std::string(buf);
}

}  // namespace

BgSchedule& BgSchedule::GetInstance() {
    static BgSchedule instance;
    return instance;
}

bool BgSchedule::Start(Display* display) {
    if (display == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    display_ = display;
    if (started_) {
        return true;
    }
    Settings settings(kNvsNs, false);
    const std::string stored = settings.GetString(kKeyRules, "");
    // 清过规则后 NVS 里存的就是 "[]"：那是「没有规则」，不是配置坏了
    if (!stored.empty() && stored != "[]") {
        std::vector<Rule> parsed;
        if (ParseRules(stored, &parsed).empty()) {
            rules_.swap(parsed);
        } else {
            ESP_LOGW(TAG, "NVS 里的时间表读不动，忽略：%s", stored.c_str());
        }
    }
    if (xTaskCreate(TaskEntry, "bg_schedule", 4096, this, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "轮询任务创建失败，时间表不会生效");
        return false;
    }
    started_ = true;
    return true;
}

void BgSchedule::TaskEntry(void* arg) {
    auto* self = static_cast<BgSchedule*>(arg);
    while (true) {
        self->Tick();
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

void BgSchedule::Tick() {
    // 时间不可信时不算：localtime 会给出 1970 年，那样算出来的色块比没有更糟
    if (!Alarm::SystemTimeReady()) {
        return;
    }
    const int now_min = NowMinuteOfDay();
    bool hit = false;
    uint32_t rgb = 0;
    Display* display = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        display = display_;
        for (const auto& rule : rules_) {
            const bool inside = (rule.start <= rule.end)
                                    ? (now_min >= rule.start && now_min < rule.end)
                                    // start > end：跨午夜区间，22:00~02:00 这种
                                    : (now_min >= rule.start || now_min < rule.end);
            if (inside) {
                hit = true;
                rgb = rule.rgb;
                break;
            }
        }
        if (have_applied_ && hit == applied_hit_ && (!hit || rgb == applied_rgb_)) {
            return;  // 状态没变，别白刷一次屏幕
        }
        have_applied_ = true;
        applied_hit_ = hit;
        applied_rgb_ = rgb;
    }
    if (display != nullptr) {
        display->SetBackgroundColor(hit ? rgb : 0);
    }
    if (hit) {
        ESP_LOGI(TAG, "背景色 -> #%06X（%s 起的时段）", (unsigned)rgb, HhMm(now_min).c_str());
    } else {
        ESP_LOGI(TAG, "背景色 -> 主题默认（%s 不在任何时段）", HhMm(now_min).c_str());
    }
}

std::string BgSchedule::ParseRules(const std::string& json, std::vector<Rule>* out) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return "规则不是合法 JSON";
    }
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return "规则要是一个数组";
    }
    std::vector<Rule> rules;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        const cJSON* s = cJSON_GetObjectItemCaseSensitive(item, "s");
        const cJSON* e = cJSON_GetObjectItemCaseSensitive(item, "e");
        const cJSON* c = cJSON_GetObjectItemCaseSensitive(item, "rgb");
        if (!cJSON_IsNumber(s) || !cJSON_IsNumber(e) || !cJSON_IsNumber(c)) {
            cJSON_Delete(root);
            return "每条规则都要有 s/e/rgb 三个数字";
        }
        Rule rule;
        rule.start = WrapMinute(s->valueint);
        // 1440（end 写 24:00）= 到当天结束，不能 wrap 成 0
        rule.end = (e->valueint == kMinutesPerDay) ? kMinutesPerDay : WrapMinute(e->valueint);
        rule.rgb = (uint32_t)c->valuedouble & 0xFFFFFF;
        if (rule.start == rule.end) {
            cJSON_Delete(root);
            return "开始时间和结束时间不能是同一分钟";
        }
        rules.push_back(rule);
        if (rules.size() > kMaxRules) {
            cJSON_Delete(root);
            return "最多 8 条规则";
        }
    }
    cJSON_Delete(root);
    if (rules.empty()) {
        return "规则是空的";
    }
    out->swap(rules);
    return "";
}

std::string BgSchedule::SetRules(const std::string& json) {
    std::vector<Rule> parsed;
    const std::string err = ParseRules(json, &parsed);
    if (!err.empty()) {
        return err;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rules_.swap(parsed);
        SaveLocked();
        // 规则换了就允许立刻按新规则重刷，不必等命中结果变化
        have_applied_ = false;
    }
    Tick();  // 用户说完就变色，别等下一个 30 秒
    return "";
}

std::string BgSchedule::ClearRules() {
    Display* display = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rules_.clear();
        SaveLocked();
        have_applied_ = false;
        display = display_;
    }
    if (display != nullptr) {
        display->SetBackgroundColor(0);
    }
    return "";
}

std::string BgSchedule::DumpJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return DumpLocked();
}

std::string BgSchedule::DumpLocked() const {
    cJSON* root = cJSON_CreateArray();
    if (root == nullptr) {
        return "[]";
    }
    for (const auto& rule : rules_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "start", HhMm(rule.start).c_str());
        cJSON_AddStringToObject(item, "end", HhMm(rule.end).c_str());
        char color[8];
        snprintf(color, sizeof(color), "#%06X", (unsigned)rule.rgb);
        cJSON_AddStringToObject(item, "color", color);
        cJSON_AddItemToArray(root, item);
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string out = text != nullptr ? text : "[]";
    if (text != nullptr) {
        cJSON_free(text);
    }
    cJSON_Delete(root);
    return out;
}

void BgSchedule::SaveLocked() const {
    cJSON* root = cJSON_CreateArray();
    if (root == nullptr) {
        return;
    }
    for (const auto& rule : rules_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "s", rule.start);
        cJSON_AddNumberToObject(item, "e", rule.end);
        cJSON_AddNumberToObject(item, "rgb", (double)rule.rgb);
        cJSON_AddItemToArray(root, item);
    }
    char* text = cJSON_PrintUnformatted(root);
    if (text != nullptr) {
        Settings settings(kNvsNs, true);
        settings.SetString(kKeyRules, text);
        cJSON_free(text);
    }
    cJSON_Delete(root);
}
