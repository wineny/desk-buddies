// 옷장 WX 탭 표. 순서가 NVS·프로토콜 wx 번호다(0=auto) — 바꾸지 말 것.
#include "ui_art.h"

#include <stddef.h>

const OutfitItem kWxItems[] = {
    {"auto", 0, 0, 0, 0, NULL, NULL, &ui_thumb_wx_auto_img},
    {"clear", 0, 0, 0, 0, NULL, NULL, &ui_thumb_wx_clear_img},
    {"rain", 0, 0, 0, 0, NULL, NULL, &ui_thumb_wx_rain_img},
    {"snow", 0, 0, 0, 0, NULL, NULL, &ui_thumb_wx_snow_img},
    {"leaf", 0, 0, 0, 0, NULL, NULL, &ui_thumb_wx_leaf_img},
    {"star", 0, 0, 0, 0, NULL, NULL, &ui_thumb_wx_star_img},
    {"petal", 0, 0, 0, 0, NULL, NULL, &ui_fx_thumb_petal_img},
};
const uint8_t kWxItemCount = sizeof(kWxItems) / sizeof(kWxItems[0]);
