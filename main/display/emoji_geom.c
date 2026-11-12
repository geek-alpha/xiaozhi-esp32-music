// 由 tools/mk_assets_bin.py 生成，勿手改
#include "emoji_geom.h"
#include <string.h>

const emoji_geom_t emoji_geom_table[] = {
    {"base", 162, 162, 18, 14, 57, 86, 60, 28, 34, 64, 3, 56, 24, 38, 68, 162, 81, 0, 100, 162, 181, 181},
    {"hoop", 162, 162, 26, 20, 68, 96, 58, 28, 52, 76, 3, 54, 22, 54, 78, 162, 81, 0, 123, 162, 204, 204},
    {"painter", 162, 162, 26, 20, 68, 95, 60, 20, 50, 78, 3, 56, 18, 52, 78, 162, 81, 0, 123, 162, 204, 204},
    {"phone", 162, 162, 26, 20, 68, 96, 58, 20, 52, 80, 3, 56, 16, 54, 80, 162, 81, 0, 123, 162, 204, 204},
    {"apple", 162, 162, 26, 20, 68, 96, 58, 20, 52, 80, 3, 54, 16, 54, 80, 162, 81, 0, 123, 162, 204, 204},
    {"book", 162, 162, 26, 20, 68, 96, 58, 20, 52, 80, 3, 54, 14, 54, 82, 162, 81, 0, 123, 162, 204, 204},
};
const int emoji_geom_count = sizeof(emoji_geom_table) / sizeof(emoji_geom_table[0]);

const emoji_geom_t *emoji_geom_find(const char *key)
{
    if (key == NULL) {
        return NULL;
    }
    for (int i = 0; i < emoji_geom_count; i++) {
        if (strcmp(emoji_geom_table[i].key, key) == 0) {
            return &emoji_geom_table[i];
        }
    }
    return NULL;
}
