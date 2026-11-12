// 由 tools/mk_assets_bin.py 生成，勿手改
#include "emoji_geom.h"
#include <string.h>

const emoji_geom_t emoji_geom_table[] = {
    {"base", 162, 162, 18, 14, 57, 86, 60, 24, 34, 68, 3, 56, 18, 38, 70, 162, 81, 0, 100, 162, 181, 181},
    {"hoop", 162, 162, 26, 20, 68, 93, 60, 26, 52, 76, 3, 62, 20, 50, 74, 162, 81, 0, 123, 162, 204, 204},
    {"painter", 162, 162, 26, 20, 68, 93, 62, 24, 48, 76, 3, 64, 16, 48, 76, 162, 81, 0, 123, 162, 204, 204},
    {"phone", 162, 162, 26, 20, 68, 93, 62, 22, 50, 78, 3, 62, 16, 50, 78, 162, 81, 0, 123, 162, 204, 204},
    {"apple", 162, 162, 26, 20, 68, 93, 74, 28, 44, 76, 3, 74, 20, 44, 76, 162, 81, 0, 123, 162, 204, 204},
    {"book", 162, 162, 26, 20, 68, 93, 74, 24, 44, 78, 3, 74, 18, 44, 76, 162, 81, 0, 123, 162, 204, 204},
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
