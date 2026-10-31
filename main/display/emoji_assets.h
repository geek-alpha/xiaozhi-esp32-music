// 由 tools/gen_emoji_assets.py 生成，勿手改
#pragma once

#include "lvgl.h"

#define EMOJI_W 120
#define EMOJI_H 120
#define MOUTH_W 22
#define MOUTH_H 26
#define MOUTH_OX 49
#define MOUTH_OY 70
#define BLINK_W 72
#define BLINK_H 42
#define BLINK_OX 22
#define BLINK_OY 36

typedef struct {
    const char * name;
    const lv_image_dsc_t * img;
    const lv_image_dsc_t * mouth;
    const lv_image_dsc_t * blink;
    const lv_image_dsc_t * blink_hair;
    const lv_image_dsc_t * hair;
} emoji_asset_t;

extern const emoji_asset_t emoji_table[];
extern const int emoji_table_count;
extern const lv_image_dsc_t emoji_neutral_hair;
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
extern const lv_image_dsc_t emoji_happy;
extern const lv_image_dsc_t emoji_happy_mouth;
extern const lv_image_dsc_t emoji_happy_blink;
extern const lv_image_dsc_t emoji_happy_blink_hair;
extern const lv_image_dsc_t emoji_laughing;
extern const lv_image_dsc_t emoji_laughing_mouth;
extern const lv_image_dsc_t emoji_funny;
extern const lv_image_dsc_t emoji_funny_mouth;
extern const lv_image_dsc_t emoji_sad;
extern const lv_image_dsc_t emoji_sad_mouth;
extern const lv_image_dsc_t emoji_sad_blink;
extern const lv_image_dsc_t emoji_sad_blink_hair;
extern const lv_image_dsc_t emoji_angry;
extern const lv_image_dsc_t emoji_angry_mouth;
extern const lv_image_dsc_t emoji_angry_blink;
extern const lv_image_dsc_t emoji_angry_blink_hair;
extern const lv_image_dsc_t emoji_crying;
extern const lv_image_dsc_t emoji_crying_mouth;
extern const lv_image_dsc_t emoji_crying_blink;
extern const lv_image_dsc_t emoji_crying_blink_hair;
extern const lv_image_dsc_t emoji_loving;
extern const lv_image_dsc_t emoji_loving_mouth;
extern const lv_image_dsc_t emoji_loving_blink;
extern const lv_image_dsc_t emoji_loving_blink_hair;
extern const lv_image_dsc_t emoji_embarrassed;
extern const lv_image_dsc_t emoji_embarrassed_mouth;
extern const lv_image_dsc_t emoji_embarrassed_blink;
extern const lv_image_dsc_t emoji_embarrassed_blink_hair;
extern const lv_image_dsc_t emoji_surprised;
extern const lv_image_dsc_t emoji_surprised_mouth;
extern const lv_image_dsc_t emoji_surprised_blink;
extern const lv_image_dsc_t emoji_surprised_blink_hair;
extern const lv_image_dsc_t emoji_shocked;
extern const lv_image_dsc_t emoji_shocked_mouth;
extern const lv_image_dsc_t emoji_shocked_blink;
extern const lv_image_dsc_t emoji_shocked_blink_hair;
extern const lv_image_dsc_t emoji_thinking;
extern const lv_image_dsc_t emoji_thinking_mouth;
extern const lv_image_dsc_t emoji_thinking_blink;
extern const lv_image_dsc_t emoji_thinking_blink_hair;
extern const lv_image_dsc_t emoji_winking;
extern const lv_image_dsc_t emoji_winking_mouth;
extern const lv_image_dsc_t emoji_winking_blink;
extern const lv_image_dsc_t emoji_winking_blink_hair;
extern const lv_image_dsc_t emoji_cool;
extern const lv_image_dsc_t emoji_cool_mouth;
extern const lv_image_dsc_t emoji_relaxed;
extern const lv_image_dsc_t emoji_relaxed_mouth;
extern const lv_image_dsc_t emoji_delicious;
extern const lv_image_dsc_t emoji_delicious_mouth;
extern const lv_image_dsc_t emoji_delicious_blink;
extern const lv_image_dsc_t emoji_delicious_blink_hair;
extern const lv_image_dsc_t emoji_kissy;
extern const lv_image_dsc_t emoji_kissy_mouth;
extern const lv_image_dsc_t emoji_kissy_blink;
extern const lv_image_dsc_t emoji_kissy_blink_hair;
extern const lv_image_dsc_t emoji_confident;
extern const lv_image_dsc_t emoji_confident_mouth;
extern const lv_image_dsc_t emoji_confident_blink;
extern const lv_image_dsc_t emoji_confident_blink_hair;
extern const lv_image_dsc_t emoji_sleepy;
extern const lv_image_dsc_t emoji_sleepy_mouth;
extern const lv_image_dsc_t emoji_silly;
extern const lv_image_dsc_t emoji_silly_mouth;
extern const lv_image_dsc_t emoji_silly_blink;
extern const lv_image_dsc_t emoji_silly_blink_hair;
extern const lv_image_dsc_t emoji_confused;
extern const lv_image_dsc_t emoji_confused_mouth;
extern const lv_image_dsc_t emoji_confused_blink;
extern const lv_image_dsc_t emoji_confused_blink_hair;
