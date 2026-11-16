#pragma once

// 캐릭터 한 마리 (docs/wardrobe-impl.md 0·2절, docs/character-pack.md "items").
//
//   root_ (lv_obj, 투명, 고정 크기)          ← 걷기 anim 은 이것만 x 로 옮긴다
//    ├ body_  lv_image  팩 프레임 / 라인아트 몸
//    ├ fx_    lv_image  ┐ neck
//    ├ face_  lv_image  │ OutfitItem. 프레임 좌상단 기준 1px 앵커
//    ├ hand_  lv_image  │
//    ├ head_  lv_image  ┘
//    └ bboketer_layers_  장면 옷(모자·안경·헤드폰·신발)
//
// 컨테이너는 아이템이 몸 밖으로 삐져나갈 최대치만큼 여유(kPadX·kPadTop)를 두고
// 몸을 그 안에 놓는다 — 조합이 바뀔 때마다 컨테이너 크기를 재는 대신 크기를 못
// 박아 두어 anim·히트박스 계산이 단순하다.
//
// scale256 = 384 면 1.5× (옷장 미리보기). 자식 lv_image 마다 scale·pivot(0,0)
// 을 걸고 자리도 1.5 배로 놓는다. antialias 는 끈다 — 픽셀 아트. 라인아트 몸은
// 1배 그림이라 그 크기 그대로 그리고 덧입힘을 다 숨긴다.
//
// 아이템은 반전본이 없어 방향(SetFacing)을 무시하며, 프레임별 점프 오프셋(SetItemShift)만큼
// 같이 움직인다. fx 슬롯 자리는 neck(목도리·턱받이)이라 z 를 몸 바로 위로 내린다.

#include <lvgl.h>

#include <cstdint>

#include "character.h"
#include "item_tables.h"

// 슬롯별 아이템 인덱스. 0 = 없음. 테이블은 팩(item_tables.h SlotItems).
struct Outfit {
    uint8_t head = 0;
    uint8_t face = 0;
    uint8_t hand = 0;
    uint8_t fx = 0;
    // 뽀케터 전용 착장. 기존 팩 아이템 인덱스와 별도로 저장한다.
    uint8_t top = 0;
    uint8_t hat = 0;
    uint8_t glasses = 0;
    uint8_t headphones = 0;
    uint8_t boots = 0;
};

class Figure {
public:
    static constexpr int kBodyW = 98;   // 몸 박스 (팩 프레임 98×64 를 발 밑변으로 맞춘다)
    static constexpr int kBodyH = 65;
    // 아이템 여유: 좌우 26px, 위 66px.
    static constexpr int kPadX = 26;
    static constexpr int kPadTop = 66;
    static constexpr int kW = kBodyW + 2 * kPadX;
    static constexpr int kH = kBodyH + kPadTop;

    Figure(lv_obj_t* parent, int scale256 = LV_SCALE_NONE);
    ~Figure();

    lv_obj_t* obj() const { return root_; }

    // 몸 좌상단(스케일 전 좌표계, 부모 기준)이 (x, y) 에 오도록 컨테이너를 놓는다.
    void SetBodyPos(int x, int y);
    // 같은 것을 부모의 실제 픽셀 좌표로. 1.5× 미리보기에서 반올림 없이 놓을 때.
    void SetBodyScreenPos(int px, int py);
    int scaled_body_w() const { return S(kBodyW); }
    int scaled_body_h() const { return S(kBodyH); }
    // 컨테이너의 현재 x 에서 계산한 몸 좌상단 x (스케일 1 기준).
    int body_x() const;
    int body_y() const { return body_y_; }

    // 몸 프레임. 높이가 다른 프레임(느낌표가 위로 붙은 반응 프레임)은 발 밑변을
    // 맞춘다. lift 는 그 프레임 안에서 몸이 떠 있는 픽셀(점프) — 발은 프레임이
    // 알아서 제자리고, 아이템만 그만큼 올려 몸을 따라 뜨게 한다.
    void SetBodySrc(const lv_image_dsc_t* src, int lift = 0);

    void SetFacing(bool right);
    bool facing_right() const { return right_; }
    void SetOutfit(const Outfit& outfit);
    void SetBboketerScene(int scene);
    const Outfit& outfit() const { return outfit_; }
    void SetHidden(bool hidden);
    // 프레임에 구워진 점프만큼 아이템을 옮긴다(PackAnim.offsets).
    void SetItemShift(int dx, int dy);
    // 아이템 전부 숨김 — 포즈 컷(PackAnim.items=false)·미니카(ride) 동안.
    void SetItemsHidden(bool hidden);
    bool items_hidden() const { return items_hidden_; }

    // 조합이 몸 위로 얼마나 올라가는지 (스케일 전 픽셀, 0 이하). 옷장 미리보기 창 계산용.
    int top_overhang() const;

private:
    int S(int v) const { return v * scale_ / LV_SCALE_NONE; }
    lv_obj_t* MakeImage();
    void Place(lv_obj_t* img, const OutfitItem* item);
    void Relayout();

    lv_obj_t* root_ = nullptr;
    lv_obj_t* body_ = nullptr;
    lv_obj_t* face_ = nullptr;
    lv_obj_t* hand_ = nullptr;
    lv_obj_t* head_ = nullptr;
    lv_obj_t* fx_ = nullptr;
    lv_obj_t* bboketer_layers_[4] = {};
    int bboketer_scene_ = -1;
    bool lineart_ = false;   // 몸이 라인아트(1배 I8) 그림이다 — 덧입힘 없이 그 크기로 그린다
    Outfit outfit_;
    bool right_ = true;
    bool items_hidden_ = false;
    int item_dx_ = 0, item_dy_ = 0;
    int scale_ = LV_SCALE_NONE;
    int body_y_ = 0;   // SetBodyPos 로 받은 y (컨테이너 y 는 이것 - kPadTop)
    int lift_ = 0;
    int body_h_ = kBodyH;
};
