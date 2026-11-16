#pragma once

// 옷장 슬롯 표 한 군데 — 캐릭터 팩(gen_pack.py 의 ActivePack().items) 아이템을 옷장·NVS·Figure 가
// 여기 SlotItems() 로만 잡는다. 4번째 슬롯(kSlotFx) 자리는 **neck**(목도리·턱받이)이다 —
// 만복이 소품 4번째 슬롯이 목이라서(docs/character-pack.md "items").

#include <cstdint>

#include "art/ui_art.h"

// 슬롯 번호. WardrobeScreen 의 탭 순서(BG 제외)·착장 JSON 키 순서와 같다.
enum OutfitSlot : uint8_t { kSlotHead = 0, kSlotFace, kSlotHand, kSlotFx };
constexpr int kSlotCount = 4;

struct ItemTable {
    const OutfitItem* items;   // [0] 은 언제나 none. items 가 없는 팩은 nullptr
    uint8_t count;
};

ItemTable SlotItems(OutfitSlot slot);

// 표 안의 index 번째. 범위 밖이면 nullptr (none 은 [0] 이고 img 가 nullptr 이다).
const OutfitItem* SlotItemAt(OutfitSlot slot, uint8_t index);

// 착장 JSON(NVS·프로토콜)의 슬롯 키: head·face·hand·neck.
const char* SlotJsonKey(OutfitSlot slot);
