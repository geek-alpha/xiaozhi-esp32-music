#include "lcd_display.h"

#include <vector>
#include <algorithm>
#include <font_awesome_symbols.h>
#include <esp_log.h>
#include <esp_random.h>
#include <time.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_heap_caps.h>
#include "assets/lang_config.h"
#include <cstring>
#include <cmath>
#include <math.h>
#include "settings.h"
#include "emoji_assets.h"
#include "application.h"
#include "device_state.h"

#include "board.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TAG "LcdDisplay"

// 竖直慢滚：回顶停 1 秒让人看清再走，滚到底停 1.5 秒再回来
// 必须用线性路径：ease_in_out 每帧起步速度都是 0，动画一被打断重开就变成走走停停
static const uint32_t kScrollMsPerPx = 32; // 实机调出来的速度，眼睛跟得上

static void StartSlowVerticalScroll(lv_obj_t* inner, int over, int base_y = 0)
{
    const uint32_t kTopStopMs = 1000;
    const uint32_t kBottomStopMs = 1500;
    uint32_t ms = (uint32_t)over * kScrollMsPerPx;
    lv_anim_del(inner, nullptr);
    lv_obj_set_y(inner, base_y);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, inner);
    lv_anim_set_exec_cb(&a, [](void* var, int32_t v)
                        { lv_obj_set_y((lv_obj_t*)var, v); });
    lv_anim_set_values(&a, base_y, base_y - over);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, kTopStopMs);
    lv_anim_set_playback_delay(&a, kBottomStopMs);
    lv_anim_set_playback_duration(&a, ms);
    lv_anim_set_repeat_delay(&a, kTopStopMs);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

#define FFT_SIZE 512
static int current_heights[40] = {0};
static float avg_power_spectrum[FFT_SIZE/2]={-25.0f};

#define COLOR_BLACK   0x0000
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F
#define COLOR_WHITE   0xFFFF

// Color definitions for dark theme
#define DARK_BACKGROUND_COLOR       lv_color_hex(0x121212)     // Dark background
#define DARK_TEXT_COLOR             lv_color_white()           // White text
#define DARK_CHAT_BACKGROUND_COLOR  lv_color_hex(0x1E1E1E)     // Slightly lighter than background
#define DARK_USER_BUBBLE_COLOR      lv_color_hex(0x1A6C37)     // Dark green
#define DARK_ASSISTANT_BUBBLE_COLOR lv_color_hex(0x333333)     // Dark gray
#define DARK_SYSTEM_BUBBLE_COLOR    lv_color_hex(0x2A2A2A)     // Medium gray
#define DARK_SYSTEM_TEXT_COLOR      lv_color_hex(0xAAAAAA)     // Light gray text
#define DARK_BORDER_COLOR           lv_color_hex(0x333333)     // Dark gray border
#define DARK_LOW_BATTERY_COLOR      lv_color_hex(0xFF0000)     // Red for dark mode

// Color definitions for light theme
#define LIGHT_BACKGROUND_COLOR       lv_color_white()           // White background
#define LIGHT_TEXT_COLOR             lv_color_black()           // Black text
#define LIGHT_CHAT_BACKGROUND_COLOR  lv_color_hex(0xE0E0E0)     // Light gray background
#define LIGHT_USER_BUBBLE_COLOR      lv_color_hex(0x95EC69)     // WeChat green
#define LIGHT_ASSISTANT_BUBBLE_COLOR lv_color_white()           // White
#define LIGHT_SYSTEM_BUBBLE_COLOR    lv_color_hex(0xE0E0E0)     // Light gray
#define LIGHT_SYSTEM_TEXT_COLOR      lv_color_hex(0x666666)     // Dark gray text
#define LIGHT_BORDER_COLOR           lv_color_hex(0xE0E0E0)     // Light gray border
#define LIGHT_LOW_BATTERY_COLOR      lv_color_black()           // Black for light mode


// Define dark theme colors
const ThemeColors DARK_THEME = {
    .background = DARK_BACKGROUND_COLOR,
    .text = DARK_TEXT_COLOR,
    .chat_background = DARK_CHAT_BACKGROUND_COLOR,
    .user_bubble = DARK_USER_BUBBLE_COLOR,
    .assistant_bubble = DARK_ASSISTANT_BUBBLE_COLOR,
    .system_bubble = DARK_SYSTEM_BUBBLE_COLOR,
    .system_text = DARK_SYSTEM_TEXT_COLOR,
    .border = DARK_BORDER_COLOR,
    .low_battery = DARK_LOW_BATTERY_COLOR
};

// Define light theme colors
const ThemeColors LIGHT_THEME = {
    .background = LIGHT_BACKGROUND_COLOR,
    .text = LIGHT_TEXT_COLOR,
    .chat_background = LIGHT_CHAT_BACKGROUND_COLOR,
    .user_bubble = LIGHT_USER_BUBBLE_COLOR,
    .assistant_bubble = LIGHT_ASSISTANT_BUBBLE_COLOR,
    .system_bubble = LIGHT_SYSTEM_BUBBLE_COLOR,
    .system_text = LIGHT_SYSTEM_TEXT_COLOR,
    .border = LIGHT_BORDER_COLOR,
    .low_battery = LIGHT_LOW_BATTERY_COLOR
};


LV_FONT_DECLARE(font_awesome_30_4);

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, DisplayFonts fonts, int width, int height)
    : panel_io_(panel_io), panel_(panel), fonts_(fonts) {
    width_ = width;
    height_ = height;

    // Load theme from settings
    Settings settings("display", false);
    current_theme_name_ = settings.GetString("theme", "light");
    hair_color_ = (uint32_t)settings.GetInt("hair_color", (int32_t)hair_color_);
    ESP_LOGI(TAG, "Hair color from NVS: #%06X", (unsigned)hair_color_);

    // Update the theme
    if (current_theme_name_ == "dark") {
        current_theme_ = DARK_THEME;
    } else if (current_theme_name_ == "light") {
        current_theme_ = LIGHT_THEME;
    }
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy,
                           DisplayFonts fonts)
    : LcdDisplay(panel_io, panel, fonts, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    // 初始化 FFT 相关内存
    fft_real = (float*)heap_caps_malloc(FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    fft_imag = (float*)heap_caps_malloc(FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    hanning_window_float = (float*)heap_caps_malloc(FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    
    // 创建窗函数
    for (int i = 0; i < FFT_SIZE; i++) {
        hanning_window_float[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (FFT_SIZE - 1)));
    }
    
    if(audio_data==nullptr){
        audio_data=(int16_t*)heap_caps_malloc(sizeof(int16_t)*1152, MALLOC_CAP_SPIRAM);
        memset(audio_data,0,sizeof(int16_t)*1152);
    }
    if(frame_audio_data==nullptr){
        frame_audio_data=(int16_t*)heap_caps_malloc(sizeof(int16_t)*1152, MALLOC_CAP_SPIRAM);
        memset(frame_audio_data,0,sizeof(int16_t)*1152);
    }
    
    ESP_LOGI(TAG,"Initialize fft_input, audio_data, frame_audio_data, spectrum_data");

    SetupUI();
}

// RGB LCD实现
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy,
                           DisplayFonts fonts)
    : LcdDisplay(panel_io, panel, fonts, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 0,
            .full_refresh = 1,
            .direct_mode = 1,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        }
    };
    
    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }
    
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    SetupUI();
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                            int width, int height,  int offset_x, int offset_y,
                            bool mirror_x, bool mirror_y, bool swap_xy,
                            DisplayFonts fonts)
    : LcdDisplay(panel_io, panel, fonts, width, height) {

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
            .io_handle = panel_io,
            .panel_handle = panel,
            .control_handle = nullptr,
            .buffer_size = static_cast<uint32_t>(width_ * 50),
            .double_buffer = false,
            .hres = static_cast<uint32_t>(width_),
            .vres = static_cast<uint32_t>(height_),
            .monochrome = false,
            /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
            .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram =false,
            .sw_rotate = false,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        }
    };
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    SetupUI();
}

LcdDisplay::~LcdDisplay() {
    // 停止FFT任务
    if (fft_task_handle != nullptr) {
        ESP_LOGI(TAG, "Stopping FFT task in destructor");
        fft_task_should_stop = true;
        
        // 等待任务停止
        int wait_count = 0;
        while (fft_task_handle != nullptr && wait_count < 100) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;
        }
        
        if (fft_task_handle != nullptr) {
            vTaskDelete(fft_task_handle);
            fft_task_handle = nullptr;
        }
    }
    
    // 然后再清理 LVGL 对象
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    DisplayLockGuard lock(this);

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, fonts_.text_font, 0);
    lv_obj_set_style_text_color(screen, current_theme_.text, 0);
    lv_obj_set_style_bg_color(screen, current_theme_.background, 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, current_theme_.background, 0);
    lv_obj_set_style_border_color(container_, current_theme_.border, 0);

    /* Status bar */
    status_bar_ = lv_obj_create(container_);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_color(status_bar_, current_theme_.background, 0);
    lv_obj_set_style_text_color(status_bar_, current_theme_.text, 0);
    
    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, 10, 0);
    lv_obj_set_style_bg_color(content_, current_theme_.chat_background, 0); // Background for chat area
    lv_obj_set_style_border_color(content_, current_theme_.border, 0); // Border color for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    
    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, 10, 0); // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    /* Status bar */
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);
    lv_obj_set_style_pad_left(status_bar_, 10, 0);
    lv_obj_set_style_pad_right(status_bar_, 10, 0);
    lv_obj_set_style_pad_top(status_bar_, 2, 0);
    lv_obj_set_style_pad_bottom(status_bar_, 2, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    // 设置状态栏的内容垂直居中
    lv_obj_set_flex_align(status_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 创建emotion_label_在状态栏最左侧
    emotion_label_ = lv_label_create(status_bar_);
    lv_obj_set_style_text_font(emotion_label_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(emotion_label_, current_theme_.text, 0);
    lv_label_set_text(emotion_label_, FONT_AWESOME_AI_CHIP);
    lv_obj_set_style_margin_right(emotion_label_, 5, 0); // 添加右边距，与后面的元素分隔

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, current_theme_.text, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, current_theme_.text, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    
    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, current_theme_.text, 0);

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(network_label_, current_theme_.text, 0);
    lv_obj_set_style_margin_left(network_label_, 5, 0); // 添加左边距，与前面的元素分隔

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, current_theme_.text, 0);
    lv_obj_set_style_margin_left(battery_label_, 5, 0); // 添加左边距，与前面的元素分隔

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, fonts_.text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, current_theme_.low_battery, 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}
#if CONFIG_IDF_TARGET_ESP32P4
#define  MAX_MESSAGES 40
#else
#define  MAX_MESSAGES 20
#endif

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }
    
    //避免出现空的消息框
    if(strlen(content) == 0) return;
    
    // 检查消息数量是否超过限制
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // 删除最早的消息（第一个子对象）
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
        }
        // Scroll to the last message immediately
        if (last_child != nullptr) {
            lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
        }
    }
    
    // 折叠系统消息（如果是系统消息，检查最后一个消息是否也是系统消息）
    if (strcmp(role, "system") == 0 && child_count > 0) {
        // 获取最后一个消息容器
        lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
        if (last_container != nullptr && lv_obj_get_child_cnt(last_container) > 0) {
            // 获取容器内的气泡
            lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
            if (last_bubble != nullptr) {
                // 检查气泡类型是否为系统消息
                void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                if (bubble_type_ptr != nullptr && strcmp((const char*)bubble_type_ptr, "system") == 0) {
                    // 如果最后一个消息也是系统消息，则删除它
                    lv_obj_del(last_container);
                }
            }
        }
    }
    
    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 1, 0);
    lv_obj_set_style_border_color(msg_bubble, current_theme_.border, 0);
    lv_obj_set_style_pad_all(msg_bubble, 8, 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);
    
    // 计算文本实际宽度
    lv_coord_t text_width = lv_txt_get_width(content, strlen(content), fonts_.text_font, 0);

    // 计算气泡宽度
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 屏幕宽度的85%
    lv_coord_t min_width = 20;  
    lv_coord_t bubble_width;
    
    // 确保文本宽度不小于最小宽度
    if (text_width < min_width) {
        text_width = min_width;
    }

    // 如果文本宽度小于最大宽度，使用文本宽度
    if (text_width < max_width) {
        bubble_width = text_width; 
    } else {
        bubble_width = max_width;
    }
    
    // 设置消息文本的宽度
    lv_obj_set_width(msg_text, bubble_width);  // 减去padding
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(msg_text, fonts_.text_font, 0);

    // 设置气泡宽度
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, current_theme_.user_bubble, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, current_theme_.text, 0);
        
        // 设置自定义属性标记气泡类型
        lv_obj_set_user_data(msg_bubble, (void*)"user");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, current_theme_.assistant_bubble, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, current_theme_.text, 0);
        
        // 设置自定义属性标记气泡类型
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, current_theme_.system_bubble, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, current_theme_.system_text, 0);
        
        // 设置自定义属性标记气泡类型
        lv_obj_set_user_data(msg_bubble, (void*)"system");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }
    
    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);
        
        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);
        
        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // 为系统消息创建全宽容器以确保居中对齐
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        // 使容器透明且无边框
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        // 将消息气泡移入此容器
        lv_obj_set_parent(msg_bubble, container);
        
        // 将气泡居中对齐在容器中
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        
        // 自动滚动底部
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }
    
    // 长回复：气泡限高不撑破表情区，文字在里面慢速上下滚，一段话滚完也留着能重看
    lv_obj_update_layout(msg_text);
    const int kBubbleChrome = 18; // pad 8*2 + border 1*2
    int chat_view_h = lv_obj_get_height(content_) - EMOJI_H - 10 - kBubbleChrome;
    int text_h = lv_obj_get_height(msg_text);
    if (chat_view_h > 0 && text_h > chat_view_h) {
        lv_obj_set_height(msg_bubble, chat_view_h + kBubbleChrome);
        lv_obj_set_pos(msg_text, 0, 0);
        StartSlowVerticalScroll(msg_text, text_h - chat_view_h);
        ESP_LOGI(TAG, "Chat bubble scroll on: text %dpx / view %dpx", text_h, chat_view_h);
    }

    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(const lv_img_dsc_t* img_dsc) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }
    
    if (img_dsc != nullptr) {
        // Create a message bubble for image preview
        lv_obj_t* img_bubble = lv_obj_create(content_);
        lv_obj_set_style_radius(img_bubble, 8, 0);
        lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_border_width(img_bubble, 1, 0);
        lv_obj_set_style_border_color(img_bubble, current_theme_.border, 0);
        lv_obj_set_style_pad_all(img_bubble, 8, 0);
        
        // Set image bubble background color (similar to system message)
        lv_obj_set_style_bg_color(img_bubble, current_theme_.assistant_bubble, 0);
        
        // 设置自定义属性标记气泡类型
        lv_obj_set_user_data(img_bubble, (void*)"image");
        
        // Create the image object inside the bubble
        lv_obj_t* preview_image = lv_image_create(img_bubble);
        
        // Copy the image descriptor and data to avoid source data changes
        lv_img_dsc_t* copied_img_dsc = (lv_img_dsc_t*)heap_caps_malloc(sizeof(lv_img_dsc_t), MALLOC_CAP_8BIT);
        if (copied_img_dsc == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate memory for image descriptor");
            lv_obj_del(img_bubble);
            return;
        }
        
        // Copy the header
        copied_img_dsc->header = img_dsc->header;
        copied_img_dsc->data_size = img_dsc->data_size;
        
        // Copy the image data
        uint8_t* copied_data = (uint8_t*)heap_caps_malloc(img_dsc->data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copied_data == nullptr) {
            // Fallback to internal RAM if SPIRAM allocation fails
            copied_data = (uint8_t*)heap_caps_malloc(img_dsc->data_size, MALLOC_CAP_8BIT);
        }
        if (copied_data == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate memory for image data (size: %lu bytes)", img_dsc->data_size);
            heap_caps_free(copied_img_dsc);
            lv_obj_del(img_bubble);
            return;
        }
        
        memcpy(copied_data, img_dsc->data, img_dsc->data_size);
        copied_img_dsc->data = copied_data;
        
        // Calculate appropriate size for the image
        lv_coord_t max_width = LV_HOR_RES * 70 / 100;  // 70% of screen width
        lv_coord_t max_height = LV_VER_RES * 50 / 100; // 50% of screen height
        
        // Calculate zoom factor to fit within maximum dimensions
        lv_coord_t img_width = copied_img_dsc->header.w;
        lv_coord_t img_height = copied_img_dsc->header.h;
        
        lv_coord_t zoom_w = (max_width * 256) / img_width;
        lv_coord_t zoom_h = (max_height * 256) / img_height;
        lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;
        
        // Ensure zoom doesn't exceed 256 (100%)
        if (zoom > 256) zoom = 256;
        
        // Set image properties
        lv_image_set_src(preview_image, copied_img_dsc);
        lv_image_set_scale(preview_image, zoom);
        
        // Add event handler to clean up copied data when image is deleted
        lv_obj_add_event_cb(preview_image, [](lv_event_t* e) {
            lv_img_dsc_t* copied_img_dsc = (lv_img_dsc_t*)lv_event_get_user_data(e);
            if (copied_img_dsc != nullptr) {
                heap_caps_free((void*)copied_img_dsc->data);
                heap_caps_free(copied_img_dsc);
            }
        }, LV_EVENT_DELETE, (void*)copied_img_dsc);
        
        // Calculate actual scaled image dimensions
        lv_coord_t scaled_width = (img_width * zoom) / 256;
        lv_coord_t scaled_height = (img_height * zoom) / 256;
        
        // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
        lv_obj_set_width(img_bubble, scaled_width + 16);
        lv_obj_set_height(img_bubble, scaled_height + 16);
        
        // Don't grow in flex layout
        lv_obj_set_style_flex_grow(img_bubble, 0, 0);
        
        // Center the image within the bubble
        lv_obj_center(preview_image);
        
        // Left align the image bubble like assistant messages
        lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the image bubble
        lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
    }
}
#else
void LcdDisplay::SetupUI() {
    DisplayLockGuard lock(this);

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, fonts_.text_font, 0);
    lv_obj_set_style_text_color(screen, current_theme_.text, 0);
    lv_obj_set_style_bg_color(screen, current_theme_.background, 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, current_theme_.background, 0);
    lv_obj_set_style_border_color(container_, current_theme_.border, 0);

    /* Status bar */
    status_bar_ = lv_obj_create(container_);
    lv_obj_set_size(status_bar_, LV_HOR_RES, fonts_.text_font->line_height);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_color(status_bar_, current_theme_.background, 0);
    lv_obj_set_style_text_color(status_bar_, current_theme_.text, 0);
    
    /* Content */
    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, 5, 0);
    lv_obj_set_style_bg_color(content_, current_theme_.chat_background, 0);
    lv_obj_set_style_border_color(content_, current_theme_.border, 0); // Border color for content

    // content_ 不挂 flex 布局：表情、天气、对话视窗各自绝对定位（LayoutContentBlocks），
    // 任何一方内容怎么变都不会挪动别人的坐标

    emotion_label_ = lv_label_create(content_);
    lv_obj_set_style_text_font(emotion_label_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(emotion_label_, current_theme_.text, 0);
    lv_label_set_text(emotion_label_, FONT_AWESOME_AI_CHIP);
    lv_obj_add_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);

    emotion_image_ = lv_image_create(content_);
    lv_obj_set_size(emotion_image_, EMOJI_W, EMOJI_H);
    lv_obj_set_style_pad_all(emotion_image_, 0, 0);
    lv_image_set_src(emotion_image_, &emoji_neutral);

    // 头发染色层：A8 遮罩当 mask，颜色走 recolor。挂成表情图的子对象，
    // 表情图隐藏/挪位时它自动跟着走，不用每个调用点都补一遍
    hair_image_ = lv_image_create(emotion_image_);
    lv_obj_remove_style_all(hair_image_);
    lv_obj_set_size(hair_image_, EMOJI_W, EMOJI_H);
    lv_obj_set_pos(hair_image_, 0, 0);
    lv_obj_set_style_image_recolor_opa(hair_image_, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor(hair_image_, lv_color_hex(hair_color_), 0);
    hair_img_ = &emoji_neutral_hair;
    lv_image_set_src(hair_image_, hair_img_);

    // 眨眼：只换眼睛那一块贴片，不需要为每个表情存一张闭眼全脸。
    // 必须挂 emotion_image_ 名下：贴片本身带原图头发像素，挂在 content_ 上会盖到染色层之上，
    // 一眨眼额头就闪回原来的蓝色
    blink_image_ = lv_image_create(emotion_image_);
    lv_obj_add_flag(blink_image_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(blink_image_, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(blink_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_all(blink_image_, 0, 0);

    // 贴片会带出「睁眼时被眼球挡住的刘海」，那几块在原图染色遮罩里是空的，眨一下闪回原蓝。
    // 挂成 blink_image_ 的子对象：跟着同步显隐/同步位置，且自然压在贴片之上
    blink_hair_image_ = lv_image_create(blink_image_);
    lv_obj_remove_style_all(blink_hair_image_);
    lv_obj_set_size(blink_hair_image_, BLINK_W, BLINK_H);
    lv_obj_set_pos(blink_hair_image_, 0, 0);
    lv_obj_set_style_image_recolor_opa(blink_hair_image_, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor(blink_hair_image_, lv_color_hex(hair_color_), 0);

    // 说话张嘴：只在脸上叠一小块嘴部贴片，不为 21 个表情各存一张全脸（省 900KB flash）
    mouth_image_ = lv_image_create(emotion_image_);
    lv_obj_add_flag(mouth_image_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(mouth_image_, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(mouth_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_all(mouth_image_, 0, 0);
    mouth_img_ = &emoji_neutral_mouth;
    lv_image_set_src(mouth_image_, mouth_img_);
    // 贴片排在染色层下面：头发遮罩透明的地方才露贴片，头发该染色的地方贴片被盖住
    lv_obj_move_foreground(hair_image_);

    preview_image_ = lv_image_create(content_);
    lv_obj_set_size(preview_image_, width_ * 0.5, height_ * 0.5);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    // 天气：外框是固定高度的视窗（撑不破表情区），里面竖排所有行，行数多就整体上下滚动。
    int wx_line_h = fonts_.text_font->line_height;
    int wx_view_lines = (LV_VER_RES - wx_line_h - 10 - EMOJI_H) / wx_line_h;
    if (wx_view_lines < 1) {
        wx_view_lines = 1;
    }
    if (wx_view_lines > 4) {
        wx_view_lines = 4;
    }
    weather_box_ = lv_obj_create(content_);
    lv_obj_set_size(weather_box_, LV_HOR_RES - 10, wx_view_lines * wx_line_h);
    lv_obj_set_style_pad_all(weather_box_, 0, 0);
    lv_obj_set_style_border_width(weather_box_, 0, 0);
    lv_obj_set_style_radius(weather_box_, 0, 0);
    lv_obj_set_style_bg_opa(weather_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(weather_box_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(weather_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(weather_box_, LV_OBJ_FLAG_HIDDEN);

    // inner 靠动画改 y 来滚，所以外框不能设 flex 布局，否则布局会把手动坐标抹回去
    weather_inner_ = lv_obj_create(weather_box_);
    lv_obj_set_width(weather_inner_, LV_HOR_RES - 10);
    lv_obj_set_height(weather_inner_, LV_SIZE_CONTENT);
    lv_obj_set_pos(weather_inner_, 0, 0);
    lv_obj_set_flex_flow(weather_inner_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(weather_inner_, 0, 0);
    lv_obj_set_style_pad_row(weather_inner_, 0, 0);
    lv_obj_set_style_border_width(weather_inner_, 0, 0);
    lv_obj_set_style_radius(weather_inner_, 0, 0);
    lv_obj_set_style_bg_opa(weather_inner_, LV_OPA_TRANSP, 0);

    // 每行允许折行：屏宽只够 11 个汉字，长行折成两行再靠纵向滚动看全，比截断丢字好
    for (lv_obj_t*& line : weather_lines_) {
        line = lv_label_create(weather_inner_);
        lv_label_set_text(line, "");
        lv_obj_set_style_text_font(line, fonts_.text_font, 0);
        lv_obj_set_style_text_color(line, current_theme_.text, 0);
        lv_obj_set_width(line, LV_HOR_RES - 10);
        lv_obj_set_height(line, LV_SIZE_CONTENT);
        lv_label_set_long_mode(line, LV_LABEL_LONG_WRAP);
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
    }

    // 对话区：外框是固定高度的视窗（撑不破表情区），文字在里面靠 y 动画滚。
    // 脱离 content_ 的 flex 流自己定位，表情居中就不会被文字宽度带偏
    chat_view_ = lv_obj_create(content_);
    lv_obj_add_flag(chat_view_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(chat_view_, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_width(chat_view_, LV_HOR_RES - 20);
    lv_obj_set_height(chat_view_, 0);
    lv_obj_set_style_pad_all(chat_view_, 0, 0);
    lv_obj_set_style_border_width(chat_view_, 0, 0);
    lv_obj_set_style_radius(chat_view_, 0, 0);
    lv_obj_set_style_bg_opa(chat_view_, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(chat_view_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(chat_view_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chat_view_, LV_OBJ_FLAG_HIDDEN);

    // 视窗里不能设 flex 布局，否则每次布局都会把手动坐标抹回去
    chat_message_label_ = lv_label_create(chat_view_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_PCT(100));
    lv_obj_set_pos(chat_message_label_, 0, 0);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, current_theme_.text, 0);

    // 建 UI 时 content_ 还没布局，宽度是 0：先强制算一次，表情才能落在真正的正中
    lv_obj_update_layout(content_);
    LayoutContentBlocks();

    /* Status bar */
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);
    lv_obj_set_style_pad_left(status_bar_, 2, 0);
    lv_obj_set_style_pad_right(status_bar_, 2, 0);

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(network_label_, current_theme_.text, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, current_theme_.text, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, current_theme_.text, 0);
    // 状态栏要显示“周五”这类中文，默认字体是 Montserrat（无汉字），换成板子的中文点阵
    lv_obj_set_style_text_font(status_label_, fonts_.text_font, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, current_theme_.text, 0);

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, current_theme_.text, 0);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, fonts_.text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, current_theme_.low_battery, 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    // 眨眼/嘴动定时器：这块板不走 start()（全项目只有 music 板调 start），所以在这里起
    lv_timer_create([](lv_timer_t* t) {
        static_cast<LcdDisplay*>(lv_timer_get_user_data(t))->BlinkTick();
    }, 100, this);
}

void LcdDisplay::SetPreviewImage(const lv_img_dsc_t* img_dsc) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        return;
    }
    
    if (img_dsc != nullptr) {
        // zoom factor 0.5
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
        // 设置图片源并显示预览图片
        lv_image_set_src(preview_image_, img_dsc);
        lv_obj_clear_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        // 隐藏emotion_label_
        if (emotion_label_ != nullptr) {
            lv_obj_add_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emotion_image_ != nullptr) {
            lv_obj_add_flag(emotion_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (blink_image_ != nullptr) {
            lv_obj_add_flag(blink_image_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        // 隐藏预览图片并恢复表情贴图
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        if (emotion_image_ != nullptr) {
            lv_obj_clear_flag(emotion_image_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
#endif

#if !CONFIG_USE_WECHAT_MESSAGE_STYLE

// 表情永远固定在 content_ 顶部正中：不靠 flex 算，直接给坐标，文字多少都挪不动它
void LcdDisplay::LayoutContentBlocks() {
    if (content_ == nullptr) {
        return;
    }
    int cw = lv_obj_get_content_width(content_);
    int ex = (cw - EMOJI_H) / 2;
    if (emotion_image_ != nullptr) {
        lv_obj_set_pos(emotion_image_, ex < 0 ? 0 : ex, 0);
    }
    if (emotion_label_ != nullptr) {
        lv_obj_align(emotion_label_, LV_ALIGN_TOP_MID, 0, 0);
    }
    if (weather_box_ != nullptr) {
        int wx = (cw - lv_obj_get_width(weather_box_)) / 2;
        lv_obj_set_pos(weather_box_, wx < 0 ? 0 : wx, EMOJI_H + 4);
    }
}


// vh 由调用方传入：刚 set_height 后 lv_obj_get_height 读回的仍是旧 coords
// （新建时是 0），底对齐算法拿到 0 会把视窗摆到屏幕外
void LcdDisplay::PlaceChatView(int vh) {
    if (chat_view_ == nullptr || content_ == nullptr) {
        return;
    }
    LayoutContentBlocks();
    int cw = lv_obj_get_content_width(content_);
    int ch = lv_obj_get_content_height(content_);
    int vw = lv_obj_get_width(chat_view_);
    // 顶在表情（有天气就在天气）下方，文字从上往下排；底对齐会让短句落到看不见的底部
    int y = EMOJI_H + 4;
    if (weather_box_ != nullptr && !lv_obj_has_flag(weather_box_, LV_OBJ_FLAG_HIDDEN)) {
        y = (int)lv_obj_get_y(weather_box_) + (int)lv_obj_get_height(weather_box_) + 4;
    }
    int x = (cw - vw) / 2;
    lv_obj_set_pos(chat_view_, x < 0 ? 0 : x, y < 0 ? 0 : y);
    // 每来一句回复都会走一次这里，串口打印本身就会拖住 LVGL：位置没变就别打
    static int last_x = -9999, last_y = -9999, last_vh = -9999;
    int nx = (int)lv_obj_get_x(chat_view_);
    int ny = (int)lv_obj_get_y(chat_view_);
    if (nx == last_x && ny == last_y && vh == last_vh) {
        return;
    }
    last_x = nx;
    last_y = ny;
    last_vh = vh;
    ESP_LOGI(TAG, "Chat view at %d,%d size %dx%d in %dx%d, emoji at %d,%d %dx%d",
             nx, ny, vw, vh, cw, ch,
             emotion_image_ == nullptr ? -1 : (int)lv_obj_get_x(emotion_image_),
             emotion_image_ == nullptr ? -1 : (int)lv_obj_get_y(emotion_image_),
             emotion_image_ == nullptr ? -1 : (int)lv_obj_get_width(emotion_image_),
             emotion_image_ == nullptr ? -1 : (int)lv_obj_get_height(emotion_image_));
}


// 一句话一句话显示：屏幕上只留最新这一句，整句替换，不滚、不累加
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr || chat_view_ == nullptr) {
        return;
    }
    const bool is_user = (role != nullptr && strcmp(role, "user") == 0);

    if (is_user || content == nullptr || strlen(content) == 0) {
        // 新一轮开口（或上位机清屏）：上一轮的回复不再留在屏幕上，用户输入也不显示
        chat_text_.clear();
        lv_anim_del(chat_message_label_, nullptr);
        lv_obj_set_y(chat_message_label_, 0);
        lv_label_set_text(chat_message_label_, "");
        lv_obj_set_height(chat_view_, 0);
        lv_obj_add_flag(chat_view_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // system 之类（升级进度、告警）和 assistant 的每一句都一样：顶掉上一句直接显示
    chat_text_ = content;
    lv_obj_clear_flag(chat_view_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(chat_message_label_, chat_text_.c_str());
    // 视窗里的文字永远贴在顶部，短句就不会在视窗里上下浮
    lv_anim_del(chat_message_label_, nullptr);
    lv_obj_set_y(chat_message_label_, 0);
    lv_obj_update_layout(chat_message_label_);

    // 去表情和天气之后剩下的高度就是视窗能占的高度，超出的一屏只显示开头
    int top = EMOJI_H + 4;
    if (weather_box_ != nullptr && !lv_obj_has_flag(weather_box_, LV_OBJ_FLAG_HIDDEN)) {
        top += lv_obj_get_height(weather_box_) + 4;
    }
    int avail = lv_obj_get_height(content_) - top - 10; // 10 = 视窗上方那条 flex 间距
    if (avail < 1) {
        avail = 1;
    }
    int text_h = lv_obj_get_height(chat_message_label_);
    int vh = text_h < avail ? text_h : avail;
    lv_obj_set_height(chat_view_, vh);
    PlaceChatView(vh);

    // 一屏装不下就按天气那套慢滚：回顶停 1 秒让人看完开头，滚到底停 1.5 秒再回来。
    // 只差几像素也滚会看着像抖，over 太小时直接裁掉
    int over = text_h - vh;
    if (over >= 4) {
        StartSlowVerticalScroll(chat_message_label_, over);
    }
    ESP_LOGI(TAG, "Chat line: text %dpx / avail %dpx / view %dpx / line %dpx / scroll %dpx",
             text_h, avail, vh, (int)lv_font_get_line_height(fonts_.text_font), over >= 4 ? over : 0);
}

#endif


void LcdDisplay::SetWeatherText(const char* text) {
    DisplayLockGuard lock(this);
    if (weather_box_ == nullptr) {
        return;
    }
    // 每秒都会被 OnClockTimer 调一次，文本没变就别动 LVGL：重设会把滚动进度打回起点
    if (weather_text_ == (text == nullptr ? "" : text)) {
        return;
    }
    weather_text_ = (text == nullptr ? "" : text);

    lv_anim_del(weather_inner_, nullptr);
    lv_obj_set_y(weather_inner_, 0);

    int used = 0;
    if (!weather_text_.empty()) {
        size_t pos = 0;
        while (used < kWeatherMaxLines) {
            size_t nl = weather_text_.find('\n', pos);
            if (nl == std::string::npos) {
                nl = weather_text_.size();
            }
            lv_label_set_text(weather_lines_[used], weather_text_.substr(pos, nl - pos).c_str());
            lv_obj_clear_flag(weather_lines_[used], LV_OBJ_FLAG_HIDDEN);
            ++used;
            if (nl >= weather_text_.size()) {
                break;
            }
            pos = nl + 1;
        }
    }
    for (int i = used; i < kWeatherMaxLines; ++i) {
        lv_obj_add_flag(weather_lines_[i], LV_OBJ_FLAG_HIDDEN);
    }

    if (used == 0) {
        lv_obj_add_flag(weather_box_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "Weather on screen: cleared");
        return;
    }
    lv_obj_clear_flag(weather_box_, LV_OBJ_FLAG_HIDDEN);

    // 折行后的真实高度要布局算完才知道，据此决定滚不滚、滚多远
    lv_obj_update_layout(weather_inner_);
    int inner_h = lv_obj_get_height(weather_inner_);
    int view_h = lv_obj_get_height(weather_box_);
    int over = inner_h - view_h;
    if (over > 0) {
        StartSlowVerticalScroll(weather_inner_, over);
        ESP_LOGI(TAG, "Weather on screen: %d lines, content %dpx / view %dpx, scroll on, stop top 1000ms bottom 1500ms",
                 used, inner_h, view_h);
        return;
    }
    ESP_LOGI(TAG, "Weather on screen: %d lines, content %dpx / view %dpx, scroll %s",
             used, inner_h, view_h, over > 0 ? "on" : "off");
}

void LcdDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    if (emotion_image_ == nullptr) {
        return;
    }

    const lv_image_dsc_t* img = &emoji_neutral;
    const lv_image_dsc_t* mouth = &emoji_neutral_mouth;
    const lv_image_dsc_t* blink = nullptr;
    const lv_image_dsc_t* blink_hair = nullptr;
    const lv_image_dsc_t* hair = &emoji_neutral_hair;
    if (emotion != nullptr) {
        for (int i = 0; i < emoji_table_count; i++) {
            if (strcmp(emoji_table[i].name, emotion) == 0) {
                img = emoji_table[i].img;
                mouth = emoji_table[i].mouth;
                blink = emoji_table[i].blink;
                blink_hair = emoji_table[i].blink_hair;
                hair = emoji_table[i].hair;
                break;
            }
        }
    }
    lv_image_set_src(emotion_image_, img);
    mouth_img_ = mouth;
    blink_img_ = blink;
    blink_hair_img_ = blink_hair;
    if (blink_hair_image_ != nullptr && blink_hair_img_ != nullptr) {
        lv_image_set_src(blink_hair_image_, blink_hair_img_);
        lv_obj_set_style_image_recolor(blink_hair_image_, lv_color_hex(hair_color_), 0);
    }
    if (hair_image_ != nullptr) {
        hair_img_ = hair;
        lv_image_set_src(hair_image_, hair_img_);
        // 颜色和换图分开管：任何一条换图路径都不该把染色带走
        lv_obj_set_style_image_recolor(hair_image_, lv_color_hex(hair_color_), 0);
        lv_obj_move_foreground(hair_image_);
    }
    if (mouth_image_ != nullptr) {
        lv_image_set_src(mouth_image_, mouth_img_);
        lv_obj_add_flag(mouth_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (blink_image_ != nullptr) {
        lv_obj_add_flag(blink_image_, LV_OBJ_FLAG_HIDDEN);
    }
    blink_frames_ = 0;
    mouth_tick_ = 0;
    mouth_open_ = false;
    mouth_active_ = false;

    lv_obj_clear_flag(emotion_image_, LV_OBJ_FLAG_HIDDEN);
    if (emotion_label_ != nullptr) {
        lv_obj_add_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (preview_image_ != nullptr) {
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    }
    LayoutContentBlocks();

    ESP_LOGI(TAG, "Emotion asset: %s placed at %d,%d in %d", emotion == nullptr ? "neutral" : emotion,
             (int)lv_obj_get_x(emotion_image_), (int)lv_obj_get_y(emotion_image_),
             (int)lv_obj_get_content_width(content_));
}

void LcdDisplay::SetHairColor(uint32_t rgb) {
    DisplayLockGuard lock(this);
    hair_color_ = rgb;
    if (hair_image_ != nullptr) {
        lv_obj_set_style_image_recolor(hair_image_, lv_color_hex(rgb), 0);
    }
    if (blink_hair_image_ != nullptr) {
        lv_obj_set_style_image_recolor(blink_hair_image_, lv_color_hex(rgb), 0);
    }
    Settings settings("display", true);
    settings.SetInt("hair_color", (int32_t)rgb);
    ESP_LOGI(TAG, "Hair color -> #%06X (saved)", (unsigned)rgb);
}

void LcdDisplay::SetIcon(const char* icon) {
    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr) {
        return;
    }
    lv_obj_set_style_text_font(emotion_label_, &font_awesome_30_4, 0);
    lv_label_set_text(emotion_label_, icon);

#if !CONFIG_USE_WECHAT_MESSAGE_STYLE
    lv_obj_clear_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);
    if (emotion_image_ != nullptr) {
        lv_obj_add_flag(emotion_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (blink_image_ != nullptr) {
        lv_obj_add_flag(blink_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (preview_image_ != nullptr) {
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void LcdDisplay::SetMusicInfo(const char* song_name) {
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // 微信模式下不显示歌名，保持原有聊天功能
    return;
#else
    // 非微信模式：在表情下方显示歌名
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }
    
    if (song_name != nullptr && strlen(song_name) > 0) {
        std::string music_text = "";
        music_text += song_name;
        lv_label_set_text(chat_message_label_, music_text.c_str());
        if (chat_view_ != nullptr) {
            lv_anim_del(chat_message_label_, nullptr);
            lv_obj_set_y(chat_message_label_, 0);
            lv_obj_update_layout(chat_message_label_);
            int lyric_h = lv_obj_get_height(chat_message_label_);
            int lyric_max = lv_obj_get_content_height(content_) - EMOJI_H - 8;
            int lyric_vh = lyric_h < lyric_max ? lyric_h : lyric_max;
            lv_obj_set_height(chat_view_, lyric_vh < 1 ? 1 : lyric_vh);
            lv_obj_clear_flag(chat_view_, LV_OBJ_FLAG_HIDDEN);
            PlaceChatView(lyric_vh);
        }

        // 确保显示 emotion_image_ 和 chat_message_label_，隐藏 preview_image_
        if (emotion_image_ != nullptr) {
            lv_obj_clear_flag(emotion_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emotion_label_ != nullptr) {
            lv_obj_add_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (preview_image_ != nullptr) {
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        // 清空歌名显示
        lv_label_set_text(chat_message_label_, "");
        if (chat_view_ != nullptr) {
            lv_obj_set_height(chat_view_, 0);
            lv_obj_add_flag(chat_view_, LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
}

void LcdDisplay::SetTheme(const std::string& theme_name) {
    DisplayLockGuard lock(this);
    
    if (theme_name == "dark" || theme_name == "DARK") {
        current_theme_ = DARK_THEME;
    } else if (theme_name == "light" || theme_name == "LIGHT") {
        current_theme_ = LIGHT_THEME;
    } else {
        // Invalid theme name, return false
        ESP_LOGE(TAG, "Invalid theme name: %s", theme_name.c_str());
        return;
    }
    
    // Get the active screen
    lv_obj_t* screen = lv_screen_active();
    
    // Update the screen colors
    lv_obj_set_style_bg_color(screen, current_theme_.background, 0);
    lv_obj_set_style_text_color(screen, current_theme_.text, 0);
    
    // Update container colors
    if (container_ != nullptr) {
        lv_obj_set_style_bg_color(container_, current_theme_.background, 0);
        lv_obj_set_style_border_color(container_, current_theme_.border, 0);
    }
    
    // Update status bar colors
    if (status_bar_ != nullptr) {
        lv_obj_set_style_bg_color(status_bar_, current_theme_.background, 0);
        lv_obj_set_style_text_color(status_bar_, current_theme_.text, 0);
        
        // Update status bar elements
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_color(network_label_, current_theme_.text, 0);
        }
        if (status_label_ != nullptr) {
            lv_obj_set_style_text_color(status_label_, current_theme_.text, 0);
        }
        if (notification_label_ != nullptr) {
            lv_obj_set_style_text_color(notification_label_, current_theme_.text, 0);
        }
        if (mute_label_ != nullptr) {
            lv_obj_set_style_text_color(mute_label_, current_theme_.text, 0);
        }
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_color(battery_label_, current_theme_.text, 0);
        }
        if (emotion_label_ != nullptr) {
            lv_obj_set_style_text_color(emotion_label_, current_theme_.text, 0);
        }
    }
    
    // Update content area colors
    if (content_ != nullptr) {
        lv_obj_set_style_bg_color(content_, current_theme_.chat_background, 0);
        lv_obj_set_style_border_color(content_, current_theme_.border, 0);
        
        // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
        // Iterate through all children of content (message containers or bubbles)
        uint32_t child_count = lv_obj_get_child_cnt(content_);
        for (uint32_t i = 0; i < child_count; i++) {
            lv_obj_t* obj = lv_obj_get_child(content_, i);
            if (obj == nullptr) continue;
            
            lv_obj_t* bubble = nullptr;
            
            // 检查这个对象是容器还是气泡
            // 如果是容器（用户或系统消息），则获取其子对象作为气泡
            // 如果是气泡（助手消息），则直接使用
            if (lv_obj_get_child_cnt(obj) > 0) {
                // 可能是容器，检查它是否为用户或系统消息容器
                // 用户和系统消息容器是透明的
                lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, 0);
                if (bg_opa == LV_OPA_TRANSP) {
                    // 这是用户或系统消息的容器
                    bubble = lv_obj_get_child(obj, 0);
                } else {
                    // 这可能是助手消息的气泡自身
                    bubble = obj;
                }
            } else {
                // 没有子元素，可能是其他UI元素，跳过
                continue;
            }
            
            if (bubble == nullptr) continue;
            
            // 使用保存的用户数据来识别气泡类型
            void* bubble_type_ptr = lv_obj_get_user_data(bubble);
            if (bubble_type_ptr != nullptr) {
                const char* bubble_type = static_cast<const char*>(bubble_type_ptr);
                
                // 根据气泡类型应用正确的颜色
                if (strcmp(bubble_type, "user") == 0) {
                    lv_obj_set_style_bg_color(bubble, current_theme_.user_bubble, 0);
                } else if (strcmp(bubble_type, "assistant") == 0) {
                    lv_obj_set_style_bg_color(bubble, current_theme_.assistant_bubble, 0); 
                } else if (strcmp(bubble_type, "system") == 0) {
                    lv_obj_set_style_bg_color(bubble, current_theme_.system_bubble, 0);
                } else if (strcmp(bubble_type, "image") == 0) {
                    lv_obj_set_style_bg_color(bubble, current_theme_.system_bubble, 0);
                }
                
                // Update border color
                lv_obj_set_style_border_color(bubble, current_theme_.border, 0);
                
                // Update text color for the message
                if (lv_obj_get_child_cnt(bubble) > 0) {
                    lv_obj_t* text = lv_obj_get_child(bubble, 0);
                    if (text != nullptr) {
                        // 根据气泡类型设置文本颜色
                        if (strcmp(bubble_type, "system") == 0) {
                            lv_obj_set_style_text_color(text, current_theme_.system_text, 0);
                        } else {
                            lv_obj_set_style_text_color(text, current_theme_.text, 0);
                        }
                    }
                }
            } else {
                // 如果没有标记，回退到之前的逻辑（颜色比较）
                // ...保留原有的回退逻辑...
                lv_color_t bg_color = lv_obj_get_style_bg_color(bubble, 0);
            
                // 改进bubble类型检测逻辑，不仅使用颜色比较
                bool is_user_bubble = false;
                bool is_assistant_bubble = false;
                bool is_system_bubble = false;
            
                // 检查用户bubble
                if (lv_color_eq(bg_color, DARK_USER_BUBBLE_COLOR) || 
                    lv_color_eq(bg_color, LIGHT_USER_BUBBLE_COLOR) ||
                    lv_color_eq(bg_color, current_theme_.user_bubble)) {
                    is_user_bubble = true;
                }
                // 检查系统bubble
                else if (lv_color_eq(bg_color, DARK_SYSTEM_BUBBLE_COLOR) || 
                         lv_color_eq(bg_color, LIGHT_SYSTEM_BUBBLE_COLOR) ||
                         lv_color_eq(bg_color, current_theme_.system_bubble)) {
                    is_system_bubble = true;
                }
                // 剩余的都当作助手bubble处理
                else {
                    is_assistant_bubble = true;
                }
            
                // 根据bubble类型应用正确的颜色
                if (is_user_bubble) {
                    lv_obj_set_style_bg_color(bubble, current_theme_.user_bubble, 0);
                } else if (is_assistant_bubble) {
                    lv_obj_set_style_bg_color(bubble, current_theme_.assistant_bubble, 0);
                } else if (is_system_bubble) {
                    lv_obj_set_style_bg_color(bubble, current_theme_.system_bubble, 0);
                }
                
                // Update border color
                lv_obj_set_style_border_color(bubble, current_theme_.border, 0);
                
                // Update text color for the message
                if (lv_obj_get_child_cnt(bubble) > 0) {
                    lv_obj_t* text = lv_obj_get_child(bubble, 0);
                    if (text != nullptr) {
                        // 回退到颜色检测逻辑
                        if (lv_color_eq(bg_color, current_theme_.system_bubble) ||
                            lv_color_eq(bg_color, DARK_SYSTEM_BUBBLE_COLOR) || 
                            lv_color_eq(bg_color, LIGHT_SYSTEM_BUBBLE_COLOR)) {
                            lv_obj_set_style_text_color(text, current_theme_.system_text, 0);
                        } else {
                            lv_obj_set_style_text_color(text, current_theme_.text, 0);
                        }
                    }
                }
            }
        }
#else
        // Simple UI mode - just update the main chat message
        if (chat_message_label_ != nullptr) {
            lv_obj_set_style_text_color(chat_message_label_, current_theme_.text, 0);
        }
        
        if (emotion_label_ != nullptr) {
            lv_obj_set_style_text_color(emotion_label_, current_theme_.text, 0);
        }
#endif
    }
    
    // Update low battery popup
    if (low_battery_popup_ != nullptr) {
        lv_obj_set_style_bg_color(low_battery_popup_, current_theme_.low_battery, 0);
    }

    // No errors occurred. Save theme to settings
    Display::SetTheme(theme_name);
}

void LcdDisplay::create_canvas(){
    DisplayLockGuard lock(this);
    if (canvas_ != nullptr) {
        lv_obj_del(canvas_);
    }
    if (canvas_buffer_ != nullptr) {
        heap_caps_free(canvas_buffer_);
        canvas_buffer_ = nullptr;
    }

    int status_bar_height=lv_obj_get_height(status_bar_);
    canvas_width_=width_;
    canvas_height_=height_-status_bar_height;

    canvas_buffer_=(uint16_t*)heap_caps_malloc(canvas_width_ * canvas_height_ * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (canvas_buffer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate canvas buffer");
        return;
    }
    ESP_LOGI(TAG, "canvas buffer allocated successfully");  
    canvas_ = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(canvas_, canvas_buffer_, canvas_width_, canvas_height_, LV_COLOR_FORMAT_RGB565);
    ESP_LOGI(TAG,"width: %d, height: %d", width_, height_);

    

    lv_obj_set_pos(canvas_, 0, status_bar_height);
    lv_obj_set_size(canvas_, canvas_width_, canvas_height_);
    lv_canvas_fill_bg(canvas_, lv_color_make(0, 0, 0), LV_OPA_TRANSP);
    lv_obj_move_foreground(canvas_);

    ESP_LOGI(TAG, "canvas created successfully");

   
   
    
     
}
void LcdDisplay::start(){
    ESP_LOGI(TAG, "Starting LcdDisplay with periodic data updates");
    
    vTaskDelay(pdMS_TO_TICKS(500));

    // 创建周期性更新任务
    fft_task_should_stop = false;  // 重置停止标志
    xTaskCreate(
        periodicUpdateTaskWrapper,
        "display_fft",      // 任务名称
        4096*2,             // 堆栈大小
        this,               // 参数
        1,                  // 优先级
        &fft_task_handle    // 保存到成员变量
    );
}

void LcdDisplay::BlinkTick() {
    if (emotion_image_ == nullptr) {
        return;
    }
    if (lv_obj_has_flag(emotion_image_, LV_OBJ_FLAG_HIDDEN)) {
        if (mouth_image_ != nullptr) {
            lv_obj_add_flag(mouth_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (blink_image_ != nullptr) {
            lv_obj_add_flag(blink_image_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // 说话中：嘴部贴片一显一隐，什么表情都能张嘴；眨眼照常跑（两者像素区不重叠）
    if (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
        if (mouth_image_ != nullptr && mouth_img_ != nullptr) {
            mouth_active_ = true;
            if (++mouth_tick_ % 2 == 0) {
                mouth_open_ = !mouth_open_;
            }
            if (mouth_open_) {
                lv_obj_set_pos(mouth_image_, MOUTH_OX, MOUTH_OY);
                lv_obj_clear_flag(mouth_image_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(hair_image_);
            } else {
                lv_obj_add_flag(mouth_image_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else if (mouth_active_) {
        mouth_active_ = false;
        mouth_open_ = false;
        if (mouth_image_ != nullptr) {
            lv_obj_add_flag(mouth_image_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (blink_frames_ > 0) {
        if (--blink_frames_ == 0 && blink_image_ != nullptr) {
            lv_obj_add_flag(blink_image_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    // 表里没眨眼贴片的表情（本来闭眼/戴墨镜）跳过眨眼
    if (blink_img_ != nullptr && blink_image_ != nullptr && ++blink_tick_ % 32 == 0) {
        lv_image_set_src(blink_image_, blink_img_);
        lv_obj_set_pos(blink_image_, BLINK_OX, BLINK_OY);
        lv_obj_clear_flag(blink_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(hair_image_);
        blink_frames_ = 2;
    }
}

void LcdDisplay::drawSpectrumIfReady() {
    if (fft_data_ready) {
        draw_spectrum(avg_power_spectrum, FFT_SIZE/2);
        fft_data_ready = false;
    }
}

void LcdDisplay::periodicUpdateTaskWrapper(void* arg) {
    auto self = static_cast<LcdDisplay*>(arg);
    self->periodicUpdateTask();
}

void LcdDisplay::periodicUpdateTask() {
    ESP_LOGI(TAG, "Periodic update task started");
    
    if(canvas_==nullptr){
        create_canvas();
    }
    else{
        ESP_LOGI(TAG, "canvas already created");
    }
  

    auto music = Board::GetInstance().GetMusic();
        
    const TickType_t displayInterval = pdMS_TO_TICKS(40);  
    const TickType_t audioProcessInterval = pdMS_TO_TICKS(15); 
    
    TickType_t lastDisplayTime = xTaskGetTickCount();
    TickType_t lastAudioTime = xTaskGetTickCount();
    
    while (!fft_task_should_stop) {
        
        TickType_t currentTime = xTaskGetTickCount();
        
        
        if (currentTime - lastAudioTime >= audioProcessInterval) {
            if(music->GetAudioData() != nullptr) {
                readAudioData();  // 快速处理，不阻塞
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            lastAudioTime = currentTime;
        }
        
        // 显示刷新（30Hz）
        if (currentTime - lastDisplayTime >= displayInterval) {
            if (fft_data_ready) {
                DisplayLockGuard lock(this);
                drawSpectrumIfReady();
                lv_area_t refresh_area;
                refresh_area.x1 = 0;
                refresh_area.y1 = height_-100;
                refresh_area.x2 = canvas_width_ -1;
                refresh_area.y2 = height_ -1; // 只刷新频谱区域
                lv_obj_invalidate_area(canvas_, &refresh_area);
                //lv_obj_invalidate(canvas_);
                fft_data_ready = false;
                lastDisplayTime = currentTime;
            }   // 绘制操作
           
            
            // 更新FPS计数
            //FPS();
        }
        
        
        
        vTaskDelay(pdMS_TO_TICKS(10));
        // 短暂延迟
        
    }
    
    ESP_LOGI(TAG, "FFT display task stopped");
    fft_task_handle = nullptr;  // 清空任务句柄
    vTaskDelete(NULL);  // 删除当前任务
}



void LcdDisplay::readAudioData(){
   
    auto music = Board::GetInstance().GetMusic();
    
    if(music->GetAudioData()!=nullptr){
        
    
        if(audio_display_last_update<=2){
            memcpy(audio_data,music->GetAudioData(),sizeof(int16_t)*1152);
            for(int i=0;i<1152;i++){
                frame_audio_data[i]+=audio_data[i];
            }
            audio_display_last_update++;
            
        }else{
            const int HOP_SIZE = 512;
            const int NUM_SEGMENTS = 1 + (1152 - FFT_SIZE) / HOP_SIZE;
    
            for (int seg = 0; seg < NUM_SEGMENTS; seg++) {
                int start = seg * HOP_SIZE;
                if (start + FFT_SIZE > 1152) break;

        // 准备当前段数据
                for (int i = 0; i < FFT_SIZE; i++) {
                    int idx = start + i;
                    //float sample =frame_audio_data[idx] / 32768.0f;
                    float sample =frame_audio_data[idx] / 32768.0f;
                    fft_real[i] = sample * hanning_window_float[i];
                    fft_imag[i] = 0.0f;
                    
                }

                compute(fft_real, fft_imag, FFT_SIZE, true);
        
        // 计算功率谱并累加（双边）
                for (int i = 0; i < FFT_SIZE/2; i++) {
                    avg_power_spectrum[i] += fft_real[i] * fft_real[i]+fft_imag[i] * fft_imag[i]; // 功率 = 幅度平方
                }
            }
        
    // 计算平均值
            for (int i = 0; i < FFT_SIZE/2; i++) {
                avg_power_spectrum[i] /= NUM_SEGMENTS;
            }

        audio_display_last_update=0;
        //memcpy(spectrum_data, avg_power_spectrum, sizeof(float) * FFT_SIZE/2);
        fft_data_ready=true;

        //draw_spectrum(avg_power_spectrum, FFT_SIZE/2);
        memset(frame_audio_data,0,sizeof(int16_t)*1152);

        }
    }else{
            ESP_LOGI(TAG, "audio_data is nullptr");
            vTaskDelay(pdMS_TO_TICKS(500));
        }   
}

uint16_t LcdDisplay::get_bar_color(int x_pos){

    static uint16_t color_table[40];
    static bool initialized = false;
    
    if (!initialized) {
        // 生成黄绿->黄->黄红的渐变
        for (int i = 0; i < 40; i++) {
            if (i < 20) {
                // 黄绿到黄：增加红色分量
                uint8_t r = static_cast<uint8_t>((i / 19.0f) * 31);
                color_table[i] = (r << 11) | (0x3F << 5);
            } else {
                // 黄到黄红：减少绿色分量
                uint8_t g = static_cast<uint8_t>((1.0f - (i - 20) / 19.0f * 0.5f) * 63);
                color_table[i] = (0x1F << 11) | (g << 5);
            }
        }
        initialized = true;
    }
    
    return color_table[x_pos];
 }


void LcdDisplay::draw_spectrum(float *power_spectrum,int fft_size){
   
    
    const int bartotal=40;
    int bar_height;
    const int bar_max_height=canvas_height_-100;
    const int bar_width=240/bartotal;
    int x_pos=0;
    int y_pos = (canvas_height_) - 1;

    float magnitude[bartotal]={0};
    float max_magnitude=0;

    const float MIN_DB = -25.0f;
    const float MAX_DB = 0.0f;
    
    for (int bin = 0; bin < bartotal; bin++) {
        int start = bin * (fft_size / bartotal);
        int end = (bin+1) * (fft_size / bartotal);
        magnitude[bin] = 0;
        int count=0;
        for (int k = start; k < end; k++) {
            magnitude[bin] += sqrt(power_spectrum[k]);
            count++;
        }
        if(count>0){
            magnitude[bin] /= count;
        }
      

        if (magnitude[bin] > max_magnitude) max_magnitude = magnitude[bin];
    }


    magnitude[1]=magnitude[1]*0.6;
    magnitude[2]=magnitude[2]*0.7;
    magnitude[3]=magnitude[3]*0.8;
    magnitude[4]=magnitude[4]*0.8;
    magnitude[5]=magnitude[5]*0.9;
    /*
    if (bartotal >= 6) {
        magnitude[0] *= 0.3f; // 最低频
        magnitude[1] *= 1.1f;
        magnitude[2] *= 1.0f;
        magnitude[3] *= 0.9f;
        magnitude[4] *= 0.8f;
        magnitude[5] *= 0.7f;
        // 更高频率保持或进一步衰减
        for (int i = 6; i < bartotal; i++) {
            magnitude[i] *= 0.6f;
        }
    }
    */    

    for (int bin = 1; bin < bartotal; bin++) {
        
        if (magnitude[bin] > 0.0f && max_magnitude > 0.0f) {
            // 相对dB值：20 * log10(magnitude/ref_level)
            magnitude[bin] = 20.0f * log10f(magnitude[bin] / max_magnitude+ 1e-10);
        } else {
            magnitude[bin] = MIN_DB;
        }
        if (magnitude[bin] > max_magnitude) max_magnitude = magnitude[bin];
    }

    clearScreen();
    
    for (int k = 1; k < bartotal; k++) {  // 跳过直流分量（k=0）
        x_pos=canvas_width_/bartotal*(k-1);
        float mag=(magnitude[k] - MIN_DB) / (MAX_DB - MIN_DB);
        mag = std::max(0.0f, std::min(1.0f, mag));
        bar_height=int(mag*(bar_max_height));
        
        int color=get_bar_color(k);
        draw_bar(x_pos,y_pos,bar_width,bar_height, color,k-1);
        //printf("x: %d, y: %d,\n", x_pos, bar_height);
    }

    
}

void LcdDisplay::draw_bar(int x,int y,int bar_width,int bar_height,uint16_t color,int bar_index){

    const int block_space=2;
    const int block_x_size=bar_width-block_space;
    const int block_y_size=4;
    
    int blocks_per_col=(bar_height/(block_y_size+block_space));
    int start_x=(block_x_size+block_space)/2+x;
    
    if(current_heights[bar_index]<bar_height) 
    {
        current_heights[bar_index]=bar_height;
    }
    else{
        int fall_speed=2;
        current_heights[bar_index]=current_heights[bar_index]-fall_speed;
        if(current_heights[bar_index]>(block_y_size+block_space)) 
        draw_block(start_x,canvas_height_-current_heights[bar_index],block_x_size,block_y_size,color,bar_index);

    }
   
    draw_block(start_x,canvas_height_-1,block_x_size,block_y_size,color,bar_index);

    for(int j=1;j<blocks_per_col;j++){
        
        int start_y=j*(block_y_size+block_space);
        draw_block(start_x,canvas_height_-start_y,block_x_size,block_y_size,color,bar_index); 
        
    }
    

}

void LcdDisplay::draw_block(int x,int y,int block_x_size,int block_y_size,uint16_t color,int bar_index){
    
    /*
    for(int dy=0;dy<block_y_size;dy++){
        for(int dx=0;dx<block_x_size;dx++){
            canvas_buffer_[(y-dy)*canvas_width_+x+dx]=color;    
        }   
    }
    */
    for (int row = y; row > y-block_y_size;row--) {
        // 一次绘制一行
        uint16_t* line_start = &canvas_buffer_[row * canvas_width_ + x];
        std::fill_n(line_start, block_x_size, color);
    }
}   

void LcdDisplay::clearScreen() {
   // DisplayLockGuard lock(this);
    // 清屏为黑色
    //for (int i = 0; i < canvas_width_ * canvas_height_; i++) {
    //    canvas_buffer_[i] = COLOR_BLACK;
    //}
    //lv_obj_invalidate(canvas_);
    std::fill_n(canvas_buffer_, canvas_width_ * canvas_height_, COLOR_BLACK);

}

void LcdDisplay::stopFft() {
    ESP_LOGI(TAG, "Stopping FFT display");
    
    // 停止FFT显示任务
    if (fft_task_handle != nullptr) {
        ESP_LOGI(TAG, "Stopping FFT display task");
        fft_task_should_stop = true;  // 设置停止标志
        
        // 等待任务停止（最多等待1秒）
        int wait_count = 0;
        while (fft_task_handle != nullptr && wait_count < 100) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;
        }
        
        if (fft_task_handle != nullptr) {
            ESP_LOGW(TAG, "FFT task did not stop gracefully, force deleting");
            vTaskDelete(fft_task_handle);
            fft_task_handle = nullptr;
        } else {
            ESP_LOGI(TAG, "FFT display task stopped successfully");
        }
    }
    
    // 使用显示锁保护所有操作
    DisplayLockGuard lock(this);
    
    // 重置FFT状态变量
    fft_data_ready = false;
    audio_display_last_update = 0;
    
    // 重置频谱条高度
    memset(current_heights, 0, sizeof(current_heights));
    
    // 重置平均功率谱数据
    for (int i = 0; i < FFT_SIZE/2; i++) {
        avg_power_spectrum[i] = -25.0f;
    }
    
    // 删除FFT画布对象，让原始UI重新显示
    if (canvas_ != nullptr) {
        lv_obj_del(canvas_);
        canvas_ = nullptr;
        ESP_LOGI(TAG, "FFT canvas deleted");
    }
    
    // 释放画布缓冲区内存
    if (canvas_buffer_ != nullptr) {
        heap_caps_free(canvas_buffer_);
        canvas_buffer_ = nullptr;
        ESP_LOGI(TAG, "FFT canvas buffer freed");
    }
    
    // 重置画布尺寸变量
    canvas_width_ = 0;
    canvas_height_ = 0;
    
    ESP_LOGI(TAG, "FFT display stopped, original UI restored");
}

void LcdDisplay::MyUI(){

    DisplayLockGuard lock(this);

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, fonts_.text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lv_color_black(), 0);
    lv_obj_set_style_border_color(container_, lv_color_white(), 0);
    
  
    /* Status bar */
    status_bar_ = lv_obj_create(container_);
    lv_obj_set_size(status_bar_, LV_HOR_RES, fonts_.text_font->line_height);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_color(status_bar_, lv_color_black(), 0);
    lv_obj_set_style_text_color(status_bar_, lv_color_white(), 0);

    /* Status bar */
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);
    lv_obj_set_style_pad_left(status_bar_, 2, 0);
    lv_obj_set_style_pad_right(status_bar_, 2, 0);
  
    


   

}


void LcdDisplay::compute(float* real, float* imag, int n, bool forward) {
    // 位反转排序
    int j = 0;
    for (int i = 0; i < n; i++) {
        if (j > i) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
        
        int m = n >> 1;
        while (m >= 1 && j >= m) {
            j -= m;
            m >>= 1;
        }
        j += m;
    }

    // FFT计算
    for (int s = 1; s <= (int)log2(n); s++) {
        int m = 1 << s;
        int m2 = m >> 1;
        float w_real = 1.0f;
        float w_imag = 0.0f;
        float angle = (forward ? -2.0f : 2.0f) * M_PI / m;
        float wm_real = cosf(angle);
        float wm_imag = sinf(angle);
        
        for (int j = 0; j < m2; j++) {
            for (int k = j; k < n; k += m) {
                int k2 = k + m2;
                float t_real = w_real * real[k2] - w_imag * imag[k2];
                float t_imag = w_real * imag[k2] + w_imag * real[k2];
                
                real[k2] = real[k] - t_real;
                imag[k2] = imag[k] - t_imag;
                real[k] += t_real;
                imag[k] += t_imag;
            }
            
            float w_temp = w_real;
            w_real = w_real * wm_real - w_imag * wm_imag;
            w_imag = w_temp * wm_imag + w_imag * wm_real;
        }
    }
    
    // 正向变换需要缩放
    if (forward) {
        for (int i = 0; i < n; i++) {
            real[i] /= n;
            imag[i] /= n;
        }
    }
    
}
