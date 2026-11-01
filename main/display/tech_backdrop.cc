#include "tech_backdrop.h"

#include <esp_log.h>

#include "bg_assets.h"

#define TAG "TechBackdrop"

namespace {

// 与 tools/gen_bg_asset.py 的 EMOJI_CX/EMOJI_CY、_tube() 端点严格对齐：
// 动态对象必须落回底图已经画好的光环和灯管上，差几像素就看得出错位。
constexpr int kCoreCx = 120;
constexpr int kCoreCy = 91;

constexpr int kBladeA[2][2] = {{6, 158}, {234, 196}};
constexpr int kBladeB[2][2] = {{234, 150}, {10, 190}};

// 雨滴：x 散在全屏，只给小幅度偏移，落的是斜雨不是喷泉
constexpr int kSparkX0[] = {26, 54, 86, 118, 146, 176, 208};
constexpr int kSparkDx[] = {5, -4, 6, -5, 4, -6, 5};
constexpr uint16_t kSparkDur[] = {1500, 1210, 1780, 1340, 1620, 1120, 1720};
constexpr uint16_t kSparkDelay[] = {0, 260, 520, 760, 180, 980, 640};

constexpr uint16_t kTrailDelay[] = {0, 340, 180, 620};
constexpr uint16_t kTrailDur[] = {2200, 2600, 2000, 2800};

struct RingCtx {
    lv_obj_t* obj;
    int cx;
    int cy;
};

RingCtx g_ring_ctx[2];

void SetX(void* var, int32_t v) { lv_obj_set_x(static_cast<lv_obj_t*>(var), v); }
void SetY(void* var, int32_t v) { lv_obj_set_y(static_cast<lv_obj_t*>(var), v); }
void SetBgOpa(void* var, int32_t v) {
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(var), (lv_opa_t)v, 0);
}
void SetBorderOpa(void* var, int32_t v) {
    lv_obj_set_style_border_opa(static_cast<lv_obj_t*>(var), (lv_opa_t)v, 0);
}

// 环要「以核心为圆心往外长」，所以 size 和 pos 必须同一条回调里改
void RingSize(void* var, int32_t v) {
    auto* r = static_cast<RingCtx*>(var);
    lv_obj_set_size(r->obj, v, v);
    lv_obj_set_pos(r->obj, r->cx - v / 2, r->cy - v / 2);
}

lv_obj_t* MakeDot(lv_obj_t* parent, int size, uint32_t color, lv_opa_t opa) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_add_flag(o, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    return o;
}

}  // namespace

void TechBackdrop::Create(lv_obj_t* parent, int w, int h, int status_h)
{
    w_ = w;
    h_ = h;
    status_h_ = status_h;

    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_size(root_, w, h);
    lv_obj_move_to_index(root_, 0);

    bg_image_ = lv_image_create(root_);
    lv_obj_remove_style_all(bg_image_);
    lv_obj_set_pos(bg_image_, 0, 0);
    lv_image_set_src(bg_image_, &bg_dark);

    // 全屏染色层：紧跟在底图之后建，后建的盖在上面，所以它压住底图但不遮动态元素。
    // accent=0 时透明度 0，透传下去完全是旧观感，不动老布局。
    tint_ = lv_obj_create(root_);
    lv_obj_remove_style_all(tint_);
    lv_obj_add_flag(tint_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(tint_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(tint_, 0, 0);
    lv_obj_set_size(tint_, w, h);
    lv_obj_set_style_bg_opa(tint_, LV_OPA_0, 0);
    lv_obj_set_style_radius(tint_, 0, 0);
    lv_obj_set_style_border_width(tint_, 0, 0);

    // 核心呼吸：能量核上再叠一层明灭的光，标出「核心在充能」
    core_ = lv_obj_create(root_);
    lv_obj_remove_style_all(core_);
    lv_obj_add_flag(core_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(core_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(core_, 46, 46);
    lv_obj_set_pos(core_, kCoreCx - 23, kCoreCy - 23);
    lv_obj_set_style_radius(core_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(core_, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_bg_opa(core_, LV_OPA_10, 0);
    lv_obj_set_style_border_width(core_, 0, 0);

    // 冲击环：从核心往外扩、边扩边淡出
    for (int i = 0; i < kRings; i++) {
        lv_obj_t* r = lv_obj_create(root_);
        lv_obj_remove_style_all(r);
        lv_obj_add_flag(r, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(r, 40, 40);
        lv_obj_set_pos(r, kCoreCx - 20, kCoreCy - 20);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_0, 0);
        lv_obj_set_style_border_width(r, 2, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(0xFF3E9E), 0);
        lv_obj_set_style_border_opa(r, LV_OPA_0, 0);
        rings_[i] = r;
        g_ring_ctx[i] = {r, kCoreCx, kCoreCy};
    }

    for (int i = 0; i < kSparks; i++) {
        sparks_[i] = MakeDot(root_, 2, i % 2 == 0 ? 0xBFF6FF : 0xFF6AB0, LV_OPA_70);
        lv_obj_set_pos(sparks_[i], kSparkX0[i], status_h_ - 6);
    }

    for (int i = 0; i < kTrail; i++) {
        trail_[i] = MakeDot(root_, 3, i % 2 == 0 ? 0xA8FFF4 : 0xFF6AB0, LV_OPA_80);
        lv_obj_set_pos(trail_[i], kBladeA[0][0], kBladeA[0][1]);
    }

    // 扫描带：一条半透明楼层上下走，像 CRT 刷屏；颜色用青，跟底图灯管同色系
    sweep_ = lv_obj_create(root_);
    lv_obj_remove_style_all(sweep_);
    lv_obj_add_flag(sweep_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(sweep_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(sweep_, w, kSweepH);
    lv_obj_set_style_bg_color(sweep_, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_bg_opa(sweep_, LV_OPA_20, 0);
    lv_obj_set_style_radius(sweep_, 0, 0);
    lv_obj_set_style_border_width(sweep_, 0, 0);
    lv_obj_set_y(sweep_, status_h_);

    ESP_LOGI(TAG, "Neon backdrop: %d rings, %d rain, %d trail, sweep %d px",
             kRings, kSparks, kTrail, kSweepH);
    SetAnimationsEnabled(true);
}

void TechBackdrop::ApplyTheme(bool dark, uint32_t accent, int strength)
{
    dark_ = dark;
    accent_ = accent;
    strength_ = strength;
    if (bg_image_ != nullptr) {
        lv_image_set_src(bg_image_, dark ? &bg_dark : &bg_light);
    }

    // accent==0 走原来那套手调色，观感跟旧版逐像素一致；配了 accent 才从它派生
    const lv_color_t base = lv_color_hex(accent != 0 ? accent : (dark ? 0x00E5FF : 0xD8408C));
    const lv_color_t ring = accent != 0 ? lv_color_mix(base, lv_color_white(), 60)
                                        : lv_color_hex(dark ? 0xFF3E9E : 0xC03878);
    const lv_color_t spark = accent != 0 ? lv_color_mix(base, lv_color_white(), 140)
                                         : lv_color_hex(dark ? 0xBFF6FF : 0x3A82B4);
    const lv_color_t trail_a = accent != 0 ? lv_color_mix(base, lv_color_white(), 100)
                                           : lv_color_hex(dark ? 0xA8FFF4 : 0x8A5AC8);
    const lv_color_t trail_b = accent != 0 ? base : lv_color_hex(dark ? 0xFF6AB0 : 0xC03878);

    // 整屏染色：底图占满全屏，不染它的话换主色只有几样小元素变，肉眼几乎看不出来。
    // 10 级映射到 0~230，默认 3 约 24%。
    if (tint_ != nullptr) {
        lv_obj_set_style_bg_color(tint_, base, 0);
        int s = strength;
        if (s < 0) s = 0;
        if (s > kMaxTint) s = kMaxTint;
        lv_obj_set_style_bg_opa(tint_, (lv_opa_t)(accent != 0 ? s * 23 : 0), 0);
    }

    if (sweep_ != nullptr) {
        lv_obj_set_style_bg_color(sweep_, base, 0);
    }
    if (core_ != nullptr) {
        lv_obj_set_style_bg_color(core_, base, 0);
    }
    for (int i = 0; i < kRings; i++) {
        if (rings_[i] != nullptr) {
            lv_obj_set_style_border_color(rings_[i], ring, 0);
        }
    }
    for (int i = 0; i < kSparks; i++) {
        if (sparks_[i] != nullptr) {
            lv_obj_set_style_bg_color(sparks_[i], spark, 0);
        }
    }
    for (int i = 0; i < kTrail; i++) {
        if (trail_[i] != nullptr) {
            lv_obj_set_style_bg_color(trail_[i], i % 2 == 0 ? trail_a : trail_b, 0);
        }
    }
}


void TechBackdrop::SetAnimationsEnabled(bool on)
{
    if (on == anim_on_) {
        return;
    }
    anim_on_ = on;
    if (sweep_ == nullptr) {
        return;
    }
    if (!on) {
        StopAnims();
        return;
    }
    StartSweepAnim();
    StartCoreAnim();
    StartRingAnim();
    StartSparkAnim();
    StartTrailAnim();
}

void TechBackdrop::StopAnims()
{
    lv_anim_del(sweep_, nullptr);
    lv_anim_del(core_, nullptr);
    for (int i = 0; i < kRings; i++) {
        if (rings_[i] != nullptr) {
            lv_anim_del(rings_[i], nullptr);
            lv_anim_del(&g_ring_ctx[i], nullptr);
        }
    }
    for (int i = 0; i < kSparks; i++) {
        if (sparks_[i] != nullptr) {
            lv_anim_del(sparks_[i], nullptr);
        }
    }
    for (int i = 0; i < kTrail; i++) {
        if (trail_[i] != nullptr) {
            lv_anim_del(trail_[i], nullptr);
        }
    }
}

void TechBackdrop::StartSweepAnim()
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sweep_);
    lv_anim_set_exec_cb(&a, SetY);
    lv_anim_set_values(&a, status_h_ - kSweepH, h_);
    lv_anim_set_duration(&a, 4600);
    lv_anim_set_playback_duration(&a, 4600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_repeat_delay(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    // 亮度跟着一起呼吸：只有位置变化会显得太机械
    lv_anim_t o;
    lv_anim_init(&o);
    lv_anim_set_var(&o, sweep_);
    lv_anim_set_exec_cb(&o, SetBgOpa);
    lv_anim_set_values(&o, LV_OPA_10, LV_OPA_30);
    lv_anim_set_duration(&o, 1600);
    lv_anim_set_playback_duration(&o, 1600);
    lv_anim_set_repeat_count(&o, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&o, lv_anim_path_ease_in_out);
    lv_anim_start(&o);
}

void TechBackdrop::StartCoreAnim()
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, core_);
    lv_anim_set_exec_cb(&a, SetBgOpa);
    lv_anim_set_values(&a, LV_OPA_10, LV_OPA_30);
    lv_anim_set_duration(&a, 1400);
    lv_anim_set_playback_duration(&a, 1400);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

void TechBackdrop::StartRingAnim()
{
    for (int i = 0; i < kRings; i++) {
        if (rings_[i] == nullptr) {
            continue;
        }
        lv_anim_t s;
        lv_anim_init(&s);
        lv_anim_set_var(&s, &g_ring_ctx[i]);
        lv_anim_set_exec_cb(&s, RingSize);
        lv_anim_set_values(&s, 40, 112);
        lv_anim_set_duration(&s, 1700);
        lv_anim_set_delay(&s, i * 850);
        lv_anim_set_repeat_count(&s, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&s, lv_anim_path_ease_out);
        lv_anim_start(&s);

        lv_anim_t o;
        lv_anim_init(&o);
        lv_anim_set_var(&o, rings_[i]);
        lv_anim_set_exec_cb(&o, SetBorderOpa);
        lv_anim_set_values(&o, 200, 0);
        lv_anim_set_duration(&o, 1700);
        lv_anim_set_delay(&o, i * 850);
        lv_anim_set_repeat_count(&o, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&o, lv_anim_path_ease_out);
        lv_anim_start(&o);
    }
}

void TechBackdrop::StartSparkAnim()
{
    for (int i = 0; i < kSparks; i++) {
        if (sparks_[i] == nullptr) {
            continue;
        }
        lv_anim_t ax;
        lv_anim_init(&ax);
        lv_anim_set_var(&ax, sparks_[i]);
        lv_anim_set_exec_cb(&ax, SetX);
        lv_anim_set_values(&ax, kSparkX0[i], kSparkX0[i] + kSparkDx[i]);
        lv_anim_set_duration(&ax, kSparkDur[i]);
        lv_anim_set_delay(&ax, kSparkDelay[i]);
        lv_anim_set_repeat_count(&ax, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&ax, lv_anim_path_linear);
        lv_anim_start(&ax);

        lv_anim_t ay;
        lv_anim_init(&ay);
        lv_anim_set_var(&ay, sparks_[i]);
        lv_anim_set_exec_cb(&ay, SetY);
        lv_anim_set_values(&ay, status_h_ - 6, h_ + 4);
        lv_anim_set_duration(&ay, kSparkDur[i]);
        lv_anim_set_delay(&ay, kSparkDelay[i]);
        lv_anim_set_repeat_count(&ay, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&ay, lv_anim_path_linear);
        lv_anim_start(&ay);
    }
}

void TechBackdrop::StartTrailAnim()
{
    for (int i = 0; i < kTrail; i++) {
        if (trail_[i] == nullptr) {
            continue;
        }
        const int(*blade)[2] = (i % 2 == 0) ? kBladeA : kBladeB;
        lv_anim_t ax;
        lv_anim_init(&ax);
        lv_anim_set_var(&ax, trail_[i]);
        lv_anim_set_exec_cb(&ax, SetX);
        lv_anim_set_values(&ax, blade[0][0], blade[1][0]);
        lv_anim_set_duration(&ax, kTrailDur[i]);
        lv_anim_set_delay(&ax, kTrailDelay[i]);
        lv_anim_set_repeat_count(&ax, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&ax, lv_anim_path_linear);
        lv_anim_start(&ax);

        lv_anim_t ay;
        lv_anim_init(&ay);
        lv_anim_set_var(&ay, trail_[i]);
        lv_anim_set_exec_cb(&ay, SetY);
        lv_anim_set_values(&ay, blade[0][1], blade[1][1]);
        lv_anim_set_duration(&ay, kTrailDur[i]);
        lv_anim_set_delay(&ay, kTrailDelay[i]);
        lv_anim_set_repeat_count(&ay, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&ay, lv_anim_path_linear);
        lv_anim_start(&ay);
    }
}
