#include "character.h"

#include <cstring>

namespace {

// 함수 지역 정적으로 둔다 — kCharacterPacks 는 다른 번역 단위의 const 전역이라
// 파일 스코프에서 초기화하면 정적 초기화 순서에 걸린다(빈 팩으로 굳을 수 있다).
Character& State() {
    // 첫 부팅 기본값: 첫 팩. NVS 복원이 이 값을 덮는다.
    static Character c = Character::kPack;
    return c;
}

}  // namespace

const CharacterPack& ActivePack() {
    const int index = static_cast<int>(State()) - 1;
    return kCharacterPacks[index >= 0 && index < kCharacterPackCount ? index : 0];
}

Character CurrentCharacter() { return State(); }

bool CharacterAvailable(Character c) {
    const int v = static_cast<int>(c);
    return v >= 1 && v <= kCharacterPackCount;
}

void SetCurrentCharacter(Character c) {
    State() = CharacterAvailable(c) ? c : Character::kPack;
}

int SelectableCharacterCount() { return kCharacterPackCount; }

Character SelectableCharacter(int index) { return PackCharacter(static_cast<uint8_t>(index)); }

int SelectableCharacterIndex(Character c) { return static_cast<int>(c) - 1; }

bool CharacterByKey(const char* key, Character* out) {
    if (key == nullptr || out == nullptr) return false;
    if (strcmp(key, "pack") == 0) {   // 팩 이름을 모를 때 쓰는 별칭 — 첫 팩
        *out = Character::kPack;
        return true;
    }
    for (uint8_t i = 0; i < kCharacterPackCount; i++) {
        if (strcmp(key, kCharacterPacks[i].name) == 0) {
            *out = PackCharacter(i);
            return true;
        }
    }
    return false;
}

const char* CharacterKey(Character c) {
    const int index = static_cast<int>(c) - 1;
    return kCharacterPacks[index >= 0 && index < kCharacterPackCount ? index : 0].name;
}
