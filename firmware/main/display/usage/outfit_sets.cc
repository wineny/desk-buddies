#include "outfit_sets.h"

#include <cstring>

#include "wardrobe_store.h"

namespace {

// 슬롯별 키(nullptr = none)와 날씨 연출. 순서는 UsageSky enum 그대로.
struct SetSpec {
    const char* head;
    const char* face;
    const char* hand;
    const char* fx;
    const char* wx;
};

// docs/alive.md E절 세트 테이블 13종. 마을 4종은 wx=auto — 파티클은 호스트
// 날씨 푸시를 따른다. sakura 의 꽃잎도 wx=auto 일 때 상시라 auto 로 둔다.
constexpr SetSpec kSets[kUsageSkyCount] = {
    {nullptr, nullptr, nullptr, nullptr, "auto"},              // brown — 기본(무착장)
    {"nightcap", "closed", nullptr, "zzz", "auto"},            // navy — 꿀잠 준비
    {"cap", "happy", nullptr, nullptr, "auto"},                // day — 낮 산책
    {"beanie", "happy", "scarf", nullptr, "auto"},             // sunset — 노을 산책
    {"lofi", "closed", "boombox", "notes", "clear"},           // room — 로파이 DJ
    {"beanie", nullptr, "marshmallow", nullptr, "leaf"},       // camp — 캠핑
    {"helmet", nullptr, "flag", "sparkle", "star"},            // space — 달 착륙
    {"chef", "happy", "coffee", nullptr, "clear"},             // cafe — 카페
    {"cathat", "wink", nullptr, "heart", "clear"},             // alley — 고양이 골목
    {nullptr, "goggles", "fish", "dots", "clear"},             // sea — 물속 탐험
    {nullptr, "blush", "balloon", "heart", "auto"},            // sakura — 봄나들이
    {nullptr, "glasses", "book", "bulb", "clear"},             // library — 책벌레
    {"hardhat", nullptr, "wrench", "sweat", "clear"},          // site — 공사 반장
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // moon (축제 자리, 11/7)
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // pub
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // lpbar
    {"cat_ear_beanie", nullptr, nullptr, nullptr, "clear"},   // vintage
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // office
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // salad
    {nullptr, "scuba_mask", nullptr, nullptr, "clear"},       // scuba
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // paraglide
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // dolopath
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // cappadocia
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // seoul
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // camping (산속 캠핑 장면)
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // beach
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // jeju
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // blossom
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // myroom
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // bedroom
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // living
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // ballpark
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // subway
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // birth
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // pepero
    {nullptr, nullptr, nullptr, nullptr, "clear"},            // xmas
};
static_assert(sizeof(kSets) / sizeof(kSets[0]) == kUsageSkyCount, "kSets 는 UsageSky 마다 한 줄(장면을 넣고 빼면 같이 고친다)");

// 마을 하늘 강수 세트. wx 는 auto — 비/눈 파티클은 실제 푸시 날씨를 따라
// 내리고, 개면 다음 푸시에서 시각 세트로 복귀한다 ("비는 오는 것").
constexpr SetSpec kRainSet = {nullptr, nullptr, "umbrella", nullptr, "auto"};
constexpr SetSpec kSnowSet = {"beanie", nullptr, "scarf", nullptr, "auto"};

// 세트 표는 슬롯을 적지 않는다(scarf 가 팩에선 neck 일 수 있다).
// 키가 어느 슬롯에 있든 그 슬롯에 입힌다. 팩에 없는 키(lofi·boombox…)는 그냥 없음.
void ResolveAny(const char* key, Outfit* outfit) {
    if (key == nullptr) return;
    uint8_t* fields[kSlotCount] = {&outfit->head, &outfit->face, &outfit->hand, &outfit->fx};
    for (int s = 0; s < kSlotCount; s++) {
        uint8_t idx = 0;
        if (OutfitItemIndexByKey(static_cast<OutfitSlot>(s), key, &idx) && idx != 0) {
            *fields[s] = idx;
            return;
        }
    }
}

}  // namespace

void AutoOutfitSet(UsageSky sky, uint8_t precip, Outfit* outfit, uint8_t* wx) {
    const SetSpec* spec = &kSets[sky < kUsageSkyCount ? sky : kUsageSkyBrown];
    const bool village = sky <= kUsageSkySunset;
    if (village && precip == 1) {
        spec = &kRainSet;
    } else if (village && precip == 2) {
        spec = &kSnowSet;
    }
    *outfit = Outfit{};
    for (const char* key : {spec->head, spec->face, spec->hand}) {
        ResolveAny(key, outfit);   // fx 열은 이펙트 키라 neck 자리엔 안 입힌다
    }
    *wx = 0;
    WxByKey(spec->wx, wx);
}
