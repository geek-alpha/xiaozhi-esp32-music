#include "tech_backdrop.h"

#include <esp_log.h>

#include "bg_assets.h"

#define TAG "TechBackdrop"

namespace {

// 与 tools/gen_bg_asset.py 的 EMOJI_CX/EMOJI_CY、_tube() 端点严格对齐：
// 动态对象必须落回底图已经画好的光环和灯管上，差几像素就看得出错位。
constexpr int kCoreCx = 120;
constexpr int kCoreCy = 91;

void SetBgOpa(void* var, int32_t v) {
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(var), (lv_opa_t)v, 0);
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

    ESP_LOGI(TAG, "Anemone backdrop: core only");

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

    // 整屏染色这条路废了：底图是不透明位图，任何半透明色叠上去都只是把整块屏蒙一层，
    // 字和图一起变淡——用户的原话就是「整个屏幕都被颜色盖掉」、
    // 「文字和背景对比不强」。要「背景就是某颜色」走 SetSolidBackground（纯色底 + 对比前景）
    if (tint_ != nullptr) {
        lv_obj_set_style_bg_color(tint_, base, 0);
        lv_obj_set_style_bg_opa(tint_, LV_OPA_TRANSP, 0);
        ESP_LOGI(TAG, "Tint #%06X: screen wash off (accent -> effects only)", (unsigned)accent);
    }

    if (core_ != nullptr) {
        lv_obj_set_style_bg_color(core_, base, 0);
    }

    // 底色接管期间任何一次重刷（换主题/换主色/开机恢复）都得把纯色层压回去，
    // 否则上面那行 tint_ opa 会把它重置成透明，用户看到「刚才设的底色又没了」
    if (solid_rgb_ != 0) {
        SetSolidBackground(solid_rgb_);
    }
}

void TechBackdrop::SetSolidBackground(uint32_t rgb)
{
    const uint32_t color = rgb & 0xFFFFFF;
    if (color == 0) {
        // 本来就没接管：SetTheme 刚调过 ApplyTheme，这里再重放一次就是白刷一帧
        if (solid_rgb_ == 0) {
            return;
        }
        solid_rgb_ = 0;
        if (bg_image_ != nullptr) {
            lv_obj_clear_flag(bg_image_, LV_OBJ_FLAG_HIDDEN);
        }
        // 回底图：按当前主题重放一次，别把 solid 期间留下的纯色/对比色留在场上
        ApplyTheme(dark_, accent_, strength_);
        ESP_LOGI(TAG, "Solid background off, theme backdrop restored");
        return;
    }

    solid_rgb_ = color;
    if (bg_image_ != nullptr) {
        lv_obj_add_flag(bg_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (tint_ != nullptr) {
        lv_obj_set_style_bg_color(tint_, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(tint_, LV_OPA_COVER, 0);
    }

    // 底色是用户直接指定的，动态层必须跟它拉开：亮底用深色元素，暗底用亮色元素。
    // 跟着 accent 派生的话，红底上的红色核心就是隐形的
    const int lum = (299 * ((color >> 16) & 0xFF) + 587 * ((color >> 8) & 0xFF)
                     + 114 * (color & 0xFF)) / 1000;
    const bool light_bg = lum > 150;
    const lv_color_t base = lv_color_hex(light_bg ? 0x14141A : 0xF2F6FF);

    if (core_ != nullptr) {
        lv_obj_set_style_bg_color(core_, base, 0);
    }
    ESP_LOGI(TAG, "Solid background #%06X (lum %d), dynamic layers -> contrast",
             (unsigned)color, lum);
}


void TechBackdrop::SetAnimationsEnabled(bool on)
{
    if (on == anim_on_) {
        return;
    }
    anim_on_ = on;
    if (core_ == nullptr) {
        return;
    }
    if (!on) {
        StopAnims();
        return;
    }
    StartCoreAnim();
}

void TechBackdrop::StopAnims()
{
    lv_anim_del(core_, nullptr);
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
