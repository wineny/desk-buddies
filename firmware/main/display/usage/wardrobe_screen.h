#pragma once

// 옷장 화면 (docs/wardrobe.md, 시안 docs/mockups/wardrobe_ui.html 후보 B "시트").
//
//   0,0   320×176  미리보기: 배경 1:1(창을 길에 맞춰 위로 민다) + 캐릭터 1.5×
//   0,176 320×64   시트: 탭 줄 22(HEAD FACE HAND FX BG) + 아이템 줄 40(썸네일 36×36 × 7)
//   4,4            X (히트 36×36)          264,4  AUTO/LOCK 칩 (히트 60×28)
//   y 237          페이지 점
//
// 열릴 때 만들고 닫힐 때 통째로 지운다 — LcdDisplay 가 unique_ptr 로 들고 있다.
// 여기서는 판정과 그리기만 하고, 결과(옷·하늘·잠금·닫기)는 Result 로 돌려준다.
// 배경 1.5× 는 lv_image 소프트웨어 변환 비용이 커서 캐릭터만 1.5× 다.
// 탭은 HEAD FACE HAND NECK BG WX CHAR, 미리보기 몸은 팩 idle 프레임, 아이템 표는
// ActivePack().items (item_tables.h). 장면 팩은 큰 옷장(BuildBig)으로 연다.
// CHAR 탭(docs/character-pack.md "런타임 선택")은 캐릭터를 고른다 — 착장이 아니라
// 닫으면서 Result 로 돌려주는 항목이고, 팩이 하나뿐이면 빠진다.

#include <lvgl.h>

#include <cstdint>
#include <memory>

#include "figure.h"
#include "display.h"
#include "usage_screen.h"

class WardrobeScreen {
public:
    // HEAD FACE HAND NECK BG WX CHAR (+ 장면 옷 탭 TOP HAT EYE HP BOOT)
    // 8개는 press_start_8 로 1px 여유로 들어간다("CHAR" 35px, 칸 320/8-4 = 36px) — 5글자는 넘친다.
    static constexpr int kMaxTabs = 12;
    static constexpr int kPerPage = 7;

    // 탭 종류. kHead~kFx 는 OutfitSlot 과 번호가 같다(착장 슬롯). 팩의 NECK 은 kFx 자리.
    enum class Tab : uint8_t {
        kHead = 0, kFace, kHand, kFx, kBg, kWx, kChar,
        kTop, kHat, kGlasses, kHeadphones, kBoots,
    };

    struct Result {
        bool close = false;
        bool screen_off = false;   // 롱프레스: 닫고 화면도 끈다
        bool changed = false;      // 옷·하늘·잠금 중 하나라도 바뀜
        // CHAR 탭 선택 — 닫고 이 캐릭터로 갈아탄다 (CharacterKey 문자열, 정적).
        // 옷장을 그 자리에서 다시 짜지 않고 닫는 이유: 미리보기 몸(Figure)도 아이템
        // 표도 캐릭터에 묶여 있어, 닫고 LcdDisplay 가 갈아탄 뒤 다시 열게 하는 쪽이 단순하다.
        const char* character = nullptr;
    };

    // parent 위를 덮는 패널을 만든다. 반드시 DisplayLockGuard 안에서.
    // body: 미리보기 몸(정지 프레임) — 팩 idle 프레임 0.
    WardrobeScreen(lv_obj_t* parent, int width, int height, const Outfit& outfit, UsageSky sky,
                   bool locked, uint8_t wx, const lv_image_dsc_t* body = nullptr);
    ~WardrobeScreen();

    Result OnTouch(const Display::TouchEvent& ev);

    const Outfit& outfit() const { return outfit_; }
    bool cast_changed() const { return cast_changed_; }   // CHAR 탭에서 켜고 껐다
    UsageSky sky() const { return sky_; }
    bool locked() const { return locked_; }
    uint8_t wx() const { return wx_; }   // 날씨 연출 (kWxItems 인덱스, 0=auto)
    // 마지막 입력 뒤 흐른 ms. 10초 무입력 닫기는 LcdDisplay 의 1초 틱이 본다.
    uint32_t idle_ms() const { return lv_tick_elaps(last_input_tick_); }
    // 탭의 아이템 표 (착장 슬롯은 item_tables.h SlotItems — 팩 아이템).
    static ItemTable TabItems(Tab tab);

private:
    struct TabSpec {
        const char* label;
        Tab tab;
    };
    static const TabSpec kTabs[kMaxTabs];
    // 지금 캐릭터에서 그 탭을 보여줄지. 팩이 하나뿐이면 CHAR 이 빠진다.
    static bool TabVisible(Tab t);
    static int TabCount();                     // 보이는 탭 수
    // 보이는 slot 번째 탭의 kTabs 인덱스. slot_ 은 **보이는** 순번이다.
    static int TabSpecAt(int slot);
    // 탭 라벨. 4번째 슬롯은 목(NECK)이다.
    static const char* TabLabel(int slot);
    Tab tab() const { return kTabs[TabSpecAt(slot_)].tab; }

    uint8_t current_index(int slot) const;
    int page_count(int slot) const;
    void SelectSlot(int slot);
    void SetPage(int page);
    void Pick(uint8_t index);
    void ApplyAutoSetPreview();   // AUTO 세트(E절)를 옷장 선택에 즉시 반영
    void RefreshPreview();
    void RefreshSheet();
    void RefreshChrome();
    lv_obj_t* MakeBox(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, lv_opa_t opa);
    lv_obj_t* MakePixelLabel(lv_obj_t* parent, uint32_t color);

    // --- 큰 버튼 옷장(장면 팩, ny 11/6 「버튼이 작아 안 눌린다, X 가 특히」). docs/lineart-firmware.md ---
    // 위 줄 = 탭(배경·옷·로찌 옷·캐릭터) + 오른쪽 위 큰 닫기, 가운데 = 3×2 큰 칸, 아래 = 큰 ◀ ▶.
    enum class BigTab : uint8_t { kBg, kRona, kRozzi, kManbok, kChar, kWx };   // 옷 탭 번호 - kRona = 캐릭터 번호
    static constexpr int kBigTiles = 6;
    static constexpr int kBigMaxTabs = 6;
    void BuildBig();
    Result OnTouchBig(const Display::TouchEvent& ev);
    void RefreshBig();
    int BigCount() const;
    int BigCurrent() const;
    const lv_image_dsc_t* BigThumb(int index) const;
    void BigPick(int index, Result* r);
    bool big_ = false;
    BigTab big_tabs_[kBigMaxTabs] = {};
    int big_tab_count_ = 0;
    int big_tab_ = 0;
    lv_obj_t* big_tab_boxes_[kBigMaxTabs] = {};
    lv_obj_t* big_tab_labels_[kBigMaxTabs] = {};
    lv_obj_t* big_tab_images_[kBigMaxTabs] = {};
    lv_obj_t* big_cards_[3] = {};
    lv_obj_t* big_card_images_[3] = {};
    lv_obj_t* big_card_labels_[3] = {};
    lv_obj_t* big_card_checks_[3] = {};
    lv_obj_t* big_hint_ = nullptr;
    int big_tab_pitch_ = 60;
    bool cast_changed_ = false;
    void RefreshBigTabs();
    void BigToggle(int who, Result* r);
    lv_obj_t* big_tiles_[kBigTiles] = {};
    lv_obj_t* big_tile_images_[kBigTiles] = {};
    lv_obj_t* big_prev_ = nullptr;
    lv_obj_t* big_next_ = nullptr;
    lv_obj_t* big_page_label_ = nullptr;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* bg_image_ = nullptr;
    std::unique_ptr<Figure> figure_;
    lv_obj_t* sheet_ = nullptr;
    lv_obj_t* tab_boxes_[kMaxTabs] = {};
    lv_obj_t* tab_labels_[kMaxTabs] = {};
    lv_obj_t* thumb_boxes_[kPerPage] = {};
    lv_obj_t* thumb_images_[kPerPage] = {};
    static constexpr int kMaxDots = 6;   // 페이지 점 최대 개수
    lv_obj_t* dots_[kMaxDots] = {};
    lv_obj_t* close_box_ = nullptr;
    lv_obj_t* lock_box_ = nullptr;
    lv_obj_t* lock_label_ = nullptr;

    Outfit outfit_;
    UsageSky sky_;
    const lv_image_dsc_t* default_body_ = nullptr;
    SkyCanvas sky_canvas_;   // 미리보기 배경용 — 대기 화면 것과 버퍼를 안 나눠 쓴다
    bool locked_;
    uint8_t wx_ = 0;
    int slot_ = 0;
    int page_ = 0;
    int width_, height_;
    uint32_t last_input_tick_ = 0;
};
