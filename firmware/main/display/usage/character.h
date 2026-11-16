#pragma once

// 지금 화면에 보여줄 캐릭터 (docs/character-pack.md "런타임 선택").
//
// 빌드에 구운 팩들(CONFIG_USAGE_CHARACTER_PACK) 가운데 하나다. 옷장 CHAR 탭 / NVS / 프로토콜이
// 이 값을 바꾼다. 값이 바뀌면 화면을 다시 짜야 한다 — 배율·좌표계·아이템 배치가 서로 다르다.
// NVS 에 이 빌드에 없는 키가 남아 있으면 첫 팩으로 시작한다.

#include <cstdint>

#include "character_pack.h"

// 값: kCharacterPacks 번호 + 1. 0 은 비워 둔다 — 두 보드가 주고받는 놀러가기 패킷이
// 이 숫자를 그대로 싣는다(1 = 첫 팩). 이름 붙은 값은 첫 팩뿐이고 나머지는 PackCharacter(i).
enum class Character : uint8_t {
    kPack = 1,    // 첫 팩
};

inline Character PackCharacter(uint8_t index) { return static_cast<Character>(index + 1); }

// 부팅 시 NVS 값으로 한 번 정한다. NVS 에 값이 없으면 kPack(첫 팩).
Character CurrentCharacter();

// 값만 바꾼다. 화면 재구성·NVS 저장은 호출자 책임이다.
// 이 빌드에 없는 캐릭터를 넣으면 기본 캐릭터로 남는다.
void SetCurrentCharacter(Character c);

// 이 빌드에서 고를 수 있는 캐릭터인가.
bool CharacterAvailable(Character c);
// 이 빌드에서 고를 수 있는 캐릭터 수. 2 미만이면 CHAR 탭을 숨긴다.
int SelectableCharacterCount();
// i 번째로 고를 수 있는 캐릭터(CHAR 탭 순서), 그 역.
Character SelectableCharacter(int index);
int SelectableCharacterIndex(Character c);

// 지금 캐릭터가 장면 전용 팩(뽀케터)인가.
inline bool CharacterHasScenes() { return ActivePack().has_scenes; }
// 장면을 빌려 쓰는 팩(만복이)인가. 뽀케터는 장면 자세 프레임까지 앱에 있다.
inline bool CharacterBorrowsScenes() { return CharacterHasScenes() && ActivePack().scene_prefix[0] != '\0'; }

// 캐릭터 키 ↔ 값 (NVS·프로토콜 표기). 모르는 키면 false.
bool CharacterByKey(const char* key, Character* out);
const char* CharacterKey(Character c);
