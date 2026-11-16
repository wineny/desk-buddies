#pragma once

// AUTO 완벽 세트 (docs/alive.md E절, 시안 docs/mockups/auto_sets.png).
//
// AUTO(비LOCK)일 때 배경(bg)마다 어울리는 착장+날씨 연출(wx)을 통째로 입힌다.
// 마을 하늘 4종(brown/navy/day/sunset)에서는 강수 세트(비=우산, 눈=비니+목도리)가
// 시각 세트보다 우선한다. 세트는 날씨 옷과 같이 **표시만** — NVS 에 저장하지
// 않으므로 재부팅·수동 착장(호스트 outfit·옷장)이 언제나 이긴다.
//
// 테이블은 키 문자열로 두고 런타임에 OutfitItemIndexByKey 로 해석한다 —
// 아이템 표의 인덱스가 바뀌어도 세트가 남의 옷으로 변하지 않게(NVS 와 같은 이유).

#include <cstdint>

#include "figure.h"
#include "usage_screen.h"

// sky 의 세트를 outfit/wx 로 푼다. precip: 0=없음 1=비 2=눈 (SetWeatherFx 값 그대로,
// 마을 하늘에서만 우선 적용). 해석 실패 슬롯은 none(0)으로 남는다.
void AutoOutfitSet(UsageSky sky, uint8_t precip, Outfit* outfit, uint8_t* wx);
