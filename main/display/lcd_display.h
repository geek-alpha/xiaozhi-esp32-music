#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>
#include "emoji_assets.h"
#include "tech_backdrop.h"

#include <atomic>
#include <vector>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>  

// Theme color structure
struct ThemeColors {
    lv_color_t background;
    lv_color_t text;
    lv_color_t chat_background;
    lv_color_t user_bubble;
    lv_color_t assistant_bubble;
    lv_color_t system_bubble;
    lv_color_t system_text;
    lv_color_t border;
    lv_color_t low_battery;
};


class LcdDisplay : public Display {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    // 手臂：单独一层 3 帧，垫在身体之下。摆臂只换 src，身体一动不动
    lv_obj_t* arms_image_ = nullptr;
    // 当前摆臂每帧毫秒数，0 = 还没起过动画。说话 260ms/帧，待机走偶发单程不靠它
    uint32_t arm_wave_ms_ = 0;
    // 待机轻摆：真人听人说话时手是静的，隔几秒才轻动一下。
    // 用 100ms 的 BlinkTick 计数当低频时钟，够用，不必再开一个 timer
    uint32_t arm_idle_tick_ = 0;
    uint32_t arm_idle_next_ = 0;
    // 当前贴的是「情绪表情」而不是中性/倾听。待机换倾听脸时不能把刚演完的情绪抹掉
    bool mood_active_ = false;
    int last_state_ = -1;
    // 整体呼吸浮动：一个 timer 算一次、一次设三层 y。
    // 不能给三层各起一条 lv_anim——arms 身上挂着换帧动画，lv_anim_del 会把它一起干掉
    lv_timer_t* float_timer_ = nullptr;
    uint32_t float_phase_ms_ = 0;
    bool float_talk_ = false;
    int float_y_ = 0;
    // 聆听时的不时轻点头：毫秒计时，和呼吸共用 40ms 那个 tick
    uint32_t nod_phase_ms_ = 0;
    uint32_t nod_wait_ms_ = 0;
    // Q 版身体：一张静态图垫在头像下面。和 21 张脸分开存，换表情只重画脸，身体只做呼吸
    lv_obj_t* body_image_ = nullptr;
    // 身体图上的下半截长发：脸图只有 EMOJI_H 高，垂到肩/胸以下的发丝画在 body 里。
    // 没有这层，换发色就是「上半截变了、下半截还是原蓝」。必须建在 body 之后、脸之前：
    // 建在 body 之前会被躯干盖住，建在脸之后会把脸盖掉
    lv_obj_t* body_hair_image_ = nullptr;
    lv_obj_t* body_hair_shade_image_ = nullptr;
    lv_obj_t* emotion_image_ = nullptr;
    // 表情补丁层：底图永远是 neutral，切表情只换这块差异图（见 emoji_asset_t）
    lv_obj_t* patch_image_ = nullptr;
    // 投影：整只小人的模糊剪影，A8 + recolor 黑，垫在最底层
    lv_obj_t* shadow_image_ = nullptr;
    // 头发染色层：挂在 emotion_image_ 下面当子对象，父图隐藏/挪位它自动跟着走
    lv_obj_t* hair_image_ = nullptr;
    const lv_image_dsc_t* hair_img_ = nullptr;
    // 染色阴影层：recolor 只有「新色」一个自由度，单靠它会把底板的发丝明暗压成平涂。
    // 这张 A8 带的是底板里比头发中位亮度暗的部分，色相取同一新色的暗版，叠上去明暗就回来了
    lv_obj_t* hair_shade_image_ = nullptr;
    // 0 = 不染色，直接用参考图底板的原始像素。底板自带深→浅的发色层次，只有用户主动换色才染
    uint32_t hair_color_ = 0;
    // HUD 主色，0 = 跟随主题默认（light 品红 / dark 青）。配过就整屏顶栏+底图动态一起换
    uint32_t accent_color_ = 0;
    // 整屏染色强度 0~10，要和 accent_color_ 一起存 NVS，否则重启后颜色回来了、染层却没了
    int accent_strength_ = 3;
    lv_obj_t* mouth_image_ = nullptr;
    lv_obj_t* blink_image_ = nullptr;
    // 聆听眼神：同一张 listening 脸的眼睛局部变体，按节奏轮播。
    // 一张静脸撑不起「在听」——真人听人说话，眼睛一直在对方脸上移动。
    // 必须建在 blink_image_ 之前：创建顺序就是层序，闭眼时眼神贴片不能压在它上面
    lv_obj_t* gaze_image_ = nullptr;
    int8_t gaze_idx_ = -1;      // 正在贴第几帧，-1 = 没贴
    uint8_t gaze_hold_ = 0;     // 当前落点还要贴多久（tick）
    uint8_t gaze_wait_ = 0;     // 距下一个落点还要等多久（tick）
    bool gaze_blink_hide_ = false;
    // 聆听偶发「嗯」：一张木脸听到底不像人，偶尔松一下嘴
    uint8_t listen_mouth_hold_ = 0;
    uint16_t listen_mouth_wait_ = 0;
    // 聆听换姿势：手只能做单程小调整，间隔比待机轻摆长得多
    uint16_t listen_arm_wait_ = 0;
    // 眨眼贴片上再压一层：贴片会带出「睁眼时被眼球挡住的刘海」，按发色把它染掉
    lv_obj_t* blink_hair_image_ = nullptr;
    const lv_image_dsc_t* mouth_img_ = nullptr;
    const lv_image_dsc_t* blink_img_ = nullptr;
    const lv_image_dsc_t* blink_hair_img_ = nullptr;
    uint16_t blink_tick_ = 0;
    // 下一次眨眼要等的 tick 数：固定 32 拍（3.2s）像个节拍器，人眼一眼看出是机器
    uint16_t blink_next_ = 0;
    uint8_t blink_frames_ = 0;
    uint16_t mouth_tick_ = 0;
    bool mouth_open_ = false;
    bool mouth_active_ = false;
    // 天气整块纵向滚动：视窗高度固定，行数多了（或长行折行后）靠上下滚动露出来
    static constexpr int kWeatherMaxLines = 8;
    lv_obj_t* weather_box_ = nullptr;
    lv_obj_t* weather_inner_ = nullptr;
    lv_obj_t* weather_lines_[kWeatherMaxLines] = {};
    std::string weather_text_;
    // 对话：屏幕上只留最新一句，超出一屏就从这句话的开头裁切
    std::string chat_text_;
    lv_obj_t* chat_view_ = nullptr;
    // 科幻底图（渐变/网格/角标烘成图 + 核心/环/火星/流光几样小对象动态）。wechat 样式分支不建
    TechBackdrop backdrop_;
    void PlaceChatView(int vh);
    void RefitWeatherView();
    // 聆听微表情：眼神游移 +「嗯」微张嘴 + 克制的换姿势，只挂在 listening 态上
    void ListenAnimTick();
    void ClearListenAnim();
    void LayoutContentBlocks();

    DisplayFonts fonts_;
    ThemeColors current_theme_;
    // 当前该用的 HUD 主色：配过用 accent_color_，没配按深浅主题取默认
    lv_color_t AccentColor() const;

    void SetupUI();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    // FFT 绘制方法
    void readAudioData();
    
    
  
    virtual void clearScreen() override;
    virtual void stopFft() override;  // 停止FFT显示
    
    // 定时任务方法
    void periodicUpdateTask();
    static void periodicUpdateTaskWrapper(void* arg);
    
    // LVGL变量
    lv_obj_t* canvas_ = nullptr;
    uint16_t* canvas_buffer_ = nullptr;
    void create_canvas();
    uint16_t get_bar_color(int x_pos);
    void draw_spectrum(float *power_spectrum,int fft_size);
    void draw_bar(int x,int y,int bar_width,int bar_height,uint16_t color,int bar_index);
    void draw_block(int x,int y,int block_x_size,int block_y_size,uint16_t color,int bar_index);
    
    int canvas_width_;
    int canvas_height_;
   
    
    int16_t* audio_data=nullptr;
    int16_t* frame_audio_data=nullptr;
    uint32_t last_fft_update = 0;
    bool fft_data_ready = false;
    float* spectrum_data=nullptr;

    // FFT 相关变量
    int audio_display_last_update = 0;
    std::atomic<bool> fft_task_should_stop = false;  // FFT任务停止标志
    TaskHandle_t fft_task_handle = nullptr;          // FFT任务句柄

    float* fft_real;
    float* fft_imag;
    float* hanning_window_float;
    void compute(float* real, float* imag, int n, bool forward);
    
    // 添加缺少的方法声明
    void drawSpectrumIfReady();
    void MyUI();

    

protected:
    // 添加protected构造函数
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, DisplayFonts fonts, int width, int height);
    
public:
    ~LcdDisplay();
    void BlinkTick();
    // 小人整体呼吸：三层 y 由同一个 tick 统一算并同时下发
    void AvatarFloatTick();
    void ApplyAvatarY();
    // 按 hair_color_ 决定染色层显不显示：0 = 隐藏（用原图），非 0 = 新色 + 阴影层
    void ApplyHairTint();
    // 头发两层提到最前。嘴/眨眼贴片每次显示后都要重提，漏一处那一帧头发就被盖住
    void RaiseHairLayers();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetHairColor(uint32_t rgb) override;
    virtual void SetAccentColor(uint32_t rgb, int strength = -1) override;
    virtual void SetIcon(const char* icon) override;
    virtual void SetMusicInfo(const char* song_name) override;
    virtual void SetWeatherText(const char* text) override;
    virtual void SetPreviewImage(const lv_img_dsc_t* img_dsc) override;
    virtual void SetChatMessage(const char* role, const char* content) override;  

    // Add theme switching function
    virtual void SetTheme(const std::string& theme_name) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void start() override;

};

// RGB LCD显示器
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy,
                  DisplayFonts fonts);
};

// MIPI LCD显示器
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy,
                   DisplayFonts fonts);
};

// // SPI LCD显示器
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy,
                  DisplayFonts fonts);
};

// QSPI LCD显示器
class QspiLcdDisplay : public LcdDisplay {
public:
    QspiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy,
                   DisplayFonts fonts);
};

// MCU8080 LCD显示器
class Mcu8080LcdDisplay : public LcdDisplay {
public:
    Mcu8080LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                      int width, int height, int offset_x, int offset_y,
                      bool mirror_x, bool mirror_y, bool swap_xy,
                      DisplayFonts fonts);
};
#endif // LCD_DISPLAY_H
