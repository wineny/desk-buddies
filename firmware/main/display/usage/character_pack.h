#pragma once

// 캐릭터 팩 (docs/character-pack.md, docs/workshop.md §3).
//
// 한 빌드에 팩을 여러 개 넣을 수 있다(CONFIG_USAGE_CHARACTER_PACK="bboketer;manbok").
// 참석자가 pixelpal 로 구운 캐릭터 한 마리 = packs/<name>/ 폴더 하나. 착장 세트 N개 ×
// 보드 상태 8종(idle 포함) 프레임 배열 + 대기 화면 터치 반응 2종. 테이블은
// tools/gen_pack.py 가 pack.json 에서 굽고, CMake 가 configure 때 부른다.
// 프레임은 전부 I4 98×64 정지 이미지라 GIF 디코더 없이 lv_image + lv_timer 로 돈다.

#include <lvgl.h>

#include <cstdint>

#include "art/ui_art.h"

struct PackAnim {
    const lv_image_dsc_t* const* frames;
    uint8_t count;
    uint16_t frame_ms;   // 1000 / manifest fps
    // 옷장 아이템 오버레이용 (docs/character-pack.md "items"). 상태 프레임은 점프(dx/dy)가 픽셀에
    // 구워져 있어 아이템도 같은 양만큼 옮겨야 한다: offsets = [dx0,dy0,dx1,dy1,...] (count×2, nullptr = 전부 0).
    // items=false 면 그 애니 동안 아이템을 숨긴다 — codex 포즈 컷처럼 몸이 다른 그림(manifest states.<st>.items).
    const int8_t* offsets = nullptr;
    bool items = false;
};

// UsageState 순서와 같다 (usage_screen.h) — gen_pack.py BOARD_STATES 와 셋이 같이 간다.
constexpr int kPackStateCount = 8;

// 대기 화면 터치·콕 반응. 세트에 그 상태 프레임이 있으면 그것을 쓰고, 없으면 tap·double 로
// 대체한다(usage_screen.cc ReactionAnim). 순서는 gen_pack.py REACTIONS 와 같다.
enum class PackReaction : uint8_t { kPet = 0, kSneeze, kPout, kDizzy, kHighFive };
constexpr int kPackReactionCount = 5;

struct PackSet {
    const char* key;                     // pack.json sets[] = POST {"set": key}
    PackAnim states[kPackStateCount];
    // 대기 화면 '미니카' 행동(pack.json "ride"): 평소엔 제자리 idle, 가끔(또는 {"act":"ride"} 주입)
    // 이 애니로 반대 끝까지 달린다. count 0 이면 행동 없음 → 옛 방식(idle 프레임으로 좌우 미끄러짐).
    PackAnim ride;                       // 오른쪽으로 달릴 때
    PackAnim ride_left;                  // 왼쪽(미러본). count 0 이면 ride 그대로
    PackAnim tap;                        // 대기 화면 몸통 탭
    PackAnim double_tap;                 // 600ms 안 두 번
    PackAnim reactions[kPackReactionCount];   // manifest 에 pet·sneeze·pout·dizzy·highfive 가 있을 때만 채운다
};

// 자리 소품(pack.json "seats" → seats.json, pixelpal mb_seats.py). 대기 화면 배경(UsageSky)마다 몸 뒤에
// 깔리는 그림 하나 — 만복이처럼 제자리에 앉는 캐릭터가 길바닥에 덩그러니 있지 않게(09-06).
// dx/dy 는 프레임(98×64) 좌상단 기준 1px(프레임 밖으로 나가도 된다), raise 는 몸을 올리는 양(1px 단위,
// 앉는 면 − 엉덩이 선). img 가 nullptr 이면 그 배경엔 자리 없음(몸은 바닥).
struct PackSeat {
    const lv_image_dsc_t* img;
    int8_t dx;
    int8_t dy;
    uint8_t raise;
};

struct CharacterPack {
    const char* name;
    const PackSet* sets;
    uint8_t set_count;
    uint8_t default_set;
    uint16_t scale256;         // 대기 화면(걷기) 배율 × 256. pack.json "scale" 또는 scale.idle. 384 = 1.5×
    uint16_t state_scale256;   // 상태 화면(thinking·done…) 배율 × 256. scale.state. 512 = 2× (nearest, 픽셀 아트)
    // 옷장 아이템 표 (pack.json "items" → items.json, pixelpal export_items.py). OutfitSlot 순서
    // head·face·hand·neck — neck 은 kSlotFx 자리를 쓴다(item_tables.h). [0] 은 언제나 none.
    // dx/dy 는 **프레임(98×64) 좌상단 기준 1px 셀**, img_l 은 없다(nullptr — idle 프레임이 방향 무관).
    // items 가 없는 팩은 nullptr/0 — 옷장이 빈 채로 열린다.
    const OutfitItem* items[4];
    uint8_t item_counts[4];
    const lv_image_dsc_t* none_thumb;
    // 자리 소품 표 (pack.json "seats"). 앞 13개 하늘(UsageSky 기본 배경)만, 없는 팩은 nullptr.
    const PackSeat* seats;
    // 장면 전용 팩(pack.json "scenes", 뽀케터): 배경·옷장이 장면 9개·전용 옷 탭으로 돈다.
    bool has_scenes;
    // 장면 assets 이름 앞머리. 뽀케터 "", 만복이 "mb_"(mb_char_<장면>.argb, mb_overlay_<장면>_hat<n>.argb).
    const char* scene_prefix;
};

// 이 빌드에 구운 팩들. 순서가 옷장 CHAR 탭 순서이고 [0] 이 첫 부팅 기본값이다.
extern const CharacterPack kCharacterPacks[];
extern const uint8_t kCharacterPackCount;

// 지금 보는 캐릭터의 팩. 빌드에는 팩이 언제나 하나 이상 있다(main/CMakeLists.txt).
const CharacterPack& ActivePack();
