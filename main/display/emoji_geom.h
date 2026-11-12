// 由 tools/mk_assets_bin.py 生成，勿手改
//
// 每个角色的贴片几何。脸图统一 162x162，但眼位/嘴位各不相同：
// 眨眼贴片宽 54~78、x 起点 32~56，视线贴片 x 起点 38~60。
// 固件按当前绑定的角色取这一份，不再用大肥鱼那套全局宏。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;
    int16_t emoji_w, emoji_h;
    int16_t mouth_w, mouth_h, mouth_ox, mouth_oy;
    int16_t blink_w, blink_h, blink_ox, blink_oy;
    int16_t gaze_count, gaze_w, gaze_h, gaze_ox, gaze_oy;
    int16_t body_w, body_h, body_ox, body_oy;
    int16_t shadow_w, shadow_h, total_h;
} emoji_geom_t;

// 顺序与 assets 分区 slot 表一致（同一个 CASTS 列表产出），下标可直接当 slot 号
extern const emoji_geom_t emoji_geom_table[];
extern const int emoji_geom_count;

// 按 key 找几何。一个角色一个固件包时，分区 slot 0 是谁都不定，
// 按下标取会拿别人的眼位画眨眼，只能按名字查。找不到返回 NULL。
const emoji_geom_t *emoji_geom_find(const char *key);

#ifdef __cplusplus
}
#endif
