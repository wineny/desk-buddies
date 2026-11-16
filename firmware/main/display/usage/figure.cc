#include "figure.h"

#include <initializer_list>

#include "character_pack.h"
#include "bboketer_scenes.h"

// --- 슬롯 표 (item_tables.h) ---------------------------------------------------

ItemTable SlotItems(OutfitSlot slot) {
    const int i = slot < kSlotCount ? slot : kSlotFx;
    return {ActivePack().items[i], ActivePack().item_counts[i]};
}

const OutfitItem* SlotItemAt(OutfitSlot slot, uint8_t index) {
    const ItemTable t = SlotItems(slot);
    if (t.items == nullptr || index >= t.count) return nullptr;
    return &t.items[index];
}

const char* SlotJsonKey(OutfitSlot slot) {
    static constexpr const char* kKeys[kSlotCount] = {"head", "face", "hand", "neck"};
    return kKeys[slot < kSlotCount ? slot : kSlotFx];
}

namespace {

const OutfitItem* ItemOf(OutfitSlot slot, uint8_t index) { return SlotItemAt(slot, index); }

void Hide(lv_obj_t* obj, bool hidden) {
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace

Figure::Figure(lv_obj_t* parent, int scale256) : scale_(scale256) {
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, S(kW), S(kH));
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_scrollbar_mode(root_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(root_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // z 순서: 몸 → neck → 얼굴 → 손 → 머리 → 장면 옷
    body_ = MakeImage();
    face_ = MakeImage();
    hand_ = MakeImage();
    head_ = MakeImage();
    fx_ = MakeImage();
    for (auto& layer : bboketer_layers_) layer = MakeImage();
    lv_obj_move_to_index(fx_, 1);   // fx 자리 = neck. 목도리·턱받이는 몸 바로 위, 얼굴·손·머리 아래
    Relayout();
}

Figure::~Figure() {
    if (root_ != nullptr) {
        lv_obj_delete(root_);
    }
}

lv_obj_t* Figure::MakeImage() {
    lv_obj_t* img = lv_image_create(root_);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    if (scale_ != LV_SCALE_NONE) {
        lv_image_set_pivot(img, 0, 0);
        lv_image_set_scale(img, scale_);
        lv_image_set_antialias(img, false);
    }
    Hide(img, true);
    return img;
}

void Figure::SetBodyPos(int x, int y) {
    body_y_ = y;
    lv_obj_set_pos(root_, S(x - kPadX), S(y - kPadTop));
}

void Figure::SetBodyScreenPos(int px, int py) {
    body_y_ = py * LV_SCALE_NONE / scale_;
    lv_obj_set_pos(root_, px - S(kPadX), py - S(kPadTop));
}

int Figure::body_x() const {
    return lv_obj_get_x(root_) * LV_SCALE_NONE / scale_ + kPadX;
}

void Figure::SetBodySrc(const lv_image_dsc_t* src, int lift) {
    lift_ = lift;
    lineart_ = IsLineart(src);
    // 라인아트(1배)는 옛 프레임(98×64 를 scale_ 배) 자리에 그 크기로 그린다. 높이 계산은 옛 프레임 기준.
    body_h_ = lineart_ ? 64 : (src != nullptr ? static_cast<int>(src->header.h) : kBodyH);
    lv_image_set_scale(body_, lineart_ ? LV_SCALE_NONE : scale_);
    lv_image_set_antialias(body_, lineart_);
    lv_image_set_src(body_, src);
    Hide(body_, src == nullptr);
    Relayout();
}

void Figure::SetFacing(bool right) {
    if (right_ == right) return;
    right_ = right;
    Relayout();
}

void Figure::SetOutfit(const Outfit& outfit) {
    outfit_ = outfit;
    Relayout();
}

void Figure::SetBboketerScene(int scene) {
    if (bboketer_scene_ == scene) return;
    bboketer_scene_ = scene;
    Relayout();
}

void Figure::SetHidden(bool hidden) { Hide(root_, hidden); }

void Figure::SetItemShift(int dx, int dy) {
    if (item_dx_ == dx && item_dy_ == dy) return;
    item_dx_ = dx;
    item_dy_ = dy;
    Relayout();
}

void Figure::SetItemsHidden(bool hidden) {
    if (items_hidden_ == hidden) return;
    items_hidden_ = hidden;
    Relayout();
}

int Figure::top_overhang() const {
    int top = 0;
    const OutfitItem* items[] = {
        ItemOf(kSlotFace, outfit_.face),
        ItemOf(kSlotHand, outfit_.hand),
        ItemOf(kSlotHead, outfit_.head),
        ItemOf(kSlotFx, outfit_.fx),
    };
    for (const OutfitItem* it : items) {
        if (it == nullptr || it->img == nullptr) continue;
        const int y = it->dy + (kBodyH - body_h_);
        if (y < top) top = y;
    }
    return top;
}

// 팩 아이템 하나를 놓는다: 프레임(98×64) 좌상단 기준 1px 셀 + 프레임별 점프 오프셋. 반전본 없음.
// 프레임은 발 밑변 맞춤이라 좌상단이 kPadTop + (kBodyH - 프레임 높이) 에 있다.
void Figure::Place(lv_obj_t* img, const OutfitItem* item) {
    if (item == nullptr || item->img == nullptr || items_hidden_) {
        lv_image_set_src(img, nullptr);
        Hide(img, true);
        return;
    }
    lv_image_set_src(img, item->img);
    lv_obj_set_pos(img, S(kPadX + item->dx + item_dx_),
                   S(kPadTop + (kBodyH - body_h_) + item->dy + item_dy_ - lift_));
    Hide(img, false);
}

void Figure::Relayout() {
    // 몸: 발 밑변을 kPadTop + kBodyH 에 맞춘다. 점프 프레임은 몸이 프레임 안에서 떠
    // 있으므로 아이템만 lift 만큼 올린다(Place).
    lv_obj_set_pos(body_, S(kPadX), S(kPadTop + kBodyH - body_h_));
    if (lineart_) {
        // 그림의 아래 가운데 = 옛 프레임의 아래 가운데. 라인아트는 옷이 그림에 들어 있어 덧입힘을 다 숨긴다.
        const lv_image_dsc_t* src = static_cast<const lv_image_dsc_t*>(lv_image_get_src(body_));
        const int w = static_cast<int>(src->header.w), h = static_cast<int>(src->header.h);
        lv_obj_set_pos(body_, S(kPadX) + S(kBodyW) / 2 - w / 2, S(kPadTop + kBodyH) - h);
        for (lv_obj_t* o : {face_, hand_, head_, fx_}) Hide(o, true);
        for (auto& layer : bboketer_layers_) Hide(layer, true);
        return;
    }
    Place(face_, ItemOf(kSlotFace, outfit_.face));
    Place(hand_, ItemOf(kSlotHand, outfit_.hand));
    Place(head_, ItemOf(kSlotHead, outfit_.head));
    Place(fx_, ItemOf(kSlotFx, outfit_.fx));
    const int options[4] = {outfit_.hat, outfit_.glasses, outfit_.headphones, outfit_.boots};
    for (int kind = 0; kind < 4; ++kind) {
        lv_obj_t* layer = bboketer_layers_[kind];
        // 스쿠버 고글은 장면 소품으로 몸 그림에 이미 들어 있다.
        const int option = bboketer_scene_ == 6 && kind == 1 ? 0 : options[kind];
        const lv_image_dsc_t* src = BboketerOverlayImage(bboketer_scene_, kind, option);
        lv_image_set_src(layer, src);
        lv_obj_set_pos(layer, S(kPadX), S(kPadTop + kBodyH - body_h_));
        Hide(layer, src == nullptr);
    }
}
