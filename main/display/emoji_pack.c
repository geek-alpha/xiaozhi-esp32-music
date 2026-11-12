// 角色像素资产的运行时装配。格式与 tools/_pack_assets.py 一一对应：
//   分区头 0x40 起是 slot 表（每项 32B：key[16] / offset / size / dsc 数 / face 数）
//   slot 内：头 16B（dsc 数 / dsc 表偏移 / face 数 / face 表偏移），
//            每 dsc 16B（偏移 / w / h / 格式 / 数据长度），每 face 20B（补丁位置与索引），
//            最后是 4 字节对齐的像素区。
#include "emoji_pack.h"

#include <string.h>

#include "emoji_assets.h"
#include "emoji_geom.h"
#include "esp_log.h"
#include "esp_partition.h"

#define TAG "EmojiPack"

#define PART_LABEL   "assets"
#define MAGIC        0x31545341u  // 'AST1'
#define SLOT_TAB_OFF 0x40
#define SLOT_ENT     32
#define DSC_ENT      16
#define FACE_ENT     20
#define NULL_IDX     0xFFFF

static const uint8_t *s_base = NULL;
static uint32_t s_size = 0;
static uint32_t s_count = 0;
static char s_cur[16] = {0};
// 当前 slot 下标：几何表与 slot 表同一个 CASTS 顺序产出，可以直接互相当索引
static int s_slot = -1;

static inline uint16_t rd16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static const lv_image_dsc_t *idx_dsc(uint16_t i)
{
    if (i == NULL_IDX || i >= (uint16_t)emoji_dsc_count) {
        return NULL;
    }
    return emoji_dsc_slots[i];
}

static void fill_dsc(lv_image_dsc_t *d, const uint8_t *data, uint16_t w, uint16_t h,
                     uint8_t kind, uint32_t size)
{
    // A8 的 stride 是一行字节数，RGB565A8 的 stride 只数 RGB565 那半行——LVGL 9 的口径
    memset(d, 0, sizeof(*d));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = (kind == 0) ? LV_COLOR_FORMAT_A8 : LV_COLOR_FORMAT_RGB565A8;
    d->header.flags = 0;
    d->header.w = w;
    d->header.h = h;
    d->header.stride = (kind == 0) ? w : (uint16_t)(w * 2);
    d->data_size = size;
    d->data = data;
}

static bool bind_slot(const uint8_t *slot)
{
    uint32_t n_dsc = rd32(slot);
    uint32_t dsc_off = rd32(slot + 4);
    uint32_t n_face = rd32(slot + 8);
    uint32_t face_off = rd32(slot + 12);

    if (n_dsc != (uint32_t)emoji_dsc_count) {
        ESP_LOGE(TAG, "dsc 数量不符：分区 %u vs 固件 %d（bin 与固件版本不配套）",
                 (unsigned)n_dsc, emoji_dsc_count);
        return false;
    }
    for (uint32_t i = 0; i < n_dsc; i++) {
        const uint8_t *e = slot + dsc_off + i * DSC_ENT;
        uint32_t off = rd32(e);
        uint32_t size = rd32(e + 12);
        if ((const uint8_t *)slot + off + size > s_base + s_size) {
            ESP_LOGE(TAG, "dsc#%u 越界 off=%u size=%u", (unsigned)i, (unsigned)off, (unsigned)size);
            return false;
        }
        fill_dsc(emoji_dsc_slots[i], slot + off, rd16(e + 4), rd16(e + 6), e[8], size);
    }
    for (uint32_t j = 0; j < n_face && j < (uint32_t)emoji_table_count; j++) {
        const uint8_t *f = slot + face_off + j * FACE_ENT;
        emoji_table[j].px = (int16_t)rd16(f);
        emoji_table[j].py = (int16_t)rd16(f + 2);
        emoji_table[j].pw = (int16_t)rd16(f + 4);
        emoji_table[j].ph = (int16_t)rd16(f + 6);
        emoji_table[j].patch = idx_dsc(rd16(f + 8));
        emoji_table[j].mouth = idx_dsc(rd16(f + 10));
        emoji_table[j].blink = idx_dsc(rd16(f + 12));
        emoji_table[j].blink_hair = idx_dsc(rd16(f + 14));
        emoji_table[j].hair = idx_dsc(rd16(f + 16));
    }
    return true;
}

static int mount_once(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PART_LABEL);
    if (part == NULL) {
        ESP_LOGE(TAG, "找不到 %s 分区（分区表没烧或名字不对）", PART_LABEL);
        return -1;
    }
    esp_partition_mmap_handle_t handle = 0;
    const void *ptr = NULL;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &handle);
    if (err != ESP_OK || ptr == NULL) {
        ESP_LOGE(TAG, "mmap 失败: %s", esp_err_to_name(err));
        return -2;
    }
    const uint8_t *base = (const uint8_t *)ptr;
    uint32_t magic = rd32(base);
    if (magic != MAGIC) {
        ESP_LOGE(TAG, "分区里没有资产（magic=0x%08x），需要烧 assets.bin", (unsigned)magic);
        return -3;
    }
    s_base = base;
    s_size = part->size;
    s_count = rd32(base + 8);
    ESP_LOGI(TAG, "assets 已挂载：%u 个角色，%u 字节", (unsigned)s_count, (unsigned)part->size);
    return 0;
}

int emoji_pack_ensure(void)
{
    static int state = -100;  // -100 = 还没试过
    if (state != -100) {
        return state;
    }
    state = mount_once();
    if (state != 0) {
        return state;
    }
    // 每角色单独打包的固件里没有 base，绑分区第一项就是这台机器的默认脸。
    // 不这么做的话，单角色包会在启动时挂载成功却绑不上人，屏上一片空。
    if (!emoji_pack_bind("base")) {
        char first[16] = {0};
        if (emoji_pack_casts(first, 1) == 1) {
            emoji_pack_bind(first);
        }
    }
    if (emoji_pack_current() == NULL) {
        state = -4;
    }
    return state;
}

bool emoji_pack_bind(const char *key)
{
    if (s_base == NULL || key == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < s_count; i++) {
        const uint8_t *ent = s_base + SLOT_TAB_OFF + i * SLOT_ENT;
        if (strncmp((const char *)ent, key, 16) != 0) {
            continue;
        }
        if (!bind_slot(s_base + rd32(ent + 16))) {
            return false;
        }
        // 几何按 key 找，不按 slot 下标：单角色分区（一个角色一个固件包）的 slot 0
        // 是谁都说得通，按下标取就会拿别人的眼位画眨眼。
        const emoji_geom_t *g = emoji_geom_find(key);
        if (g == NULL) {
            ESP_LOGW(TAG, "几何表里没有 %s，贴片按 %s 的坐标画", key, emoji_geom_table[0].key);
            s_slot = -1;
        } else {
            s_slot = (int)(g - emoji_geom_table);
        }
        strncpy(s_cur, key, sizeof(s_cur) - 1);
        s_cur[sizeof(s_cur) - 1] = '\0';
        ESP_LOGI(TAG, "已切到角色 %s", s_cur);
        return true;
    }
    ESP_LOGW(TAG, "分区里没有角色 %s", key);
    return false;
}

int emoji_pack_casts(char *out, int max)
{
    if (s_base == NULL || out == NULL || max <= 0) {
        return s_base == NULL ? -1 : 0;
    }
    int n = 0;
    for (uint32_t i = 0; i < s_count && n < max; i++, n++) {
        memcpy(out + n * 16, s_base + SLOT_TAB_OFF + i * SLOT_ENT, 16);
        out[n * 16 + 15] = '\0';
    }
    return n;
}

const char *emoji_pack_current(void)
{
    return s_cur[0] ? s_cur : NULL;
}

const emoji_geom_t *emoji_pack_geom(void)
{
    if (s_slot < 0 || s_slot >= emoji_geom_count) {
        return &emoji_geom_table[0];  // 还没绑定：回大肥鱼，别让贴片坐标读成 0
    }
    return &emoji_geom_table[s_slot];
}
