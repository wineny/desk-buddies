// 장면 스프라이트: 장면 배경의 움직이는 부분만 잘라 토글하는 작은 이미지들.
// 표는 장면 팩이 굽는다(packs/bboketer/scenes/bboketer_scene_anim_table.c).
#pragma once

#include <stdint.h>

#include "lvgl.h"

typedef struct {
    uint8_t sky;                       // UsageSky 인덱스
    int16_t x, y;                      // 화면 좌표 (배경과 같은 320×240 기준)
    uint8_t frame_count;               // 2~3
    uint16_t period_ms;                // 프레임 전환 주기
    const lv_image_dsc_t* frames[3];   // RGB565 crop (배경 포함 불투명)
} SceneSprite;
