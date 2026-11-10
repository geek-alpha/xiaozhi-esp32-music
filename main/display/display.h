#ifndef DISPLAY_H
#define DISPLAY_H

#include <lvgl.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

#include <string>
#include <chrono>

struct DisplayFonts {
    const lv_font_t* text_font = nullptr;
    const lv_font_t* icon_font = nullptr;
    const lv_font_t* emoji_font = nullptr;
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void SetEmotion(const char* emotion);
    // 换头像头发颜色：A8 遮罩 + recolor，表情图本身不用重画。没实现的板子自然什么都不做
    virtual void SetHairColor(uint32_t rgb) {}
    // 换屏幕 HUD 主色（顶栏/核心/冲击环/流光），并控制整屏染色强度。0 = 跟随主题默认色
    // strength: 0~10，-1 = 不改当前强度（只换色）
    virtual void SetAccentColor(uint32_t rgb, int strength = -1) {}
    // 换屏幕底色（整屏背景 + 顶栏/容器），0 = 恢复主题默认。
    // 和 SetAccentColor 分两层：那个是整屏染色，这个是底色本身
    virtual void SetBackgroundColor(uint32_t rgb) {}
    virtual void SetChatMessage(const char* role, const char* content);
    virtual void SetMusicInfo(const char* song_name);
    virtual void SetIcon(const char* icon);
    // 待机屏上的天气。默认空实现：没有字幕位的板子不受影响
    virtual void SetWeatherText(const char* text) {}
    // 服务器 tts stop：这一轮真的说完了。字幕要等这个信号才回顶重播，别拿句子间隔去猜
    virtual void NotifySpeechFinished() {}
    virtual void SetPreviewImage(const lv_img_dsc_t* image);
    virtual void SetTheme(const std::string& theme_name);
    virtual std::string GetTheme() { return current_theme_name_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    virtual void start() {}
    virtual void clearScreen() {}  // 清除FFT显示，默认为空实现
    virtual void stopFft() {}      // 停止FFT显示，默认为空实现

    inline int width() const { return width_; }
    inline int height() const { return height_; }

protected:
    int width_ = 0;
    int height_ = 0;
    
    esp_pm_lock_handle_t pm_lock_ = nullptr;
    lv_display_t *display_ = nullptr;

    lv_obj_t *emotion_label_ = nullptr;
    lv_obj_t *network_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *notification_label_ = nullptr;
    lv_obj_t *mute_label_ = nullptr;
    lv_obj_t *battery_label_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* low_battery_popup_ = nullptr;
    lv_obj_t* low_battery_label_ = nullptr;
    
    const char* battery_icon_ = nullptr;
    const char* network_icon_ = nullptr;
    bool muted_ = false;
    std::string current_theme_name_;

    std::chrono::system_clock::time_point last_status_update_time_;
    esp_timer_handle_t notification_timer_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};


class DisplayLockGuard {
public:
    DisplayLockGuard(Display *display) : display_(display) {
        if (!display_->Lock(30000)) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() {
        display_->Unlock();
    }

private:
    Display *display_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override {
        return true;
    }
    virtual void Unlock() override {}
};

#endif
