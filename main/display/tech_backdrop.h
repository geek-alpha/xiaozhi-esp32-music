#ifndef TECH_BACKDROP_H
#define TECH_BACKDROP_H

#include <lvgl.h>

// 金鱼娘待机背景：底图（水色渐变/尾纱/气泡/鳞片/樱花瓣，见 tools/gen_bg_asset.py）
// 压在最底层，动态只剩脸后那圈呼吸暖光。
// 上浮粒子、从脸后扩散的冲击环、底部两侧飞的流光已按用户要求全部去掉：
// 那些动效在 240x240 上只是噪点。
//
// 动态只有一个小对象（核心 46x46），底图不参与动画，一次 blit 完事，
// 不会跟表情动画和音频处理抢 CPU。
class TechBackdrop {
public:
    // 挂在 screen 上并压到最底层；status_h 是状态栏高度，气泡飞到顶栏下方就重头再来
    void Create(lv_obj_t* parent, int w, int h, int status_h);

    // 主题切换换底图（dark / light），不改动已建好的动态对象。
    // accent 非 0 时动态层（核心/流光）按它派生，0 = 沿用主题默认那套硬编码色。
    // strength 是整屏染色强度 0~10：底图占了全屏 100% 面积，只换那几样小元素的颜色肉眼看不出，
    // 必须再压一层全屏半透明主色（color grading）整屏才会真的变。0 = 不染，只换动态层。
    void ApplyTheme(bool dark, uint32_t accent = 0, int strength = kDefaultTint);

    // 底色接管：rgb 非 0 时整屏铺这个纯色（不透明），底图隐藏；0 = 交回主题底图。
    // 跟 ApplyTheme 的 strength 不是一回事——那是半透明洗色，叠在不透明底图上只能把
    // 颜色洗淡，用户要的正红/正黄出不来。这条是「背景本身就是这个颜色」。
    // 动态层按底色亮度取对比色，否则核心/环/气泡跟底同色直接消失。
    void SetSolidBackground(uint32_t rgb);

    // 省电/熄屏时停动画，回前台再起
    void SetAnimationsEnabled(bool on);

private:
    // 默认染色强度：底图本身很暗，240x240 上叠 ~24% 的主色已经很明显，再高就糊掉纹理
    static constexpr int kDefaultTint = 3;
    static constexpr int kMaxTint = 10;

    void StartCoreAnim();
    void StopAnims();

    lv_obj_t* root_ = nullptr;
    lv_obj_t* bg_image_ = nullptr;
    // 全屏染色层，顺序在底图之上、所有动态元素之下
    lv_obj_t* tint_ = nullptr;
    lv_obj_t* core_ = nullptr;

    int w_ = 0;
    int h_ = 0;
    int status_h_ = 0;
    bool dark_ = true;
    uint32_t accent_ = 0;
    int strength_ = kDefaultTint;
    bool anim_on_ = false;
    // 非 0 = 用户在管的底色。ApplyTheme 每次重刷都要把它重新压回去，
    // 不然换主题/换主色一轮，纯色底就被 ApplyTheme 里的 opa 重置成透明了
    uint32_t solid_rgb_ = 0;
};

#endif
