#include "bboketer_scenes.h"

#include <cstdio>

#include <esp_log.h>

#include "assets.h"
#include "bboketer_cast_table.h"
#include "bboketer_lineart_table.h"
#include "character.h"
#include "usage_screen.h"
#include "wardrobe_store.h"

extern "C" const lv_image_dsc_t bboketer_scene_full_dolopath_img;
extern "C" const lv_image_dsc_t bboketer_scene_full_cappadocia_img;
extern "C" const lv_image_dsc_t bboketer_scene_full_seoul_img;

namespace {
// assets 에 자리가 없어 앱 안 C 배열로 구운 배경(tools/apply_felt_bgs.py IN_APP). 나머지는 nullptr = assets 에만 있다.
const lv_image_dsc_t* const kFullImages[kBboketerSceneCount] = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    &bboketer_scene_full_dolopath_img, &bboketer_scene_full_cappadocia_img, &bboketer_scene_full_seoul_img,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};
}  // namespace

const lv_image_dsc_t* BboketerSceneImage(int index) {
    if (index < 0 || index >= kBboketerSceneCount) return nullptr;
    static lv_image_dsc_t images[kBboketerSceneCount] = {};
    static bool checked[kBboketerSceneCount] = {};
    if (!checked[index]) {
        checked[index] = true;
        static const char* names[kBboketerSceneCount] = {
            "moon", "pub", "lpbar", "vintage", "office", "salad", "scuba", "paraglide",
            "dolopath", "cappadocia", "seoul", "camping", "beach", "jeju", "blossom",
            "myroom", "bedroom", "living", "ballpark", "subway",
        };
        char filename[32];
        std::snprintf(filename, sizeof(filename), "bg_%s.rgb", names[index]);
        void* data = nullptr;
        size_t size = 0;
        if (Assets::GetInstance().GetAssetData(filename, data, size) && data != nullptr &&
            size == 320 * 240 * 2) {
            images[index].header.magic = LV_IMAGE_HEADER_MAGIC;
            images[index].header.cf = LV_COLOR_FORMAT_RGB565;
            images[index].header.w = 320;
            images[index].header.h = 240;
            images[index].header.stride = 320 * 2;
            images[index].data_size = size;
            images[index].data = static_cast<const uint8_t*>(data);
        } else if (kFullImages[index] != nullptr) {
            images[index] = *kFullImages[index];   // assets 에 없으면 앱 안 그림
        }
    }
    return images[index].data != nullptr ? &images[index] : nullptr;
}

namespace {
// 라인아트 그림 파일 이름에 쓰는 몸 이름(tools/apply_lineart.py BODIES 와 같은 순서).
constexpr const char* kBodyNames[kBboketerBodyCount] = {
    "moon", "pub", "lpbar", "vintage", "office", "salad", "scuba", "paraglide",
    "dolopath", "cappadocia", "seoul", "camping", "beach", "jeju", "blossom",
    "myroom", "bedroom", "living", "ballpark", "subway",
    "birth", "seol", "chu", "pepero", "xmas",
};
static_assert(kLineartBodyCount == kBboketerBodyCount, "bboketer_lineart_table.h 와 몸 수가 다르다");

bool g_visit = false;
int g_rozzi_item = 0;

const lv_image_dsc_t* LoadImage(lv_image_dsc_t* desc, bool* checked, const char* filename,
                                int width, int height, lv_color_format_t format) {
    if (!*checked) {
        void* data = nullptr;
        size_t size = 0;
        const int bytes = format == LV_COLOR_FORMAT_ARGB8888 ? 4 : 2;
        if (Assets::GetInstance().GetAssetData(filename, data, size) && data != nullptr &&
            size == static_cast<size_t>(width * height * bytes)) {
            desc->header.magic = LV_IMAGE_HEADER_MAGIC;
            desc->header.cf = format;
            desc->header.w = width;
            desc->header.h = height;
            desc->header.stride = width * bytes;
            desc->data_size = size;
            desc->data = static_cast<const uint8_t*>(data);
            *checked = true;
        }
    }
    return desc->data != nullptr ? desc : nullptr;
}

// assets 의 라인아트 I8: w(u16) h(u16) + 팔레트 1024B + 색 번호 w×h. 크기가 안 맞으면 nullptr.
const lv_image_dsc_t* LoadLineart(lv_image_dsc_t* desc, bool* checked, const char* filename) {
    if (!*checked) {
        *checked = true;
        void* data = nullptr;
        size_t size = 0;
        if (Assets::GetInstance().GetAssetData(filename, data, size) && data != nullptr && size > 4 + 1024) {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            const int w = p[0] | (p[1] << 8), h = p[2] | (p[3] << 8);
            if (w > 0 && h > 0 && size == 4 + 1024 + static_cast<size_t>(w) * static_cast<size_t>(h)) {
                desc->header.magic = LV_IMAGE_HEADER_MAGIC;
                desc->header.cf = LV_COLOR_FORMAT_I8;
                desc->header.flags = kLineartFlag;
                desc->header.w = w;
                desc->header.h = h;
                desc->header.stride = w;
                desc->data_size = size - 4;
                desc->data = p + 4;
            }
        }
        if (desc->data == nullptr) ESP_LOGW("Lineart", "missing %s", filename);
    }
    return desc->data != nullptr ? desc : nullptr;
}

// 캐릭터별 몸 그림(lr_ 로나, lz_ 로찌, lm_ 만복이). alt = 놀러가기 전용 자세(lm_<몸>_v).
enum Who { kRona = 0, kRozzi = 1, kManbok = 2, kWhoCount = 3 };
constexpr const char* kWhoPrefix[kWhoCount] = {"lr", "lz", "lm"};
constexpr const char* kWhoItem[kWhoCount] = {"rona", "rozzi", "manbok"};

const lv_image_dsc_t* BodyOf(Who who, int body, int tail) {
    if (body < 0 || body >= kBboketerBodyCount || tail < 0 || tail > 2) return nullptr;
    static constexpr const char* kTails[3] = {"", "_v", "_f"};
    static lv_image_dsc_t images[kWhoCount][kBboketerBodyCount][3] = {};
    static bool checked[kWhoCount][kBboketerBodyCount][3] = {};
    char filename[32];
    std::snprintf(filename, sizeof(filename), "%s_%s%s.i8", kWhoPrefix[who], kBodyNames[body], kTails[tail]);
    return LoadLineart(&images[who][body][tail], &checked[who][body][tail], filename);
}

const lv_image_dsc_t* ItemOf(Who who, int item) {
    if (item < 0 || item >= kBboketerItemCount) item = 0;
    static lv_image_dsc_t images[kWhoCount][kBboketerItemCount] = {};
    static bool checked[kWhoCount][kBboketerItemCount] = {};
    char filename[32];
    std::snprintf(filename, sizeof(filename), "li_%s_%d.i8", kWhoItem[who], item);
    return LoadLineart(&images[who][item], &checked[who][item], filename);
}

bool VisitAlt(int body) {
    return body >= 0 && body < kBboketerBodyCount && kLineartScenes[body].manbok_visit_alt;
}
}  // namespace

namespace {
uint8_t g_cast_mask = 0;
uint8_t g_cast_item[kCastCount] = {};
// 반응 자세 → 낱장 번호(tools/lineart_cast.py POSES, apply_lineart.py RONA_STATES·MANBOK_STATES 와 같다).
constexpr uint8_t kCastPoseFile[kCastCount][kCastPoseCount] = {
    {0, 5, 7, 5, 1, 3}, {0, 5, 7, 5, 1, 3}, {0, 4, 5, 6, 2, 6}};
constexpr const char* kCastStateFile[kUsageStateCount] = {nullptr,      "thinking", "working", "juggling",
                                                          "permission", "error",    "done",    "sleeping"};
}  // namespace

// 저장값이 없으면 지금 캐릭터로 정한다. 부팅 때 캐릭터는 UsageScreen 을 만든 뒤에야 복원되므로
// 불러올 때 굳히지 않고 읽을 때마다 본다(굳히면 만복이 보드가 로나+로찌로 저장된다).
uint8_t BboketerCastMask() {
    if (g_cast_mask != 0) return g_cast_mask;
    return CharacterBorrowsScenes() ? 4 : 3;
}
void BboketerSetCastMask(uint8_t mask) {
    if (mask >= 1 && mask <= 7) g_cast_mask = mask;
}
int BboketerCastItem(int who) { return who >= 0 && who < kCastCount ? g_cast_item[who] : 0; }
void BboketerSetCastItem(int who, int item) {
    if (who >= 0 && who < kCastCount && item >= 0 && item < kBboketerItemCount) g_cast_item[who] = static_cast<uint8_t>(item);
}

void BboketerCastLoad() {
    uint8_t mask = 0;
    if (!WardrobeLoadCast(&mask, g_cast_item) || mask < 1 || mask > 7) {
        mask = 0;
    }
    g_cast_mask = mask;
}

void BboketerCastSave() { WardrobeSaveCast(BboketerCastMask(), g_cast_item); }

CastSpot BboketerCastSpot(int scene, uint8_t mask, int who) {
    if (scene < 0 || scene >= kBboketerBodyCount || mask < 1 || mask > 7 || who < 0 || who >= kCastCount) {
        return CastSpot{false, 0, 0, 0};
    }
    const CastMember& m = kCastLayouts[scene][mask - 1].member[who];
    return CastSpot{m.by != 0, m.cx, m.by, m.tail};
}

BboketerProp BboketerCastProp(int scene, uint8_t mask) {
    if (scene < 0 || scene >= kBboketerBodyCount || mask < 1 || mask > 7) return BboketerProp{};
    const CastLayout& l = kCastLayouts[scene][mask - 1];
    if (l.prop == nullptr) return BboketerProp{};
    static lv_image_dsc_t images[kBboketerBodyCount][7] = {};
    static bool checked[kBboketerBodyCount][7] = {};
    return BboketerProp{LoadLineart(&images[scene][mask - 1], &checked[scene][mask - 1], l.prop), l.px, l.py, l.front != 0};
}

const lv_image_dsc_t* BboketerCastBody(int scene, int who, uint8_t tail) {
    if (who < 0 || who >= kCastCount) return nullptr;
    if (scene == kBboketerVintageBody) return ItemOf(static_cast<Who>(who), g_cast_item[who]);
    return BodyOf(static_cast<Who>(who), scene, tail);
}

const lv_image_dsc_t* BboketerCastPose(int who, uint8_t pose) {
    if (who < 0 || who >= kCastCount || pose >= kCastPoseCount) return nullptr;
    const int n = kCastPoseFile[who][pose];
    static lv_image_dsc_t images[kCastCount][8] = {};
    static bool checked[kCastCount][8] = {};
    char filename[32];
    std::snprintf(filename, sizeof(filename), "lc_%s_%d.i8", kWhoItem[who], n);
    return LoadLineart(&images[who][n], &checked[who][n], filename);
}

const lv_image_dsc_t* BboketerCastState(int who, int state) {
    if (who < 0 || who >= kCastCount || state <= 0 || state >= kUsageStateCount) return nullptr;
    static lv_image_dsc_t images[kCastCount][kUsageStateCount] = {};
    static bool checked[kCastCount][kUsageStateCount] = {};
    char filename[32];
    std::snprintf(filename, sizeof(filename), "ss_%s_%s.i8", kWhoItem[who], kCastStateFile[state]);
    return LoadLineart(&images[who][state], &checked[who][state], filename);
}

void BboketerSetMirror(bool) {}
void BboketerSetVisit(bool on) { g_visit = on; }
void BboketerSetRozziItem(int item) { g_rozzi_item = item; }

const lv_image_dsc_t* UsageSpecialBgImage(int kind) {
    constexpr int kKinds = 6;
    if (kind < 0 || kind >= kKinds) return nullptr;
    static lv_image_dsc_t images[kKinds] = {};
    static bool checked[kKinds] = {};
    // bg_wed.rgb 는 굽지 않는다(2026-11-06). 결혼기념일에는 nullptr 이라 평소 장면 위에 글자 띠만 나온다.
    static const char* const kFiles[kKinds] = {"bg_birth.rgb", "bg_wed.rgb",    "bg_seol.rgb",
                                               "bg_chu.rgb",   "bg_pepero.rgb", "bg_xmas.rgb"};
    return LoadImage(&images[kind], &checked[kind], kFiles[kind], 320, 240, LV_COLOR_FORMAT_RGB565);
}

int BboketerSpecialBody(const lv_image_dsc_t* bg) {
    // UsageSpecialDay 순서(생일·결혼기념일·설날·추석·빼빼로데이·크리스마스) → 몸 번호. 결혼기념일은 자세가 없다.
    static constexpr int kBodies[6] = {kBboketerBirthBody, -1, kBboketerSeolBody, kBboketerChuBody,
                                       kBboketerPeperoBody, kBboketerXmasBody};
    if (bg == nullptr) return -1;
    for (int k = 0; k < 6; ++k) {
        if (kBodies[k] >= 0 && UsageSpecialBgImage(k) == bg) return kBodies[k];
    }
    return -1;
}

const lv_image_dsc_t* BboketerPickedBgImage(int index) {
    static constexpr int kKinds[3] = {0, 4, 5};   // UsageSpecialDay 의 생일·빼빼로데이·크리스마스
    return index >= 0 && index < 3 ? UsageSpecialBgImage(kKinds[index]) : nullptr;
}

BboketerNose BboketerSceneNose(int scene) {
    if (scene < 0 || scene >= kBboketerBodyCount) return BboketerNose{460, 550};
    const LineartPlace& p = kLineartScenes[scene].place;
    if (!CharacterBorrowsScenes()) return BboketerNose{p.nose_x, p.nose_y};
    return BboketerNose{p.mb_nose_x, p.mb_nose_y};
}

BboketerPlace BboketerScenePlace(int scene) {
    if (scene < 0 || scene >= kBboketerBodyCount) return BboketerPlace{0, 202, 206, 62, 0, 0};
    const LineartPlace& p = kLineartScenes[scene].place;
    return BboketerPlace{p.dx, p.foot, p.rozzi_x, p.manbok_x, p.rozzi_dy, p.manbok_dy};
}

BboketerGuestPlace BboketerSceneGuestPlace(int scene) {
    if (scene < 0 || scene >= kBboketerBodyCount) scene = kBboketerVintageBody;
    const LineartPlace& p = kLineartScenes[scene].place;
    return BboketerGuestPlace{p.g_rona, p.g_manbok, p.g_rozzi, p.g_rona_dy, p.g_manbok_dy, p.g_rozzi_dy};
}

BboketerProp BboketerSceneProp(int scene, int layout) {
    if (scene < 0 || scene >= kBboketerBodyCount || layout < 0 || layout > 2) return BboketerProp{};
    const LineartProp& p = kLineartScenes[scene].props[layout];
    if (p.file == nullptr) return BboketerProp{};
    static lv_image_dsc_t images[kBboketerBodyCount][3] = {};
    static bool checked[kBboketerBodyCount][3] = {};
    return BboketerProp{LoadLineart(&images[scene][layout], &checked[scene][layout], p.file), p.x, p.y,
                        p.front != 0};
}

const lv_image_dsc_t* BboketerRozziImage(int scene) {
    // 만복이 집에 놀러 온 로찌는 집주인 옷 칸을 모르니 기본(정장)을 입는다.
    if (scene == kBboketerVintageBody) return ItemOf(kRozzi, CharacterBorrowsScenes() ? 0 : g_rozzi_item);
    return BodyOf(kRozzi, scene, false);
}

const lv_image_dsc_t* BboketerGuestBody(int scene, const char* prefix) {
    if (prefix[0] == '\0') return BodyOf(kRona, scene, false);
    return BodyOf(kManbok, scene, VisitAlt(scene));
}

const lv_image_dsc_t* BboketerBodyImage(int scene, int top) {
    const Who who = CharacterBorrowsScenes() ? kManbok : kRona;
    if (scene == kBboketerVintageBody) return ItemOf(who, top);
    return BodyOf(who, scene, who == kManbok && g_visit && VisitAlt(scene));
}

const lv_image_dsc_t* BboketerOverlayImage(int, int, int) { return nullptr; }

namespace {
OutfitItem g_outfit_items[kWhoCount][kBboketerItemCount] = {};
}

ItemTable BboketerCastItems(int who) {
    if (who < 0 || who >= kCastCount) return {nullptr, 0};
    static char keys[kBboketerItemCount][6];
    for (int i = 0; i < kBboketerItemCount; ++i) {
        std::snprintf(keys[i], sizeof(keys[i]), "i%d", i);
        g_outfit_items[who][i].key = keys[i];
        g_outfit_items[who][i].thumb = ItemOf(static_cast<Who>(who), i);
    }
    return {g_outfit_items[who], kBboketerItemCount};
}

ItemTable BboketerOutfitItems(int kind) {
    // 0 = 지금 캐릭터 옷, 1 = 로찌 옷(로나 팩만). 저장은 outfit.top·outfit.hat 칸이다.
    auto& items = g_outfit_items;
    static OutfitItem none[1] = {};
    static bool ready = false;
    if (!ready) {
        static char keys[kBboketerItemCount][6];
        for (int i = 0; i < kBboketerItemCount; ++i) {
            std::snprintf(keys[i], sizeof(keys[i]), "i%d", i);
            for (auto& row : items) row[i].key = keys[i];
        }
        none[0].key = "none";
        ready = true;
    }
    Who who;
    if (kind == 0) {
        who = CharacterBorrowsScenes() ? kManbok : kRona;
    } else if (kind == 1 && !CharacterBorrowsScenes()) {
        who = kRozzi;
    } else {
        return {none, 1};
    }
    for (int i = 0; i < kBboketerItemCount; ++i) items[who][i].thumb = ItemOf(who, i);
    return {items[who], kBboketerItemCount};
}

#define BG_ITEM(name) {#name, 0, 0, 0, 0, nullptr, nullptr, &bboketer_scene_thumb_##name##_img}

const OutfitItem kBboketerSceneBgItems[kBboketerBgItemCount] = {
    BG_ITEM(moon), BG_ITEM(pub), BG_ITEM(lpbar), BG_ITEM(vintage),
    BG_ITEM(office), BG_ITEM(salad), BG_ITEM(scuba), BG_ITEM(paraglide),
    BG_ITEM(dolopath), BG_ITEM(cappadocia), BG_ITEM(seoul), BG_ITEM(camping),
    BG_ITEM(beach), BG_ITEM(jeju), BG_ITEM(blossom),
    BG_ITEM(myroom), BG_ITEM(bedroom), BG_ITEM(living), BG_ITEM(ballpark), BG_ITEM(subway), BG_ITEM(birth), BG_ITEM(pepero), BG_ITEM(xmas),
};

#undef BG_ITEM

// 장면에서 몸을 톡 치면 나오는 자세. 라인아트에는 장면마다 따로 그린 탭 자세가 없어서 팩의 tap 프레임을 쓴다.
extern "C" const lv_image_dsc_t pp_bboketer_tap_0_img;
static const lv_image_dsc_t* const kPoseTap[] = {&pp_bboketer_tap_0_img};
#define TAP {kPoseTap, 1, 1500, nullptr, false}
const PackAnim kBboketerScenePoseAnims[kBboketerBodyCount] = {
    TAP, TAP, TAP, TAP, TAP, TAP, TAP, TAP, TAP, TAP, TAP, TAP, TAP, TAP, TAP, TAP, TAP, TAP, TAP, TAP,
    TAP, TAP, TAP, TAP, TAP,
};
#undef TAP
