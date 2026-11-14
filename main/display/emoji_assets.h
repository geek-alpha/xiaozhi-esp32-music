// 由 tools/gen_emoji_assets.py --pack 生成，勿手改
#pragma once

#include "lvgl.h"
#include "emoji_pack.h"

#define EMOJI_W 162
#define EMOJI_H 162
#define MOUTH_W 18
#define MOUTH_H 14
#define MOUTH_OX 57
#define MOUTH_OY 86
#define BLINK_W 60
#define BLINK_H 24
#define BLINK_OX 34
#define BLINK_OY 68
#define EMOJI_BODY_W 162
#define EMOJI_BODY_H 81
#define EMOJI_BODY_OX 0
#define EMOJI_BODY_OY 100
#define EMOJI_TOTAL_H 181
#define EMOJI_SHADOW_W 162
#define EMOJI_SHADOW_H 181
#define EMOJI_ARM_FRAMES 3
#define GAZE_COUNT 3
#define GAZE_W 56
#define GAZE_H 18
#define GAZE_OX 38
#define GAZE_OY 70

typedef struct {
    const char * name;
    const lv_image_dsc_t * patch;
    int16_t px, py, pw, ph;
    const lv_image_dsc_t * mouth;
    const lv_image_dsc_t * blink;
    const lv_image_dsc_t * blink_hair;
    const lv_image_dsc_t * hair;
} emoji_asset_t;

// 像素在 assets 分区里，data 由 emoji_pack_bind() 运行时填，所以不是 const
extern lv_image_dsc_t emoji_shadow;
extern lv_image_dsc_t emoji_body;
extern lv_image_dsc_t emoji_body_hair;
extern lv_image_dsc_t emoji_body_hair_shade;
extern lv_image_dsc_t emoji_arm_0;
extern lv_image_dsc_t emoji_arm_1;
extern lv_image_dsc_t emoji_arm_2;
extern lv_image_dsc_t emoji_neutral;
extern lv_image_dsc_t emoji_pad_0;
extern lv_image_dsc_t emoji_patch_listening;
extern lv_image_dsc_t emoji_patch_happy;
extern lv_image_dsc_t emoji_patch_laughing;
extern lv_image_dsc_t emoji_patch_funny;
extern lv_image_dsc_t emoji_patch_sad;
extern lv_image_dsc_t emoji_patch_angry;
extern lv_image_dsc_t emoji_patch_crying;
extern lv_image_dsc_t emoji_patch_loving;
extern lv_image_dsc_t emoji_patch_embarrassed;
extern lv_image_dsc_t emoji_patch_surprised;
extern lv_image_dsc_t emoji_patch_shocked;
extern lv_image_dsc_t emoji_patch_thinking;
extern lv_image_dsc_t emoji_patch_winking;
extern lv_image_dsc_t emoji_patch_cool;
extern lv_image_dsc_t emoji_patch_relaxed;
extern lv_image_dsc_t emoji_patch_delicious;
extern lv_image_dsc_t emoji_patch_kissy;
extern lv_image_dsc_t emoji_patch_confident;
extern lv_image_dsc_t emoji_patch_sleepy;
extern lv_image_dsc_t emoji_patch_silly;
extern lv_image_dsc_t emoji_patch_confused;
extern lv_image_dsc_t emoji_neutral_mouth;
extern lv_image_dsc_t emoji_neutral_blink;
extern lv_image_dsc_t emoji_neutral_blink_hair;
extern lv_image_dsc_t emoji_listening_mouth;
extern lv_image_dsc_t emoji_listening_blink;
extern lv_image_dsc_t emoji_listening_blink_hair;
extern lv_image_dsc_t emoji_happy_mouth;
extern lv_image_dsc_t emoji_happy_blink;
extern lv_image_dsc_t emoji_happy_blink_hair;
extern lv_image_dsc_t emoji_laughing_mouth;
extern lv_image_dsc_t emoji_pad_1;
extern lv_image_dsc_t emoji_pad_2;
extern lv_image_dsc_t emoji_funny_mouth;
extern lv_image_dsc_t emoji_pad_3;
extern lv_image_dsc_t emoji_pad_4;
extern lv_image_dsc_t emoji_sad_mouth;
extern lv_image_dsc_t emoji_sad_blink;
extern lv_image_dsc_t emoji_sad_blink_hair;
extern lv_image_dsc_t emoji_angry_mouth;
extern lv_image_dsc_t emoji_angry_blink;
extern lv_image_dsc_t emoji_angry_blink_hair;
extern lv_image_dsc_t emoji_crying_mouth;
extern lv_image_dsc_t emoji_crying_blink;
extern lv_image_dsc_t emoji_crying_blink_hair;
extern lv_image_dsc_t emoji_loving_mouth;
extern lv_image_dsc_t emoji_loving_blink;
extern lv_image_dsc_t emoji_loving_blink_hair;
extern lv_image_dsc_t emoji_embarrassed_mouth;
extern lv_image_dsc_t emoji_embarrassed_blink;
extern lv_image_dsc_t emoji_embarrassed_blink_hair;
extern lv_image_dsc_t emoji_surprised_mouth;
extern lv_image_dsc_t emoji_surprised_blink;
extern lv_image_dsc_t emoji_surprised_blink_hair;
extern lv_image_dsc_t emoji_shocked_mouth;
extern lv_image_dsc_t emoji_shocked_blink;
extern lv_image_dsc_t emoji_shocked_blink_hair;
extern lv_image_dsc_t emoji_thinking_mouth;
extern lv_image_dsc_t emoji_thinking_blink;
extern lv_image_dsc_t emoji_thinking_blink_hair;
extern lv_image_dsc_t emoji_winking_mouth;
extern lv_image_dsc_t emoji_winking_blink;
extern lv_image_dsc_t emoji_winking_blink_hair;
extern lv_image_dsc_t emoji_cool_mouth;
extern lv_image_dsc_t emoji_pad_5;
extern lv_image_dsc_t emoji_pad_6;
extern lv_image_dsc_t emoji_relaxed_mouth;
extern lv_image_dsc_t emoji_pad_7;
extern lv_image_dsc_t emoji_pad_8;
extern lv_image_dsc_t emoji_delicious_mouth;
extern lv_image_dsc_t emoji_delicious_blink;
extern lv_image_dsc_t emoji_delicious_blink_hair;
extern lv_image_dsc_t emoji_kissy_mouth;
extern lv_image_dsc_t emoji_kissy_blink;
extern lv_image_dsc_t emoji_kissy_blink_hair;
extern lv_image_dsc_t emoji_confident_mouth;
extern lv_image_dsc_t emoji_confident_blink;
extern lv_image_dsc_t emoji_confident_blink_hair;
extern lv_image_dsc_t emoji_sleepy_mouth;
extern lv_image_dsc_t emoji_pad_9;
extern lv_image_dsc_t emoji_pad_10;
extern lv_image_dsc_t emoji_silly_mouth;
extern lv_image_dsc_t emoji_silly_blink;
extern lv_image_dsc_t emoji_silly_blink_hair;
extern lv_image_dsc_t emoji_confused_mouth;
extern lv_image_dsc_t emoji_confused_blink;
extern lv_image_dsc_t emoji_confused_blink_hair;
extern lv_image_dsc_t emoji_neutral_hair;
extern lv_image_dsc_t emoji_listening_hair;
extern lv_image_dsc_t emoji_happy_hair;
extern lv_image_dsc_t emoji_laughing_hair;
extern lv_image_dsc_t emoji_funny_hair;
extern lv_image_dsc_t emoji_sad_hair;
extern lv_image_dsc_t emoji_angry_hair;
extern lv_image_dsc_t emoji_crying_hair;
extern lv_image_dsc_t emoji_loving_hair;
extern lv_image_dsc_t emoji_embarrassed_hair;
extern lv_image_dsc_t emoji_surprised_hair;
extern lv_image_dsc_t emoji_shocked_hair;
extern lv_image_dsc_t emoji_thinking_hair;
extern lv_image_dsc_t emoji_winking_hair;
extern lv_image_dsc_t emoji_cool_hair;
extern lv_image_dsc_t emoji_relaxed_hair;
extern lv_image_dsc_t emoji_delicious_hair;
extern lv_image_dsc_t emoji_kissy_hair;
extern lv_image_dsc_t emoji_confident_hair;
extern lv_image_dsc_t emoji_sleepy_hair;
extern lv_image_dsc_t emoji_silly_hair;
extern lv_image_dsc_t emoji_confused_hair;
extern lv_image_dsc_t emoji_hair_shade;
extern lv_image_dsc_t emoji_gaze_0;
extern lv_image_dsc_t emoji_gaze_1;
extern lv_image_dsc_t emoji_gaze_2;
extern lv_image_dsc_t emoji_pad_11;
extern lv_image_dsc_t emoji_pad_12;
extern lv_image_dsc_t emoji_pad_13;
extern lv_image_dsc_t emoji_pad_14;
extern lv_image_dsc_t emoji_pad_15;
extern lv_image_dsc_t emoji_pad_16;

extern lv_image_dsc_t *const emoji_dsc_slots[];
extern const int emoji_dsc_count;
extern emoji_asset_t emoji_table[];
extern const int emoji_table_count;

// 表里第 i 项对应分区 slot 的第 i 个 dsc，生成器与骨架同步产出，不会错位
