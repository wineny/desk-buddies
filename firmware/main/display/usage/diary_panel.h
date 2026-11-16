#pragma once

// 만복이 일기 판(docs/manbok-diary.md). 하트를 꾹 눌러 연 문구 패널 맨 위 「육아일기 읽기」나,
// 흐르는 줄에 일기가 지나갈 때 누르면 뜬다. 날씨 판과 같은 틀(화면 8,8 에서 304×224)이다.
//
//   목록   최근 일기 3개씩. 줄을 누르면 그 글. ▲▼·위아래로 밀기로 넘긴다.
//   글     전체 글을 쪽으로 나눠 보여 준다. 사진이 있으면 1쪽 왼쪽에 96×128, 누르면 화면 가득.
//   사진   화면 가득. 여러 장이면 왼쪽·오른쪽을 누르거나 밀어 넘기고(「2/3」), 가운데를 누르면 글로.
//          목록·글의 작은 사진에는 나머지 장 수를 「+2」로 붙인다.
//
// 열 때 만들고 닫을 때 통째로 지운다 — LcdDisplay 가 unique_ptr 로 들고 있다. 여기서는 그리기와
// 판정만 하고, 닫기는 Result 로 돌려준다. 무입력 닫기는 LcdDisplay 의 1초 틱이 본다.
// 사진은 직접 받지 않는다 — need_photo 로 LcdDisplay 에 부탁하고, 오면 SetPhoto 로 받는다.

#include <lvgl.h>

#include <functional>
#include <string>
#include <vector>

#include "diary_data.h"
#include "display.h"

class DiaryPanel {
public:
    struct Result {
        bool close = false;
        bool menu = false;   // 만복이 메뉴에서 연 판의 목록 < — 메뉴로 돌아간다
    };
    using NeedPhoto = std::function<void(const std::string& url, const std::string& key)>;

    // 반드시 DisplayLockGuard 안에서. font 가 nullptr 이면(글꼴 assets 가 없는 빌드) 만들지 않는 게 맞다.
    // due_days: 1970-01-01 부터 센 예정일(0 이면 없음). open_entry >= 0 이면 그 글을 바로 연다.
    // photos: 이미 받아 둔 사진(첫 그리기 전에 넣어 다시 부탁하지 않게).
    // from_menu: 만복이 메뉴에서 열었다 — 목록 왼쪽 위에 < 가 생기고 누르면 Result.menu.
    DiaryPanel(lv_obj_t* parent, const lv_font_t* font, const DiaryData& data, int due_days, int open_entry,
               const std::vector<DiaryPhotoPtr>& photos, NeedPhoto need_photo, bool from_menu = false);
    ~DiaryPanel();

    Result OnTouch(const Display::TouchEvent& ev);
    void SetData(const DiaryData& data);        // 열린 채로 새 목록이 오면(새 일기) 다시 그린다
    void SetPhoto(const DiaryPhotoPtr& photo);  // 부탁한 사진이 왔다
    void PhotoFailed(const std::string& key);   // 못 받았다 — 다음 그리기 때 다시 부탁한다
    uint32_t idle_ms() const { return lv_tick_elaps(last_input_tick_); }

    // 시험용: 지금 무엇을 보고 있나. "list:0" / "read:2:1" / "photo:2:0".
    std::string ViewName() const;

private:
    enum class Mode : uint8_t { kList, kRead, kPhoto };
    struct Line {
        int x;
        int y;
        std::string text;
    };

    void Rebuild();
    void BuildList();
    void BuildRead();
    void BuildPhoto();
    void Layout(const DiaryEntry& e);   // 글을 쪽·줄로 나눈다(pages_)
    void OpenEntry(int index);
    void Turn(int delta);               // 목록 쪽 / 글 쪽 넘기기
    int PageCount() const;
    const DiaryPhoto* PhotoAt(const DiaryEntry& e, int index);   // 있으면 그것, 없으면 부탁하고 nullptr
    const DiaryPhoto* PhotoFor(const DiaryEntry& e) { return PhotoAt(e, 0); }
    void TurnPhoto(int delta);          // 크게 보기에서 앞뒤 사진
    void MakeMoreBadge(lv_obj_t* parent, int right, int bottom, int more);   // 「+N」

    lv_obj_t* MakeLabel(lv_obj_t* parent, int x, int y, const char* text, uint32_t color, int width = 0);
    int MakeChip(lv_obj_t* parent, int x, int y, const char* text, uint32_t bg, uint32_t fg);
    void MakeArrows(int page, int count);
    lv_obj_t* MakeImage(lv_obj_t* parent, int x, int y, const uint8_t* pixels, int w, int h, lv_image_dsc_t* dsc);

    lv_obj_t* parent_ = nullptr;
    lv_obj_t* root_ = nullptr;       // 판(목록·글)
    lv_obj_t* body_ = nullptr;       // 판 안 내용. 바뀔 때마다 비우고 다시 짓는다
    lv_obj_t* full_ = nullptr;       // 사진 크게 보기(화면 전체)
    const lv_font_t* font_ = nullptr;
    DiaryData data_;
    int due_days_ = 0;
    NeedPhoto need_photo_;
    Mode mode_ = Mode::kList;
    int list_page_ = 0;
    int entry_ = -1;
    int page_ = 0;
    int photo_index_ = 0;            // 크게 보기에서 몇 번째 사진
    std::vector<std::vector<Line>> pages_;
    bool entry_has_photo_ = false;
    std::vector<DiaryPhotoPtr> photos_;   // 이 판이 쓰는 사진(최근 몇 장)
    std::vector<std::string> asked_;      // 이미 부탁한 사진 키(같은 걸 또 부탁하지 않게)
    std::vector<std::unique_ptr<lv_image_dsc_t>> dscs_;   // 그린 사진의 서술자. lv_image 가 가리키므로 다시 짓기 전까지 산다
    uint32_t last_input_tick_ = 0;
    bool from_menu_ = false;
};
