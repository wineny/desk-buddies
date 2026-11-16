// 옷장·날씨 연출 공용 그림과 표. 그림은 tools/gen_felt_art.py 가 굽는다.
#pragma once

#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 옷장 칸 하나. 캐릭터 팩 아이템(tools/gen_pack.py)·장면 옷·배경·날씨 연출이 같이 쓴다.
// dx/dy 는 프레임(98×64) 좌상단 기준 1px, w/h 는 크기. 인덱스 0 은 언제나 "none".
typedef struct {
    const char* key;               // 프로토콜·NVS 와 같은 문자열 ("cap", "rain" ...)
    int8_t dx, dy;
    uint8_t w, h;
    const lv_image_dsc_t* img;     // 오버레이 그림. none 이면 NULL
    const lv_image_dsc_t* img_l;   // 좌우 반전본 (팩 아이템은 NULL)
    const lv_image_dsc_t* thumb;   // 옷장 썸네일
} OutfitItem;

// 날씨 연출(옷장 WX 탭): auto clear rain snow leaf star petal. ui_wx_items.c.
extern const OutfitItem kWxItems[];
extern const uint8_t kWxItemCount;

// 펠트 날씨 그림(파티클·썸네일).
extern const lv_image_dsc_t ui_fx_leaf_red_img;
extern const lv_image_dsc_t ui_fx_leaf_gold_img;
extern const lv_image_dsc_t ui_fx_petal_img;
extern const lv_image_dsc_t ui_fx_rain_img;
extern const lv_image_dsc_t ui_fx_flake_img;
extern const lv_image_dsc_t ui_fx_ball_img;
extern const lv_image_dsc_t ui_fx_thumb_sun_img;
extern const lv_image_dsc_t ui_fx_thumb_rain_img;
extern const lv_image_dsc_t ui_fx_thumb_snow_img;
extern const lv_image_dsc_t ui_fx_thumb_leaf_img;
extern const lv_image_dsc_t ui_fx_thumb_petal_img;
extern const lv_image_dsc_t ui_thumb_wx_auto_img;
extern const lv_image_dsc_t ui_thumb_wx_clear_img;
extern const lv_image_dsc_t ui_thumb_wx_rain_img;
extern const lv_image_dsc_t ui_thumb_wx_snow_img;
extern const lv_image_dsc_t ui_thumb_wx_leaf_img;
extern const lv_image_dsc_t ui_thumb_wx_star_img;

#ifdef __cplusplus
}
#endif
