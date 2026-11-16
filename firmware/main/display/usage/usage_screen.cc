#include "usage_screen.h"

#include "notify_sound.h"
#include "poke_link.h"

#include <cctype>
#include <cinttypes>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <strings.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cmath>

#include "art/ui_art.h"
#include "art/ui_poke.h"
#include "wardrobe_store.h"
#include "bboketer_scenes.h"

// 윗줄 시계 칸 날씨 그림 (art/weather_icons.c, tools/gen_weather_icons.py). 순서 = UsageWeatherIcon.
extern "C" {
extern const lv_image_dsc_t weather_icon_clear_img;
extern const lv_image_dsc_t weather_icon_partly_img;
extern const lv_image_dsc_t weather_icon_cloud_img;
extern const lv_image_dsc_t weather_icon_rain_img;
extern const lv_image_dsc_t weather_icon_snow_img;
extern const lv_image_dsc_t weather_icon_moon_img;
}

// 시안의 픽셀 폰트(Press Start 2P, fonts/ 에 정적 LVGL 폰트로 변환). 라벨·큰
// 퍼센트·칩·상태 이름이 이 폰트고, 문장(남은 시간·ctx)은 몬세라트다.
// 반드시 전역 스코프에서 선언한다 — 익명 네임스페이스 안에 두면 내부 링키지의
// 다른 심볼이 되어 링크가 깨진다. 픽셀 폰트는 8의 배수 크기에서만 또렷하다.
LV_FONT_DECLARE(press_start_8)
LV_FONT_DECLARE(press_start_24)

namespace {

static_assert(kUsageSkyFirstScene == 13, "scene sprite indices follow the original 13 backgrounds");
static_assert(kUsageSkyLastScene - kUsageSkyFirstScene + 1 == kBboketerSceneCount, "장면 enum 과 bboketer_scenes 표 칸 수");
static_assert(kUsageSkyCount - kUsageSkyFirstScene == kBboketerBgItemCount, "옷장 BG 칸 = 장면 + 기념일 셋");

// 캐릭터 팩 (docs/character-pack.md). 테이블 순서가 UsageState 와 같아야 한다.
static_assert(kPackStateCount == kUsageStateCount, "character_pack.h / gen_pack.py BOARD_STATES 갱신");
constexpr uint32_t kPackTapMs = 2000;      // 몸통 탭 → pack.json touch.tap 애니
constexpr uint32_t kPackDoubleMs = 2000;   // 600ms 안 두 번 → touch.double 애니
constexpr uint32_t kPackPoutWindowMs = 3000;   // 이 안에 3번 탭 = 삐짐
constexpr uint32_t kPackDizzyWindowMs = 4500;  // 이 안에 5번 탭 = 어지러움
constexpr int kPackNoseDefaultX = 460, kPackNoseDefaultY = 550;   // 장면 밖 앉은 자세의 코(프레임 천분율)
constexpr int kPackNoseHitX = 75, kPackNoseHitY = 90;
constexpr int kPackHeadBelowNose = 120;        // 코보다 이만큼 아래까지 머리로 친다
constexpr uint32_t kPackPetMs = 3000;      // 몸통 홀드(쓰다듬) → tap 애니를 길게

// 팔레트. 시안 아티팩트(클로드봇 표정 시안)의 CSS 값 그대로.
constexpr uint32_t kBg = 0x161008;             // 어두운 바탕 (대기·상태 공통)
constexpr uint32_t kIdleName = 0x9C7B5E;       // 토스트·콕 패널 테두리
constexpr uint32_t kLimitName = 0xD8C4AE;      // "WEEK"
constexpr uint32_t kLimitPct = 0xF0E7DD;       // 큰 퍼센트
constexpr uint32_t kLimitLeft = 0xE8D8C4;      // "2d 4h left"
// 사용량 글자 뒤 판. 장면마다 밝기가 달라도 같은 글자색으로 읽히게 한다.
constexpr lv_opa_t kLimitPlateOpa = LV_OPA_80;
constexpr int kLimitPlatePadX = 6, kLimitPlatePadY = 6, kLimitPlateRadius = 5;
constexpr int kLimitPlateY = 12;              // 배지 판과 세로 가운데를 맞춘다
constexpr int kLimitPlateH = 25, kLimitTextGap = 4;   // 한 줄 판: 이름 · % · 남은 시간(8px) + 막대. 빈칸 글자(8px)보다 좁게
constexpr int kLimitBarY = 18, kLimitBarH = 3;        // 판 위쪽에서 막대까지
constexpr uint32_t kLimitBarFill = 0xFF8A4C;
// 윗줄 시계 칸. 판은 사용량 칸과 같은 색·높이·모서리이고 막대가 없어 글자를 가운데에 둔다.
constexpr int kClockTextY = (kLimitPlateH - 8) / 2;     // 8px 픽셀 폰트
constexpr int kClockIconPx = 14;
constexpr int kClockIconY = (kLimitPlateH - kClockIconPx) / 2;
constexpr int kClockDegPx = 4;                          // ° 대신 그리는 네모 고리 한 변

// 하늘의 배경 그림. 장면·기념일은 assets 에서 읽는다(없으면 nullptr). 장면 앞 13칸(옛 하늘 키)은 그림이 없다.
const lv_image_dsc_t* SkyBackground(UsageSky sky) {
    if (sky >= kUsageSkyFirstScene && sky <= kUsageSkyLastScene) {
        return BboketerSceneImage(sky - kUsageSkyFirstScene);
    }
    if (sky > kUsageSkyLastScene && sky < kUsageSkyCount) {
        return BboketerPickedBgImage(sky - kUsageSkyBirth);
    }
    return nullptr;
}

constexpr uint32_t kStateChipBg = 0x0C0804;    // 칩 배경
constexpr uint32_t kStateChipText = 0xFF8A4C;  // 칩 글자
constexpr uint32_t kStateCtxText = 0xB08A6E;   // 좌하 컨텍스트 라인
constexpr uint32_t kStatePulse = 0xFF8A4C;     // 권한 대기 테두리
constexpr lv_opa_t kStatePulseMaxOpa = 153;    // 60%
constexpr uint32_t kStatePulseHalfMs = 700;    // 시안 ring 1.4s 의 절반

// 대기 화면 좌표. 시안(480×320)을 2/3 로 줄인 320×240 기준값이고, 다른 해상도는
// 높이 비례로 늘인다. 픽셀 폰트 메트릭(라벨 8px·퍼센트 24px, 여백 없음)에
// 맞춘 값이다: 라벨 → 8px 띄우고 퍼센트 → 12px 띄우고 남은 시간.
constexpr int kRefH = 240;
constexpr int kNameY = 22;
constexpr int kPctY = 38;
constexpr int kLeftY = 74;
constexpr int kPixelLetterSpace = 1;  // 8px 픽셀 폰트는 1px 자간을 줘야 시안 느낌

// 대기 화면 캐릭터 박스(98×65)와 길. 미니카는 이 길을 따라 x 로 옮긴다.
constexpr int kWalkW = 98;
constexpr int kWalkH = 65;
constexpr int kWalkFootY = 206;         // 발 밑변 = 배경의 길 아랫선 (320×240 기준)
constexpr uint32_t kWalkLegMs = 7000;   // 한쪽 끝 → 반대 끝
constexpr uint32_t kPackRideMs = 3000;        // 미니카: 한쪽 끝 → 반대 끝 (걷기보다 빠르게)
constexpr uint32_t kPackRideMinMs = 20000;    // 제자리에 앉아 있는 시간 20~60초
constexpr uint32_t kPackRideSpreadMs = 40000;
constexpr int kUsageDayFromH = 7;             // 낮 하늘 시작 (KST) — UsageVillageSkyForClock
constexpr int kUsageDayUntilH = 19;           // 낮 하늘 끝
constexpr uint32_t kWalkRestMs = 500;   // 끝에서 돌아서기 전 멈춤
constexpr int kSpaceFloatBase = 8;      // 우주 무중력 부유 중심 높이
constexpr int kSpaceFloatAmp = 7;       // 부유 진폭 (1~15px). 5 는 "두둥실이 안 보인다"(09-12 데모 피드백)

// 파티클 (시안 drawWeather + sakura 꽃잎). 종류는 drop_kind_ 값과 같다.
constexpr uint8_t kDropRain = 1, kDropSnow = 2, kDropPetal = 3, kDropLeaf = 4, kDropStar = 5;
constexpr uint32_t kDropTickMs = 90;

// 터치 반응(docs/touch.md).
constexpr int kBodyHitPad = 8;              // 몸통 히트박스 여유
constexpr int kBadgePlateXY = 8;            // 좌상 배지 판 자리
constexpr int kBadgePlatePx = 32;
constexpr int kBadgeGap = 31;               // 상태 칩을 판 오른쪽으로 민다 (pad 13 + 31 = 44)
constexpr int kBadgeBelowY = kBadgePlateXY + kBadgePlatePx + 4;   // 토스트·읽음 표시는 판 아래
constexpr uint32_t kBadgeClaude = 0xFF8A4C;   // 배지 글자 색: Claude Code
constexpr uint32_t kBadgeCodex = 0x5AA0FF;    // 배지 글자 색: Codex
constexpr int kBadgeHitW = 64;              // 좌상 배지 히트 박스 — 손끝 하나가 넉넉히 들어간다
constexpr int kBadgeHitH = 64;
constexpr uint32_t kAgentToastMs = 1500;    // 800 은 눈에 안 띄었다 (09-12)
constexpr uint32_t kAgentPopMs = 1100;      // 전환 팝(가운데 이름) 표시 시간
constexpr int kAgentPopY = 100;             // 팝 이름 좌상단 y
constexpr int kHangerHit = 38;              // 우하단 옷걸이 히트 박스 (버튼 34×30, 여유 포함)

// 콕 찌르기 (docs/poke.md). 좌하단 하트는 옷걸이와 대칭(왼쪽 여백 4, 아래 여백 6, 히트 32).
constexpr int kTickerX = 42, kTickerRight = 42, kTickerBottom = 5, kTickerH = 22;   // 흐르는 줄 자리
constexpr int kPokeBtnMarginX = 4;
constexpr int kPokeBtnMarginY = 5;
constexpr int kPokeBtnHit = kHangerHit;
constexpr int kPokeRowH = 36;               // 패널 한 줄 (문구 2× 높이 30 + 여유) — 손가락 폭
constexpr int kPokePanelPad = 4;
constexpr int kPokeTextScale = 2;
constexpr uint32_t kPokePanelMs = 6000;     // 패널 자동 닫힘
constexpr uint32_t kDiaryToastMs = 8000;    // 새 일기 알림
constexpr uint32_t kPokePopMs = 4000;       // 받은 쪽 팝 표시 시간
constexpr int kPokePopScale = 4;            // 하트 16 → 64px
constexpr int kPokePopCenterY = 88;
constexpr int kPokePopTextCenterY = 145;    // 하트 밑변(y120) 아래로 문구 판이 붙는다
constexpr uint32_t kPokeReplyPopMs = 7000;  // 답장 버튼이 뜨면 고를 시간을 더 준다
constexpr uint32_t kHighFivePopMs = 2600;
constexpr uint32_t kPokeReadMs = 1800;
constexpr int kPokeReplyCenterY = 182;      // 문구 판 아래 답장 버튼 줄
constexpr int kPokeReplyGap = 8;
constexpr int kPokeReplyHitPad = 5;
// 받은 문구마다 3칸씩(문구 1 = 1~3, 문구 2 = 4~6 …). 패킷 reply 값과 같은 순서다.
const lv_image_dsc_t* const kPokeReplies[UI_POKE_REPLY_COUNT] = {
    &ui_poke_reply_1_img,  &ui_poke_reply_2_img,  &ui_poke_reply_3_img,
    &ui_poke_reply_4_img,  &ui_poke_reply_5_img,  &ui_poke_reply_6_img,
    &ui_poke_reply_7_img,  &ui_poke_reply_8_img,  &ui_poke_reply_9_img,
    &ui_poke_reply_10_img, &ui_poke_reply_11_img, &ui_poke_reply_12_img,
    &ui_poke_reply_13_img, &ui_poke_reply_14_img, &ui_poke_reply_15_img};
static_assert(UI_POKE_REPLY_COUNT == UI_POKE_PHRASE_COUNT * UI_POKE_REPLIES_PER_PHRASE,
              "gen_poke.py REPLIES 는 문구마다 3칸");
const lv_image_dsc_t* const kPokeTripRows[3] = {&ui_poke_trip_img, &ui_poke_invite_img,
                                                &ui_poke_home_img};
constexpr lv_opa_t kPokeOffOpa = LV_OPA_70;     // 지금 못 누르는 오른쪽 칸 줄을 판 색으로 덮는 정도
constexpr uint32_t kPokeDivider = 0x5A544C;     // 두 칸 사이 세로줄
const lv_image_dsc_t* const kPokePhrases[UI_POKE_PHRASE_COUNT] = {
    &ui_poke_phrase_1_img, &ui_poke_phrase_2_img, &ui_poke_phrase_3_img,
    &ui_poke_phrase_4_img, &ui_poke_phrase_5_img,
};

constexpr const char* kTag = "UsageScreen";

// 상태별 라벨·색. 순서는 UsageState 와 같다. 캐릭터 그림은 팩의 상태 프레임이다.
struct StateSpec {
    const char* label;
    uint32_t color;
};
constexpr StateSpec kStateSpecs[kUsageStateCount] = {
    {"", 0xC77A4E},              // idle
    {"THINKING...", 0xC77A4E},   // thinking
    {"WORKING...", 0xC77A4E},    // working
    {"JUGGLING", 0xC77A4E},      // juggling
    {"PERMISSION?", 0xFFD34D},   // permission
    {"ERROR!", 0xFF6B5E},        // error
    {"DONE!", 0x8FD98A},         // done
    {"ZZZ...", 0xC77A4E},        // sleeping
};

// 몬세라트(문장용). 반드시 정적으로 링크된 폰트만 돌려준다. 에셋 유래 폰트를
// 쓰면 재적용 때 해제되어 죽는다. sdkconfig 에서 켜 둔 크기는 12/14/16/20 넷이다.
// 심볼 선언은 lvgl 의 lv_font.h 가 CONFIG_LV_FONT_MONTSERRAT_* 에 맞춰
// 전역에 해준다. 여기서 다시 선언하면 익명 네임스페이스 탓에 내부 링키지의
// 다른 심볼이 되어 링크가 깨진다.
const lv_font_t* FontForSize(int size) {
    if (size <= 12) return &lv_font_montserrat_12;
    if (size <= 14) return &lv_font_montserrat_14;
    if (size <= 16) return &lv_font_montserrat_16;
    return &lv_font_montserrat_20;
}

// 픽셀 폰트를 입히고 자간을 준다. 8px 은 자간 없이는 글자가 붙어 보인다.
void UsePixelFont(lv_obj_t* label, const lv_font_t* font) {
    lv_obj_set_style_text_font(label, font, 0);
    if (font == &press_start_8) {
        lv_obj_set_style_text_letter_space(label, kPixelLetterSpace, 0);
    }
}

void SetHidden(lv_obj_t* obj, bool hidden) {
    if (obj == nullptr) return;
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

// 배경·테두리·여백을 모두 지운 민무늬 판. 스크롤도 끈다.
void StylePlain(lv_obj_t* obj, uint32_t color, lv_opa_t opa) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

// lv_image 를 (cx, cy) 가운데에 배율 scale 로 놓는다. 피벗이 원본 중심이라 커져도 가운데를 지킨다.
void PlaceScaledImage(lv_obj_t* img, const lv_image_dsc_t* dsc, int cx, int cy, int scale) {
    const int w = static_cast<int>(dsc->header.w), h = static_cast<int>(dsc->header.h);
    lv_image_set_src(img, dsc);
    lv_image_set_pivot(img, w / 2, h / 2);
    lv_image_set_scale(img, LV_SCALE_NONE * scale);
    lv_obj_set_pos(img, cx - w / 2, cy - h / 2);
}

// 민무늬 사각형 (파티클·판용). 배경만 있고 스크롤·클릭 없음.
lv_obj_t* MakePlainBox(lv_obj_t* parent, int w, int h) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_size(o, w, h);
    return o;
}

// 한 줄 라벨. align 으로 좌상/우상 모서리에 붙인다 — 폭은 내용에 맞춘다.
lv_obj_t* MakeLabel(lv_obj_t* parent, uint32_t color, int font_size, lv_align_t align,
                    int x_ofs, int y_ofs) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, FontForSize(font_size), 0);
    lv_obj_align(label, align, x_ofs, y_ofs);
    return label;
}

}  // namespace

// --- SkyCanvas --------------------------------------------------------------

SkyCanvas::~SkyCanvas() {
    if (buf_ != nullptr) {
        heap_caps_free(buf_);
    }
}

const lv_image_dsc_t* SkyCanvas::Render(const lv_image_dsc_t* src) {
    if (src == nullptr) {
        return nullptr;   // 앱에 배경을 안 굽는 빌드에서 assets 가 빠진 경우
    }
    const bool i4 = src->header.cf == LV_COLOR_FORMAT_I4;
    if (!i4 && src->header.cf != LV_COLOR_FORMAT_I8) {
        return src;   // 팔레트 배경이 아니면 그대로
    }
    const int w = src->header.w;
    const int h = src->header.h;
    const size_t size = static_cast<size_t>(w) * h * 2;
    if (buf_ == nullptr) {
        buf_ = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
        if (buf_ == nullptr) {
            ESP_LOGW("SkyCanvas", "PSRAM %u alloc failed — draw I8 directly", static_cast<unsigned>(size));
            return src;
        }
        dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
        dsc_.header.w = w;
        dsc_.header.h = h;
        dsc_.header.stride = w * 2;
        dsc_.data_size = size;
        dsc_.data = buf_;
    }
    if (dsc_.header.w != w || dsc_.header.h != h) {
        return src;   // 배경은 전부 320×240 — 다르면 그리지 않고 원본으로
    }
    // 팔레트(BGRA 16 또는 256개) → RGB565 LUT → 인덱스 풀기
    uint16_t lut[256];
    const uint8_t* pal = src->data;
    const int palette_count = i4 ? 16 : 256;
    for (int i = 0; i < palette_count; i++) {
        const uint8_t b = pal[i * 4], g = pal[i * 4 + 1], r = pal[i * 4 + 2];
        lut[i] = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    const uint8_t* idx = src->data + palette_count * 4;
    uint16_t* out = reinterpret_cast<uint16_t*>(buf_);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t packed = idx[y * src->header.stride + x / (i4 ? 2 : 1)];
            const uint8_t index = i4 ? ((x & 1) ? packed & 0x0F : packed >> 4) : packed;
            out[y * w + x] = lut[index];
        }
    }
    // 이미지 캐시는 이 빌드에서 꺼져 있어(LV_CACHE_DEF_SIZE=0) 드롭이 필요 없다.
    // 같은 dsc 포인터를 다시 물려도 매 그리기마다 버퍼를 새로 읽는다.
    return &dsc_;
}

UsageScreen::UsageScreen(lv_obj_t* parent, int width, int height)
    : width_(width), height_(height) {
    // 전체를 덮는 불투명 패널. SetTheme() 은 부모(container_)의 배경색만
    // 덮어쓰고 자식은 건드리지 않으므로, 여기 색은 살아남는다.
    panel_ = lv_obj_create(parent);
    lv_obj_set_pos(panel_, 0, 0);
    lv_obj_set_size(panel_, width, height);
    StylePlain(panel_, kBg, LV_OPA_COVER);

    const int pad_x = width * 4 / 100;    // 시안의 좌우 4% (320 에서 13)
    const int pad_y = height * 4 / 100;
    auto sy = [height](int ref_y) { return ref_y * height / kRefH; };
    pad_x_ = pad_x;
    pad_y_ = pad_y;

    // 대기 화면 배경(밤 마을·길·잔디). 맨 먼저 만들어 모든 글자·캐릭터 아래에
    // 깔린다. 320×240 정적 그림이라 다른 해상도면 lv_image 가 그냥 왼쪽 위에
    // 붙는다 — 이 보드 전용으로 그린 그림이다. 플래시엔 I8, 표시는 PSRAM RGB565.
    bg_image_ = lv_image_create(panel_);
    lv_image_set_src(bg_image_, sky_canvas_.Render(SkyBackground(sky_)));
    lv_obj_set_pos(bg_image_, 0, 0);
    SetHidden(bg_image_, true);

    // --- 대기 화면: 왼쪽 블록은 왼끝, 오른쪽 블록(week) 은 오른끝 맞춤 ---
    for (int i = 0; i < kLimitBlocks; i++) {
        limit_plates_[i] = MakePlainBox(panel_, 1, 1);
        lv_obj_set_style_bg_color(limit_plates_[i], lv_color_hex(kBg), 0);
        lv_obj_set_style_bg_opa(limit_plates_[i], kLimitPlateOpa, 0);
        lv_obj_set_style_radius(limit_plates_[i], kLimitPlateRadius, 0);
        lv_obj_remove_flag(limit_plates_[i], LV_OBJ_FLAG_CLICKABLE);
        SetHidden(limit_plates_[i], true);
        limit_bar_bg_[i] = MakePlainBox(panel_, 1, kLimitBarH);
        lv_obj_set_style_bg_color(limit_bar_bg_[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(limit_bar_bg_[i], LV_OPA_20, 0);
        lv_obj_remove_flag(limit_bar_bg_[i], LV_OBJ_FLAG_CLICKABLE);
        SetHidden(limit_bar_bg_[i], true);
        limit_bar_fill_[i] = MakePlainBox(panel_, 1, kLimitBarH);
        lv_obj_set_style_bg_color(limit_bar_fill_[i], lv_color_hex(kLimitBarFill), 0);
        lv_obj_set_style_bg_opa(limit_bar_fill_[i], LV_OPA_COVER, 0);
        lv_obj_remove_flag(limit_bar_fill_[i], LV_OBJ_FLAG_CLICKABLE);
        SetHidden(limit_bar_fill_[i], true);
        const lv_align_t align = (i == 0) ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT;
        const int x_ofs = (i == 0) ? pad_x : -pad_x;
        name_labels_[i] = MakeLabel(panel_, kLimitName, 12, align, x_ofs, sy(kNameY));
        UsePixelFont(name_labels_[i], &press_start_8);
        pct_labels_[i] = MakeLabel(panel_, kLimitPct, 12, align, x_ofs, sy(kPctY));
        UsePixelFont(pct_labels_[i], &press_start_8);
        left_labels_[i] = MakeLabel(panel_, kLimitLeft, 12, align, x_ofs, sy(kLeftY));
        UsePixelFont(left_labels_[i], &press_start_8);
        left2_labels_[i] = MakeLabel(panel_, kLimitLeft, 12, align, x_ofs, sy(kLeftY));
        UsePixelFont(left2_labels_[i], &press_start_8);
        // 자간 0 — 판을 좁혀 윗줄에 시계 칸 자리를 낸다(옷장 글자는 그대로 1px).
        for (lv_obj_t* label : {name_labels_[i], pct_labels_[i], left_labels_[i], left2_labels_[i]}) {
            lv_obj_set_style_text_letter_space(label, 0, 0);
        }
        SetHidden(name_labels_[i], true);
        SetHidden(pct_labels_[i], true);
        SetHidden(left_labels_[i], true);
        SetHidden(left2_labels_[i], true);
    }

    // 윗줄 시계 칸. 글자·그림은 판의 자식이라 판 기준 좌표로 놓는다(LayoutClockPlate).
    clock_plate_ = MakePlainBox(panel_, 1, kLimitPlateH);
    lv_obj_set_style_bg_color(clock_plate_, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(clock_plate_, kLimitPlateOpa, 0);
    lv_obj_set_style_radius(clock_plate_, kLimitPlateRadius, 0);
    lv_obj_remove_flag(clock_plate_, LV_OBJ_FLAG_CLICKABLE);
    for (lv_obj_t** label : {&clock_ampm_, &clock_time_, &clock_temp_}) {
        *label = MakeLabel(clock_plate_, kLimitPct, 12, LV_ALIGN_TOP_LEFT, 0, kClockTextY);
        UsePixelFont(*label, &press_start_8);
        lv_obj_set_style_text_letter_space(*label, 0, 0);
    }
    clock_icon_ = lv_image_create(clock_plate_);
    lv_obj_remove_flag(clock_icon_, LV_OBJ_FLAG_CLICKABLE);
    SetHidden(clock_icon_, true);
    clock_deg_ = MakePlainBox(clock_plate_, kClockDegPx, kClockDegPx);
    lv_obj_set_style_bg_opa(clock_deg_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(clock_deg_, lv_color_hex(kLimitPct), 0);
    lv_obj_set_style_border_width(clock_deg_, 1, 0);
    SetHidden(clock_deg_, true);
    SetHidden(clock_plate_, true);

    // 길 위의 팩 캐릭터.  위 블록(y≤~90)과 겹치지 않는다.
    //
    // 자리 소품: 몸(figure_ 컨테이너) 바로 아래 z — 먼저 만든다. 아이템과 같은 nearest 확대.
    seat_img_ = lv_image_create(panel_);
    lv_image_set_pivot(seat_img_, 0, 0);
    lv_image_set_antialias(seat_img_, false);
    SetHidden(seat_img_, true);
    prop_img_ = lv_image_create(panel_);
    SetHidden(prop_img_, true);
    rozzi_img_ = lv_image_create(panel_);
    lv_image_set_antialias(rozzi_img_, false);
    SetHidden(rozzi_img_, true);
    CastCreate();               // 로찌 그림 바로 위에 놓인다
    ApplyCharacterGeometry();   // 걷기 좌표 + seat_img_·몸 배율
    figure_ = std::make_unique<Figure>(panel_, walk_scale_);
    figure_->SetBodyPos(walk_x_min_, walk_y_);
    figure_->SetHidden(true);
    front_img_ = lv_image_create(panel_);
    SetHidden(front_img_, true);
    // 세트를 미리 정해 둔다. NVS 복원(LcdDisplay::SetUsageSet)이 덮기 전까지 pack.json default.
    pack_set_ = ActivePack().default_set;
    ESP_LOGI(kTag, "character pack %s: %u sets, default %s", ActivePack().name,
             ActivePack().set_count, pack_set().key);

    // 우하단 옷걸이(옷장 진입). 히트는 kHangerHit 정사각형, 그림은 하트 버튼과 같은 34×30.
    hanger_ = lv_image_create(panel_);
    lv_image_set_src(hanger_, &ui_poke_btn_hanger_img);
    lv_obj_set_pos(hanger_, width - kPokeBtnMarginX - static_cast<int>(ui_poke_btn_hanger_img.header.w),
                   height - kPokeBtnMarginY - static_cast<int>(ui_poke_btn_hanger_img.header.h));
    SetHidden(hanger_, true);

    // 흐르는 줄: 하트와 옷걸이 사이. 글꼴이 assets 에 있는 빌드에서만 보인다.
    ticker_.Create(panel_, kTickerX, height - kTickerBottom - kTickerH, width - kTickerX - kTickerRight, kTickerH);

    // 좌하단 하트(콕 찌르기). 옷걸이와 같은 자리 규칙으로 대칭, 히트는 kPokeBtnHit 정사각형.
    poke_btn_ = lv_image_create(panel_);
    lv_image_set_src(poke_btn_, &ui_poke_btn_heart_img);
    lv_obj_remove_flag(poke_btn_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(poke_btn_, kPokeBtnMarginX,
                   height - kPokeBtnMarginY - static_cast<int>(ui_poke_btn_heart_img.header.h));
    SetHidden(poke_btn_, true);

    // --- 상태 화면 위젯 (전부 숨긴 채 만들어 둔다) -------------------------
    //
    // 캐릭터를 먼저, 글자를 나중에 만들어 글자가 항상 위에 깔리게 한다.
    // 상태 그림은 Figure(상태 배율)에 물려 옷장 아이템이 같이 뜬다.
    state_figure_ = std::make_unique<Figure>(panel_, ActivePack().state_scale256);
    state_figure_->SetHidden(true);

    for (int i = 0; i < kLimitBlocks; i++) {
        chip_labels_[i] = lv_label_create(panel_);
        lv_label_set_text(chip_labels_[i], "");
        lv_obj_set_style_text_color(chip_labels_[i], lv_color_hex(kStateChipText), 0);
        UsePixelFont(chip_labels_[i], &press_start_8);
        lv_obj_set_style_bg_color(chip_labels_[i], lv_color_hex(kStateChipBg), 0);
        lv_obj_set_style_bg_opa(chip_labels_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(chip_labels_[i], 3, 0);
        // 시안의 chip padding 3px 5px. 8px 폰트는 위아래 여백이 없어 4px 로
        // 살짝 더 준다.
        lv_obj_set_style_pad_hor(chip_labels_[i], 5, 0);
        lv_obj_set_style_pad_ver(chip_labels_[i], 4, 0);
        lv_obj_align(chip_labels_[i], i == 0 ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT,
                     i == 0 ? pad_x : -pad_x, pad_y);
        SetHidden(chip_labels_[i], true);
    }

    ctx_label_ = lv_label_create(panel_);
    lv_label_set_text(ctx_label_, "");
    /* 높이까지 한 줄로 못 박는다. 폭만 정하면 LONG_DOT 은 줄바꿈으로 두 줄이
     * 되어 캐릭터 위로 올라온다(권한 문구로 실기기에서 확인). */
    lv_obj_set_size(ctx_label_, width * 55 / 100, lv_font_get_line_height(FontForSize(12)));
    lv_label_set_long_mode(ctx_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(ctx_label_, lv_color_hex(kStateCtxText), 0);
    lv_obj_set_style_text_font(ctx_label_, FontForSize(12), 0);
    lv_obj_align(ctx_label_, LV_ALIGN_BOTTOM_LEFT, pad_x, -pad_y);
    SetHidden(ctx_label_, true);

    state_label_ = lv_label_create(panel_);
    lv_label_set_text(state_label_, "");
    UsePixelFont(state_label_, &press_start_8);
    // ctx(몬세라트 12, 줄높이 ~14)와 밑변을 맞춘다: 8px 픽셀 폰트는 그만큼
    // 더 내려야 같은 줄로 보인다.
    lv_obj_align(state_label_, LV_ALIGN_BOTTOM_RIGHT, -pad_x,
                 -pad_y - (lv_font_get_line_height(FontForSize(12)) - 8) / 2);
    SetHidden(state_label_, true);

    BuildSceneSprites();
    BuildParticles();

    // --- 에이전트 배지 + 토스트 (docs/agent-badge.md). 맨 마지막 = 맨 위 z ---
    badge_plate_ = MakePlainBox(panel_, kBadgePlatePx, kBadgePlatePx);
    lv_obj_set_pos(badge_plate_, kBadgePlateXY, kBadgePlateXY);
    lv_obj_set_style_bg_color(badge_plate_, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(badge_plate_, kLimitPlateOpa, 0);   // 사용량 칸과 같은 판 — 어느 장면에서나 읽히게
    lv_obj_set_style_radius(badge_plate_, kLimitPlateRadius, 0);
    lv_obj_remove_flag(badge_plate_, LV_OBJ_FLAG_CLICKABLE);
    SetHidden(badge_plate_, true);
    // 배지는 글자다(로고 그림을 싣지 않는다): Claude Code = 「CC」, Codex = 「CX」.
    badge_ = lv_label_create(panel_);
    lv_label_set_text(badge_, "");
    UsePixelFont(badge_, &press_start_8);
    lv_obj_remove_flag(badge_, LV_OBJ_FLAG_CLICKABLE);
    SetHidden(badge_, true);
    toast_ = lv_label_create(panel_);
    lv_label_set_text(toast_, "");
    lv_obj_set_style_text_color(toast_, lv_color_hex(kIdleName), 0);
    UsePixelFont(toast_, &press_start_8);
    // 대기 화면에선 큰 퍼센트 윗줄과 3px 겹친다 — 어두운 바탕을 깔아 읽히게.
    lv_obj_set_style_bg_color(toast_, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(toast_, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(toast_, 2, 0);
    lv_obj_set_style_pad_ver(toast_, 1, 0);
    SetHidden(toast_, true);
    // 전환 팝 — 배지 탭 순간 화면 가운데에 큰 이름. 실기기에서 작은 배지 교체만으론
    // 바뀐 걸 모른다는 데모 피드백(09-12).
    agent_pop_label_ = lv_label_create(panel_);
    lv_label_set_text(agent_pop_label_, "");
    lv_obj_set_style_text_color(agent_pop_label_, lv_color_hex(0xF0E7DD), 0);
    UsePixelFont(agent_pop_label_, &press_start_24);
    lv_obj_set_style_bg_color(agent_pop_label_, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(agent_pop_label_, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(agent_pop_label_, 4, 0);
    lv_obj_set_style_pad_ver(agent_pop_label_, 2, 0);
    lv_obj_remove_flag(agent_pop_label_, LV_OBJ_FLAG_CLICKABLE);
    SetHidden(agent_pop_label_, true);

    // 콕 찌르기: 문구 패널 + 받은 쪽 팝. 문구 이미지는 판 색이 kBg 라 패널과 이어져 보인다.
    // 왼쪽 칸은 문구 5줄, 오른쪽 칸은 놀러가기·놀러오기·집으로 가기(ny 11/7 「한 번에 두 칸」).
    int phrase_w = 0;
    for (const lv_image_dsc_t* dsc : kPokePhrases) {
        phrase_w = LV_MAX(phrase_w, static_cast<int>(dsc->header.w));
    }
    int trip_w = 0;
    for (const lv_image_dsc_t* dsc : kPokeTripRows) {
        trip_w = LV_MAX(trip_w, static_cast<int>(dsc->header.w));
    }
    poke_left_w_ = kPokePanelPad + phrase_w * kPokeTextScale + 16;
    poke_panel_w_ = poke_left_w_ + trip_w * kPokeTextScale + 16 + kPokePanelPad;
    poke_base_h_ = kPokeRowH * UI_POKE_PHRASE_COUNT + 2 * kPokePanelPad;
    poke_panel_ = lv_obj_create(panel_);
    StylePlain(poke_panel_, kBg, LV_OPA_COVER);
    lv_obj_set_style_border_width(poke_panel_, 1, 0);
    lv_obj_set_style_border_color(poke_panel_, lv_color_hex(kIdleName), 0);
    lv_obj_remove_flag(poke_panel_, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < UI_POKE_PHRASE_COUNT; i++) {
        lv_obj_t* row = lv_image_create(poke_panel_);
        lv_image_set_antialias(row, false);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        poke_phrase_rows_[i] = row;
    }
    poke_divider_ = lv_obj_create(poke_panel_);
    StylePlain(poke_divider_, kPokeDivider, LV_OPA_COVER);
    lv_obj_remove_flag(poke_divider_, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 3; i++) {
        lv_obj_t* row = lv_image_create(poke_panel_);
        lv_image_set_antialias(row, false);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        poke_trip_rows_[i] = row;
    }
    // 만복이 육아일기: 일기를 받은 보드만 맨 위에 한 줄(SetDiaryAvailable). docs/manbok-diary.md
    poke_diary_row_ = lv_image_create(poke_panel_);
    lv_image_set_antialias(poke_diary_row_, false);
    lv_obj_remove_flag(poke_diary_row_, LV_OBJ_FLAG_CLICKABLE);
    poke_diary_sep_ = lv_obj_create(poke_panel_);
    StylePlain(poke_diary_sep_, kPokeDivider, LV_OPA_COVER);
    lv_obj_remove_flag(poke_diary_sep_, LV_OBJ_FLAG_CLICKABLE);
    LayoutPokePanel();
    SetHidden(poke_panel_, true);
    poke_pop_ = lv_image_create(panel_);
    lv_image_set_antialias(poke_pop_, false);
    lv_obj_remove_flag(poke_pop_, LV_OBJ_FLAG_CLICKABLE);
    PlaceScaledImage(poke_pop_, &ui_poke_heart_img, width / 2, kPokePopCenterY, kPokePopScale);
    SetHidden(poke_pop_, true);
    poke_pop_text_ = lv_image_create(panel_);
    lv_image_set_antialias(poke_pop_text_, false);
    lv_obj_remove_flag(poke_pop_text_, LV_OBJ_FLAG_CLICKABLE);
    SetHidden(poke_pop_text_, true);
    // 답장 버튼 3칸. 그림·자리는 받은 문구에 따라 LayoutPokeReplies 가 정한다.
    for (lv_obj_t*& btn : poke_reply_btn_) {
        btn = lv_image_create(panel_);
        lv_image_set_antialias(btn, false);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        SetHidden(btn, true);
    }
    poke_read_ = lv_image_create(panel_);
    lv_image_set_antialias(poke_read_, false);
    lv_obj_remove_flag(poke_read_, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(poke_read_, &ui_poke_read_img);
    SetHidden(poke_read_, true);
    UpdateBadge();
}

// --- 콕 찌르기 (docs/poke.md) -----------------------------------------------------

bool UsageScreen::HitPokeButton(int x, int y) const {
    return x < kPokeBtnHit && y >= height_ - kPokeBtnHit;
}

// 문구 패널 배치. 육아일기 줄이 있으면 패널이 한 줄 길어지고 나머지가 한 줄 내려간다.
// 자식 좌표는 부모 테두리(1px) 안쪽 기준이라 그만큼 뺀다.
static_assert(UI_POKE_PHRASE_COUNT == 5, "poke_phrase_rows_ 는 5칸이다");

void UsageScreen::LayoutPokePanel() {
    if (poke_panel_ == nullptr) return;
    poke_top_h_ = diary_available_ ? kPokeRowH : 0;
    const int h = poke_base_h_ + poke_top_h_;
    poke_panel_x_ = (width_ - poke_panel_w_) / 2;
    poke_panel_y_ = (height_ - h) / 2;
    lv_obj_set_pos(poke_panel_, poke_panel_x_, poke_panel_y_);
    lv_obj_set_size(poke_panel_, poke_panel_w_, h);
    const int top = kPokePanelPad + poke_top_h_;
    for (int i = 0; i < UI_POKE_PHRASE_COUNT; i++) {
        PlaceScaledImage(poke_phrase_rows_[i], kPokePhrases[i], poke_left_w_ / 2 - 1, top + kPokeRowH * i + kPokeRowH / 2 - 1,
                         kPokeTextScale);
    }
    lv_obj_set_pos(poke_divider_, poke_left_w_ - 1, top + 4);
    lv_obj_set_size(poke_divider_, 1, poke_base_h_ - 2 * kPokePanelPad - 10);
    for (int i = 0; i < 3; i++) {
        PlaceScaledImage(poke_trip_rows_[i], kPokeTripRows[i], (poke_left_w_ + poke_panel_w_) / 2 - 1,
                         top + kPokeRowH * i + kPokeRowH / 2 - 1, kPokeTextScale);
    }
    SetHidden(poke_diary_row_, !diary_available_);
    SetHidden(poke_diary_sep_, !diary_available_);
    if (diary_available_) {
        PlaceScaledImage(poke_diary_row_, &ui_poke_diary_img, poke_panel_w_ / 2 - 1, kPokePanelPad + kPokeRowH / 2 - 1,
                         kPokeTextScale);
        lv_obj_set_pos(poke_diary_sep_, 8, kPokePanelPad + kPokeRowH - 1);
        lv_obj_set_size(poke_diary_sep_, poke_panel_w_ - 18, 1);
    }
}

void UsageScreen::SetDiaryAvailable(bool available) {
    if (diary_available_ == available) return;
    diary_available_ = available;
    LayoutPokePanel();
}

void UsageScreen::ShowDiaryToast(const std::string& text) {
    const lv_font_t* font = ticker_.font();
    if (font == nullptr || panel_ == nullptr) return;   // 한글 글꼴이 없는 빌드 — 흐르는 줄·소리만
    if (diary_toast_ == nullptr) {
        diary_toast_ = lv_label_create(panel_);
        lv_obj_set_style_text_font(diary_toast_, font, 0);
        lv_obj_set_style_text_color(diary_toast_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_color(diary_toast_, lv_color_hex(kBg), 0);
        lv_obj_set_style_bg_opa(diary_toast_, LV_OPA_80, 0);
        lv_obj_set_style_border_color(diary_toast_, lv_color_hex(0xFFAAB9), 0);
        lv_obj_set_style_border_width(diary_toast_, 1, 0);
        lv_obj_set_style_radius(diary_toast_, 4, 0);
        lv_obj_set_style_pad_hor(diary_toast_, 6, 0);
        lv_obj_set_style_pad_ver(diary_toast_, 3, 0);
        lv_obj_remove_flag(diary_toast_, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_label_set_text(diary_toast_, text.c_str());
    lv_obj_set_pos(diary_toast_, pad_x_, kBadgeBelowY);
    SetHidden(diary_toast_, false);
    lv_obj_move_foreground(diary_toast_);
    if (diary_toast_timer_ != nullptr) lv_timer_delete(diary_toast_timer_);
    diary_toast_timer_ = lv_timer_create(OnDiaryToastDone, kDiaryToastMs, this);
    lv_timer_set_repeat_count(diary_toast_timer_, 1);
}

void UsageScreen::HideDiaryToast() {
    if (diary_toast_ != nullptr) SetHidden(diary_toast_, true);
    if (diary_toast_timer_ != nullptr) {
        lv_timer_delete(diary_toast_timer_);
        diary_toast_timer_ = nullptr;
    }
}

void UsageScreen::OnDiaryToastDone(lv_timer_t* timer) {
    auto* self = static_cast<UsageScreen*>(lv_timer_get_user_data(timer));
    self->diary_toast_timer_ = nullptr;   // 한 번 울린 타이머는 LVGL 이 지운다
    if (self->diary_toast_ != nullptr) SetHidden(self->diary_toast_, true);
}

// 지금 할 수 있는 것만 밝게. 아무도 놀러 가 있지 않으면 놀러가기·놀러오기, 누가 놀러 가 있으면 집으로 가기.
void UsageScreen::TripRowsOn(bool on[3]) const {
    const bool visiting = trip_ != Trip::kNone;
    const bool asking = PokeLink::GetInstance().TripAsking() || PokeLink::GetInstance().InviteWaiting();
    on[0] = !visiting && !asking;
    on[1] = on[0];
    on[2] = visiting && trip_ != Trip::kReturning && trip_ != Trip::kGuestExit;
}

void UsageScreen::OpenPokePanel() {
    ESP_LOGI(kTag, "poke panel open");
    poke_panel_open_ = true;
    TripRowsOn(poke_trip_on_);
    for (int i = 0; i < 3; i++) {
        // 투명도를 낮추면 글자 판이 비쳐 회색 상자가 보인다. 판 색(kBg)으로 덧칠해 글자만 흐리게 한다.
        lv_obj_set_style_image_recolor(poke_trip_rows_[i], lv_color_hex(kBg), 0);
        lv_obj_set_style_image_recolor_opa(poke_trip_rows_[i], poke_trip_on_[i] ? LV_OPA_TRANSP : kPokeOffOpa, 0);
    }
    LayoutPokePanel();
    SetHidden(poke_panel_, false);
    lv_obj_move_foreground(poke_panel_);
    poke_panel_timer_ = lv_timer_create(OnPokePanelTimeout, kPokePanelMs, this);
    lv_timer_set_repeat_count(poke_panel_timer_, 1);
}

void UsageScreen::ClosePokePanel() {
    if (!poke_panel_open_) return;
    ESP_LOGI(kTag, "poke panel close");
    poke_panel_open_ = false;
    SetHidden(poke_panel_, true);
    if (poke_panel_timer_ != nullptr) {
        lv_timer_delete(poke_panel_timer_);
        poke_panel_timer_ = nullptr;
    }
}

void UsageScreen::OnPokePanelTimeout(lv_timer_t* timer) {
    auto* self = static_cast<UsageScreen*>(lv_timer_get_user_data(timer));
    self->poke_panel_timer_ = nullptr;   // 한 번 울린 타이머는 LVGL 이 지운다
    self->ClosePokePanel();
}

// 패널이 열린 동안은 모든 터치가 여기서 끝난다. 줄 탭 = 그 문구로 전송, 그 밖의 탭 = 닫기.
UsageScreen::TouchAction UsageScreen::OnPokePanelTouch(const Display::TouchEvent& ev) {
    if (ev.kind != Display::TouchEvent::kTap) return TouchAction::kNone;
    const int x = ev.x0 - poke_panel_x_;
    int y = ev.y0 - poke_panel_y_ - kPokePanelPad;
    if (poke_top_h_ > 0 && y >= 0 && y < poke_top_h_ && x >= 0 && x < poke_panel_w_) {
        ClosePokePanel();
        ESP_LOGI(kTag, "poke panel pick diary");
        diary_open_entry_ = -1;   // 목록부터
        return TouchAction::kOpenDiary;
    }
    y -= poke_top_h_;
    const int row = y >= 0 ? y / kPokeRowH : -1;
    const bool on_row = x >= 0 && x < poke_panel_w_ && row >= 0 && row < UI_POKE_PHRASE_COUNT;
    ClosePokePanel();
    if (!on_row) return TouchAction::kNone;
    if (x >= poke_left_w_) {
        if (row >= 3 || !poke_trip_on_[row]) return TouchAction::kNone;   // 흐린 줄·빈 자리는 닫기만
        constexpr TouchAction kTripActions[3] = {TouchAction::kTripGo, TouchAction::kTripInvite,
                                                 TouchAction::kTripHome};
        ESP_LOGI(kTag, "poke panel pick trip %d", row);
        return kTripActions[row];
    }
    poke_phrase_ = static_cast<uint8_t>(row + 1);
    ESP_LOGI(kTag, "poke panel pick %u", poke_phrase_);
    return TouchAction::kPokeSend;
}

void UsageScreen::ShowPokePop(uint8_t phrase) {
    ClosePokePanel();   // 팝이 패널을 덮으면 안 보이는 줄이 눌린다
    const bool has_text = phrase >= 1 && phrase <= UI_POKE_PHRASE_COUNT;
    if (has_text) {
        PlaceScaledImage(poke_pop_text_, kPokePhrases[phrase - 1], width_ / 2, kPokePopTextCenterY,
                         kPokeTextScale);
        lv_obj_move_foreground(poke_pop_text_);
    }
    SetHidden(poke_pop_text_, !has_text);
    SetHidden(poke_pop_, false);
    lv_obj_move_foreground(poke_pop_);
    if (has_text) LayoutPokeReplies(phrase);
    SetPokeReplies(has_text);
    StartPokePopTimer(has_text ? kPokeReplyPopMs : kPokePopMs);
    if (state_ == kUsageStateIdle && !idle_paused_) {
        BeginPackReaction(ReactionAnim(PackReaction::kPet), kPackTapMs);
    }
}

void UsageScreen::ShowPokeReplyPop(uint8_t reply) {
    if (reply < 1 || reply > UI_POKE_REPLY_COUNT) return;
    ClosePokePanel();
    SetPokeReplies(false);
    PlaceScaledImage(poke_pop_text_, kPokeReplies[reply - 1], width_ / 2, kPokePopTextCenterY, kPokeTextScale);
    lv_obj_move_foreground(poke_pop_text_);
    SetHidden(poke_pop_text_, false);
    SetHidden(poke_pop_, false);
    lv_obj_move_foreground(poke_pop_);
    StartPokePopTimer(kPokePopMs);
    lv_obj_set_pos(poke_read_, pad_x_, kBadgeBelowY);
    SetHidden(poke_read_, false);
    lv_obj_move_foreground(poke_read_);
    if (poke_read_timer_ != nullptr) {
        lv_timer_reset(poke_read_timer_);
    } else {
        poke_read_timer_ = lv_timer_create(OnPokeReadDone, kPokeReadMs, this);
        lv_timer_set_repeat_count(poke_read_timer_, 1);
    }
}

void UsageScreen::ShowHighFivePop() {
    ClosePokePanel();
    SetPokeReplies(false);
    SetHidden(poke_pop_, true);
    PlaceScaledImage(poke_pop_text_, &ui_poke_high5_img, width_ / 2, kPokePopCenterY, kPokeTextScale);
    lv_obj_move_foreground(poke_pop_text_);
    SetHidden(poke_pop_text_, false);
    StartPokePopTimer(kHighFivePopMs);
    if (state_ == kUsageStateIdle && !idle_paused_) {
        BeginPackReaction(ReactionAnim(PackReaction::kHighFive), kHighFivePopMs);
    }
}

void UsageScreen::StartPokePopTimer(uint32_t ms) {
    if (poke_pop_timer_ != nullptr) {
        lv_timer_delete(poke_pop_timer_);
    }
    poke_pop_timer_ = lv_timer_create(OnPokePopDone, ms, this);
    lv_timer_set_repeat_count(poke_pop_timer_, 1);
}

// 받은 문구의 답장 3칸을 2× 로 가운데 정렬한다. 손가락 판정은 좌표로 한다(OnPokeRepliesTouch).
// 줄 폭은 gen_poke.py 가 320 안으로 검사한다.
void UsageScreen::LayoutPokeReplies(uint8_t phrase) {
    poke_reply_base_ = static_cast<uint8_t>((phrase - 1) * UI_POKE_REPLIES_PER_PHRASE);
    const lv_image_dsc_t* const* set = &kPokeReplies[poke_reply_base_];
    int replies_w = kPokeReplyGap * (UI_POKE_REPLIES_PER_PHRASE - 1);
    for (int i = 0; i < UI_POKE_REPLIES_PER_PHRASE; i++) {
        replies_w += set[i]->header.w * kPokeTextScale;
    }
    int reply_x = (width_ - replies_w) / 2;
    for (int i = 0; i < UI_POKE_REPLIES_PER_PHRASE; i++) {
        const int w = set[i]->header.w * kPokeTextScale;
        const int h = set[i]->header.h * kPokeTextScale;
        PlaceScaledImage(poke_reply_btn_[i], set[i], reply_x + w / 2, kPokeReplyCenterY, kPokeTextScale);
        poke_reply_hit_[i] = {static_cast<lv_coord_t>(reply_x - kPokeReplyHitPad),
                              static_cast<lv_coord_t>(kPokeReplyCenterY - h / 2 - kPokeReplyHitPad),
                              static_cast<lv_coord_t>(reply_x + w + kPokeReplyHitPad),
                              static_cast<lv_coord_t>(kPokeReplyCenterY + h / 2 + kPokeReplyHitPad)};
        reply_x += w + kPokeReplyGap;
    }
}

void UsageScreen::SetPokeReplies(bool open) {
    poke_replies_open_ = open;
    for (lv_obj_t* btn : poke_reply_btn_) {
        SetHidden(btn, !open);
        if (open) lv_obj_move_foreground(btn);
    }
}

// 답장 버튼 탭이면 전송으로 넘긴다. 버튼 밖 탭은 평소 터치로 흘려보낸다.
UsageScreen::TouchAction UsageScreen::OnPokeRepliesTouch(const Display::TouchEvent& ev) {
    if (ev.kind != Display::TouchEvent::kTap) return TouchAction::kNone;
    for (int i = 0; i < UI_POKE_REPLIES_PER_PHRASE; i++) {
        const lv_area_t& a = poke_reply_hit_[i];
        if (ev.x0 >= a.x1 && ev.x0 < a.x2 && ev.y0 >= a.y1 && ev.y0 < a.y2) {
            poke_reply_ = static_cast<uint8_t>(poke_reply_base_ + i + 1);
            SetPokeReplies(false);
            SetHidden(poke_pop_, true);
            SetHidden(poke_pop_text_, true);
            ESP_LOGI(kTag, "poke reply pick %u", poke_reply_);
            return TouchAction::kPokeReply;
        }
    }
    return TouchAction::kNone;
}

void UsageScreen::OnPokePopDone(lv_timer_t* timer) {
    auto* self = static_cast<UsageScreen*>(lv_timer_get_user_data(timer));
    self->poke_pop_timer_ = nullptr;
    self->SetPokeReplies(false);
    SetHidden(self->poke_pop_, true);
    SetHidden(self->poke_pop_text_, true);
}

void UsageScreen::OnPokeReadDone(lv_timer_t* timer) {
    auto* self = static_cast<UsageScreen*>(lv_timer_get_user_data(timer));
    self->poke_read_timer_ = nullptr;
    SetHidden(self->poke_read_, true);
}

// --- 에이전트 배지 --------------------------------------------------------------

void UsageScreen::UpdateBadge() {
    if (badge_ == nullptr) return;
    const UsageAgent shown = agent_;   // 배지 = 선택. 항상 하나는 떠 있다 (§10, BOTH 없음)
    if (shown != badge_shown_) {
        badge_shown_ = shown;
        ESP_LOGI(kTag, "agent badge %s", AgentKey(shown));
    }
    // 상태 화면 칩을 배지만큼 민다. 대기 화면 칸은 LayoutLimitPlate 가 놓는다.
    lv_obj_align(chip_labels_[0], LV_ALIGN_TOP_LEFT, pad_x_ + kBadgeGap, pad_y_);
    // 글자 색으로도 가른다 — Claude 주황, Codex 파랑 (tools/qa_agent.py 가 색으로 확인한다).
    lv_label_set_text(badge_, shown == kUsageAgentClaude ? "CC" : "CX");
    lv_obj_set_style_text_color(badge_, lv_color_hex(shown == kUsageAgentClaude ? kBadgeClaude : kBadgeCodex), 0);
    // 글자 폭과 상관없이 판 가운데 — 너비를 판에 맞추고 가운데 정렬(숨겨진 첫 배치에서 폭 0 이어도 맞다).
    const int badge_line_h = lv_font_get_line_height(lv_obj_get_style_text_font(badge_, LV_PART_MAIN));
    lv_obj_set_width(badge_, kBadgePlatePx);
    lv_obj_set_style_text_align(badge_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(badge_, kBadgePlateXY, kBadgePlateXY + (kBadgePlatePx - badge_line_h) / 2);
    // 옷장·화면 끄기(idle_paused_) 중엔 숨긴다 — 오발탭 방지. WiFi 가이드는 전체
    // 화면 오버레이라 저절로 가려진다.
    SetHidden(badge_plate_, idle_paused_);
    SetHidden(badge_, idle_paused_);
    lv_obj_move_foreground(badge_plate_);
    lv_obj_move_foreground(badge_);
}

void UsageScreen::SetAgent(UsageAgent agent, bool toast) {
    if (agent >= kUsageAgentCount) return;
    const bool changed = agent != agent_;
    agent_ = agent;
    static constexpr const char* kNames[kUsageAgentCount] = {"CLAUDE", "CODEX"};
    ESP_LOGI(kTag, "agent select -> %s", AgentKey(agent_));
    UpdateBadge();
    // 다른 쪽 숫자가 남아 있으면 안 된다. 받은 적 없으면 칸을 비운다.
    if (changed) SetStats(agent_stats_valid_[agent_] ? agent_stats_[agent_] : UsageStats{});
    if (toast) {
        ShowAgentToast(kNames[agent_]);
        ShowAgentPop(agent_);
    }
}

void UsageScreen::SetAgentStats(UsageAgent agent, const UsageStats& stats) {
    if (agent >= kUsageAgentCount) return;
    agent_stats_[agent] = stats;
    agent_stats_valid_[agent] = true;
    if (agent == agent_) SetStats(stats);
}

UsageAgent UsageScreen::CycleAgent() {
    // Claude ↔ Codex
    SetAgent(agent_ == kUsageAgentClaude ? kUsageAgentCodex : kUsageAgentClaude, /*toast=*/true);
    return agent_;
}

bool UsageScreen::HitBadge(int x, int y) const {
    // 가려진 동안(옷장·화면 끄기)은 LcdDisplay 가 앞에서 거른다.
    // 시계 칸이 떠 있으면 배지 히트 박스를 그 왼쪽 끝(x 44)까지 줄인다 — 칸을 누르다 에이전트가 바뀌지 않게.
    const bool clock_shown = clock_plate_ != nullptr && !lv_obj_has_flag(clock_plate_, LV_OBJ_FLAG_HIDDEN);
    return x < (clock_shown ? lv_obj_get_x(clock_plate_) : kBadgeHitW) && y < kBadgeHitH;
}

bool UsageScreen::HitClockPlate(int x, int y) const {
    if (clock_plate_ == nullptr || lv_obj_has_flag(clock_plate_, LV_OBJ_FLAG_HIDDEN)) return false;
    const int left = lv_obj_get_x(clock_plate_);
    return x >= left && x < left + lv_obj_get_width(clock_plate_) + 4 && y < kBadgeHitH;
}

void UsageScreen::ShowAgentToast(const char* text) {
    if (toast_ == nullptr) return;
    lv_label_set_text(toast_, text);
    lv_obj_set_pos(toast_, pad_x_, kBadgeBelowY);
    SetHidden(toast_, false);
    lv_obj_move_foreground(toast_);
    if (toast_timer_ != nullptr) {
        lv_timer_reset(toast_timer_);
        return;
    }
    toast_timer_ = lv_timer_create(OnAgentToastDone, kAgentToastMs, this);
    lv_timer_set_repeat_count(toast_timer_, 1);   // 한 번 울리고 LVGL 이 지운다
}

void UsageScreen::OnAgentToastDone(lv_timer_t* timer) {
    auto* self = static_cast<UsageScreen*>(lv_timer_get_user_data(timer));
    self->toast_timer_ = nullptr;
    SetHidden(self->toast_, true);
}

void UsageScreen::ShowAgentPop(UsageAgent agent) {
    if (agent_pop_label_ == nullptr) return;
    static constexpr const char* kNames[kUsageAgentCount] = {"CLAUDE", "CODEX"};
    lv_label_set_text(agent_pop_label_, kNames[agent]);
    lv_obj_align(agent_pop_label_, LV_ALIGN_TOP_MID, 0, kAgentPopY);
    SetHidden(agent_pop_label_, false);
    lv_obj_move_foreground(agent_pop_label_);
    if (agent_pop_timer_ != nullptr) {
        lv_timer_reset(agent_pop_timer_);
        return;
    }
    agent_pop_timer_ = lv_timer_create(OnAgentPopDone, kAgentPopMs, this);
    lv_timer_set_repeat_count(agent_pop_timer_, 1);
}

void UsageScreen::OnAgentPopDone(lv_timer_t* timer) {
    auto* self = static_cast<UsageScreen*>(lv_timer_get_user_data(timer));
    self->agent_pop_timer_ = nullptr;
    SetHidden(self->agent_pop_label_, true);
}

void UsageScreen::ShowIdle(bool visible) {
    idle_visible_ = visible;
    LayoutClockPlate();   // 왼쪽 한도 칸(i=0)이 시계 칸 오른쪽에 붙으니 먼저
    for (int i = 0; i < kLimitBlocks; i++) {
        // 숨길 때는 무조건, 되살릴 때는 값이 있는 것만.
        const bool show = visible && i < visible_cells_ && limit_present_[i];
        SetHidden(name_labels_[i], !show);
        SetHidden(pct_labels_[i], !show);
        SetHidden(left_labels_[i], !show);
        SetHidden(left2_labels_[i], !show);
        SetHidden(limit_plates_[i], !show);
        SetHidden(limit_bar_bg_[i], !show);
        SetHidden(limit_bar_fill_[i], !show);
        if (show) LayoutLimitPlate(i);
    }
    SetHidden(bg_image_, !visible);
    ApplySceneProps();
    SetHidden(hanger_, !visible);
    SetHidden(poke_btn_, !visible);
    ticker_.SetVisible(visible);
    if (!visible) ClosePokePanel();
    for (int i = 0; i < sprite_count_; i++) {
        SetHidden(sprite_objs_[i], !visible);
    }
    for (int i = 0; i < drop_count_; i++) {
        // 별똥별은 재등장 대기(speed<0) 중이면 계속 숨겨 둔다.
        const bool waiting_star = drop_kind_[i] == kDropStar && drop_speed_[i] < 0;
        SetHidden(drops_[i], !visible || waiting_star);
    }
    if (visible) {
        if (!idle_paused_) {
            StartWalk();  // 이미 걷고 있으면 그대로 둔다
        }
    } else {
        StopWalk();
    }
}

// 한 줄 판: 왼쪽에 이름 · %, 오른쪽 끝에 남은 시간, 그 아래 얇은 막대. 시안 C(tools/sim_lineart.py).
void UsageScreen::LayoutLimitPlate(int i) {
    lv_obj_update_layout(panel_);
    const int name_w = lv_obj_get_width(name_labels_[i]);
    const int pct_w = lv_obj_get_width(pct_labels_[i]);
    const bool has_left = lv_label_get_text(left_labels_[i])[0] != '\0';
    const bool has_left2 = lv_label_get_text(left2_labels_[i])[0] != '\0';
    const int left2_w = has_left2 ? lv_obj_get_width(left2_labels_[i]) + kLimitTextGap : 0;
    const int left_w = has_left ? lv_obj_get_width(left_labels_[i]) + kLimitTextGap + left2_w : 0;
    const int w = name_w + kLimitTextGap + pct_w + left_w + kLimitPlatePadX * 2;
    const int top = kLimitPlateY;
    int x = lv_obj_get_width(panel_) - pad_x_ + kLimitPlatePadX - w;
    if (i == 0) {
        // 왼쪽 칸은 배지 오른쪽, 시계 칸이 떠 있으면 그 오른쪽.
        x = kBadgePlateXY + kBadgePlatePx + 4;
        if (!lv_obj_has_flag(clock_plate_, LV_OBJ_FLAG_HIDDEN)) {
            x = lv_obj_get_x(clock_plate_) + lv_obj_get_width(clock_plate_) + 4;
        }
    }
    lv_obj_set_pos(limit_plates_[i], x, top);
    lv_obj_set_size(limit_plates_[i], w, kLimitPlateH);
    const int text_x = x + kLimitPlatePadX, text_y = top + kLimitPlatePadY;
    lv_obj_align(name_labels_[i], LV_ALIGN_TOP_LEFT, text_x, text_y);
    lv_obj_align(pct_labels_[i], LV_ALIGN_TOP_LEFT, text_x + name_w + kLimitTextGap, text_y);
    if (has_left) {
        const int left_x = x + w - kLimitPlatePadX - left_w + kLimitTextGap;
        lv_obj_align(left_labels_[i], LV_ALIGN_TOP_LEFT, left_x, text_y);
        if (has_left2) {
            lv_obj_align(left2_labels_[i], LV_ALIGN_TOP_LEFT,
                         left_x + lv_obj_get_width(left_labels_[i]) + kLimitTextGap, text_y);
        }
    }
    const int bar_w = w - kLimitPlatePadX * 2;
    const int fill_w = bar_w * LV_CLAMP(0, limit_bar_pct_[i], 100) / 100;
    lv_obj_set_pos(limit_bar_bg_[i], text_x, top + kLimitBarY);
    lv_obj_set_size(limit_bar_bg_[i], bar_w, kLimitBarH);
    lv_obj_set_pos(limit_bar_fill_[i], text_x, top + kLimitBarY);
    lv_obj_set_size(limit_bar_fill_[i], fill_w, kLimitBarH);
    SetHidden(limit_bar_fill_[i], fill_w == 0);
}

// 시계 칸: 「PM 2:05 (그림) 11°」. 낱말 사이는 4px(빈칸 글자 8px 의 절반)라 라벨을 나눠 놓는다.
void UsageScreen::LayoutClockPlate() {
    const bool show = clock_valid_ && idle_visible_;
    SetHidden(clock_plate_, !show);
    if (!show) return;
    const bool has_icon = weather_icon_ != kUsageWeatherNone;
    SetHidden(clock_icon_, !has_icon);
    SetHidden(clock_temp_, !weather_has_temp_);
    SetHidden(clock_deg_, !weather_has_temp_);
    lv_obj_update_layout(panel_);   // 라벨 폭은 보이는 동안에만 계산된다
    int x = kLimitPlatePadX;
    lv_obj_set_pos(clock_ampm_, x, kClockTextY);
    x += lv_obj_get_width(clock_ampm_) + kLimitTextGap;
    lv_obj_set_pos(clock_time_, x, kClockTextY);
    x += lv_obj_get_width(clock_time_);
    if (has_icon) {
        x += kLimitTextGap;
        lv_obj_set_pos(clock_icon_, x, kClockIconY);
        x += kClockIconPx;
    }
    if (weather_has_temp_) {
        x += kLimitTextGap;
        lv_obj_set_pos(clock_temp_, x, kClockTextY);
        x += lv_obj_get_width(clock_temp_) + 1;
        lv_obj_set_pos(clock_deg_, x, kClockTextY);   // 글자 윗줄에 붙는 ° 자리
        x += kClockDegPx;
    }
    lv_obj_set_pos(clock_plate_, kBadgePlateXY + kBadgePlatePx + 4, kLimitPlateY);
    lv_obj_set_size(clock_plate_, x + kLimitPlatePadX, kLimitPlateH);
    // 왼쪽 한도 칸이 떠 있으면 바뀐 폭에 맞춰 다시 붙인다.
    if (!lv_obj_has_flag(limit_plates_[0], LV_OBJ_FLAG_HIDDEN)) LayoutLimitPlate(0);
}

void UsageScreen::SetClock(int hour, int minute) {
    if (clock_plate_ == nullptr) return;
    if (clock_valid_ && hour == clock_hour_ && minute == clock_minute_) return;   // 10초마다 불린다
    clock_valid_ = true;
    clock_hour_ = hour;
    clock_minute_ = minute;
    // 12시간제, 오전·오후를 앞에, 시는 0 없이: 「PM 2:05」 「AM 12:30」.
    const int h12 = hour % 12 == 0 ? 12 : hour % 12;
    char text[12];
    snprintf(text, sizeof(text), "%d:%02d", h12, minute);
    lv_label_set_text(clock_ampm_, hour < 12 ? "AM" : "PM");
    lv_label_set_text(clock_time_, text);
    LayoutClockPlate();
}

void UsageScreen::SetWeatherInfo(UsageWeatherIcon icon, bool has_temp, int temp) {
    if (clock_plate_ == nullptr) return;
    static const lv_image_dsc_t* const kIcons[kUsageWeatherIconCount] = {
        &weather_icon_clear_img, &weather_icon_partly_img, &weather_icon_cloud_img,
        &weather_icon_rain_img,  &weather_icon_snow_img,   &weather_icon_moon_img,
    };
    weather_icon_ = (icon >= 0 && icon < kUsageWeatherIconCount) ? icon : kUsageWeatherNone;
    if (weather_icon_ != kUsageWeatherNone) lv_image_set_src(clock_icon_, kIcons[weather_icon_]);
    weather_has_temp_ = has_temp;
    if (has_temp) {
        char text[12];
        snprintf(text, sizeof(text), "%d", LV_CLAMP(-99, temp, 999));
        lv_label_set_text(clock_temp_, text);
    }
    LayoutClockPlate();
}

bool UsageIsNightNow() {
    const time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year < 2024 - 1900) return false;   // SNTP 전
    return tm_now.tm_hour >= 23 || tm_now.tm_hour < 7;
}

UsageSky UsageVillageSkyForClock() {
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year < 2025 - 1900) {
        return kUsageSkyNavy;
    }
    const bool day = tm_now.tm_hour >= kUsageDayFromH && tm_now.tm_hour < kUsageDayUntilH;
    return day ? kUsageSkyDay : kUsageSkyNavy;
}

void UsageScreen::SetSky(UsageSky sky) {
    if (sky >= kUsageSkyCount) return;
    const bool changed = sky != sky_;
    const bool was_scene = IsBboketerScene();
    sky_ = sky;
    lv_image_set_src(bg_image_, sky_canvas_.Render(special_bg_ != nullptr ? special_bg_ : SkyBackground(sky)));
    lv_obj_invalidate(bg_image_);   // src 포인터가 같아도 내용이 바뀌었다
    if (changed) {
        // 같은 하늘이면 파티클·스프라이트를 안 건드린다 — 옷장을 변경 없이
        // 닫기만 해도 비가 끊기고 물고기가 화면 밖으로 순간이동하던 문제.
        BuildSceneSprites();
        BuildParticles();
        RefreshSeat();   // 자리 소품·몸 높이를 새 배경 것으로
        ApplyScenePlace();
        if (CharacterHasScenes() && (was_scene || IsBboketerScene()) &&
            state_ == kUsageStateIdle && !idle_paused_ &&
            !lv_obj_has_flag(bg_image_, LV_OBJ_FLAG_HIDDEN)) {
            StopWalk();
            StartWalk();
        }
    }
}

// --- 장면 스프라이트: 배경의 움직이는 부분 ------------------------------------

void UsageScreen::SetSpecialBg(const lv_image_dsc_t* image) {
    if (image == special_bg_) return;
    special_bg_ = image;
    lv_image_set_src(bg_image_, sky_canvas_.Render(special_bg_ != nullptr ? special_bg_ : SkyBackground(sky_)));
    lv_obj_invalidate(bg_image_);
    BuildSceneSprites();   // 장면의 움직이는 조각은 기념일 배경과 안 맞는다
    BuildParticles();      // 기념일 배경이면 실내 장면이어도 날씨가 내린다
    ApplyScenePlace();
    if (IsBboketerScene() && state_ == kUsageStateIdle && !idle_paused_) {
        ShowPackFrame(figure_.get(), walk_anim(), walk_leg_frame_);
    }
}

void UsageScreen::BuildSceneSprites() {
    for (int i = 0; i < sprite_count_; i++) {
        lv_obj_delete(sprite_objs_[i]);
        sprite_objs_[i] = nullptr;
    }
    sprite_count_ = 0;
    if (sprite_timer_ != nullptr) {
        lv_timer_delete(sprite_timer_);
        sprite_timer_ = nullptr;
    }
    if (special_bg_ != nullptr) {
        return;
    }
    const bool visible = !lv_obj_has_flag(bg_image_, LV_OBJ_FLAG_HIDDEN);
    for (uint8_t i = 0; i < kBboketerSceneSpriteCount && sprite_count_ < kMaxSceneSprites; i++) {
        const SceneSprite& s = kBboketerSceneSprites[i];
        if (s.sky != static_cast<uint8_t>(sky_)) continue;
        lv_obj_t* img = lv_image_create(panel_);
        lv_image_set_src(img, s.frames[0]);
        lv_obj_set_pos(img, s.x, s.y);
        lv_obj_move_to_index(img, 1);
        SetHidden(img, !visible);
        sprite_specs_[sprite_count_] = &s;
        sprite_frame_[sprite_count_] = 0;
        sprite_acc_[sprite_count_] = 0;
        sprite_objs_[sprite_count_++] = img;
    }
    if (sprite_count_ > 0) {
        sprite_timer_ = lv_timer_create(OnSceneSpriteTick, 50, this);
    }
}

void UsageScreen::OnSceneSpriteTick(lv_timer_t* timer) {
    auto* self = static_cast<UsageScreen*>(lv_timer_get_user_data(timer));
    if (self == nullptr) return;
    for (int i = 0; i < self->sprite_count_; i++) {
        const SceneSprite& s = *self->sprite_specs_[i];
        self->sprite_acc_[i] += 50;
        if (self->sprite_acc_[i] < s.period_ms) continue;
        self->sprite_acc_[i] = 0;
        self->sprite_frame_[i] = (self->sprite_frame_[i] + 1) % s.frame_count;
        lv_image_set_src(self->sprite_objs_[i], s.frames[self->sprite_frame_[i]]);
    }
}

// --- 파티클: 비/눈(호스트 날씨) + 벚꽃잎(sakura 상시) -------------------------

void UsageScreen::SetWeatherFx(uint8_t fx) {
    if (fx == weather_fx_) return;
    weather_fx_ = fx;
    ESP_LOGI(kTag, "weather fx -> %u%s", fx, wx_mode_ != 0 ? " (wx override, ignored)" : "");
    if (wx_mode_ == 0) BuildParticles();
}

void UsageScreen::SetWeatherMode(uint8_t wx) {
    if (wx == wx_mode_) return;
    wx_mode_ = wx;
    ESP_LOGI(kTag, "weather mode -> %u", wx);
    BuildParticles();
}

// 실외 장면에서만 날씨가 내린다. 새 장면은 실내로 치니, 실외 장면을 넣으면 아래 목록에 더한다.
// 기념일 배경은 모두 실외다. 장면 앞 옛 하늘 칸은 이 구분이 없다.
bool UsageScreen::WeatherShownHere() const {
    if (special_bg_ != nullptr || sky_ < kUsageSkyFirstScene) return true;
    switch (sky_) {
        case kUsageSkyDolopath: case kUsageSkyCappadocia: case kUsageSkySeoul: case kUsageSkyCamping:
        case kUsageSkyBeach: case kUsageSkyJeju: case kUsageSkyBlossom: case kUsageSkyBallpark:
        case kUsageSkyBirth: case kUsageSkyPepero: case kUsageSkyXmas:
            return true;
        default:
            return false;
    }
}

void UsageScreen::BuildParticles() {
    for (int i = 0; i < drop_count_; i++) {
        lv_obj_delete(drops_[i]);
        drops_[i] = nullptr;
    }
    drop_count_ = 0;
    if (drop_timer_ != nullptr) {
        lv_timer_delete(drop_timer_);
        drop_timer_ = nullptr;
    }
    const bool visible = !lv_obj_has_flag(bg_image_, LV_OBJ_FLAG_HIDDEN);
    // WX 선택(옷장)이 auto 가 아니면 push 날씨 대신 그 연출만. clear 는 꽃잎도 끈다.
    uint8_t fx = weather_fx_;                       // auto: push 그대로 (0/비/눈)
    switch (wx_mode_) {
        case 1: fx = 0; break;                      // clear
        case 2: fx = kDropRain; break;
        case 3: fx = kDropSnow; break;
        case 4: fx = kDropLeaf; break;
        case 5: fx = kDropStar; break;
        case 6: fx = kDropPetal; break;
        default: break;
    }
    if (!WeatherShownHere()) fx = 0;                // 실내·우주·바닷속에는 날씨가 없다
    const int rain = (fx == kDropRain) ? 16 : 0;
    const int snow = (fx == kDropSnow) ? 14 : 0;
    const int leaf = (fx == kDropLeaf) ? 9 : 0;
    const int star = (fx == kDropStar) ? 2 : 0;
    // 꽃잎은 sakura 의 정체성이라 WX 를 강제해도 항상 날린다 — 비/눈 파티클과 겹쳐 내린다.
    const int petal = (sky_ == kUsageSkySakura || fx == kDropPetal) ? 12 : 0;
    struct { int n; uint8_t kind; } groups[] = {{rain, kDropRain}, {snow, kDropSnow},
                                                {petal, kDropPetal}, {leaf, kDropLeaf}, {star, kDropStar}};
    for (const auto& g : groups) {
        for (int i = 0; i < g.n && drop_count_ < kMaxDrops; i++) {
            lv_obj_t* o;
            int speed = 0;
            if (g.kind == kDropStar) {
                o = lv_obj_create(panel_);
                lv_obj_set_style_border_width(o, 0, 0);
                lv_obj_set_style_radius(o, 0, 0);
                lv_obj_set_style_pad_all(o, 0, 0);
                lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
                lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
                lv_obj_set_size(o, 9, 2);                 // 긴 꼬리
                lv_obj_set_style_bg_color(o, lv_color_white(), 0);
                speed = -static_cast<int>(lv_rand(5, 40));   // 음수 = 재등장 대기 틱
            } else {
                // 펠트 그림(tools/gen_felt_art.py). 크기·색이 섞이게 번갈아 쓴다.
                const lv_image_dsc_t* art = &ui_fx_petal_img;
                speed = 2 + (i % 3);
                if (g.kind == kDropRain) {
                    art = &ui_fx_rain_img;
                    speed = 7 + static_cast<int>(lv_rand(0, 3));
                } else if (g.kind == kDropSnow) {
                    art = i % 3 == 0 ? &ui_fx_flake_img : &ui_fx_ball_img;
                    speed = 2 + (i % 2);
                } else if (g.kind == kDropLeaf) {
                    art = i % 2 == 0 ? &ui_fx_leaf_red_img : &ui_fx_leaf_gold_img;
                }
                o = lv_image_create(panel_);
                lv_image_set_src(o, art);
            }
            lv_obj_remove_flag(o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
            lv_obj_move_to_index(o, lv_obj_get_index(bg_image_) + 1);   // 배경 바로 위, 캐릭터·글자 뒤
            drop_x_[drop_count_] = static_cast<int16_t>(lv_rand(0, width_ - 1));
            drop_y_[drop_count_] = static_cast<int16_t>(lv_rand(0, height_ - 1));
            lv_obj_set_pos(o, drop_x_[drop_count_], drop_y_[drop_count_]);
            SetHidden(o, !visible || g.kind == kDropStar);   // 별똥별은 대기에서 시작
            drop_kind_[drop_count_] = g.kind;
            drop_speed_[drop_count_] = static_cast<int16_t>(speed);
            drops_[drop_count_++] = o;
        }
    }
    if (drop_count_ > 0) {
        drop_timer_ = lv_timer_create(OnDropTick, kDropTickMs, this);
        if (idle_paused_) {
            lv_timer_pause(drop_timer_);   // 옷장·꺼진 화면 중 재구축돼도 멈춘 채로
        }
    }
}

void UsageScreen::OnDropTick(lv_timer_t* timer) {
    auto* self = static_cast<UsageScreen*>(lv_timer_get_user_data(timer));
    if (self == nullptr) return;
    for (int i = 0; i < self->drop_count_; i++) {
        lv_obj_t* o = self->drops_[i];
        if (self->drop_kind_[i] == kDropStar) {
            // 별똥별: 대각선 활강, 화면을 벗어나면 잠시 숨었다가 위쪽에서 재등장.
            if (self->drop_speed_[i] < 0) {
                if (++self->drop_speed_[i] == 0) {
                    if (lv_obj_has_flag(self->bg_image_, LV_OBJ_FLAG_HIDDEN)) {
                        self->drop_speed_[i] = -1;   // 대기 화면이 아니면 계속 대기
                        continue;
                    }
                    self->drop_x_[i] = static_cast<int16_t>(lv_rand(0, self->width_ * 2 / 3));
                    self->drop_y_[i] = static_cast<int16_t>(lv_rand(0, self->height_ / 3));
                    lv_obj_set_pos(o, self->drop_x_[i], self->drop_y_[i]);
                    self->drop_speed_[i] = 1;
                    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
                }
                continue;
            }
            const int x = self->drop_x_[i] + 7, y = self->drop_y_[i] + 3;
            if (x > self->width_ || y > self->height_) {
                lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
                self->drop_speed_[i] = -static_cast<int>(lv_rand(15, 60));
                continue;
            }
            self->drop_x_[i] = static_cast<int16_t>(x);
            self->drop_y_[i] = static_cast<int16_t>(y);
            lv_obj_set_pos(o, x, y);
            continue;
        }
        int y = self->drop_y_[i] + self->drop_speed_[i];
        int x = self->drop_x_[i];
        if (self->drop_kind_[i] == kDropLeaf) {
            x += static_cast<int>(lv_rand(0, 4)) - 2;   // 낙엽은 크게 흔들린다
        } else if (self->drop_kind_[i] != kDropRain) {
            x += static_cast<int>(lv_rand(0, 2)) - 1;   // 눈·꽃잎은 좌우로 하늘하늘
        }
        if (y > self->height_) {
            // 가장 긴 빗줄기(11px)도 화면 위에서 자연스럽게 진입. 재진입 깊이에
            // 랜덤을 섞어(위상 재추첨) 같은 속도끼리 줄 맞춰 떨어지지 않게 한다.
            y = -12 - static_cast<int>(lv_rand(0, 36));
            x = static_cast<int>(lv_rand(0, self->width_ - 1));
        }
        self->drop_x_[i] = static_cast<int16_t>(x);
        self->drop_y_[i] = static_cast<int16_t>(y);
        lv_obj_set_pos(o, x, y);
    }
}

// --- 캐릭터 런타임 교체 (docs/character-pack.md "런타임 선택") -----------------

// 걷기 좌표는 늘 **스케일 전** 단위다(Figure::SetBodyPos 규약). 팩 배율
// (pack.json scale)이 걸리면 발 밑변이 길 아랫선에 오게 y 를 잡고 좌우 범위를 배율로 나눈다.
void UsageScreen::ApplyCharacterGeometry() {
    walk_scale_ = ActivePack().scale256;
    walk_x_min_ = pad_x_ * LV_SCALE_NONE / walk_scale_;
    walk_x_max_ = (width_ - pad_x_) * LV_SCALE_NONE / walk_scale_ - kWalkW;
    walk_y_ = kWalkFootY * height_ / kRefH * LV_SCALE_NONE / walk_scale_ - kWalkH;
    walk_y_ground_ = walk_y_;
    if (seat_img_ != nullptr) {
        lv_image_set_scale(seat_img_, walk_scale_);
    }
}

bool UsageScreen::SetCharacter(Character c) {
    if (!CharacterAvailable(c)) {
        ESP_LOGW(kTag, "character %d is not in this build", static_cast<int>(c));
        return false;
    }
    if (c == CurrentCharacter()) return true;

    // 상태 화면이었으면 그 상태로 되돌려 놓는다. ctx 는 라벨에만 살아 있어 지금 베낀다.
    const UsageState prev_state = state_;
    char prev_ctx[64] = {};
    if (prev_state != kUsageStateIdle && ctx_label_ != nullptr) {
        const char* t = lv_label_get_text(ctx_label_);
        if (t != nullptr) snprintf(prev_ctx, sizeof(prev_ctx), "%s", t);
    }

    // 1) 지금 캐릭터를 화면에서 내린다 — 상태 화면이면 대기로(팩 애니 해제), 미니카·반응도 끊는다.
    if (prev_state != kUsageStateIdle) SetState(kUsageStateIdle, nullptr);
    StopWalk();
    StopPackState();

    // 2) 캐릭터를 바꾸고 좌표계를 새로 잡는다.
    SetCurrentCharacter(c);
    pack_set_ = ActivePack().default_set;   // 세트 번호는 팩마다 다르다
    ApplyCharacterGeometry();

    // 3) 두 몸을 다시 만든다 — Figure 는 **생성 시점에** 배율이 굳는다.
    //    z 순서는 만든 순서라 옛 자리를 기억해 되돌린다(안 하면 새 몸이 라벨·옷걸이 위로
    //    올라온다). 지우면 뒤 형제 index 가 하나 당겨지고 새로 꽂으면 다시 밀리므로,
    //    몸 하나씩 (index 기억 → 파기 → 생성 → 이동) 순서로 해야 맞는다.
    const int32_t z_walk = lv_obj_get_index(figure_->obj());
    figure_.reset();
    figure_ = std::make_unique<Figure>(panel_, walk_scale_);
    lv_obj_move_to_index(figure_->obj(), z_walk);
    figure_->SetBodyPos(walk_x_min_, walk_y_);
    figure_->SetFacing(walk_right_);
    figure_->SetHidden(true);

    const int state_scale = ActivePack().state_scale256;
    const int32_t z_state = lv_obj_get_index(state_figure_->obj());
    state_figure_.reset();
    state_figure_ = std::make_unique<Figure>(panel_, state_scale);
    lv_obj_move_to_index(state_figure_->obj(), z_state);
    state_figure_->SetHidden(true);

    ApplyScenePlace();

    // 4) 옛 캐릭터의 재생 상태를 지운다. 팩 세트는 기본값으로 — NVS 복원은 호출자 몫이다.
    pack_react_anim_ = nullptr;
    pack_state_anim_ = nullptr;
    pack_state_frame_ = 0;
    pack_react_frame_ = 0;
    walk_leg_frame_ = 0;

    ESP_LOGI(kTag, "character -> %s (walk scale %d, state scale %d)", CharacterKey(c), walk_scale_,
             state_scale);

    // 5) 다시 그린다. 옷은 호출자가 그 캐릭터 것으로 입힌다(아이템 표가 다르다).
    if (prev_state == kUsageStateIdle) {
        ShowIdle(true);
    } else {
        SetState(prev_state, prev_ctx[0] != '\0' ? prev_ctx : nullptr);
    }
    return true;
}

// --- 대기 화면: 캐릭터 ----------------------------------------------------------

namespace {
constexpr int kFrameLeft = 62;   // 가운데 선 2배 프레임의 왼쪽(화면 px)
constexpr uint32_t kSceneBobMs = 800;   // 두둥실 박자
constexpr int kSceneBobPx = 2;          // 오르내리는 높이(화면 px)
}  // namespace

int UsageScreen::SceneDx() const {
    if (IsBboketerScene()) {
        // 만복이는 혼자면 가운데, 로나는 로찌 자리 때문에 늘 왼쪽이다. 손님이 오면 로나·만복이·로찌 순서로 앉는다.
        const int scene = BodyScene();
        const BboketerGuestPlace guest = BboketerSceneGuestPlace(scene);
        const BboketerPlace place = BboketerScenePlace(scene);
        int dx = CharacterBorrowsScenes() ? place.manbok_x - kFrameLeft : place.dx;
        if (TripHasGuest()) dx = (CharacterBorrowsScenes() ? guest.manbok : guest.rona) - kFrameLeft;
        return dx * LV_SCALE_NONE / walk_scale_;
    }
    return 0;
}

int UsageScreen::SceneWalkY() const {
    // 프레임 발 밑변은 y=62(apply_soft_sheets.py BOTTOM=2), 몸 박스 밑변이 프레임 밑변이다.
    if (IsBboketerScene()) {
        return (BboketerScenePlace(BodyScene()).foot + 4 + SceneHostDy()) * LV_SCALE_NONE / walk_scale_ - kWalkH;
    }
    return walk_y_ground_;
}

int UsageScreen::SceneHostDy() const {
    const int scene = BodyScene();
    if (TripHasGuest()) {
        const BboketerGuestPlace guest = BboketerSceneGuestPlace(scene);
        return CharacterBorrowsScenes() ? guest.manbok_dy : guest.rona_dy;
    }
    return CharacterBorrowsScenes() ? BboketerScenePlace(scene).manbok_dy : 0;
}

namespace {
// 라인아트(1배) 그림을 아래 가운데 (cx, bottom) 에 놓는다.
void PlaceLineart(lv_obj_t* img, const lv_image_dsc_t* dsc, int cx, int bottom) {
    lv_image_set_src(img, dsc);
    lv_image_set_pivot(img, 0, 0);
    lv_image_set_scale(img, LV_SCALE_NONE);
    lv_image_set_antialias(img, true);
    lv_obj_set_pos(img, cx - static_cast<int>(dsc->header.w) / 2, bottom - static_cast<int>(dsc->header.h));
}
}  // namespace

// 같이 노는 소품과 앞 탁자. 배경이 보일 때만(상태 화면은 검은 바탕이라 숨긴다).
void UsageScreen::ApplySceneProps() {
    if (prop_img_ == nullptr || front_img_ == nullptr) return;
    BboketerProp prop{};
    if (IsBboketerScene() && !lv_obj_has_flag(bg_image_, LV_OBJ_FLAG_HIDDEN)) {
        prop = BboketerCastProp(BodyScene(), CastDrawMask());
    }
    for (int i = 0; i < 2; ++i) {
        lv_obj_t* o = i == 0 ? prop_img_ : front_img_;
        const bool show = prop.image != nullptr && prop.front == (i == 1);
        if (show) {
            lv_image_set_src(o, prop.image);
            lv_obj_set_pos(o, prop.x, prop.y);
        }
        SetHidden(o, !show);
    }
    RaiseFrontProp();
}

void UsageScreen::RaiseFrontProp() {
    if (front_img_ != nullptr) {
        // 앞 탁자는 몸·로찌·손님보다 위, 글자 띠·버튼보다 아래다. 손님 그림은 늦게(패널 맨 끝에) 만들어지므로
        // 앞 탁자를 올리지 않고 손님 그림을 앞 탁자 아래로 내린다(올리면 띠·버튼까지 덮는다 — 리뷰 11/7).
        for (lv_obj_t* o : {figure_->obj(), rozzi_img_, trip_img_, trip_rozzi_img_}) {
            if (o == nullptr) continue;
            const int front = static_cast<int>(lv_obj_get_index(front_img_));
            if (static_cast<int>(lv_obj_get_index(o)) > front) lv_obj_move_to_index(o, front);
        }
    }
}

void UsageScreen::ApplyScenePlace() {
    if (rozzi_img_ == nullptr) return;
    ApplySceneProps();
    if (!IsBboketerScene()) {
        SetHidden(rozzi_img_, true);
        return;
    }
    BboketerSetVisit(TripHasGuest());
    BboketerSetRozziItem(figure_->outfit().hat);
    walk_y_ = SceneWalkY();
    SetHidden(rozzi_img_, true);   // 장면 캐릭터는 usage_cast.cc 가 그린다
    ApplyCast();
    if (scene_bob_timer_ == nullptr) {
        scene_bob_timer_ = lv_timer_create(OnSceneBob, kSceneBobMs, this);
    }
    return;
    const int scene = BodyScene();
    const BboketerPlace place = BboketerScenePlace(scene);
    // 로찌는 로나와 같이 움직인다. 로나가 놀러 나간 동안에는 집에도 없다.
    const lv_image_dsc_t* image = CharacterBorrowsScenes() || TripBodyGone() || state_ != kUsageStateIdle
                                      ? nullptr
                                      : BboketerRozziImage(scene);
    if (image == nullptr) {
        SetHidden(rozzi_img_, true);
        return;
    }
    const BboketerGuestPlace guest = BboketerSceneGuestPlace(scene);
    const int rozzi_x = TripHasGuest() ? guest.rozzi : place.rozzi_x;
    const int dy = TripHasGuest() ? guest.rozzi_dy : place.rozzi_dy;
    if (IsLineart(image)) {
        PlaceLineart(rozzi_img_, image, rozzi_x + 64, place.foot + 4 + dy);
    } else {
        PlaceScaledImage(rozzi_img_, image, rozzi_x + 64, place.foot - 60 + dy, 2);
    }
    rozzi_y_ = lv_obj_get_y(rozzi_img_);
    SetHidden(rozzi_img_, false);
    if (scene_bob_timer_ == nullptr) {
        scene_bob_timer_ = lv_timer_create(OnSceneBob, kSceneBobMs, this);
    }
}

void UsageScreen::OnSceneBob(lv_timer_t* timer) {
    auto* self = static_cast<UsageScreen*>(lv_timer_get_user_data(timer));
    if (self != nullptr) self->SceneBobTick();
}

// 집주인과 로찌는 번갈아 오르내린다. 손님은 DrawTrip 이 같은 박자를 읽어 집주인과 반대로 움직인다.
void UsageScreen::SceneBobTick() {
    scene_bob_up_ = !scene_bob_up_;
    if (!IsBboketerScene()) return;
    if (CastActive()) {
        ApplyCast();
        return;
    }
    if (!lv_obj_has_flag(rozzi_img_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_y(rozzi_img_, rozzi_y_ - (scene_bob_up_ ? 0 : kSceneBobPx));
    }
    const bool idle_body = state_ == kUsageStateIdle && pack_react_anim_ == nullptr &&
                           !lv_obj_has_flag(figure_->obj(), LV_OBJ_FLAG_HIDDEN);
    if (idle_body) {
        figure_->SetBodyPos(figure_->body_x(), walk_y_ - (scene_bob_up_ ? kSceneBobPx * LV_SCALE_NONE / walk_scale_ : 0));
    }
}

void UsageScreen::StartWalk() {
    ApplyScenePlace();
    if (IsBboketerScene()) {
        StartWalkFrom(walk_x_center(), true);
        return;
    }
    if (pack_has_ride()) {   // 미니카 팩은 정중앙에 앉아 있다 (사용자 확정 09-06)
        StartWalkFrom(walk_x_center(), true);
        return;
    }
    StartWalkFrom(walk_right_ ? walk_x_min_ : walk_x_max_, true);
}

// from_x 에서 지금 방향의 끝까지 걷는다. 끝까지 남은 거리에 비례한 시간을 줘서
// 터치 반응 뒤 멈춘 자리에서 이어 걸어도 속도가 같다.
void UsageScreen::StartWalkFrom(int from_x, bool rest_first) {
    if (CastActive()) {
        // 장면에서는 몸을 쓰지 않는다. SetStats 가 매번 부르므로 달라진 것만 그린다(CastPut).
        figure_->SetHidden(true);
        if (!cast_visible_) {
            cast_visible_ = true;
            ApplyScenePlace();
        }
        TripOnIdleResume();
        return;
    }
    if (walk_leg_timer_ != nullptr) return;  // 걷는 중 — SetStats 가 매번 부르므로 재시작 금지
    if (reaction_ != Reaction::kNone) return;  // 반응 중 — 타이머가 끝나면 EndReaction 이 부른다
    if (TripBodyGone()) {                      // 내 캐릭터가 놀러 나갔다 — 몸을 다시 꺼내지 않는다
        TripOnIdleResume();
        return;
    }

    walk_leg_frame_ = 0;
    uint32_t leg_ms = kWalkLegMs;
    // 팩 애니를 manifest fps 로 돌린다. ride 가 있는 팩은 평소 제자리(idle)에 앉아 있고
    // 미니카 예약이 차면 StartPackRide 가 pack_riding_ 을 세우고 여기로 다시 와 x 를 옮긴다.
    const PackAnim& idle_anim = walk_anim();
    ShowPackFrame(figure_.get(), idle_anim, 0);
    walk_leg_timer_ = lv_timer_create(OnWalkLegFrame, idle_anim.frame_ms, this);
    if (IsBboketerScene()) {
        figure_->SetBodyPos(walk_x_center(), walk_y_);
        figure_->SetHidden(false);
        return;   // 장면별 자세는 제자리에서 재생한다. 화면 위를 미끄러지지 않는다.
    }
    if (pack_has_ride() && !pack_riding_) {
        ApplySeat(from_x, true);   // 제자리 = 자리에 앉는다(walk_y_ 를 raise 만큼 올린 뒤 몸을 둔다)
        figure_->SetBodyPos(from_x, walk_y_ - (sky_ == kUsageSkySpace ? kSpaceFloatBase : 0));
        figure_->SetHidden(false);
        ArmPackRide();
        return;
    }
    if (pack_riding_) leg_ms = kPackRideMs;

    int to = walk_right_ ? walk_x_max_ : walk_x_min_;
    if (pack_riding_ && pack_ride_leg_ == 1) to = walk_x_center();   // 돌아오는 길은 가운데까지
    const int span = walk_x_max_ - walk_x_min_;
    const int left = (to > from_x) ? (to - from_x) : (from_x - to);
    const uint32_t duration = span > 0 ? leg_ms * static_cast<uint32_t>(left) / span : leg_ms;
    ApplySeat(from_x, false);   // 달리는 동안은 바닥 높이, 자리 숨김
    figure_->SetBodyPos(from_x, walk_y_ - (sky_ == kUsageSkySpace ? kSpaceFloatBase : 0));
    figure_->SetHidden(false);

    // anim 값은 몸 x 이고, 옮기는 것은 컨테이너(왼쪽 여유 kPadX 만큼 앞에 있다).
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, figure_->obj());
    lv_anim_set_values(&a, from_x, to);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_delay(&a, rest_first ? kWalkRestMs : 0);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
        // 걷기 anim 값은 스케일 전 몸 x — 팩 배율이 걸려 있으면 화면 px 로 올린다.
        const int s = ActivePack().scale256;
        lv_obj_set_x(static_cast<lv_obj_t*>(obj), (v - Figure::kPadX) * s / LV_SCALE_NONE);
    });
    lv_anim_set_completed_cb(&a, OnWalkLegDone);
    lv_anim_set_user_data(&a, this);
    lv_anim_start(&a);
}

void UsageScreen::StopLegTimer() {
    if (walk_leg_timer_ != nullptr) {
        lv_timer_delete(walk_leg_timer_);
        walk_leg_timer_ = nullptr;
    }
    StopPackRideTimer();   // 걷기가 끊기면 미니카 예약·진행도 같이 끝 — 다음 StartWalkFrom 이 제자리부터
    pack_riding_ = false;
    pack_ride_leg_ = 0;
    if (ride_return_sky_ != kUsageSkyCount) {
        // 미니카 외출 끝(도착·반응·상태 전환·옷장 어느 쪽이든) — 실내로 돌아온다.
        const UsageSky back = ride_return_sky_;
        ride_return_sky_ = kUsageSkyCount;
        ESP_LOGI(kTag, "pack ride back: sky %d -> %d", static_cast<int>(sky_), static_cast<int>(back));
        SetSky(back);
    }
}

void UsageScreen::StopPackRideTimer() {
    if (pack_ride_timer_ != nullptr) {
        lv_timer_delete(pack_ride_timer_);
        pack_ride_timer_ = nullptr;
    }
}

void UsageScreen::ArmPackRide() {
    StopPackRideTimer();
    const uint32_t ms = kPackRideMinMs + lv_rand(0, kPackRideSpreadMs);
    pack_ride_timer_ = lv_timer_create(OnPackRideDue, ms, this);
    lv_timer_set_repeat_count(pack_ride_timer_, 1);   // 한 번 울리고 LVGL 이 지운다
}

void UsageScreen::OnPackRideDue(lv_timer_t* timer) {
    auto* self = static_cast<UsageScreen*>(lv_timer_get_user_data(timer));
    if (self == nullptr) return;
    self->pack_ride_timer_ = nullptr;   // repeat 1 — 콜백 뒤 LVGL 이 지우므로 우리는 손대지 않는다
    self->StartPackRide();
}

// 가운데서 한쪽 끝까지 달렸다가 돌아서서 가운데로(왕복 kPackRideMs). 방향은 지난번과 반대(끝에서 한 번 뒤집히므로 저절로).
void UsageScreen::StartPackRide() {
    StopPackRideTimer();
    if (state_ != kUsageStateIdle || reaction_ != Reaction::kNone || !pack_has_ride() || pack_riding_) return;
    if (walk_leg_timer_ == nullptr) return;   // 제자리 대기 중이 아니면(옷장 뒤·꺼진 화면) 안 탄다
    const int x = figure_->body_x();
    lv_anim_delete(figure_->obj(), nullptr);
    StopLegTimer();
    if (x >= walk_x_max_) walk_right_ = false;
    else if (x <= walk_x_min_) walk_right_ = true;
    figure_->SetFacing(walk_right_);
    pack_riding_ = true;
    pack_ride_leg_ = 0;
    ESP_LOGI(kTag, "pack ride %s from x=%d", walk_right_ ? "right" : "left", x);
    // 외출: 실내(방·카페·도서관)에서 출발하면 시각에 맞는 마을로 나간다. StopLegTimer 가 되돌린다.
    if (sky_ == kUsageSkyRoom || sky_ == kUsageSkyCafe || sky_ == kUsageSkyLibrary) {
        ride_return_sky_ = sky_;
        const UsageSky out = UsageVillageSkyForClock();
        ESP_LOGI(kTag, "pack ride out: sky %d -> %d", static_cast<int>(sky_), static_cast<int>(out));
        SetSky(out);
    }
    StartWalkFrom(x, false);
}

// --- 자리 소품 (docs/character-pack.md "seats") ---------------------------------

const PackSeat* UsageScreen::seat() const {
    // 자리 표는 기본 배경 13칸뿐이다(gen_pack SKY_ORDER) — 장면 하늘은 표 밖이다.
    if (ActivePack().seats == nullptr || !pack_has_ride() || pack_riding_ ||
        state_ != kUsageStateIdle || sky_ > kUsageSkySite) {
        return nullptr;
    }
    const PackSeat& st = ActivePack().seats[sky_];
    return st.img != nullptr ? &st : nullptr;
}

// walk_y_ 를 자리 높이만큼 올리고(없으면 바닥) seat_img_ 를 몸 x 기준 프레임 좌표에 둔다.
// 좌표는 스케일 전 단위 → 화면 px 는 × scale256/256 (Figure::SetBodyPos 와 같은 식).
void UsageScreen::ApplySeat(int body_x, bool show) {
    const PackSeat* st = seat();
    walk_y_ = SceneWalkY() - (st != nullptr ? st->raise : 0);
    if (st == nullptr || !show) {
        SetHidden(seat_img_, true);
        return;
    }
    const int s = ActivePack().scale256;
    lv_image_set_src(seat_img_, st->img);
    lv_obj_set_pos(seat_img_, (body_x + st->dx) * s / LV_SCALE_NONE, (walk_y_ground_ + st->dy) * s / LV_SCALE_NONE);
    SetHidden(seat_img_, false);
}

void UsageScreen::RefreshSeat() {
    if (seat_img_ == nullptr) return;
    // 앉아 있는 중(제자리 idle·반응 아님·보이는 중)일 때만 자리·높이를 옮긴다. 달리는 중이면 seat() 가 없어 바닥.
    const bool seated = state_ == kUsageStateIdle && !pack_riding_ && reaction_ == Reaction::kNone &&
                        walk_leg_timer_ != nullptr &&
                        !lv_obj_has_flag(figure_->obj(), LV_OBJ_FLAG_HIDDEN);
    const int x = figure_->body_x();
    ApplySeat(x, seated);
    if (seated) {
        figure_->SetBodyPos(x, walk_y_ - (sky_ == kUsageSkySpace ? kSpaceFloatBase : 0));
        const PackSeat* st = seat();
        ESP_LOGI(kTag, "seat: sky=%d raise=%u", static_cast<int>(sky_), st != nullptr ? st->raise : 0);
    }
}

bool UsageScreen::TriggerPackRide() {
    if (state_ != kUsageStateIdle || idle_paused_ || !pack_has_ride() || pack_riding_ ||
        reaction_ != Reaction::kNone || walk_leg_timer_ == nullptr) {
        ESP_LOGW(kTag, "ride rejected (state=%d paused=%d riding=%d)", state_, idle_paused_, pack_riding_);
        return false;
    }
    StartPackRide();
    return pack_riding_;
}

// 우주 하늘의 무중력 두둥실 — 발이 안 닿는 게 의도라는 걸 사인 부유로 보여 준다.
int UsageScreen::WalkLift() {
    walk_float_t_++;
    return kSpaceFloatBase + static_cast<int>(lroundf(kSpaceFloatAmp * sinf(walk_float_t_ / 5.0f)));
}

// 팩 idle 프레임 순환. 스쿼시가 프레임에 들어 있어 통통 bob 은 안 준다 — 우주 부유만.
void UsageScreen::OnWalkLegFrame(lv_timer_t* timer) {
    auto* self = static_cast<UsageScreen*>(lv_timer_get_user_data(timer));
    if (self == nullptr) return;
    const PackAnim& idle_anim = self->walk_anim();
    if (idle_anim.count == 0) return;
    self->walk_leg_frame_ = static_cast<uint8_t>((self->walk_leg_frame_ + 1) % idle_anim.count);
    self->ShowPackFrame(self->figure_.get(), idle_anim, self->walk_leg_frame_);
    const int lift = (self->sky_ == kUsageSkySpace) ? self->WalkLift() : 0;
    self->figure_->SetBodyPos(self->figure_->body_x(), self->walk_y_ - lift);
}


void UsageScreen::StopWalk() {
    TripOnStopWalk();                     // 놀러가기: 손님은 떠나고, 나간 내 캐릭터는 계속 나가 있다
    CastReactStop();
    CastHide();
    lv_anim_delete(figure_->obj(), nullptr);
    StopLegTimer();
    // 진행 중인 터치 반응도 끊는다 — 상태 화면이 이기고, 화면을 끄면 안 보인다.
    if (reaction_timer_ != nullptr) {
        lv_timer_delete(reaction_timer_);
        reaction_timer_ = nullptr;
    }
    reaction_ = Reaction::kNone;
    pack_react_anim_ = nullptr;
    figure_->SetBodySrc(nullptr);
    figure_->SetHidden(true);
    SetHidden(seat_img_, true);
}

// --- 대기 화면: 터치 반응 ----------------------------------------------------

bool UsageScreen::HitBody(int x, int y) const {
    if (lv_obj_has_flag(figure_->obj(), LV_OBJ_FLAG_HIDDEN)) return false;
    // 팩 배율: 터치(화면 px)를 스케일 전 좌표로 내려서 같은 식에 넣는다.
    x = x * LV_SCALE_NONE / ActivePack().scale256;
    y = y * LV_SCALE_NONE / ActivePack().scale256;
    const int bx = figure_->body_x();
    return x >= bx - kBodyHitPad && x < bx + kWalkW + kBodyHitPad &&
           y >= walk_y_ - kBodyHitPad && y < walk_y_ + kWalkH + kBodyHitPad;
}

void UsageScreen::EndReaction() {
    if (reaction_timer_ != nullptr) {
        lv_timer_delete(reaction_timer_);
        reaction_timer_ = nullptr;
    }
    reaction_ = Reaction::kNone;
    pack_react_anim_ = nullptr;
    const int x = figure_->body_x();
    figure_->SetBodySrc(nullptr);
    ESP_LOGI(kTag, "reaction over, resume walk from x=%d", x);
    if (state_ == kUsageStateIdle) {
        StartWalkFrom(x, false);
    }
}

// 팩 반응: 걷기를 멈춘 자리에서 anim 을 ms 동안 돌리고 OnReactionTick 이 걷기로 되돌린다.
// 이미 반응 중이면 애니와 남은 시간만 갈아끼운다(x 는 그대로).
void UsageScreen::BeginPackReaction(const PackAnim* anim, uint32_t ms) {
    if (CastActive()) {                // 콕·하이파이브를 받으면 화면의 캐릭터가 다 같이 반긴다
        CastReactAll(ms);
        return;
    }
    if (TripBodyGone()) return;   // 놀러 나간 동안 콕·하이파이브가 와도 몸을 꺼내지 않는다
    if (anim == nullptr || anim->count == 0) return;
    lv_anim_delete(figure_->obj(), nullptr);
    StopLegTimer();
    reaction_ = Reaction::kPoke;   // "반응 중" 표시로만 쓴다 (StartWalkFrom 이 본다)
    pack_react_anim_ = anim;
    pack_react_frame_ = 0;
    pack_react_left_ms_ = ms;
    ShowPackFrame(figure_.get(), *anim, 0);
    figure_->SetHidden(false);
    if (reaction_timer_ == nullptr) {
        reaction_timer_ = lv_timer_create(OnReactionTick, anim->frame_ms, this);
    } else {
        lv_timer_set_period(reaction_timer_, anim->frame_ms);
        lv_timer_reset(reaction_timer_);
    }
}

// 세트에 전용 프레임이 있으면 쓴다. 없으면 장면 자세, 그것도 아니면 touch.tap·double 로 대체한다.
const PackAnim* UsageScreen::ReactionAnim(PackReaction r) const {
    const PackSet& set = pack_set();
    const PackAnim& own = set.reactions[static_cast<int>(r)];
    if (own.count > 0) return &own;
    if (IsBboketerScene() && !CharacterBorrowsScenes()) return &kBboketerScenePoseAnims[BodyScene()];
    return (r == PackReaction::kDizzy || r == PackReaction::kHighFive) ? &set.double_tap : &set.tap;
}

// 몸통 탭 판정. 연타가 먼저다: 4.5초 안 5번 = 어지러움, 3초 안 3번 = 삐짐. 그다음 위치로 고른다 —
// 코 = 재채기, 머리 = 쓰다듬기, 몸 = tap 반응. 코 위치는 프레임(98×64) 비율의 천분율이다.
UsageScreen::TouchAction UsageScreen::OnPackBodyTap(int x, int y) {
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    constexpr int kTaps = sizeof(pack_tap_ms_) / sizeof(pack_tap_ms_[0]);
    for (int i = kTaps - 1; i > 0; i--) pack_tap_ms_[i] = pack_tap_ms_[i - 1];
    pack_tap_ms_[0] = now;
    if (pack_tap_ms_[kTaps - 1] != 0 && now - pack_tap_ms_[kTaps - 1] <= kPackDizzyWindowMs) {
        memset(pack_tap_ms_, 0, sizeof(pack_tap_ms_));
        ESP_LOGI(kTag, "tap x5 -> dizzy");
        BeginPackReaction(ReactionAnim(PackReaction::kDizzy), kPackDoubleMs);
        return TouchAction::kNone;
    }
    if (pack_tap_ms_[2] != 0 && now - pack_tap_ms_[2] <= kPackPoutWindowMs) {
        ESP_LOGI(kTag, "tap x3 -> pout");
        BeginPackReaction(ReactionAnim(PackReaction::kPout), kPackTapMs);
        return TouchAction::kNone;
    }
    const int fx = (x * LV_SCALE_NONE / ActivePack().scale256 - figure_->body_x()) * 1000 / kWalkW;
    const int fy = (y * LV_SCALE_NONE / ActivePack().scale256 - walk_y_) * 1000 / kWalkH;
    int nose_x = kPackNoseDefaultX, nose_y = kPackNoseDefaultY;
    if (IsBboketerScene()) {
        const BboketerNose nose = BboketerSceneNose(BodyScene());
        nose_x = nose.x;
        nose_y = nose.y;
    }
    if (abs(fx - nose_x) < kPackNoseHitX && abs(fy - nose_y) < kPackNoseHitY) {
        ESP_LOGI(kTag, "tap nose -> sneeze");
        BeginPackReaction(ReactionAnim(PackReaction::kSneeze), kPackTapMs);
    } else if (fy < nose_y + kPackHeadBelowNose) {
        ESP_LOGI(kTag, "tap head -> pet");
        BeginPackReaction(ReactionAnim(PackReaction::kPet), kPackTapMs);
    } else {
        ESP_LOGI(kTag, "tap body -> pack tap");
        BeginPackReaction((IsBboketerScene() && !CharacterBorrowsScenes())
                              ? &kBboketerScenePoseAnims[BodyScene()]
                              : &pack_set().tap,
                          kPackTapMs);
    }
    return TouchAction::kNone;
}

// 장면의 터치. 눌린 캐릭터만 반응한다. 연타는 몸통 탭과 같은 규칙이다.
UsageScreen::TouchAction UsageScreen::OnCastTouch(const Display::TouchEvent& ev, bool in_hanger) {
    using Kind = Display::TouchEvent;
    int who = -1;
    uint8_t pose = kCastPoseTap;
    const bool hit = !in_hanger && CastHit(ev.x0, ev.y0, &who, &pose);
    ESP_LOGI(kTag, "cast touch kind=%d at (%u,%u) who=%d pose=%u", static_cast<int>(ev.kind), ev.x0, ev.y0, who, pose);
    const uint8_t bit = hit ? static_cast<uint8_t>(1 << who) : 0;
    switch (ev.kind) {
        case Kind::kTap: {
            if (in_hanger) return TouchAction::kOpenWardrobe;
            if (!hit) return TouchAction::kNone;
            NotifySound::GetInstance().PlayTap(who);   // 누른 캐릭터 목소리(로나·로찌·만복이 순)
            const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            constexpr int kTaps = sizeof(pack_tap_ms_) / sizeof(pack_tap_ms_[0]);
            if (who != cast_tap_who_) {
                memset(pack_tap_ms_, 0, sizeof(pack_tap_ms_));
                cast_tap_who_ = static_cast<int8_t>(who);
            }
            for (int i = kTaps - 1; i > 0; i--) pack_tap_ms_[i] = pack_tap_ms_[i - 1];
            pack_tap_ms_[0] = now;
            if (pack_tap_ms_[kTaps - 1] != 0 && now - pack_tap_ms_[kTaps - 1] <= kPackDizzyWindowMs) {
                memset(pack_tap_ms_, 0, sizeof(pack_tap_ms_));
                CastReact(bit, kCastPoseDizzy, kPackDoubleMs);
            } else if (pack_tap_ms_[2] != 0 && now - pack_tap_ms_[2] <= kPackPoutWindowMs) {
                CastReact(bit, kCastPosePout, kPackTapMs);
            } else {
                CastReact(bit, pose, kPackTapMs);
            }
            return TouchAction::kNone;
        }
        case Kind::kHold:
            if (hit) CastReact(bit, kCastPosePet, kPackPetMs);
            return TouchAction::kNone;
        case Kind::kLongPress:
            return hit ? TouchAction::kNone : TouchAction::kScreenOff;
        default:
            return TouchAction::kNone;
    }
}

// LVGL 타이머 문맥(lvgl_port 락 안).
void UsageScreen::OnReactionTick(lv_timer_t* timer) {
    auto* self = static_cast<UsageScreen*>(lv_timer_get_user_data(timer));
    if (self == nullptr) return;
    if (self->pack_react_anim_ != nullptr) {
        const PackAnim& a = *self->pack_react_anim_;
        if (self->pack_react_left_ms_ <= a.frame_ms) {
            self->EndReaction();
            return;
        }
        self->pack_react_left_ms_ -= a.frame_ms;
        self->pack_react_frame_ = static_cast<uint8_t>((self->pack_react_frame_ + 1) % a.count);
        self->ShowPackFrame(self->figure_.get(), a, self->pack_react_frame_);
        return;
    }
    self->EndReaction();
}

UsageScreen::TouchAction UsageScreen::OnTouch(const Display::TouchEvent& ev) {
    using Kind = Display::TouchEvent;
    if (poke_replies_open_) {
        const TouchAction a = OnPokeRepliesTouch(ev);
        if (a != TouchAction::kNone) return a;
    }
    if (state_ != kUsageStateIdle) return TouchAction::kNone;
    if (poke_panel_open_) return OnPokePanelTouch(ev);
    // 새 일기 알림(배지 아래) 탭 = 그 글.
    if (diary_toast_ != nullptr && !lv_obj_has_flag(diary_toast_, LV_OBJ_FLAG_HIDDEN) && ev.kind == Kind::kTap) {
        lv_area_t a;
        lv_obj_get_coords(diary_toast_, &a);
        if (ev.x0 >= a.x1 - 6 && ev.x0 <= a.x2 + 6 && ev.y0 >= a.y1 - 6 && ev.y0 <= a.y2 + 6) {
            HideDiaryToast();
            diary_open_entry_ = -2;
            return TouchAction::kOpenDiary;
        }
    }
    if (ticker_.Hit(ev.x0, ev.y0)) {
        if (ev.kind == Kind::kTap) {
            if (ticker_.showing_diary()) {   // 일기가 지나가는 중이면 그 글을 연다
                diary_open_entry_ = -2;
                return TouchAction::kOpenDiary;
            }
            ticker_.PauseForTap();
        }
        return TouchAction::kNone;
    }
    // 윗줄 시계·날씨 칸 탭 = 날씨 판. 배지 히트 박스가 칸 왼쪽까지 줄어 있어 배지보다 먼저 봐도 된다.
    if (HitClockPlate(ev.x0, ev.y0)) {
        return ev.kind == Kind::kTap ? TouchAction::kOpenWeather : TouchAction::kNone;
    }
    // 좌상 배지 탭 = 에이전트 전환 (docs/agent-badge.md §2). 몸통보다 먼저.
    // 롱프레스·홀드는 배지에서 무시 (빈 곳 롱프레스 화면 끄기와도 안 겹치게).
    if (HitBadge(ev.x0, ev.y0)) {
        if (ev.kind == Kind::kTap) {
            CycleAgent();
            return TouchAction::kAgentCycled;
        }
        return TouchAction::kNone;
    }
    // 좌하단 하트 (docs/poke.md). 탭 = 콕, 홀드 = 문구 패널. 롱프레스는 여기서
    // 끝낸다 — 빈 곳으로 흘러가면 화면이 꺼진다.
    if (HitPokeButton(ev.x0, ev.y0)) {
        if (ev.kind == Kind::kTap) {
            ESP_LOGI(kTag, "poke button tap");
            poke_phrase_ = 0;
            return TouchAction::kPokeSend;
        }
        if (ev.kind == Kind::kHold) {
            if (manbok_menu_ && ticker_.font() != nullptr) return TouchAction::kOpenManbokMenu;
            OpenPokePanel();
        }
        return TouchAction::kNone;
    }
    // 옷걸이 탭 = 옷장, 몸통 탭 = touch.tap 애니 2초, 몸통 홀드 = 쓰다듬기 애니,
    // 빈 곳 롱프레스 = 화면 끄기.
    const bool in_hanger = ev.x0 >= width_ - kHangerHit && ev.y0 >= height_ - kHangerHit;
    if (CastActive()) return OnCastTouch(ev, in_hanger);
    if (ev.kind == Kind::kTap && !in_hanger && HitTripGuest(ev.x0, ev.y0)) {
        OnTripGuestTap();
        return TouchAction::kNone;
    }
    const bool on_body = !TripBodyGone() && HitBody(ev.x0, ev.y0);
    ESP_LOGI(kTag, "touch kind=%d at (%u,%u) body_x=%d %s", static_cast<int>(ev.kind), ev.x0, ev.y0,
             figure_->body_x(), on_body ? "on body" : "miss");
    switch (ev.kind) {
        case Kind::kTap:
            if (ev.x0 >= width_ - kHangerHit && ev.y0 >= height_ - kHangerHit) {
                ESP_LOGI(kTag, "hanger tap -> wardrobe");   // 팩 아이템 옷장 (세트 전환은 {"set"} 주입만)
                return TouchAction::kOpenWardrobe;
            }
            if (!on_body) return TouchAction::kNone;
            return OnPackBodyTap(ev.x0, ev.y0);
        case Kind::kHold:
            if (!on_body) return TouchAction::kNone;
            ESP_LOGI(kTag, "pet -> pack pet (long)");
            BeginPackReaction(ReactionAnim(PackReaction::kPet), kPackPetMs);
            return TouchAction::kNone;
        case Kind::kLongPress:
            return on_body ? TouchAction::kNone : TouchAction::kScreenOff;
        default:
            return TouchAction::kNone;
    }
}

// --- 캐릭터 팩: 세트 전환 ---------------------------------------------------------

bool UsageScreen::SetPackSet(const char* key, bool toast) {
    if (key == nullptr) return false;
    for (uint8_t i = 0; i < ActivePack().set_count; i++) {
        if (strcmp(ActivePack().sets[i].key, key) != 0) continue;
        if (i != pack_set_) {
            pack_set_ = i;
            ApplyPackSet();
        }
        ESP_LOGI(kTag, "pack set -> %s", key);
        if (toast) {
            char text[16];
            size_t n = 0;
            for (; key[n] != '\0' && n + 1 < sizeof(text); n++) {
                text[n] = static_cast<char>(toupper(static_cast<unsigned char>(key[n])));
            }
            text[n] = '\0';
            ShowAgentToast(text);
        }
        return true;
    }
    ESP_LOGW(kTag, "pack set unknown: %s", key);
    return false;
}

void UsageScreen::CyclePackSet() {
    if (ActivePack().set_count < 2) return;
    SetPackSet(ActivePack().sets[(pack_set_ + 1) % ActivePack().set_count].key, /*toast=*/true);
}

// 세트가 바뀌었다 — 지금 보이는 프레임을 새 세트의 같은 자리로 갈아끼운다.
void UsageScreen::ApplyPackSet() {
    if (state_ != kUsageStateIdle) {
        if (pack_state_anim_ == nullptr) return;
        pack_state_anim_ = &pack_set().states[state_];
        if (pack_state_frame_ >= pack_state_anim_->count) pack_state_frame_ = 0;
        ShowPackFrame(state_figure_.get(), *pack_state_anim_, pack_state_frame_);
        if (pack_state_timer_ != nullptr) {
            lv_timer_set_period(pack_state_timer_, pack_state_anim_->frame_ms);
        }
        return;
    }
    if (pack_react_anim_ != nullptr) {
        EndReaction();   // 반응은 끊고 새 세트로 걷기 복귀
        return;
    }
    if (walk_leg_timer_ != nullptr) {
        const PackAnim& idle_anim = walk_anim();
        walk_leg_frame_ = static_cast<uint8_t>(walk_leg_frame_ % idle_anim.count);
        ShowPackFrame(figure_.get(), idle_anim, walk_leg_frame_);
        lv_timer_set_period(walk_leg_timer_, idle_anim.frame_ms);
    }
}

void UsageScreen::OnPackStateFrame(lv_timer_t* timer) {
    auto* self = static_cast<UsageScreen*>(lv_timer_get_user_data(timer));
    if (self == nullptr || self->pack_state_anim_ == nullptr) return;
    const PackAnim& a = *self->pack_state_anim_;
    self->pack_state_frame_ = static_cast<uint8_t>((self->pack_state_frame_ + 1) % a.count);
    self->ShowPackFrame(self->state_figure_.get(), a, self->pack_state_frame_);
}

void UsageScreen::StopPackState() {
    if (pack_state_timer_ != nullptr) {
        lv_timer_delete(pack_state_timer_);
        pack_state_timer_ = nullptr;
    }
    pack_state_anim_ = nullptr;
    state_figure_->SetBodySrc(nullptr);
    state_figure_->SetHidden(true);
}

// 팩 프레임 한 장 + 옷장 아이템(docs/character-pack.md "items"): 점프가 프레임 픽셀에 구워져 있어
// 아이템도 manifest offsets 만큼 옮긴다. 포즈 컷(items=false — 몸이 다른 그림)·미니카 동안은 숨긴다.
void UsageScreen::ShowPackFrame(Figure* fig, const PackAnim& anim, uint8_t i) {
    if (fig == nullptr || anim.count == 0) return;
    if (i >= anim.count) i = 0;
    fig->SetItemsHidden(!anim.items);
    const int dx = anim.offsets != nullptr ? anim.offsets[i * 2] : 0;
    const int dy = anim.offsets != nullptr ? anim.offsets[i * 2 + 1] : 0;
    fig->SetItemShift(dx, dy);
    if (CharacterHasScenes()) {
        // 장면 대기 자세면 assets 의 몸을 쓴다. 파일이 없으면 프레임으로 대체한다.
        if (IsBboketerScene() && fig == figure_.get() && &anim == &walk_anim()) {
            const int scene = BodyScene();
            fig->SetBboketerScene(scene);
            const lv_image_dsc_t* image = BboketerBodyImage(scene, fig->outfit().top);
            fig->SetBodySrc(image != nullptr ? image : anim.frames[i]);
            return;
        }
        fig->SetBboketerScene(-1);
    }
    fig->SetBodySrc(anim.frames[i]);
}

// 한쪽 끝에 닿았다. LVGL 타이머 문맥에서 불리므로 lvgl_port 락 안이다 —
// 다른 태스크의 SetState 와는 그 락으로 직렬화된다.
void UsageScreen::OnWalkLegDone(lv_anim_t* anim) {
    auto* self = static_cast<UsageScreen*>(lv_anim_get_user_data(anim));
    if (self == nullptr || self->state_ != kUsageStateIdle) return;
    // 방향을 바꾸고 반대 방향본으로 갈아끼운다. lv_anim 은 completed 콜백이
    // 끝나면 스스로 지워지므로 여기서 삭제하지 않는다.
    self->walk_right_ = !self->walk_right_;
    if (self->pack_riding_ && self->pack_ride_leg_ == 0) {
        // 끝에 닿았다 — 돌아서서 가운데로(미러본). 프레임 타이머만 내리고 pack_riding_ 은 유지.
        lv_timer_delete(self->walk_leg_timer_);
        self->walk_leg_timer_ = nullptr;
        self->pack_ride_leg_ = 1;
        self->figure_->SetFacing(self->walk_right_);
        self->StartWalkFrom(self->figure_->body_x(), false);
        return;
    }
    self->StopLegTimer();   // 팩 미니카면 여기서 pack_riding_ 도 풀려 StartWalk 가 가운데에 앉힌다
    self->figure_->SetBodySrc(nullptr);
    self->figure_->SetFacing(self->walk_right_);
    self->StartWalk();
}

void UsageScreen::SetOutfit(const Outfit& outfit) {
    // 팩 아이템 오버레이(ActivePack().items). 대기 화면 몸과 상태 화면 몸 둘 다.
    figure_->SetOutfit(outfit);
    state_figure_->SetOutfit(outfit);
    if (IsBboketerScene() && state_ == kUsageStateIdle) {
        ShowPackFrame(figure_.get(), walk_anim(), walk_leg_frame_);
    }
    ApplyScenePlace();   // 로찌 옷(outfit.hat 칸)
}

void UsageScreen::PauseIdleWalk(bool paused) {
    // 래치를 남긴다 — 옷장/화면 끄기 중 stats 푸시가 ShowIdle(true) 를 타고
    // 대기 애니를 가려진 채 되살리는 것을 막는다 (ShowIdle 이 본다).
    idle_paused_ = paused;
    // 파티클도 같이 멈춘다 — 옷장·꺼진 화면 밑에서 움직여 봐야 무효화 부하만 쌓인다.
    if (drop_timer_ != nullptr) {
        paused ? lv_timer_pause(drop_timer_) : lv_timer_resume(drop_timer_);
    }
    ticker_.SetPaused(paused);
    if (paused) {
        ClosePokePanel();
        StopWalk();
    } else if (state_ == kUsageStateIdle) {
        StartWalk();
    }
    UpdateBadge();   // 옷장·화면 끄기 중엔 배지도 숨긴다 (idle_paused_ 를 본다)
}

void UsageScreen::SetStats(const UsageStats& s) {
    // 글자는 맥이 만들어 보낸 것을 그대로 찍는다. 펌웨어는 뜻을 모른다.
    visible_cells_ = s.cell_count;
    if (visible_cells_ > UsageStats::kMaxCells) {
        visible_cells_ = UsageStats::kMaxCells;
    }

    for (int i = 0; i < kLimitBlocks; i++) {
        // left 가 비면 그 한도가 없는 것이다. 칸째 숨긴다. 5시간 한도는 없어졌으니
        // 맥이 옛 값이나 추정치("5h~")를 보내도 그리지 않는다.
        limit_present_[i] = i < visible_cells_ && s.cells[i].left[0] != '\0' &&
                            strncasecmp(s.cells[i].name, "5h", 2) != 0;
        if (!limit_present_[i]) {
            chip_text_[i][0] = '\0';
            continue;
        }
        const UsageStats::Cell& c = s.cells[i];

        // 이름은 대문자로: "5h" → "5H", "week" → "WEEK". 칩도 같은 글자를 쓴다.
        char name[sizeof(c.name)];
        strncpy(name, c.name, sizeof(name));
        name[sizeof(name) - 1] = '\0';
        for (char* p = name; *p != '\0'; p++) {
            *p = static_cast<char>(toupper(static_cast<unsigned char>(*p)));
        }
        lv_label_set_text(name_labels_[i], name);
        lv_label_set_text(pct_labels_[i], c.left);
        limit_bar_pct_[i] = atoi(c.left);   // "89%" → 89. 숫자가 아니면 0 이라 막대가 빈다

        // right 가 비면(진짜 한도를 못 구한 폴백) 남은 시간도 비운다.
        // 「4d 7h」 을 첫 낱말과 나머지로 나눠 빈칸 글자 대신 좁은 틈(kLimitTextGap)으로 띄운다.
        char left[sizeof(c.right)];
        strncpy(left, c.right, sizeof(left));
        left[sizeof(left) - 1] = '\0';
        char* rest = strchr(left, ' ');
        if (rest != nullptr) {
            *rest++ = '\0';
            while (*rest == ' ') rest++;
        }
        lv_label_set_text(left_labels_[i], left);
        lv_label_set_text(left2_labels_[i], rest != nullptr ? rest : "");

        snprintf(chip_text_[i], sizeof(chip_text_[i]), "%s %s", name, c.left);
    }

    // 상태 화면의 상단 칩. 상태 화면이 떠 있는 동안에도 최신 값으로 갱신된다.
    for (int i = 0; i < kLimitBlocks; i++) {
        lv_label_set_text(chip_labels_[i], chip_text_[i]);
        if (state_ != kUsageStateIdle) {
            SetHidden(chip_labels_[i], chip_text_[i][0] == '\0');
        }
    }

    if (state_ == kUsageStateIdle) {
        ShowIdle(true);
    }
}

// --- 상태 화면 ------------------------------------------------------------------

void UsageScreen::StartPermissionPulse() {
    // border 가 아니라 outline 을 쓴다. border 는 콘텐츠 영역을 3px 줄여
    // 칩·라벨이 움찔하는데, outline 은 레이아웃에 영향이 없다. 패널이 화면
    // 전체라 바깥쪽 outline 은 잘려 안 보이므로 음수 pad 로 안쪽에 그린다.
    lv_obj_set_style_outline_color(panel_, lv_color_hex(kStatePulse), 0);
    lv_obj_set_style_outline_width(panel_, 3, 0);
    lv_obj_set_style_outline_pad(panel_, -3, 0);
    lv_obj_set_style_outline_opa(panel_, LV_OPA_TRANSP, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, panel_);
    lv_anim_set_values(&a, LV_OPA_TRANSP, kStatePulseMaxOpa);
    lv_anim_set_duration(&a, kStatePulseHalfMs);
    lv_anim_set_playback_duration(&a, kStatePulseHalfMs);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
        lv_obj_set_style_outline_opa(static_cast<lv_obj_t*>(obj),
                                     static_cast<lv_opa_t>(v), 0);
    });
    lv_anim_start(&a);
}

void UsageScreen::StopPermissionPulse() {
    lv_anim_delete(panel_, nullptr);
    lv_obj_set_style_outline_width(panel_, 0, 0);
    lv_obj_set_style_outline_opa(panel_, LV_OPA_TRANSP, 0);
}

constexpr int kLineartStateFoot = 206;   // 라인아트 상태 그림 발 줄(tools/lineart_state_compose.py FOOT)

void UsageScreen::SetState(UsageState state, const char* ctx) {
    if (state >= kUsageStateCount) return;

    if (state == kUsageStateIdle) {
        if (state_ == kUsageStateIdle) return;
        StopPermissionPulse();
        StopPackState();
        SetHidden(chip_labels_[0], true);
        SetHidden(chip_labels_[1], true);
        SetHidden(ctx_label_, true);
        SetHidden(state_label_, true);
        state_ = kUsageStateIdle;
        ShowIdle(true);
        UpdateBadge();         // 배지를 이름 칸 앞으로 되돌린다
        return;
    }

    const StateSpec& spec = kStateSpecs[state];
    if (state_ == kUsageStateIdle) {
        ShowIdle(false);
    }

    if (CastActive()) {
        // 켜진 캐릭터의 상태 그림을 나란히 놓는다. state_ 를 정한 뒤 아래에서 ApplyCast 가 그린다.
        StopPackState();
        state_figure_->SetHidden(true);
    } else {
        // 팩 프레임 교체. 같은 프레임 배열이면(thinking→working 둘 다 busy 등) 재시작하지 않는다.
        const PackAnim* anim = &pack_set().states[state];
        if (pack_state_anim_ == nullptr || pack_state_anim_->frames != anim->frames) {
            StopPackState();
            pack_state_anim_ = anim;
            pack_state_frame_ = 0;
            const lv_image_dsc_t* first = anim->frames[0];
            // 시안의 배치: 중심 (0.5, 0.55). 팩 프레임은 전부 98×64, 상태 화면 배율(state_scale256)로 확대.
            // 몸은 state_figure_(Figure) — 옷장 아이템이 같이 뜬다.
            const int s = ActivePack().state_scale256;
            // 라인아트(1배) 프레임은 옛 98×64 프레임 자리에 앉는다(Figure 가 아래 가운데를 맞춘다).
            const bool lineart = (first->header.flags & LV_IMAGE_FLAGS_USER1) != 0;
            const int fw = lineart ? 98 : static_cast<int>(first->header.w);
            const int fh = lineart ? 64 : static_cast<int>(first->header.h);
            state_img_x_ = width_ / 2 - fw * s / LV_SCALE_NONE / 2;
            state_img_y_ = height_ * 55 / 100 - fh * s / LV_SCALE_NONE / 2;
            if (lineart) {
                // 라인아트 상태 그림(로나·로찌 한 장 / 만복이)은 화면 가운데, 발 줄 kLineartStateFoot.
                state_img_y_ = kLineartStateFoot - fh * s / LV_SCALE_NONE;
            } else if (IsBboketerScene()) {   // 대기 자세와 같은 바닥·같은 자리
                state_img_x_ += SceneDx() * s / LV_SCALE_NONE;
                state_img_y_ = BboketerScenePlace(BodyScene()).foot - 124 + SceneHostDy();
            }
            // Figure 는 몸 박스(kBodyH) 밑변 맞춤이라 64 짜리 프레임 좌상단이 박스보다 1px 아래 — 그만큼 올린다.
            state_figure_->SetBodyScreenPos(state_img_x_,
                                            state_img_y_ - (Figure::kBodyH - fh) * s / LV_SCALE_NONE);
            ShowPackFrame(state_figure_.get(), *anim, 0);
            state_figure_->SetHidden(false);
            if (anim->count > 1) {
                pack_state_timer_ = lv_timer_create(OnPackStateFrame, anim->frame_ms, this);
            }
        }
    }

    // 수면은 칩을 흐리게, 컨텍스트 라인은 비운다.
    const bool sleeping = (state == kUsageStateSleeping);
    for (int i = 0; i < kLimitBlocks; i++) {
        lv_obj_set_style_opa(chip_labels_[i], sleeping ? LV_OPA_40 : LV_OPA_COVER, 0);
        SetHidden(chip_labels_[i], chip_text_[i][0] == '\0');
    }

    const bool has_ctx = !sleeping && ctx != nullptr && ctx[0] != '\0';
    lv_label_set_text(ctx_label_, has_ctx ? ctx : "");
    SetHidden(ctx_label_, !has_ctx);

    lv_label_set_text(state_label_, spec.label);
    lv_obj_set_style_text_color(state_label_, lv_color_hex(spec.color), 0);
    SetHidden(state_label_, false);

    if (state == kUsageStatePermission && state_ != kUsageStatePermission) {
        StartPermissionPulse();
    } else if (state != kUsageStatePermission && state_ == kUsageStatePermission) {
        StopPermissionPulse();
    }

    state_ = state;
    if (rozzi_img_ != nullptr) SetHidden(rozzi_img_, true);
    if (CastActive()) {
        cast_visible_ = true;
        ApplyCast();
    }
    UpdateBadge();   // 상태 화면에서도 배지는 남는다 — 칩 앞으로 옮긴다 (확정)
}
