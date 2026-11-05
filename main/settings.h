#ifndef SETTINGS_H
#define SETTINGS_H

#include <functional>
#include <string>
#include <nvs_flash.h>

// 写 NVS 会短暂关掉 flash cache，IDF 会断言当前任务的栈必须在内部 RAM。
// 栈在 PSRAM 的任务（音乐播放、网络收发、MCP 回调）直接写会 abort 整机重启
// （watch3.log:3914 assert esp_task_stack_is_sane_cache_disabled）。
// 所以所有写路径都必须经过它：自己栈不安全就切到专用 worker 上写。
void RunNvsWrite(const std::function<void()>& fn);

class Settings {
public:
    Settings(const std::string& ns, bool read_write = false);
    ~Settings();

    std::string GetString(const std::string& key, const std::string& default_value = "");
    void SetString(const std::string& key, const std::string& value);
    int32_t GetInt(const std::string& key, int32_t default_value = 0);
    void SetInt(const std::string& key, int32_t value);
    void EraseKey(const std::string& key);
    void EraseAll();

private:
    std::string ns_;
    nvs_handle_t nvs_handle_ = 0;
    bool read_write_ = false;
    bool dirty_ = false;
};

#endif
