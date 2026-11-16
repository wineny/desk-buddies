#include "wardrobe_store.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>

#include "settings.h"
#include "bboketer_scenes.h"

namespace {

constexpr const char* kTag = "WardrobeStore";
constexpr const char* kNs = "wardrobe";
// 착장은 **캐릭터별**로 산다 (docs/character-pack.md "런타임 선택"): 팩마다 아이템 표가 달라
// 한 기록을 공유할 수 없다.
constexpr const char* kOutfitKeyPack = "outfit_p";   // 팩을 나누기 전의 팩 공용 기록(읽기만)
// 캐릭터를 나누기 전의 단일 키. 값이 있으면 그때 살아 있던 캐릭터의 기록이다 — 새 키가
// 비어 있을 때만 읽는다(모르는 슬롯 키는 "벗음"이 되므로 다른 캐릭터가 읽어도 안전하고,
// bg·wx·agent 는 캐릭터와 무관하게 쓰이던 값이라 그대로 살린다).
constexpr const char* kOutfitKeyLegacy = "outfit";
constexpr const char* kUnlockKey = "unlock";   // 언락 인벤토리 — 캐릭터 공용
constexpr const char* kSetKey = "set";     // 캐릭터 팩 착장 세트 (docs/character-pack.md)
constexpr const char* kCastKey = "cast";        // 켜진 캐릭터 비트 묶음 (docs/character-select-plan.md)
constexpr const char* kCastItemsKey = "cast_it";   // 세 마리 옷방 아이템
constexpr const char* kCharKey = "char";   // 지금 보는 캐릭터 (팩 이름)

// 팩마다 아이템 표가 달라 키도 나눈다. 팩 이름은 8자 이하라(gen_pack) 15자 제한 안에 든다.
const char* OutfitKeyNow() {
    static char key[16];
    snprintf(key, sizeof(key), "outfit_%s", ActivePack().name);
    return key;
}

// 언락 비트 자리. 순서를 바꾸지 말고 뒤에 추가만 할 것 (docs/wardrobe.md 획득 규칙).
constexpr const char* kUnlockKeys[] = {"crown", "shades", "lofi", "flag"};

// 장면 앞 기본 하늘 13칸의 키. UsageSky 순서이고 NVS·프로토콜 "bg" 값이다.
// 그림은 없다 — 옛 저장값을 읽고 그대로 다시 쓰기 위해 이름만 남긴다.
constexpr const char* kBaseSkyKeys[] = {
    "brown", "navy", "day", "sunset", "room", "camp", "space",
    "cafe", "alley", "sea", "sakura", "library", "site",
};
constexpr uint8_t kBaseSkyCount = sizeof(kBaseSkyKeys) / sizeof(kBaseSkyKeys[0]);
static_assert(kBaseSkyCount == kUsageSkyFirstScene, "기본 하늘 키는 UsageSky 앞 13칸과 같다");

}  // namespace

bool OutfitItemIndexByKey(OutfitSlot slot, const char* key, uint8_t* out) {
    if (key == nullptr || key[0] == '\0') {
        *out = 0;
        return true;
    }
    const ItemTable t = SlotItems(slot);   // 팩 모드면 팩 아이템 표 (item_tables.h)
    for (uint8_t i = 0; t.items != nullptr && i < t.count; i++) {
        if (strcmp(t.items[i].key, key) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

const char* OutfitItemKeyByIndex(OutfitSlot slot, uint8_t index) {
    const ItemTable t = SlotItems(slot);
    return (t.items != nullptr && index < t.count) ? t.items[index].key : "none";
}

bool SkyByKey(const char* key, UsageSky* out) {
    if (key == nullptr) return false;
    for (uint8_t i = 0; i < kBaseSkyCount; i++) {
        if (strcmp(kBaseSkyKeys[i], key) == 0) {
            *out = static_cast<UsageSky>(i);
            return true;
        }
    }
    // 목록에서 뺀 장면 이름이 NVS 에 남아 있으면 비슷한 장면으로 잇는다(2026-11-06 europe·dolomeadow 뺌).
    if (strcmp(key, "europe") == 0 || strcmp(key, "dolomeadow") == 0) key = "dolopath";
    // 축제(festival)는 2026-11-07 뺐다. 같은 첫 칸·밤 배경인 우주로 잇는다.
    if (strcmp(key, "festival") == 0) key = "moon";
    for (uint8_t i = 0; i < kBboketerBgItemCount; i++) {
        if (strcmp(kBboketerSceneBgItems[i].key, key) == 0) {
            *out = static_cast<UsageSky>(kBaseSkyCount + i);
            return true;
        }
    }
    return false;
}

const char* SkyKey(UsageSky sky) {
    if (sky < kBaseSkyCount) return kBaseSkyKeys[sky];
    if (sky < kUsageSkyCount) return kBboketerSceneBgItems[sky - kBaseSkyCount].key;
    return "navy";
}

bool WxByKey(const char* key, uint8_t* out) {
    if (key == nullptr) return false;
    for (uint8_t i = 0; i < kWxItemCount; i++) {
        if (strcmp(kWxItems[i].key, key) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

const char* WxKey(uint8_t wx) {
    return wx < kWxItemCount ? kWxItems[wx].key : "auto";
}

constexpr const char* kAgentKeys[kUsageAgentCount] = {"claude", "codex"};   // 옛 "both" 는 → claude

std::string WardrobeLoadSet() {
    Settings settings(kNs, false);
    return settings.GetString(kSetKey, "");
}

std::string WardrobeLoadDate(const char* key) {
    Settings settings(kNs, false);
    return settings.GetString(key, "");
}

void WardrobeSaveDate(const char* key, const char* value) {
    if (key == nullptr || value == nullptr) return;
    Settings settings(kNs, true);
    settings.SetString(key, value);
}

std::string WardrobeLoadCharacter() {
    Settings settings(kNs, false);
    return settings.GetString(kCharKey, "");
}

void WardrobeSaveCharacter(const char* key) {
    if (key == nullptr) return;
    Settings settings(kNs, true);
    settings.SetString(kCharKey, key);
    ESP_LOGI(kTag, "saved character %s", key);
}

bool WardrobeLoadCast(uint8_t* mask, uint8_t items[3]) {
    Settings settings(kNs, false);
    const int32_t m = settings.GetInt(kCastKey, 0);
    if (m < 1 || m > 7) return false;
    const int32_t packed = settings.GetInt(kCastItemsKey, 0);
    *mask = static_cast<uint8_t>(m);
    for (int i = 0; i < 3; ++i) items[i] = static_cast<uint8_t>((packed >> (8 * i)) & 0xFF);
    return true;
}

void WardrobeSaveCast(uint8_t mask, const uint8_t items[3]) {
    Settings settings(kNs, true);
    settings.SetInt(kCastKey, mask);
    settings.SetInt(kCastItemsKey, items[0] | (items[1] << 8) | (items[2] << 16));
}

void WardrobeSaveSet(const char* key) {
    if (key == nullptr) return;
    Settings settings(kNs, true);
    settings.SetString(kSetKey, key);
    ESP_LOGI(kTag, "saved set %s", key);
}

bool AgentByKey(const char* key, UsageAgent* out) {
    if (key == nullptr) return false;
    for (uint8_t i = 0; i < kUsageAgentCount; i++) {
        if (strcmp(kAgentKeys[i], key) == 0) {
            *out = static_cast<UsageAgent>(i);
            return true;
        }
    }
    return false;
}

const char* AgentKey(UsageAgent agent) {
    return agent < kUsageAgentCount ? kAgentKeys[agent] : "claude";
}

int UnlockBit(const char* key) {
    if (key == nullptr) return -1;
    for (size_t i = 0; i < sizeof(kUnlockKeys) / sizeof(kUnlockKeys[0]); i++) {
        if (strcmp(kUnlockKeys[i], key) == 0) return static_cast<int>(i);
    }
    return -1;
}

uint32_t WardrobeLoadUnlock() {
    Settings settings(kNs, false);
    return static_cast<uint32_t>(settings.GetInt(kUnlockKey, 0));
}

void WardrobeSaveUnlock(uint32_t mask) {
    Settings settings(kNs, true);
    settings.SetInt(kUnlockKey, static_cast<int32_t>(mask));
}

void WardrobeSaveOutfit(const Outfit& outfit, bool locked, UsageSky sky, uint8_t wx,
                        UsageAgent agent) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "head", OutfitItemKeyByIndex(kSlotHead, outfit.head));
    cJSON_AddStringToObject(root, "face", OutfitItemKeyByIndex(kSlotFace, outfit.face));
    cJSON_AddStringToObject(root, "hand", OutfitItemKeyByIndex(kSlotHand, outfit.hand));
    // 4번째 슬롯 키는 "neck" (item_tables.h)
    cJSON_AddStringToObject(root, SlotJsonKey(kSlotFx), OutfitItemKeyByIndex(kSlotFx, outfit.fx));
    if (CharacterHasScenes()) {
        const uint8_t indices[5] = {outfit.top, outfit.hat, outfit.glasses, outfit.headphones, outfit.boots};
        const char* names[5] = {"top", "hat", "glasses", "headphones", "boots"};
        for (int k = 0; k < 5; ++k) {
            const ItemTable table = BboketerOutfitItems(k);
            const uint8_t index = indices[k] < table.count ? indices[k] : 0;
            cJSON_AddStringToObject(root, names[k], table.items[index].key);
        }
    }
    cJSON_AddStringToObject(root, "bg", SkyKey(sky));
    cJSON_AddStringToObject(root, "wx", WxKey(wx));
    cJSON_AddStringToObject(root, "agent", AgentKey(agent));
    cJSON_AddNumberToObject(root, "lock", locked ? 1 : 0);
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr) return;
    Settings settings(kNs, true);
    settings.SetString(OutfitKeyNow(), text);
    ESP_LOGI(kTag, "saved %s -> %s", OutfitKeyNow(), text);
    cJSON_free(text);
}

bool WardrobeLoadOutfit(Outfit* outfit, bool* locked, UsageSky* sky, uint8_t* wx,
                        UsageAgent* agent) {
    Settings settings(kNs, false);
    std::string text = settings.GetString(OutfitKeyNow(), "");
    if (text.empty()) {
        text = settings.GetString(kOutfitKeyPack, "");   // 모르는 아이템 키는 벗은 것으로 읽힌다
        if (!text.empty()) ESP_LOGI(kTag, "outfit from shared pack key for %s", OutfitKeyNow());
    }
    if (text.empty()) {
        text = settings.GetString(kOutfitKeyLegacy, "");   // 캐릭터를 나누기 전의 기록
        if (!text.empty()) ESP_LOGI(kTag, "outfit from legacy key for %s", OutfitKeyNow());
    }
    if (text.empty()) return false;
    cJSON* root = cJSON_Parse(text.c_str());
    if (root == nullptr) {
        ESP_LOGW(kTag, "stored outfit unparsable: %s", text.c_str());
        return false;
    }
    auto slot_of = [root](OutfitSlot slot, const char* name, uint8_t* out) {
        const cJSON* it = cJSON_GetObjectItem(root, name);
        // 모르는 키(아이템이 사라진 옛 저장)는 벗은 것으로 친다.
        if (!cJSON_IsString(it) || !OutfitItemIndexByKey(slot, it->valuestring, out)) *out = 0;
    };
    slot_of(kSlotHead, "head", &outfit->head);
    slot_of(kSlotFace, "face", &outfit->face);
    slot_of(kSlotHand, "hand", &outfit->hand);
    slot_of(kSlotFx, SlotJsonKey(kSlotFx), &outfit->fx);
    if (CharacterHasScenes()) {
        uint8_t* fields[5] = {&outfit->top, &outfit->hat, &outfit->glasses,
                              &outfit->headphones, &outfit->boots};
        const char* names[5] = {"top", "hat", "glasses", "headphones", "boots"};
        for (int k = 0; k < 5; ++k) {
            const cJSON* value = cJSON_GetObjectItem(root, names[k]);
            const ItemTable table = BboketerOutfitItems(k);
            if (!cJSON_IsString(value)) continue;
            for (uint8_t i = 0; i < table.count; ++i) {
                if (strcmp(value->valuestring, table.items[i].key) == 0) {
                    *fields[k] = i;
                    break;
                }
            }
        }
    }
    const cJSON* bg = cJSON_GetObjectItem(root, "bg");
    if (!cJSON_IsString(bg) || !SkyByKey(bg->valuestring, sky)) *sky = kUsageSkyNavy;
    const cJSON* jwx = cJSON_GetObjectItem(root, "wx");   // 옛 저장엔 없다 → auto
    if (!cJSON_IsString(jwx) || !WxByKey(jwx->valuestring, wx)) *wx = 0;
    const cJSON* jagent = cJSON_GetObjectItem(root, "agent");   // 없거나 옛 "both" → claude
    if (!cJSON_IsString(jagent) || !AgentByKey(jagent->valuestring, agent)) {
        *agent = kUsageAgentClaude;
    }
    const cJSON* lock = cJSON_GetObjectItem(root, "lock");
    *locked = cJSON_IsNumber(lock) && lock->valueint != 0;
    cJSON_Delete(root);
    return true;
}
