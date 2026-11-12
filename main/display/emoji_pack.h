// 角色像素资产的运行时装配：像素在 assets 分区里，app 只留 dsc 骨架。
//
// app 分区装不下 6 个角色的原始图像（每个约 500KB），所以像素搬去独立分区、
// mmap 直读。换角色 = 重填一批指针，不重新烧 app。
#pragma once

#include <stdbool.h>
#include "emoji_geom.h"

#ifdef __cplusplus
extern "C" {
#endif

// 挂载 assets 分区并绑定默认角色。返回 0 成功，负数为错误码。
// 幂等：重复调用只做第一次。
int emoji_pack_ensure(void);

// 切换到指定角色（如 "base" / "hoop"）。失败不动当前指针。
bool emoji_pack_bind(const char *key);

// 可用角色写在 out（每项 16 字节），返回个数；-1 表示分区未挂载。
int emoji_pack_casts(char *out, int max);

// 当前绑定的角色 key；未绑定返回 NULL。
const char *emoji_pack_current(void);

// 当前角色的贴片几何（眨眼/视线/嘴的坐标与尺寸）。未绑定返回大肥鱼那份。
// 各角色眼位不同，这些值不能退化成全局宏，否则一眨眼糊在脸中间。
const emoji_geom_t *emoji_pack_geom(void);

#ifdef __cplusplus
}
#endif
