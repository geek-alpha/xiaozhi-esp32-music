#ifndef TECH_BACKDROP_H
#define TECH_BACKDROP_H

#include <lvgl.h>

// 机甲格斗待机背景：底图（装甲板/交叉光刃/能量核/弹药格，见 tools/gen_bg_asset.py）压在最底层，
// 动态只剩几样便宜的东西——光刃上跑的流光、从核心扩散的冲击环、斜坠的火星、护盾扫描带、核心呼吸。
//
// 每样动态都是小对象：冲击环直径封顶 112（脏区约 24KB）、火星 2x2、流光 3x3。
// 底图不参与动画，一次 blit 完事，不会跟表情动画和音频处理抢 CPU。
class TechBackdrop {
public:
    // 挂在 screen 上并压到最底层；status_h 是状态栏高度，扫描带起点要对齐
    void Create(lv_obj_t* parent, int w, int h, int status_h);

    // 主题切换换底图（dark / light），不改动已建好的动态对象。
    // accent 非 0 时动态层（扫描带/核心/环/火星/流光）按它派生，0 = 沿用主题默认那套硬编码色。
    // strength 是整屏染色强度 0~10：底图占了全屏 100% 面积，只换那几样小元素的颜色肉眼看不出，
    // 必须再压一层全屏半透明主色（color grading）整屏才会真的变。0 = 不染，只换动态层。
    void ApplyTheme(bool dark, uint32_t accent = 0, int strength = kDefaultTint);

    // 省电/熄屏时停动画，回前台再起
    void SetAnimationsEnabled(bool on);

private:
    static constexpr int kRings = 2;
    static constexpr int kSparks = 7;
    static constexpr int kTrail = 4;
    static constexpr int kSweepH = 18;
    // 默认染色强度：底图本身很暗，240x320 上叠 ~24% 的主色已经很明显，再高就糊掉装甲板纹理
    static constexpr int kDefaultTint = 3;
    static constexpr int kMaxTint = 10;

    void StartSweepAnim();
    void StartCoreAnim();
    void StartRingAnim();
    void StartSparkAnim();
    void StartTrailAnim();
    void StopAnims();

    lv_obj_t* root_ = nullptr;
    lv_obj_t* bg_image_ = nullptr;
    // 全屏染色层，顺序在底图之上、所有动态元素之下
    lv_obj_t* tint_ = nullptr;
    lv_obj_t* sweep_ = nullptr;
    lv_obj_t* core_ = nullptr;
    lv_obj_t* rings_[kRings] = {};
    lv_obj_t* sparks_[kSparks] = {};
    lv_obj_t* trail_[kTrail] = {};

    int w_ = 0;
    int h_ = 0;
    int status_h_ = 0;
    bool dark_ = true;
    uint32_t accent_ = 0;
    int strength_ = kDefaultTint;
    bool anim_on_ = false;
};

#endif
