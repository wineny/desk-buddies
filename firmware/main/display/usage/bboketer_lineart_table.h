// tools/apply_lineart.py 가 굽는다. 손으로 고치지 말 것. 설명은 docs/lineart-firmware.md.
#pragma once

#include <cstdint>

constexpr int kLineartBodyCount = 25;
// 몸 번호: moon pub lpbar vintage office salad scuba paraglide dolopath cappadocia seoul camping beach jeju blossom myroom bedroom living ballpark subway birth seol chu pepero xmas

struct LineartPlace {
    int16_t dx, foot, rozzi_x, manbok_x;   // 집(로나집·만복이집). foot + 4 = 로나 발
    int16_t rozzi_dy, manbok_dy;            // 로나 발에서 로찌·만복이 발까지
    int16_t g_rona, g_manbok, g_rozzi;      // 놀러가기 프레임 왼쪽(로찌는 64×64 2배 프레임 왼쪽)
    int16_t g_rona_dy, g_manbok_dy, g_rozzi_dy;
    int16_t nose_x, nose_y, mb_nose_x, mb_nose_y;   // 98×64 프레임 천분율(집 자세)
};
struct LineartProp {
    const char* file;   // nullptr = 없음
    int16_t x, y;       // 화면 왼쪽 위
    uint8_t front;      // 1 = 캐릭터 위에 그린다
};
// 배치: 0 = 로나집, 1 = 만복이집, 2 = 놀러가기
struct LineartScene {
    LineartPlace place;
    LineartProp props[3];
    bool manbok_visit_alt;   // 놀러가기에서 만복이가 lm_<장면>_v 를 쓴다
};

static constexpr LineartScene kLineartScenes[kLineartBodyCount] = {
    {{-36, 207, 153, 62, 0, 2, -19, 70, 188, 0, 2, 0, 536, 348, 505, 377}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // moon
    {{-57, 187, 99, 73, 0, 0, 5, 73, 188, 0, 0, 0, 421, 220, 505, 393}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // pub
    {{-66, 211, 177, 26, 1, 0, -35, 52, 199, 0, 2, 1, 413, 238, 513, 371}, {{"lp_lpbar_r.i8", 145, 163, 1}, {"lp_lpbar_m.i8", 191, 163, 0}, {"lp_lpbar_v.i8", 183, 165, 1}}, false},   // lpbar
    {{-36, 209, 137, 66, 2, 3, -8, 66, 173, 0, 3, 2, 434, 165, 505, 349}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // vintage
    {{-45, 200, 159, 61, -1, 2, -27, 78, 196, 0, 2, 14, 599, 336, 497, 426}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // office
    {{-54, 194, 148, 61, 2, 1, -21, 61, 179, 0, 1, 2, 411, 342, 500, 387}, {{"lf_salad_r.i8", 0, 184, 1}, {"lf_salad_m.i8", 0, 184, 1}, {"lf_salad_v.i8", 0, 185, 1}}, false},   // salad
    {{-19, 124, 158, 61, 83, 81, 34, 16, 170, -31, 85, 83, 495, 385, 503, 393}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // scuba
    {{-36, 197, 156, 62, 8, 9, 72, -34, 198, 0, 12, 8, 429, 512, 526, 387}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // paraglide
    {{-45, 205, 154, 62, 4, 4, -19, 70, 185, 0, 4, 4, 518, 330, 500, 420}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // dolopath
    {{-59, 206, 165, 62, 3, 3, -32, 79, 192, 0, 3, 3, 589, 202, 500, 437}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // cappadocia
    {{-36, 206, 146, 62, 0, 3, -25, 66, 190, 0, 2, 0, 490, 342, 508, 426}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, true},   // seoul
    {{97, 209, 39, 125, -12, -28, 159, 62, 41, -31, 40, -39, 390, 269, 548, 398}, {{"lp_camping_r.i8", 127, 126, 0}, {"lp_camping_m.i8", 127, 126, 1}, {"lp_camping_v.i8", 127, 93, 0}}, true},   // camping
    {{-56, 185, 170, 70, 20, 22, -18, 70, 193, -2, 24, 24, 653, 183, 505, 387}, {{"lp_beach_r.i8", 142, 185, 1}, {"lp_beach_m.i8", 151, 35, 0}, {"lp_beach_v.i8", 107, 194, 1}}, false},   // beach
    {{-60, 206, 157, 62, 0, 3, -36, 48, 190, 0, 3, 0, 441, 250, 500, 382}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // jeju
    {{-45, 206, 148, 62, 1, 3, -25, 66, 186, 0, 3, 1, 380, 317, 492, 338}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // blossom
    {{-32, 209, 130, 61, 0, 0, 1, 63, 156, -1, 0, 2, 431, 366, 503, 360}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // myroom
    {{-32, 213, 131, 62, 1, 0, 2, 63, 157, -1, 0, 3, 416, 317, 497, 398}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // bedroom
    {{-32, 209, 130, 62, 1, 0, 1, 63, 157, -2, 0, 3, 390, 342, 503, 404}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // living
    {{-41, 211, 134, 62, 2, 0, 2, 64, 157, -1, 0, 4, 497, 214, 485, 377}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // ballpark
    {{-32, 209, 130, 63, 1, 0, 2, 64, 157, -2, 0, 4, 474, 366, 515, 409}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // subway
    {{-45, 206, 146, 62, 0, 3, -23, 66, 184, 0, 3, 0, 411, 183, 497, 409}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // birth
    {{-45, 206, 148, 62, 1, 3, -25, 66, 186, 0, 3, 1, 413, 525, 500, 415}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // seol
    {{-45, 207, 148, 62, -1, 2, -25, 65, 186, 0, 2, -1, 441, 263, 497, 338}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // chu
    {{-45, 206, 148, 62, 1, 3, -25, 65, 186, 0, 3, 1, 423, 305, 503, 377}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // pepero
    {{-45, 206, 148, 62, 1, 3, -25, 66, 185, 0, 3, 1, 482, 214, 492, 366}, {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}}, false},   // xmas
};
