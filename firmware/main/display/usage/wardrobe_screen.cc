#include "wardrobe_screen.h"

#include <esp_log.h>

#include "outfit_sets.h"
#include "character.h"
#include "character_pack.h"
#include "bboketer_scenes.h"

LV_FONT_DECLARE(press_start_8)
LV_FONT_DECLARE(press_start_16)
LV_FONT_DECLARE(press_start_24)

namespace {

constexpr const char* kTag = "Wardrobe";

// 시안 gen_wardrobe_ui.py 의 C 팔레트.
constexpr uint32_t kBg = 0x161008;
constexpr uint32_t kSheet = 0x0C0804;
constexpr lv_opa_t kSheetOpa = 235;        // 92%
constexpr uint32_t kSheetLine = 0x2A2018;
constexpr uint32_t kTabText = 0xB08A6E;
constexpr uint32_t kTabOn = 0xFF8A4C;
constexpr uint32_t kTabOnText = 0x1B1006;
constexpr uint32_t kThumbBg = 0x1A1410;
constexpr uint32_t kThumbBg2 = 0x221A14;
constexpr uint32_t kChip = 0x0C0804;
constexpr uint32_t kChipText = 0xFF8A4C;
constexpr uint32_t kDot = 0x5A4636;
constexpr uint32_t kDotOn = 0xFF8A4C;

// 320×240 기준 배치.
constexpr int kPreviewH = 176;
constexpr int kTabRowH = 22;
constexpr int kItemRowY = kPreviewH + 24;   // 200
constexpr int kItemRowH = 40;
constexpr int kThumb = 36;
constexpr int kThumbPitch = 44;
constexpr int kThumbX0 = 8;                 // 4 + (44 - 36) / 2
constexpr int kDotY = 237;
constexpr int kCloseHit = 36;
// 탭 줄(그리기 24px)이 손가락엔 얇아 판정만 위아래로 넓힌다. 미리보기 아래 8px 는 탭,
// 썸네일 그림 시작(y202) 직전까지가 경계.
constexpr int kTabHitY0 = kPreviewH - 8;    // 168
constexpr int kTabHitY1 = kItemRowY + 2;    // 202
constexpr int kLockX = 264, kLockY = 4, kLockW = 48, kLockH = 18;
constexpr int kLockHitX = 260, kLockHitH = 28;
constexpr int kRoadY = 206;                 // 배경의 길 아랫선
constexpr int kRoadInPreview = 168;         // 미리보기에서 길을 둘 자리 (시안 ROAD_Y + 8 - PH)
constexpr int kTopMargin = 4;               // 조합이 이보다 위로 나가면 창을 내린다
constexpr int kScale = 384;                 // 1.5×

constexpr int kPixelLetterSpace = 1;

// 배경 그림은 장면·기념일만 있다(assets). 장면 앞 기본 하늘 13칸은 그림이 없다.
const lv_image_dsc_t* SkyImage(UsageSky sky) {
    if (sky >= kUsageSkyFirstScene && sky <= kUsageSkyLastScene) {
        return BboketerSceneImage(sky - kUsageSkyFirstScene);   // assets 가 없으면 nullptr
    }
    if (sky > kUsageSkyLastScene && sky < kUsageSkyCount) {
        return BboketerPickedBgImage(sky - kUsageSkyBirth);
    }
    return nullptr;
}

void SetHidden(lv_obj_t* obj, bool hidden) {
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace

using Tab = WardrobeScreen::Tab;

const WardrobeScreen::TabSpec WardrobeScreen::kTabs[WardrobeScreen::kMaxTabs] = {
    {"HEAD", Tab::kHead}, {"FACE", Tab::kFace}, {"HAND", Tab::kHand},
    {"NECK", Tab::kFx},   {"BG", Tab::kBg},     {"WX", Tab::kWx},
    {"CHAR", Tab::kChar},
    {"TOP", Tab::kTop}, {"HAT", Tab::kHat}, {"EYE", Tab::kGlasses},
    {"HP", Tab::kHeadphones}, {"BOOT", Tab::kBoots},
};

bool WardrobeScreen::TabVisible(Tab t) {
    if (t == Tab::kChar) return SelectableCharacterCount() >= 2;   // 고를 캐릭터가 하나면 숨긴다
    if (CharacterHasScenes()) {
        if (CharacterBorrowsScenes()) return t == Tab::kBg || t == Tab::kHat;   // 장면을 빌리는 팩은 모자만
        return t == Tab::kBg || t >= Tab::kTop;
    }
    return t < Tab::kTop;
}

int WardrobeScreen::TabCount() {
    int n = 0;
    for (int i = 0; i < kMaxTabs; i++) {
        if (TabVisible(kTabs[i].tab)) n++;
    }
    return n;
}

int WardrobeScreen::TabSpecAt(int slot) {
    int n = 0;
    for (int i = 0; i < kMaxTabs; i++) {
        if (!TabVisible(kTabs[i].tab)) continue;
        if (n == slot) return i;
        n++;
    }
    return 0;
}

const char* WardrobeScreen::TabLabel(int slot) {
    if (slot < 0 || slot >= TabCount()) return "";
    return kTabs[TabSpecAt(slot)].label;
}

// CHAR 탭 항목: 이 빌드에 구운 팩. 썸네일 전용 에셋은 없다 — 참석자가 구운 팩마다
// 그림을 따로 만들 수 없으니 각자의 대기 프레임(98×65 / 98×64)을 썸네일 칸에 맞게 축소해 쓴다
// (RefreshSheet 의 kChar 분기). key 는 CharacterKey() 와 같은 문자열이라 그대로 Result 로 나간다.
// 팩 이름·프레임이 다른 번역 단위의 const 전역이라 constexpr 이 안 되고, 정적 초기화 순서를
// 피하려고 함수 지역 정적으로 늦게 채운다.
namespace {
ItemTable CharItems() {
    static constexpr int kMaxCharacters = 8;
    static OutfitItem items[kMaxCharacters] = {};
    static uint8_t count = 0;
    if (count == 0) {
        const int n = SelectableCharacterCount();
        for (int i = 0; i < n && i < kMaxCharacters; i++) {
            const Character c = SelectableCharacter(i);
            const CharacterPack& pack = kCharacterPacks[static_cast<int>(c) - 1];
            const lv_image_dsc_t* frame = pack.sets[pack.default_set].states[kUsageStateIdle].frames[0];
            items[i] = {CharacterKey(c), 0, 0, 0, 0, frame, nullptr, frame};
            count++;
        }
    }
    return {items, count};
}
}  // namespace

// 테이블 길이는 링크 시점 상수가 아니라(다른 번역 단위의 const uint8_t) 여기서 읽는다.
ItemTable WardrobeScreen::TabItems(Tab tab) {
    switch (tab) {
        case Tab::kTop: return BboketerOutfitItems(0);
        case Tab::kHat: return BboketerOutfitItems(1);
        case Tab::kGlasses: return BboketerOutfitItems(2);
        case Tab::kHeadphones: return BboketerOutfitItems(3);
        case Tab::kBoots: return BboketerOutfitItems(4);
        case Tab::kHead: case Tab::kFace: case Tab::kHand: case Tab::kFx:
            return SlotItems(static_cast<OutfitSlot>(tab));
        case Tab::kBg: return {kBboketerSceneBgItems, kBboketerBgItemCount};   // 장면 + 기념일
        case Tab::kWx: return {kWxItems, kWxItemCount};
        default: return CharItems();
    }
}

namespace {
uint8_t SlotCount(Tab tab) {
    const ItemTable t = WardrobeScreen::TabItems(tab);
    return t.items != nullptr ? t.count : 0;
}
}  // namespace

lv_obj_t* WardrobeScreen::MakeBox(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color,
                                  lv_opa_t opa) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(box, opa, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(box, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return box;
}

lv_obj_t* WardrobeScreen::MakePixelLabel(lv_obj_t* parent, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_font(label, &press_start_8, 0);
    lv_obj_set_style_text_letter_space(label, kPixelLetterSpace, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

WardrobeScreen::WardrobeScreen(lv_obj_t* parent, int width, int height, const Outfit& outfit,
                               UsageSky sky, bool locked, uint8_t wx, const lv_image_dsc_t* body)
    : outfit_(outfit), sky_(sky), default_body_(body),
      locked_(locked), wx_(wx), width_(width), height_(height) {
    last_input_tick_ = lv_tick_get();
    const bool scene_sky = sky_ >= kUsageSkyFirstScene && sky_ < kUsageSkyCount;
    if (!scene_sky) sky_ = kUsageSkyFirstScene;   // 배경 그림은 장면에만 있다
    if (CharacterHasScenes()) locked_ = true;     // 장면을 바꿔도 고른 옷을 유지한다.
    root_ = MakeBox(parent, 0, 0, width, height, kBg, LV_OPA_COVER);
    if (CharacterHasScenes()) {
        big_ = true;
        BuildBig();
        ESP_LOGI(kTag, "open big (top=%u hat=%u sky=%d)", outfit_.top, outfit_.hat, static_cast<int>(sky_));
        return;
    }

    // --- 미리보기: 배경 1:1 + 캐릭터 1.5× ---
    bg_image_ = lv_image_create(root_);
    lv_obj_remove_flag(bg_image_, LV_OBJ_FLAG_CLICKABLE);
    figure_ = std::make_unique<Figure>(root_, kScale);
    figure_->SetBodySrc(default_body_);
    figure_->SetOutfit(outfit_);
    RefreshPreview();

    // --- 시트 ---
    sheet_ = MakeBox(root_, 0, kPreviewH, width, height - kPreviewH, kSheet, kSheetOpa);
    MakeBox(sheet_, 0, 0, width, 1, kSheetLine, LV_OPA_COVER);
    const int tab_w = width / TabCount();
    for (int i = 0; i < TabCount(); i++) {
        tab_boxes_[i] = MakeBox(sheet_, i * tab_w + 2, 2, tab_w - 4, kTabRowH - 2, kTabOn, LV_OPA_COVER);
        tab_labels_[i] = MakePixelLabel(sheet_, kTabText);
        lv_label_set_text(tab_labels_[i], TabLabel(i));
        lv_obj_align(tab_labels_[i], LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_update_layout(tab_labels_[i]);
        lv_obj_set_pos(tab_labels_[i], i * tab_w + (tab_w - lv_obj_get_width(tab_labels_[i])) / 2, 8);
    }
    for (int i = 0; i < kPerPage; i++) {
        const int x = kThumbX0 + i * kThumbPitch;
        const int y = kItemRowY - kPreviewH + (kItemRowH - kThumb) / 2;
        thumb_boxes_[i] = MakeBox(sheet_, x, y, kThumb, kThumb, (i % 2) ? kThumbBg2 : kThumbBg, LV_OPA_COVER);
        lv_obj_set_style_outline_color(thumb_boxes_[i], lv_color_hex(kTabOn), 0);
        lv_obj_set_style_outline_pad(thumb_boxes_[i], 0, 0);
        thumb_images_[i] = lv_image_create(thumb_boxes_[i]);
        lv_obj_remove_flag(thumb_images_[i], LV_OBJ_FLAG_CLICKABLE);
    }
    for (int i = 0; i < kMaxDots; i++) {
        dots_[i] = MakeBox(sheet_, 0, kDotY - kPreviewH, 3, 2, kDot, LV_OPA_COVER);
    }

    // --- X · AUTO/LOCK ---
    close_box_ = MakeBox(root_, 4, 4, 24, 24, kSheet, 204);
    lv_obj_t* x_label = MakePixelLabel(close_box_, kTabText);
    lv_label_set_text(x_label, "X");
    lv_obj_center(x_label);
    lock_box_ = MakeBox(root_, kLockX, kLockY, kLockW, kLockH, kChip, LV_OPA_COVER);
    lock_label_ = MakePixelLabel(lock_box_, kChipText);
    lv_obj_center(lock_label_);

    SelectSlot(0);
    RefreshChrome();
    ESP_LOGI(kTag, "open (head=%u face=%u hand=%u fx=%u sky=%d wx=%u %s)", outfit_.head, outfit_.face,
             outfit_.hand, outfit_.fx, static_cast<int>(sky_), wx_, locked_ ? "LOCK" : "AUTO");
}

WardrobeScreen::~WardrobeScreen() {
    figure_.reset();   // root_ 보다 먼저 — 자기 컨테이너를 스스로 지운다
    if (root_ != nullptr) {
        lv_obj_delete(root_);
    }
}

uint8_t WardrobeScreen::current_index(int slot) const {
    switch (kTabs[TabSpecAt(slot)].tab) {
        case Tab::kTop: return outfit_.top;
        case Tab::kHat: return outfit_.hat;
        case Tab::kGlasses: return outfit_.glasses;
        case Tab::kHeadphones: return outfit_.headphones;
        case Tab::kBoots: return outfit_.boots;
        case Tab::kHead: return outfit_.head;
        case Tab::kFace: return outfit_.face;
        case Tab::kHand: return outfit_.hand;
        case Tab::kFx: return outfit_.fx;
        case Tab::kBg:
            return sky_ >= kUsageSkyFirstScene && sky_ < kUsageSkyCount
                       ? static_cast<uint8_t>(sky_ - kUsageSkyFirstScene) : 0;
        case Tab::kWx: return wx_;
        default: return static_cast<uint8_t>(SelectableCharacterIndex(CurrentCharacter()));
    }
}

int WardrobeScreen::page_count(int slot) const {
    const int n = SlotCount(kTabs[TabSpecAt(slot)].tab);
    return n > 0 ? (n + kPerPage - 1) / kPerPage : 1;
}

void WardrobeScreen::SelectSlot(int slot) {
    const int n = TabCount();
    slot_ = (slot % n + n) % n;
    page_ = current_index(slot_) / kPerPage;   // 지금 입은 것이 보이는 페이지로
    const int pages = page_count(slot_);
    if (page_ >= pages) page_ = pages - 1;     // ACT(current 0xFF) 같은 슬롯은 첫 페이지로
    RefreshSheet();
}

void WardrobeScreen::SetPage(int page) {
    const int n = page_count(slot_);
    if (page < 0) page = 0;
    if (page >= n) page = n - 1;
    if (page == page_) return;
    page_ = page;
    RefreshSheet();
}

void WardrobeScreen::Pick(uint8_t index) {
    const Tab tb = tab();
    if (index >= SlotCount(tb)) return;
    if (tb == Tab::kChar) return;   // 착장이 아니다 — OnTouch 가 Result 로 낸다
    switch (tb) {
        case Tab::kTop: outfit_.top = index; break;
        case Tab::kHat: outfit_.hat = index; break;
        case Tab::kGlasses: outfit_.glasses = index; break;
        case Tab::kHeadphones: outfit_.headphones = index; break;
        case Tab::kBoots: outfit_.boots = index; break;
        case Tab::kHead: outfit_.head = index; break;
        case Tab::kFace: outfit_.face = index; break;
        case Tab::kHand: outfit_.hand = index; break;
        case Tab::kFx: outfit_.fx = index; break;
        case Tab::kBg: sky_ = static_cast<UsageSky>(kUsageSkyFirstScene + index); break;
        default: wx_ = index; break;
    }
    // 옷·fx 만 손으로 고르면 LOCK(자동 규칙은 쉰다). BG 는 AUTO 세트의 입구
    // (docs/alive.md E절 — 장면을 고르면 그 세트 통째)라 LOCK 을 걸지 않고,
    // WX 는 자체에 auto 항목이 있는 연출이라 원래 안 걸었다.
    const bool wearable = tb <= Tab::kFx || tb >= Tab::kTop;
    if (CharacterHasScenes()) locked_ = true;
    if (wearable) locked_ = true;
    ESP_LOGI(kTag, "pick %s=%s%s", TabLabel(slot_), TabItems(tb).items[index].key,
             wearable ? " (LOCK)" : "");
    if (tb == Tab::kBg && !locked_) {
        ApplyAutoSetPreview();   // 고른 장면의 세트(옷+wx)를 그 자리에서 입힌다
    }
    figure_->SetOutfit(outfit_);
    RefreshPreview();
    RefreshSheet();
    RefreshChrome();
}

// AUTO 세트(E절): 지금 배경의 세트로 착장·연출 선택을 통째로 바꾼다. 닫을 때
// 그대로 적용·저장되므로 "옷장에서 확정한 수동 착장"이 된다 — 이후 자동 경로
// (날씨 푸시·시각 전환)는 여전히 비저장으로 갈아입힌다. 강수 우선은 자동 경로
// 소관이라 여기선 안 따진다.
void WardrobeScreen::ApplyAutoSetPreview() {
    AutoOutfitSet(sky_, /*precip=*/0, &outfit_, &wx_);
    ESP_LOGI(kTag, "auto set preview: sky=%d head=%u face=%u hand=%u fx=%u wx=%u",
             static_cast<int>(sky_), outfit_.head, outfit_.face, outfit_.hand, outfit_.fx, wx_);
}

// 창: 캐릭터 발을 길(미리보기 y 168)에 두고, 조합이 위로 삐져나가면 그만큼 전체를 내린다.
void WardrobeScreen::RefreshPreview() {
    lv_image_set_src(bg_image_, sky_canvas_.Render(SkyImage(sky_)));
    lv_obj_invalidate(bg_image_);   // src 포인터가 같아도 내용이 바뀌었다
    if (CharacterHasScenes() && sky_ >= kUsageSkyFirstScene && sky_ < kUsageSkyCount) {
        const int scene = BboketerSkyBody(sky_ - kUsageSkyFirstScene);
        figure_->SetBboketerScene(scene);
        const lv_image_dsc_t* body = BboketerBodyImage(scene, outfit_.top);
        figure_->SetBodySrc(body != nullptr ? body
                                            : (CharacterBorrowsScenes() ? default_body_
                                                                        : kBboketerScenePoseAnims[scene].frames[0]));
        figure_->SetItemsHidden(true);  // 장면 소품은 자세 이미지에 포함되어 있다
    } else
    {
        figure_->SetBboketerScene(-1);
        figure_->SetBodySrc(default_body_);
        figure_->SetItemsHidden(false);
    }
    const int body_h = figure_->scaled_body_h();
    const int body_w = figure_->scaled_body_w();
    int body_top = kRoadInPreview - body_h;
    const int overhang = figure_->top_overhang() * kScale / LV_SCALE_NONE;   // ≤ 0
    int shift = 0;
    if (body_top + overhang < kTopMargin) {
        shift = kTopMargin - (body_top + overhang);
    }
    lv_obj_set_pos(bg_image_, 0, kRoadInPreview - kRoadY + shift);
    figure_->SetBodyScreenPos((width_ - body_w) / 2, body_top + shift);
}

void WardrobeScreen::RefreshSheet() {
    const ItemTable table = TabItems(tab());
    const uint8_t count = SlotCount(tab());
    const uint8_t current = current_index(slot_);
    for (int i = 0; i < TabCount(); i++) {
        const bool on = (i == slot_);
        SetHidden(tab_boxes_[i], !on);
        lv_obj_set_style_text_color(tab_labels_[i], lv_color_hex(on ? kTabOnText : kTabText), 0);
    }
    for (int i = 0; i < kPerPage; i++) {
        const int index = page_ * kPerPage + i;
        if (index >= count) {
            SetHidden(thumb_boxes_[i], true);
            continue;
        }
        const OutfitItem& item = table.items[index];
        SetHidden(thumb_boxes_[i], false);
        lv_image_set_src(thumb_images_[i], item.thumb);
        const lv_image_dsc_t* thumb = item.thumb;
        if (thumb == nullptr) {   // 썸네일 없는 칸(none)은 빈 칸으로 둔다
            lv_obj_set_style_outline_width(thumb_boxes_[i], index == current ? 2 : 0, 0);
            continue;
        }
        // CHAR 탭은 전용 썸네일이 없어 대기 프레임(98×65 쯤)을 칸 폭에 맞게 축소한다.
        // 나머지 탭은 이미 36px 로 구운 썸네일이라 배율 1.
        const int tw = static_cast<int>(thumb->header.w);
        const int scale = (tab() == Tab::kChar && tw > kThumb) ? kThumb * LV_SCALE_NONE / tw
                                                               : LV_SCALE_NONE;
        // 피벗을 좌상단으로 못 박는다 — 기본값(이미지 중앙)으로 축소하면 중심이 제자리에
        // 남아 그림이 36px 칸 밖으로 밀려 잘린다(실기기에서 빈 칸으로 보였다).
        lv_image_set_pivot(thumb_images_[i], 0, 0);
        lv_image_set_scale(thumb_images_[i], scale);
        lv_image_set_antialias(thumb_images_[i], false);
        // 배경 썸네일(36×27)은 세로 가운데. 나머지는 36×36 그대로.
        const int th = static_cast<int>(thumb->header.h) * scale / LV_SCALE_NONE;
        lv_obj_set_pos(thumb_images_[i], 0, (kThumb - th) / 2);
        lv_obj_set_style_outline_width(thumb_boxes_[i], index == current ? 2 : 0, 0);
    }
    const int pages = page_count(slot_);
    const int x0 = width_ / 2 - (pages * 6) / 2;
    for (int i = 0; i < kMaxDots; i++) {
        const bool show = pages >= 2 && i < pages;
        SetHidden(dots_[i], !show);
        lv_obj_set_x(dots_[i], x0 + i * 6);
        lv_obj_set_style_bg_color(dots_[i], lv_color_hex(i == page_ ? kDotOn : kDot), 0);
    }
}

void WardrobeScreen::RefreshChrome() {
    lv_label_set_text(lock_label_, locked_ ? "LOCK" : "AUTO");
    lv_obj_set_style_bg_color(lock_box_, lv_color_hex(locked_ ? kTabOn : kChip), 0);
    lv_obj_set_style_text_color(lock_label_, lv_color_hex(locked_ ? kTabOnText : kChipText), 0);
    lv_obj_center(lock_label_);
}

WardrobeScreen::Result WardrobeScreen::OnTouch(const Display::TouchEvent& ev) {
    using Kind = Display::TouchEvent;
    Result r;
    last_input_tick_ = lv_tick_get();
    if (big_) return OnTouchBig(ev);
    const int x = ev.x0, y = ev.y0;
    ESP_LOGI(kTag, "touch kind=%d at (%d,%d) slot=%d page=%d", static_cast<int>(ev.kind), x, y, slot_, page_);
    switch (ev.kind) {
        case Kind::kTap:
            if (x < kCloseHit && y < kCloseHit) {
                ESP_LOGI(kTag, "close (X)");
                r.close = true;
            } else if (x >= kLockHitX && y < kLockHitH) {
                if (CharacterHasScenes()) break;
                locked_ = !locked_;
                ESP_LOGI(kTag, "toggle -> %s", locked_ ? "LOCK" : "AUTO");
                if (!locked_) {
                    // AUTO 로 풀면 지금 배경의 세트가 바로 입혀진다 (E절) —
                    // 라벨만 바뀌면 "아무 일도 안 벌어진" 것처럼 보인다.
                    ApplyAutoSetPreview();
                    figure_->SetOutfit(outfit_);
                    RefreshPreview();
                    RefreshSheet();
                }
                RefreshChrome();
                r.changed = true;
            } else if (y >= kTabHitY0 && y < kTabHitY1) {
                SelectSlot(x * TabCount() / width_);
                ESP_LOGI(kTag, "slot -> %s", TabLabel(slot_));
            } else if (y >= kTabHitY1 && x >= kThumbX0 - 4) {
                const int i = (x - (kThumbX0 - 4)) / kThumbPitch;
                if (i < kPerPage && tab() == Tab::kChar) {
                    // CHAR: 착장이 아니라 "닫고 갈아탄다" — 키를 Result 로 돌려준다.
                    const int index = page_ * kPerPage + i;
                    if (index < SlotCount(Tab::kChar)) {
                        r.character = TabItems(Tab::kChar).items[index].key;
                        ESP_LOGI(kTag, "pick CHAR=%s -> switch", r.character);
                        r.close = true;
                    }
                } else if (i < kPerPage) {
                    Pick(static_cast<uint8_t>(page_ * kPerPage + i));
                    r.changed = true;
                }
            }
            break;
        case Kind::kSwipeLeft: SetPage(page_ + 1); ESP_LOGI(kTag, "page -> %d", page_); break;
        case Kind::kSwipeRight: SetPage(page_ - 1); ESP_LOGI(kTag, "page -> %d", page_); break;
        case Kind::kSwipeUp: SelectSlot(slot_ + 1); ESP_LOGI(kTag, "slot -> %s", TabLabel(slot_)); break;
        case Kind::kSwipeDown: SelectSlot(slot_ - 1); ESP_LOGI(kTag, "slot -> %s", TabLabel(slot_)); break;
        case Kind::kLongPress:
            ESP_LOGI(kTag, "close + screen off (long press)");
            r.close = true;
            r.screen_off = true;
            break;
        default:
            break;
    }
    return r;
}

// --- 큰 버튼 옷장 ----------------------------------------------------------------------------------------
namespace {
constexpr int kBigTabW = 56, kBigTabH = 44, kBigTabX0 = 4;
constexpr int kBigCloseX = 248, kBigCloseW = 68;        // 닫기 칸(그림). 판정은 더 넓다(OnTouchBig)
constexpr int kBigGridY = 52, kBigTileW = 100, kBigTileH = 72, kBigTilePitchX = 106, kBigTilePitchY = 76;
constexpr int kBigBarY = 204, kBigBarH = 34, kBigArrowW = 100;
constexpr int kBigCardH = 146;   // CHAR 탭 카드 높이
constexpr uint32_t kBigTile = 0x2A2018;
constexpr uint32_t kBigButton = 0x3A2C20;
constexpr uint32_t kBigClose = 0xC8503C;
}  // namespace

// WX 칸 다섯 개: 맑음·비·눈·단풍·벚꽃. 값은 kWxItems 번호다.
constexpr uint8_t kBigWx[] = {1, 2, 3, 4, 6};
constexpr int kBigWxCount = sizeof(kBigWx) / sizeof(kBigWx[0]);
const lv_image_dsc_t* const kBigWxThumbs[kBigWxCount] = {&ui_fx_thumb_sun_img, &ui_fx_thumb_rain_img,
                                                         &ui_fx_thumb_snow_img, &ui_fx_thumb_leaf_img,
                                                         &ui_fx_thumb_petal_img};

void WardrobeScreen::BuildBig() {
    for (int i = 0; i < kBigMaxTabs; ++i) {
        big_tab_boxes_[i] = MakeBox(root_, kBigTabX0, 4, kBigTabW, kBigTabH, kBigButton, LV_OPA_COVER);
        lv_obj_set_style_radius(big_tab_boxes_[i], 6, 0);
        big_tab_labels_[i] = MakePixelLabel(big_tab_boxes_[i], kTabText);
        big_tab_images_[i] = lv_image_create(big_tab_boxes_[i]);
        lv_obj_remove_flag(big_tab_images_[i], LV_OBJ_FLAG_CLICKABLE);
        lv_image_set_antialias(big_tab_images_[i], true);
    }
    RefreshBigTabs();
    close_box_ = MakeBox(root_, kBigCloseX, 4, kBigCloseW, kBigTabH, kBigClose, LV_OPA_COVER);
    lv_obj_set_style_radius(close_box_, 6, 0);
    lv_obj_t* x_label = MakePixelLabel(close_box_, 0xFFFFFF);
    lv_obj_set_style_text_font(x_label, &press_start_24, 0);
    lv_label_set_text(x_label, "X");
    lv_obj_center(x_label);
    for (int i = 0; i < kBigTiles; ++i) {
        const int x = kBigTabX0 + (i % 3) * kBigTilePitchX, y = kBigGridY + (i / 3) * kBigTilePitchY;
        big_tiles_[i] = MakeBox(root_, x, y, kBigTileW, kBigTileH, kBigTile, LV_OPA_COVER);
        lv_obj_set_style_radius(big_tiles_[i], 6, 0);
        lv_obj_set_style_clip_corner(big_tiles_[i], true, 0);
        lv_obj_set_style_outline_color(big_tiles_[i], lv_color_hex(kTabOn), 0);
        lv_obj_set_style_outline_pad(big_tiles_[i], 0, 0);
        big_tile_images_[i] = lv_image_create(big_tiles_[i]);
        lv_obj_remove_flag(big_tile_images_[i], LV_OBJ_FLAG_CLICKABLE);
    }
    for (int i = 0; i < 2; ++i) {
        lv_obj_t* box = MakeBox(root_, i == 0 ? kBigTabX0 : width_ - kBigTabX0 - kBigArrowW, kBigBarY, kBigArrowW,
                                kBigBarH, kBigButton, LV_OPA_COVER);
        lv_obj_set_style_radius(box, 6, 0);
        lv_obj_t* label = MakePixelLabel(box, kTabText);
        lv_obj_set_style_text_font(label, &press_start_16, 0);
        lv_label_set_text(label, i == 0 ? "<" : ">");
        lv_obj_center(label);
        (i == 0 ? big_prev_ : big_next_) = box;
    }
    big_page_label_ = MakePixelLabel(root_, kTabText);
    static constexpr const char* kCardNames[3] = {"RONA", "ROZZI", "MANBOK"};
    for (int i = 0; i < 3; ++i) {
        big_cards_[i] = MakeBox(root_, kBigTabX0 + i * kBigTilePitchX, kBigGridY, kBigTileW, kBigCardH, kBigTile, LV_OPA_COVER);
        lv_obj_set_style_radius(big_cards_[i], 6, 0);
        lv_obj_set_style_outline_color(big_cards_[i], lv_color_hex(kTabOn), 0);
        lv_obj_set_style_outline_pad(big_cards_[i], 0, 0);
        big_card_images_[i] = lv_image_create(big_cards_[i]);
        lv_obj_remove_flag(big_card_images_[i], LV_OBJ_FLAG_CLICKABLE);
        lv_image_set_antialias(big_card_images_[i], true);
        const lv_image_dsc_t* art = BboketerCastPose(i, kCastPoseIdle);
        if (art != nullptr) {
            const int w = static_cast<int>(art->header.w), h = static_cast<int>(art->header.h);
            const int scale = LV_MIN(92 * LV_SCALE_NONE / w, 112 * LV_SCALE_NONE / h);
            lv_image_set_src(big_card_images_[i], art);
            lv_image_set_pivot(big_card_images_[i], 0, 0);
            lv_image_set_scale(big_card_images_[i], scale);
            lv_obj_set_pos(big_card_images_[i], (kBigTileW - w * scale / LV_SCALE_NONE) / 2,
                           122 - h * scale / LV_SCALE_NONE);
        }
        big_card_labels_[i] = MakePixelLabel(big_cards_[i], kTabText);
        lv_label_set_text(big_card_labels_[i], kCardNames[i]);
        lv_obj_align(big_card_labels_[i], LV_ALIGN_BOTTOM_MID, 0, -6);
        big_card_checks_[i] = MakeBox(big_cards_[i], kBigTileW - 30, 4, 26, 16, kTabOn, LV_OPA_COVER);
        lv_obj_set_style_radius(big_card_checks_[i], 8, 0);
        lv_obj_t* on = MakePixelLabel(big_card_checks_[i], kTabOnText);
        lv_label_set_text(on, "ON");
        lv_obj_center(on);
    }
    big_hint_ = MakePixelLabel(root_, kTabText);
    lv_label_set_text(big_hint_, "TAP TO TURN ON/OFF");
    lv_obj_align(big_hint_, LV_ALIGN_BOTTOM_MID, 0, -16);
    big_tab_ = 0;
    page_ = BigCurrent() / kBigTiles;
    RefreshBig();
}

int WardrobeScreen::BigCount() const {
    switch (big_tabs_[big_tab_]) {
        case BigTab::kBg: return kBboketerBgItemCount;
        case BigTab::kRona:
        case BigTab::kRozzi:
        case BigTab::kManbok: return kBboketerItemCount;
        case BigTab::kChar: return 3;
        case BigTab::kWx: return kBigWxCount;
    }
    return 0;
}

int WardrobeScreen::BigCurrent() const {
    switch (big_tabs_[big_tab_]) {
        case BigTab::kBg:
            return sky_ >= kUsageSkyFirstScene && sky_ < kUsageSkyCount ? sky_ - kUsageSkyFirstScene : 0;
        case BigTab::kRona:
        case BigTab::kRozzi:
        case BigTab::kManbok:
            return BboketerCastItem(static_cast<int>(big_tabs_[big_tab_]) - static_cast<int>(BigTab::kRona));
        case BigTab::kChar: return 0;
        case BigTab::kWx:
            for (int i = 0; i < kBigWxCount; ++i) {
                if (kBigWx[i] == wx_) return i;
            }
            return 0;   // 옛 저장값(자동·별똥별)은 맑음으로 보인다
    }
    return 0;
}

const lv_image_dsc_t* WardrobeScreen::BigThumb(int index) const {
    switch (big_tabs_[big_tab_]) {
        case BigTab::kBg:
            return index < kBboketerSceneCount ? BboketerSceneImage(index)
                                               : BboketerPickedBgImage(index - kBboketerSceneCount);
        case BigTab::kRona:
        case BigTab::kRozzi:
        case BigTab::kManbok:
            return BboketerCastItems(static_cast<int>(big_tabs_[big_tab_]) - static_cast<int>(BigTab::kRona)).items[index].thumb;
        case BigTab::kChar: return nullptr;
        case BigTab::kWx: return kBigWxThumbs[index];
    }
    return nullptr;
}

// 탭 줄을 켜진 캐릭터에 맞춘다. 옷 탭은 글자가 칸에 안 들어가서 얼굴 그림으로 보인다.
void WardrobeScreen::RefreshBigTabs() {
    const BigTab was = big_tab_count_ > 0 ? big_tabs_[big_tab_] : BigTab::kBg;
    const uint8_t mask = BboketerCastMask();
    big_tab_count_ = 0;
    big_tabs_[big_tab_count_++] = BigTab::kBg;
    big_tabs_[big_tab_count_++] = BigTab::kWx;
    if (mask & 1) big_tabs_[big_tab_count_++] = BigTab::kRona;
    if (mask & 2) big_tabs_[big_tab_count_++] = BigTab::kRozzi;
    if (mask & 4) big_tabs_[big_tab_count_++] = BigTab::kManbok;
    big_tabs_[big_tab_count_++] = BigTab::kChar;
    big_tab_pitch_ = (kBigCloseX - kBigTabX0) / big_tab_count_;
    big_tab_ = 0;
    for (int i = 0; i < big_tab_count_; ++i) {
        if (big_tabs_[i] == was) big_tab_ = i;
    }
    for (int i = 0; i < kBigMaxTabs; ++i) {
        const bool used = i < big_tab_count_;
        SetHidden(big_tab_boxes_[i], !used);
        if (!used) continue;
        lv_obj_set_pos(big_tab_boxes_[i], kBigTabX0 + i * big_tab_pitch_, 4);
        lv_obj_set_size(big_tab_boxes_[i], big_tab_pitch_ - 4, kBigTabH);
        const BigTab tab = big_tabs_[i];
        const bool picture = tab == BigTab::kRona || tab == BigTab::kRozzi || tab == BigTab::kManbok;
        SetHidden(big_tab_labels_[i], picture);
        SetHidden(big_tab_images_[i], !picture);
        if (picture) {
            const lv_image_dsc_t* art = BboketerCastPose(static_cast<int>(tab) - static_cast<int>(BigTab::kRona), kCastPoseIdle);
            if (art != nullptr) {
                const int w = static_cast<int>(art->header.w), h = static_cast<int>(art->header.h);
                const int scale = LV_MIN(36 * LV_SCALE_NONE / h, (big_tab_pitch_ - 8) * LV_SCALE_NONE / w);
                lv_image_set_src(big_tab_images_[i], art);
                lv_image_set_pivot(big_tab_images_[i], 0, 0);
                lv_image_set_scale(big_tab_images_[i], scale);
                lv_obj_set_pos(big_tab_images_[i], (big_tab_pitch_ - 4 - w * scale / LV_SCALE_NONE) / 2,
                               (kBigTabH - h * scale / LV_SCALE_NONE) / 2);
            }
        } else {
            lv_label_set_text(big_tab_labels_[i], tab == BigTab::kBg ? "BG" : tab == BigTab::kWx ? "WX" : "CHAR");
            lv_obj_center(big_tab_labels_[i]);
        }
    }
}

void WardrobeScreen::BigToggle(int who, Result* r) {
    if (who < 0 || who >= 3) return;
    const uint8_t next = BboketerCastMask() ^ static_cast<uint8_t>(1 << who);
    if (next == 0) {
        lv_label_set_text(big_hint_, "KEEP AT LEAST ONE");   // 마지막 한 마리는 끌 수 없다
        return;
    }
    lv_label_set_text(big_hint_, "TAP TO TURN ON/OFF");
    BboketerSetCastMask(next);
    cast_changed_ = true;
    r->changed = true;
    RefreshBigTabs();
    RefreshBig();
    ESP_LOGI(kTag, "cast toggle who=%d -> %u", who, next);
}

void WardrobeScreen::RefreshBig() {
    for (int i = 0; i < big_tab_count_; ++i) {
        const bool on = i == big_tab_;
        lv_obj_set_style_bg_color(big_tab_boxes_[i], lv_color_hex(on ? kTabOn : kBigButton), 0);
        lv_obj_set_style_text_color(big_tab_labels_[i], lv_color_hex(on ? kTabOnText : kTabText), 0);
    }
    const bool cards = big_tabs_[big_tab_] == BigTab::kChar;
    const uint8_t cast = BboketerCastMask();
    for (int i = 0; i < 3; ++i) {
        SetHidden(big_cards_[i], !cards);
        if (!cards) continue;
        const bool on = (cast & (1 << i)) != 0;
        lv_obj_set_style_outline_width(big_cards_[i], on ? 3 : 0, 0);
        lv_obj_set_style_image_opa(big_card_images_[i], on ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_set_style_text_color(big_card_labels_[i], lv_color_hex(on ? 0xFFE6C8 : 0x6E5A48), 0);
        SetHidden(big_card_checks_[i], !on);
    }
    const bool wx_tab = big_tabs_[big_tab_] == BigTab::kWx;
    SetHidden(big_hint_, !cards && !wx_tab);
    if (wx_tab) lv_label_set_text(big_hint_, "OUTDOOR SCENES ONLY");
    if (cards) {
        lv_label_set_text(big_hint_, "TAP TO TURN ON/OFF");
        for (lv_obj_t* tile : big_tiles_) SetHidden(tile, true);
        SetHidden(big_prev_, true);
        SetHidden(big_next_, true);
        SetHidden(big_page_label_, true);
        return;
    }
    const int count = BigCount();
    const int pages = count > 0 ? (count + kBigTiles - 1) / kBigTiles : 1;
    if (page_ >= pages) page_ = pages - 1;
    if (page_ < 0) page_ = 0;
    const int current = BigCurrent();
    for (int i = 0; i < kBigTiles; ++i) {
        const int index = page_ * kBigTiles + i;
        SetHidden(big_tiles_[i], index >= count);
        if (index >= count) continue;
        const lv_image_dsc_t* thumb = BigThumb(index);
        lv_image_set_src(big_tile_images_[i], thumb);
        if (thumb != nullptr) {
            const int w = static_cast<int>(thumb->header.w), h = static_cast<int>(thumb->header.h);
            const int scale = LV_MIN((kBigTileW - 4) * LV_SCALE_NONE / w, (kBigTileH - 4) * LV_SCALE_NONE / h);
            lv_image_set_pivot(big_tile_images_[i], 0, 0);
            lv_image_set_scale(big_tile_images_[i], scale);
            lv_image_set_antialias(big_tile_images_[i], true);
            lv_obj_set_pos(big_tile_images_[i], (kBigTileW - w * scale / LV_SCALE_NONE) / 2,
                           (kBigTileH - h * scale / LV_SCALE_NONE) / 2);
        }
        lv_obj_set_style_outline_width(big_tiles_[i], index == current ? 3 : 0, 0);
    }
    SetHidden(big_prev_, pages < 2);
    SetHidden(big_next_, pages < 2);
    lv_label_set_text_fmt(big_page_label_, "%d/%d", page_ + 1, pages);
    lv_obj_set_pos(big_page_label_, width_ / 2 - 12, kBigBarY + kBigBarH / 2 - 4);
    SetHidden(big_page_label_, pages < 2);
}

void WardrobeScreen::BigPick(int index, Result* r) {
    if (index < 0 || index >= BigCount()) return;
    switch (big_tabs_[big_tab_]) {
        case BigTab::kBg:
            sky_ = static_cast<UsageSky>(kUsageSkyFirstScene + index);
            break;
        case BigTab::kRona:
        case BigTab::kRozzi:
        case BigTab::kManbok:
            BboketerSetCastItem(static_cast<int>(big_tabs_[big_tab_]) - static_cast<int>(BigTab::kRona), index);
            sky_ = kUsageSkyVintage;   // 옷은 옷방에서 입는다 — 고르면 옷방으로 간다
            break;
        case BigTab::kChar:
            return;
        case BigTab::kWx:
            wx_ = kBigWx[index];
            break;
    }
    locked_ = true;
    r->changed = true;
    ESP_LOGI(kTag, "pick tab=%d index=%d (top=%u hat=%u sky=%d)", big_tab_, index, outfit_.top, outfit_.hat,
             static_cast<int>(sky_));
    RefreshBig();
}

WardrobeScreen::Result WardrobeScreen::OnTouchBig(const Display::TouchEvent& ev) {
    using Kind = Display::TouchEvent;
    Result r;
    const int x = ev.x0, y = ev.y0;
    const int pages = (BigCount() + kBigTiles - 1) / kBigTiles;
    ESP_LOGI(kTag, "big touch kind=%d at (%d,%d) tab=%d page=%d", static_cast<int>(ev.kind), x, y, big_tab_, page_);
    switch (ev.kind) {
        case Kind::kTap:
            if (x >= kBigCloseX - 6 && y < kBigGridY) {   // 닫기: 오른쪽 위 넓게(ny 「X 가 안 눌린다」)
                ESP_LOGI(kTag, "close (X)");
                r.close = true;
            } else if (y < kBigGridY) {
                const int tab = (x - kBigTabX0 + 2) / big_tab_pitch_;
                if (tab >= 0 && tab < big_tab_count_ && tab != big_tab_) {
                    big_tab_ = tab;
                    page_ = BigCurrent() / kBigTiles;
                    RefreshBig();
                }
            } else if (big_tabs_[big_tab_] == BigTab::kChar) {
                if (y < kBigGridY + kBigCardH) BigToggle(LV_CLAMP(0, (x - kBigTabX0 + 3) / kBigTilePitchX, 2), &r);
            } else if (y >= kBigBarY - 4) {
                if (x < width_ / 2 - 30 && page_ > 0) {
                    --page_;
                    RefreshBig();
                } else if (x > width_ / 2 + 30 && page_ + 1 < pages) {
                    ++page_;
                    RefreshBig();
                }
            } else {
                const int col = LV_CLAMP(0, (x - kBigTabX0 + 3) / kBigTilePitchX, 2);
                const int row = LV_CLAMP(0, (y - kBigGridY + 2) / kBigTilePitchY, 1);
                BigPick(page_ * kBigTiles + row * 3 + col, &r);
            }
            break;
        case Kind::kSwipeLeft:
            if (page_ + 1 < pages) ++page_;
            RefreshBig();
            break;
        case Kind::kSwipeRight:
            if (page_ > 0) --page_;
            RefreshBig();
            break;
        case Kind::kLongPress:
            r.close = true;
            r.screen_off = true;
            break;
        default:
            break;
    }
    return r;
}
