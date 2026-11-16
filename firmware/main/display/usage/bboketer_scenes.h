#pragma once

#include "art/ui_art.h"
#include "art/scene_sprites.h"
#include "character_pack.h"
#include "item_tables.h"

constexpr int kBboketerSceneCount = 20;
constexpr int kBboketerBgItemCount = kBboketerSceneCount + 3;   // 옷장 BG 칸: 장면 20개 + 생일·빼빼로데이·크리스마스 = 23(3×2 여섯 칸 × 4쪽, 마지막 쪽 5칸)

#define DECLARE_SCENE(name) \
    extern "C" const lv_image_dsc_t bboketer_scene_thumb_##name##_img;

DECLARE_SCENE(moon)
DECLARE_SCENE(pub)
DECLARE_SCENE(lpbar)
DECLARE_SCENE(vintage)
DECLARE_SCENE(office)
DECLARE_SCENE(salad)
DECLARE_SCENE(scuba)
DECLARE_SCENE(paraglide)
DECLARE_SCENE(dolopath)
DECLARE_SCENE(cappadocia)
DECLARE_SCENE(seoul)
DECLARE_SCENE(camping)
DECLARE_SCENE(beach)
DECLARE_SCENE(jeju)
DECLARE_SCENE(blossom)
DECLARE_SCENE(myroom)
DECLARE_SCENE(bedroom)
DECLARE_SCENE(living)
DECLARE_SCENE(ballpark)
DECLARE_SCENE(subway)
DECLARE_SCENE(birth)
DECLARE_SCENE(pepero)
DECLARE_SCENE(xmas)

#undef DECLARE_SCENE

// Same order as the scene UsageSky values and scenes/scenes.json (tools/apply_felt_bgs.py).
// assets 파티션의 전체 색상 RGB565 배경. dolopath·cappadocia·seoul 은 assets 에 자리가 없어 앱 안 C 배열을 쓴다.
// 둘 다 없으면 nullptr — 호출자가 대체 배경을 고른다.
const lv_image_dsc_t* BboketerSceneImage(int index);

// --- 라인아트 캐릭터 (docs/lineart-firmware.md, tools/apply_lineart.py) ---
// 몸 번호(BodyScene): 0~19 = 장면(UsageSky 순서), 20~24 = 기념일(생일·설날·추석·빼빼로데이·크리스마스).
// 그림은 assets 의 I8(1배)이고 header.flags 에 kLineartFlag 가 있다. 그리는 쪽은 이 표시를 보면
// 1배로 그리고 그림의 아래 가운데를 옛 98×64 2배 프레임의 아래 가운데(foot + 4)에 맞춘다.
constexpr int kBboketerBirthBody = kBboketerSceneCount;   // 기념일 몸 번호는 장면 바로 뒤: 생일·설날·추석·빼빼로데이·크리스마스
constexpr int kBboketerSeolBody = kBboketerBirthBody + 1;
constexpr int kBboketerChuBody = kBboketerBirthBody + 2;
constexpr int kBboketerPeperoBody = kBboketerBirthBody + 3;
constexpr int kBboketerXmasBody = kBboketerBirthBody + 4;
constexpr int kBboketerBodyCount = kBboketerBirthBody + 5;
// 옷장 BG 칸 번호(장면 20개 + 생일·빼빼로데이·크리스마스) → 몸 번호.
constexpr int BboketerSkyBody(int bg_index) {
    return bg_index == kBboketerSceneCount       ? kBboketerBirthBody
           : bg_index == kBboketerSceneCount + 1 ? kBboketerPeperoBody
           : bg_index == kBboketerSceneCount + 2 ? kBboketerXmasBody
                                                 : bg_index;
}
constexpr int kBboketerVintageBody = 3;        // 옷방. 여기서만 아이템(옷)을 입는다
constexpr int kBboketerItemCount = 17;         // 아이템 보드 17칸. 0 = 정장(기본)
constexpr uint32_t kLineartFlag = LV_IMAGE_FLAGS_USER1;
inline bool IsLineart(const lv_image_dsc_t* d) { return d != nullptr && (d->header.flags & kLineartFlag) != 0; }

// 지금 몸(로나 팩이면 로나, 만복이 팩이면 만복이). 옷방이면 top 번 아이템.
const lv_image_dsc_t* BboketerBodyImage(int scene, int top);
const lv_image_dsc_t* BboketerOverlayImage(int scene, int kind, int option);   // 라인아트에는 덧입힘이 없다 — nullptr
// kind 0 = 지금 캐릭터 옷(17칸), kind 1 = 로찌 옷(로나 팩만). 나머지 종류는 「없음」 한 칸.
ItemTable BboketerOutfitItems(int kind);
// 코 위치. 옛 프레임(98×64) 비율의 천분율이다. 터치 판정(usage_screen.cc)이 쓴다.
struct BboketerNose {
    int16_t x;
    int16_t y;
};
BboketerNose BboketerSceneNose(int scene);
constexpr int kBabyRoomScene = 7;          // 내부 이름 paraglide
void BboketerSetMirror(bool on);           // 라인아트 시안은 뒤집지 않는다 — 아무것도 안 한다
// --- 켜진 캐릭터 (docs/character-select-plan.md, tools/lineart_cast.py) ---
// 번호 0 로나, 1 로찌, 2 만복이. 조합은 비트 묶음(1 << 번호)이고 1~7 이다.
constexpr int kCastCount = 3;
enum CastPose : uint8_t { kCastPoseIdle = 0, kCastPoseTap, kCastPosePet, kCastPoseSneeze, kCastPosePout, kCastPoseDizzy, kCastPoseCount };
struct CastSpot {
    bool on;
    int16_t cx;     // 몸 가운데 x
    int16_t by;     // 발 줄 y
    uint8_t tail;   // 그림 꼬리표: 0 기본, 1 _v, 2 _f
};
uint8_t BboketerCastMask();
void BboketerSetCastMask(uint8_t mask);
int BboketerCastItem(int who);                  // 옷방에서 입는 아이템 번호
void BboketerSetCastItem(int who, int item);
void BboketerCastLoad();                        // NVS. 없으면 BboketerCastMask 가 지금 캐릭터로 답한다(로나 보드 = 로나+로찌, 만복이 보드 = 만복이)
void BboketerCastSave();
CastSpot BboketerCastSpot(int scene, uint8_t mask, int who);
const lv_image_dsc_t* BboketerCastBody(int scene, int who, uint8_t tail);
const lv_image_dsc_t* BboketerCastPose(int who, uint8_t pose);    // 반응·걷기 그림
const lv_image_dsc_t* BboketerCastState(int who, int state);      // 상태 그림. state 는 UsageState 값
ItemTable BboketerCastItems(int who);
// 놀러가기(손님이 있음) 배치인가. 만복이의 놀러가기 전용 자세(서울 브이·캠핑 뒷모습)를 고를 때 쓴다.
void BboketerSetVisit(bool on);
// 로찌가 옷방에서 입는 아이템(로나 팩의 outfit.hat 칸에 저장).
void BboketerSetRozziItem(int item);
// 장면별 자리(화면 px). foot + 4 = 로나(집) 발. *_dy = 로나 발에서 그 캐릭터 발까지.
struct BboketerPlace {
    int16_t dx;
    int16_t foot;
    int16_t rozzi_x;
    int16_t manbok_x;
    int16_t rozzi_dy;
    int16_t manbok_dy;
};
BboketerPlace BboketerScenePlace(int scene);
// 손님이 왔을 때 셋이 앉는 자리(화면 px, 프레임 왼쪽). 어느 집이든 로나·만복이·로찌 순서다.
struct BboketerGuestPlace {
    int16_t rona;
    int16_t manbok;
    int16_t rozzi;
    int16_t rona_dy;
    int16_t manbok_dy;
    int16_t rozzi_dy;
};
BboketerGuestPlace BboketerSceneGuestPlace(int scene);
// 같이 노는 소품(공·모닥불·축음기)과 앞 탁자(샐러드). layout 0 = 로나집, 1 = 만복이집, 2 = 놀러가기.
struct BboketerProp {
    const lv_image_dsc_t* image;   // nullptr = 없음
    int16_t x, y;                  // 화면 왼쪽 위
    bool front;                    // 캐릭터 위에 그린다
};
BboketerProp BboketerSceneProp(int scene, int layout);
BboketerProp BboketerCastProp(int scene, uint8_t mask);   // 켜진 조합의 소품. mask 0 = 없음
// 로나 옆에 앉는 로찌. 파일이 없으면 nullptr.
const lv_image_dsc_t* BboketerRozziImage(int scene);
// 놀러 온 손님의 장면 자세. prefix 는 그 팩의 scene_prefix("" = 로나).
const lv_image_dsc_t* BboketerGuestBody(int scene, const char* prefix);
// 기념일 배경(assets 320×240 RGB565). kind 는 UsageSpecialDay 값. 파일이 없으면 nullptr.
const lv_image_dsc_t* UsageSpecialBgImage(int kind);
// 기념일 배경 그림 → 몸 번호(20~24). 기념일 배경이 아니면 -1.
int BboketerSpecialBody(const lv_image_dsc_t* bg);
// 옷장에서 고른 기념일 배경. index 0 = 생일, 1 = 빼빼로데이, 2 = 크리스마스.
const lv_image_dsc_t* BboketerPickedBgImage(int index);
extern const OutfitItem kBboketerSceneBgItems[kBboketerBgItemCount];
extern "C" const SceneSprite kBboketerSceneSprites[];
extern "C" const uint8_t kBboketerSceneSpriteCount;
extern const PackAnim kBboketerScenePoseAnims[kBboketerBodyCount];
