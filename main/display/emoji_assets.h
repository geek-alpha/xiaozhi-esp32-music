// 由 tools/gen_emoji_assets.py 生成，勿手改
#pragma once

#include "lvgl.h"

#define EMOJI_W 162
#define EMOJI_H 162
#define MOUTH_W 30
#define MOUTH_H 35
#define MOUTH_OX 66
#define MOUTH_OY 94
#define BLINK_W 98
#define BLINK_H 56
#define BLINK_OX 28
#define BLINK_OY 50

#define EMOJI_BODY_W 122
#define EMOJI_BODY_H 80
#define EMOJI_BODY_OX 20
#define EMOJI_BODY_OY 130
#define EMOJI_TOTAL_H 210

#define EMOJI_SHADOW_W 162
#define EMOJI_SHADOW_H 210

extern const lv_image_dsc_t emoji_shadow;
#define EMOJI_ARM_FRAMES 3
extern const lv_image_dsc_t emoji_body;
extern const lv_image_dsc_t emoji_arm_0;
extern const lv_image_dsc_t emoji_arm_1;
extern const lv_image_dsc_t emoji_arm_2;
// 表情表：patch 是相对底图 emoji_neutral 的差异块，NULL 表示这张就是底图本身。
// C 侧只显示一张底图，切表情 = 换补丁的 src/pos，不用为 22 张脸各存一份全图
typedef struct {
    const char * name;
    const lv_image_dsc_t * patch;
    int16_t px, py, pw, ph;
    const lv_image_dsc_t * mouth;
    const lv_image_dsc_t * blink;
    const lv_image_dsc_t * blink_hair;
    const lv_image_dsc_t * hair;
} emoji_asset_t;

extern const emoji_asset_t emoji_table[];
extern const int emoji_table_count;
#define GAZE_COUNT 3
#define GAZE_W 96
#define GAZE_H 54
#define GAZE_OX 30
#define GAZE_OY 50
extern const lv_image_dsc_t emoji_gaze_0;
extern const lv_image_dsc_t emoji_gaze_1;
extern const lv_image_dsc_t emoji_gaze_2;
extern const lv_image_dsc_t emoji_neutral_hair;
extern const lv_image_dsc_t emoji_listening_hair;
extern const lv_image_dsc_t emoji_happy_hair;
extern const lv_image_dsc_t emoji_laughing_hair;
extern const lv_image_dsc_t emoji_funny_hair;
extern const lv_image_dsc_t emoji_sad_hair;
extern const lv_image_dsc_t emoji_angry_hair;
extern const lv_image_dsc_t emoji_crying_hair;
extern const lv_image_dsc_t emoji_loving_hair;
extern const lv_image_dsc_t emoji_embarrassed_hair;
extern const lv_image_dsc_t emoji_surprised_hair;
extern const lv_image_dsc_t emoji_shocked_hair;
extern const lv_image_dsc_t emoji_thinking_hair;
extern const lv_image_dsc_t emoji_winking_hair;
extern const lv_image_dsc_t emoji_cool_hair;
extern const lv_image_dsc_t emoji_relaxed_hair;
extern const lv_image_dsc_t emoji_delicious_hair;
extern const lv_image_dsc_t emoji_kissy_hair;
extern const lv_image_dsc_t emoji_confident_hair;
extern const lv_image_dsc_t emoji_sleepy_hair;
extern const lv_image_dsc_t emoji_silly_hair;
extern const lv_image_dsc_t emoji_confused_hair;
extern const lv_image_dsc_t emoji_neutral;
extern const lv_image_dsc_t emoji_neutral_mouth;
extern const lv_image_dsc_t emoji_neutral_blink;
extern const lv_image_dsc_t emoji_neutral_blink_hair;
extern const lv_image_dsc_t emoji_patch_listening;
extern const lv_image_dsc_t emoji_listening_mouth;
extern const lv_image_dsc_t emoji_listening_blink;
extern const lv_image_dsc_t emoji_listening_blink_hair;
extern const lv_image_dsc_t emoji_patch_happy;
extern const lv_image_dsc_t emoji_happy_mouth;
extern const lv_image_dsc_t emoji_happy_blink;
extern const lv_image_dsc_t emoji_happy_blink_hair;
extern const lv_image_dsc_t emoji_patch_laughing;
extern const lv_image_dsc_t emoji_laughing_mouth;
extern const lv_image_dsc_t emoji_patch_funny;
extern const lv_image_dsc_t emoji_funny_mouth;
extern const lv_image_dsc_t emoji_patch_sad;
extern const lv_image_dsc_t emoji_sad_mouth;
extern const lv_image_dsc_t emoji_sad_blink;
extern const lv_image_dsc_t emoji_sad_blink_hair;
extern const lv_image_dsc_t emoji_patch_angry;
extern const lv_image_dsc_t emoji_angry_mouth;
extern const lv_image_dsc_t emoji_angry_blink;
extern const lv_image_dsc_t emoji_angry_blink_hair;
extern const lv_image_dsc_t emoji_patch_crying;
extern const lv_image_dsc_t emoji_crying_mouth;
extern const lv_image_dsc_t emoji_crying_blink;
extern const lv_image_dsc_t emoji_crying_blink_hair;
extern const lv_image_dsc_t emoji_patch_loving;
extern const lv_image_dsc_t emoji_loving_mouth;
extern const lv_image_dsc_t emoji_loving_blink;
extern const lv_image_dsc_t emoji_loving_blink_hair;
extern const lv_image_dsc_t emoji_patch_embarrassed;
extern const lv_image_dsc_t emoji_embarrassed_mouth;
extern const lv_image_dsc_t emoji_embarrassed_blink;
extern const lv_image_dsc_t emoji_embarrassed_blink_hair;
extern const lv_image_dsc_t emoji_patch_surprised;
extern const lv_image_dsc_t emoji_surprised_mouth;
extern const lv_image_dsc_t emoji_surprised_blink;
extern const lv_image_dsc_t emoji_surprised_blink_hair;
extern const lv_image_dsc_t emoji_patch_shocked;
extern const lv_image_dsc_t emoji_shocked_mouth;
extern const lv_image_dsc_t emoji_shocked_blink;
extern const lv_image_dsc_t emoji_shocked_blink_hair;
extern const lv_image_dsc_t emoji_patch_thinking;
extern const lv_image_dsc_t emoji_thinking_mouth;
extern const lv_image_dsc_t emoji_thinking_blink;
extern const lv_image_dsc_t emoji_thinking_blink_hair;
extern const lv_image_dsc_t emoji_patch_winking;
extern const lv_image_dsc_t emoji_winking_mouth;
extern const lv_image_dsc_t emoji_winking_blink;
extern const lv_image_dsc_t emoji_winking_blink_hair;
extern const lv_image_dsc_t emoji_patch_cool;
extern const lv_image_dsc_t emoji_cool_mouth;
extern const lv_image_dsc_t emoji_patch_relaxed;
extern const lv_image_dsc_t emoji_relaxed_mouth;
extern const lv_image_dsc_t emoji_patch_delicious;
extern const lv_image_dsc_t emoji_delicious_mouth;
extern const lv_image_dsc_t emoji_delicious_blink;
extern const lv_image_dsc_t emoji_delicious_blink_hair;
extern const lv_image_dsc_t emoji_patch_kissy;
extern const lv_image_dsc_t emoji_kissy_mouth;
extern const lv_image_dsc_t emoji_kissy_blink;
extern const lv_image_dsc_t emoji_kissy_blink_hair;
extern const lv_image_dsc_t emoji_patch_confident;
extern const lv_image_dsc_t emoji_confident_mouth;
extern const lv_image_dsc_t emoji_confident_blink;
extern const lv_image_dsc_t emoji_confident_blink_hair;
extern const lv_image_dsc_t emoji_patch_sleepy;
extern const lv_image_dsc_t emoji_sleepy_mouth;
extern const lv_image_dsc_t emoji_patch_silly;
extern const lv_image_dsc_t emoji_silly_mouth;
extern const lv_image_dsc_t emoji_silly_blink;
extern const lv_image_dsc_t emoji_silly_blink_hair;
extern const lv_image_dsc_t emoji_patch_confused;
extern const lv_image_dsc_t emoji_confused_mouth;
extern const lv_image_dsc_t emoji_confused_blink;
extern const lv_image_dsc_t emoji_confused_blink_hair;
