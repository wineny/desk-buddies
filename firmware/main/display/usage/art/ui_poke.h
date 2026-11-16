// 콕 찌르기 에셋. gen_poke.py 가 만든다 — 손으로 고치지 말 것.
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_POKE_PHRASE_COUNT 5
#define UI_POKE_REPLIES_PER_PHRASE 3
#define UI_POKE_REPLY_COUNT 15   // 패킷 reply 1~15

extern const lv_image_dsc_t ui_poke_heart_img;
extern const lv_image_dsc_t ui_poke_btn_heart_img;     // 대기 화면 하트 버튼 34×30 (tools/gen_bottom_buttons.py)
extern const lv_image_dsc_t ui_poke_btn_hanger_img;    // 대기 화면 옷장 버튼 34×30
extern const lv_image_dsc_t ui_poke_phrase_1_img;   // 밥 먹자
extern const lv_image_dsc_t ui_poke_phrase_2_img;   // 고생했어
extern const lv_image_dsc_t ui_poke_phrase_3_img;   // 사랑해
extern const lv_image_dsc_t ui_poke_phrase_4_img;   // 보고 싶어
extern const lv_image_dsc_t ui_poke_phrase_5_img;   // 일찍 와
extern const lv_image_dsc_t ui_poke_reply_1_img;   // 좋아!
extern const lv_image_dsc_t ui_poke_reply_2_img;   // 10분만
extern const lv_image_dsc_t ui_poke_reply_3_img;   // 먼저 먹어
extern const lv_image_dsc_t ui_poke_reply_4_img;   // 고마워
extern const lv_image_dsc_t ui_poke_reply_5_img;   // 너도!
extern const lv_image_dsc_t ui_poke_reply_6_img;   // 안아줘
extern const lv_image_dsc_t ui_poke_reply_7_img;   // 나도 사랑해
extern const lv_image_dsc_t ui_poke_reply_8_img;   // 알아ㅋ
extern const lv_image_dsc_t ui_poke_reply_9_img;   // ♥
extern const lv_image_dsc_t ui_poke_reply_10_img;   // 나도
extern const lv_image_dsc_t ui_poke_reply_11_img;   // 곧 갈게
extern const lv_image_dsc_t ui_poke_reply_12_img;   // 전화할래?
extern const lv_image_dsc_t ui_poke_reply_13_img;   // 응!
extern const lv_image_dsc_t ui_poke_reply_14_img;   // 노력할게
extern const lv_image_dsc_t ui_poke_reply_15_img;   // 늦을듯 ㅠㅠ
extern const lv_image_dsc_t ui_poke_read_img;      // 읽음
extern const lv_image_dsc_t ui_poke_high5_img;     // 하이파이브!
extern const lv_image_dsc_t ui_poke_trip_img;      // 놀러가기
extern const lv_image_dsc_t ui_poke_away_img;      // 놀러 갔어요
extern const lv_image_dsc_t ui_poke_invite_img;    // 놀러오기
extern const lv_image_dsc_t ui_poke_home_img;      // 집으로 가기
extern const lv_image_dsc_t ui_poke_diary_img;     // 육아일기 읽기
#define UI_MANBOK_MENU_COUNT 6
extern const lv_image_dsc_t ui_manbok_menu_1_img;   // 육아일기
extern const lv_image_dsc_t ui_manbok_menu_2_img;   // 초음파 사진
extern const lv_image_dsc_t ui_manbok_menu_3_img;   // 엄마 건강
extern const lv_image_dsc_t ui_manbok_menu_4_img;   // 출산 준비
extern const lv_image_dsc_t ui_manbok_menu_5_img;   // 메시지 보내기
extern const lv_image_dsc_t ui_manbok_menu_6_img;   // 놀러가기

#ifdef __cplusplus
}
#endif
