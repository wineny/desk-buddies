#pragma once

// 만복이 메뉴(docs/manbok-diary.md). 일기를 받는 보드(NVS diary token 있음)에서 하트를 꾹 누르면 문구 패널
// 대신 뜬다. 6칸: 육아일기 · 초음파 사진 / 엄마 건강 · 출산 준비 / 메시지 보내기 · 놀러가기.
//
//   육아일기     메뉴를 닫고 일기 판(diary_panel)을 연다 — LcdDisplay 가 Result.open_diary 로 처리한다
//   초음파 사진  진료 한 번 = 한 줄(새 것 위, 한 쪽 2번), 사진 4장 썸네일. 누르면 화면 가득, 옆으로 밀면 다음 장
//   엄마 건강    오늘 칼로리·카페인·몸무게 카드 + 끼니별 음식(주의 음식 노란 !) + 이번 주 운동
//   출산 준비    출산일·D-day, 안 끝난 일을 마감 가까운 순으로. 줄을 누르면 제목 전체
//   메시지 보내기·놀러가기  옛 문구 패널의 왼쪽·오른쪽 칸. 누르면 Result.poke_phrase / Result.trip
//
// 모든 칸 화면: < = 메뉴, X = 닫기(초음파 크게 보기의 X 만 시안대로 목록으로). 무입력 닫기(60초)는 LcdDisplay 의 1초 틱이 본다.
// 열 때 만들고 닫을 때 통째로 지운다. 사진·자료는 직접 받지 않는다 — Hooks 로 부탁하고 Set* 로 받는다.

#include <lvgl.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "diary_data.h"
#include "display.h"
#include "manbok_data.h"

class ManbokPanel {
public:
    enum class Screen : uint8_t { kMenu, kUltrasound, kHealth, kPrep, kMessage, kVisit };
    struct Result {
        bool close = false;
        bool open_diary = false;   // 육아일기 칸: 메뉴를 닫고 일기 판을 연다
        int poke_phrase = 0;       // 1~5: 그 문구를 보낸다(메시지 보내기)
        int trip = -1;             // 0 놀러가기 · 1 놀러오기 · 2 집으로 가기
    };
    struct Hooks {
        std::function<void(const std::string& url, const std::string& key)> need_photo;
        std::function<void()> want_parts;           // 세 갈래를 받아 달라(5분 안에 받은 건 받는 쪽이 건너뛴다)
        std::function<void(bool on[3])> trip_on;    // 지금 누를 수 있는 놀러가기 칸
    };

    // 반드시 DisplayLockGuard 안에서. diary_count: 받은 일기 수, 아직 못 받았으면 -1.
    ManbokPanel(lv_obj_t* parent, const lv_font_t* font, const ManbokData& data, int diary_count,
                const std::vector<DiaryPhotoPtr>& photos, Hooks hooks, Screen screen);
    ~ManbokPanel();

    Result OnTouch(const Display::TouchEvent& ev);
    void Open(Screen screen);
    void SetData(const ManbokData& data);
    void SetDiaryCount(int n);
    void SetPhoto(const DiaryPhotoPtr& photo);
    void PhotoFailed(const std::string& key);
    uint32_t idle_ms() const { return lv_tick_elaps(last_input_tick_); }

    // 시험용: "menu" / "us:0" / "usphoto:3" / "health:0" / "prep:1" / "prepitem:4" / "message" / "visit"
    std::string ViewName() const;

private:
    enum class Mode : uint8_t { kMenu, kUsList, kUsPhoto, kHealth, kPrep, kPrepItem, kMessage, kVisit };
    struct Placed {   // 엄마 건강 줄 안의 한 조각
        int x;
        int w;
        std::string text;
        uint32_t color;
        bool badge;
    };
    struct HLine {
        std::string label;
        uint32_t label_color;
        std::vector<Placed> parts;
        bool group_end;
    };

    void Rebuild();
    void BuildMenu();
    void BuildUsList();
    void BuildUsPhoto();
    void BuildHealth();
    void BuildPrep();
    void BuildPrepItem();
    void BuildMessage();
    void BuildVisit();
    void LayoutHealth();
    void BuildHeader(const char* title, bool back, bool close);
    void BuildFrame(bool menu_style);
    void MakeArrows(int page, int count, int up_y, int down_y);
    lv_obj_t* MakeLabel(lv_obj_t* parent, int x, int y, const char* text, uint32_t color, int width = 0);
    lv_obj_t* MakeCentered(lv_obj_t* parent, int y, const char* text, uint32_t color);
    lv_obj_t* MakeImage(lv_obj_t* parent, int x, int y, const DiaryImage& im);
    void MakeScaled(const lv_image_dsc_t* dsc, int cx, int cy, bool dim);
    const DiaryPhoto* PhotoAt(int flat);
    int PhotoTotal() const;
    bool FlatToVisit(int flat, int* visit, int* index) const;
    int PageCount() const;
    void Turn(int delta);
    void TurnPhoto(int delta);

    lv_obj_t* parent_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* full_ = nullptr;
    const lv_font_t* font_ = nullptr;
    ManbokData data_;
    int diary_count_ = -1;
    Hooks hooks_;
    Mode mode_ = Mode::kMenu;
    int page_ = 0;
    int photo_ = 0;       // 초음파 크게 보기: 모든 진료를 이은 몇 번째 사진
    int prep_item_ = 0;
    bool trip_on_[3] = {};
    std::vector<HLine> health_lines_;
    std::vector<DiaryPhotoPtr> photos_;
    std::vector<std::string> asked_;
    std::vector<std::unique_ptr<lv_image_dsc_t>> dscs_;
    uint32_t last_input_tick_ = 0;
};
