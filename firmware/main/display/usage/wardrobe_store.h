#pragma once

// 옷장 저장·조회 (docs/wardrobe.md ③, wardrobe-impl.md 6절).
//
// - 옷은 NVS 네임스페이스 "wardrobe" 에 **키 문자열 JSON** 으로 저장한다.
//   {"head":"lofi","face":"none","hand":"none","fx":"none","bg":"navy","lock":1}
//   인덱스가 아니라 키로 저장하는 이유: 아이템 표가 늘거나 순서가 바뀌어도
//   입던 옷이 남의 옷으로 변하지 않는다. 프로토콜(outfit 필드)과 같은 표기.
// - 언락 인벤토리는 비트마스크 하나("unlock"). 비트 자리는 kUnlockKeys 순서로
//   못 박는다 — 뒤에 항목을 **추가만** 할 것(순서를 바꾸면 인벤토리가 섞인다).
//   호스트 미러는 ~/.claudebot/wardrobe.json (claude_state.py).
//
// DisplayLockGuard 와 무관한 순수 NVS/표 조회라 아무 태스크에서나 불러도 된다.

#include <cstdint>
#include <string>

#include "figure.h"
#include "usage_screen.h"

// 슬롯 번호(OutfitSlot)·슬롯 표는 item_tables.h — 팩 아이템 표, fx 자리가 neck 이다.

// 키("lofi") → 그 슬롯 표의 인덱스. 모르는 키면 false. "none"/null 은 0.
bool OutfitItemIndexByKey(OutfitSlot slot, const char* key, uint8_t* out);
const char* OutfitItemKeyByIndex(OutfitSlot slot, uint8_t index);

// 하늘 키("sunset") ↔ UsageSky. 기본 13칸 + 장면·기념일(kBboketerSceneBgItems).
bool SkyByKey(const char* key, UsageSky* out);
const char* SkyKey(UsageSky sky);

// 날씨 연출 키("rain") ↔ kWxItems 인덱스 (0=auto). 옷장 WX 탭.
bool WxByKey(const char* key, uint8_t* out);
const char* WxKey(uint8_t wx);

// 언락 가능한 아이템(획득 규칙은 호스트 claude_state.py 가 판정). 비트 자리 고정.
int UnlockBit(const char* key);   // -1 = 언락 대상이 아닌 키

uint32_t WardrobeLoadUnlock();
void WardrobeSaveUnlock(uint32_t mask);

// 캐릭터 팩 착장 세트(docs/character-pack.md). 같은 네임스페이스의 "set" 키에 문자열.
// 팩 캐릭터가 아니면 아무도 안 부른다. Load 는 없으면 "".
std::string WardrobeLoadSet();
void WardrobeSaveSet(const char* key);

// 지금 보는 캐릭터(docs/character-pack.md "런타임 선택"). "char" 키에 CharacterKey() 문자열.
// Load 는 없으면 "" — 그러면 첫 부팅 기본값(첫 팩)이 그대로 남는다.
std::string WardrobeLoadCharacter();
// 예정일·기념일(docs/gift-board-plan.md). 키 "due"·"birthday"·"anniversary". 없으면 "".
std::string WardrobeLoadDate(const char* key);
void WardrobeSaveDate(const char* key, const char* value);
void WardrobeSaveCharacter(const char* key);
// 켜진 캐릭터(비트 묶음)와 세 마리의 옷방 아이템 번호. 저장이 없으면 false.
bool WardrobeLoadCast(uint8_t* mask, uint8_t items[3]);
void WardrobeSaveCast(uint8_t mask, const uint8_t items[3]);

// 에이전트 선택 키("claude"/"codex") ↔ UsageAgent (docs/agent-badge.md §2·§10).
bool AgentByKey(const char* key, UsageAgent* out);
const char* AgentKey(UsageAgent agent);

// 착장은 **캐릭터별** 키에 산다("outfit_<팩 이름>") — 팩마다 아이템 표가 달라
// 인덱스를 공유할 수 없다. 그래서 이 두 함수는 **지금 캐릭터**를 보고 키를 고른다 —
// 캐릭터를 바꾼 **뒤에** 부를 것. bg·wx·agent·lock 은 캐릭터 공용 값이지만 같은 JSON 에
// 들어 있어 양쪽 기록에 각자 쓰인다(전환 때는 착장·lock 만 적용한다).
// agent 는 착장 JSON 의 "agent" 키 — wx 와 같은 결. 옛 저장엔 없다 → both.
void WardrobeSaveOutfit(const Outfit& outfit, bool locked, UsageSky sky, uint8_t wx,
                        UsageAgent agent);
bool WardrobeLoadOutfit(Outfit* outfit, bool* locked, UsageSky* sky, uint8_t* wx,
                        UsageAgent* agent);
