#pragma once

// 흐르는 줄(docs/gift-board-plan.md): 대기 화면 맨 아래, 하트와 옷걸이 사이. D-day 와 팁을 번갈아
// 흘린다. 글꼴·팁은 assets 파티션에 있다(font_galmuri11_12_1.bin, tips.json). 둘 중 글꼴이 없으면
// 조용히 숨는다 — 단일 팩 빌드에는 들어 있지 않다. 날짜는 코드에 쓰지 않고 NVS 에만 둔다.

#include <lvgl.h>

#include <ctime>
#include <string>
#include <vector>

// 기념일 종류. UsageSpecialDayToday 가 돌려주고 assets 파일 이름과 짝이다.
enum class UsageSpecialDay : int8_t { kNone = -1, kBirthday = 0, kAnniversary, kSeollal, kChuseok, kPepero, kChristmas };

// 오늘이 무슨 날인가. birthday·birthday2·anniversary 는 "MM-DD", 없으면 "". 시계가 안 맞았으면 kNone.
UsageSpecialDay UsageSpecialDayToday(const std::string& birthday, const std::string& birthday2,
                                     const std::string& anniversary);
// 날짜 문자열 검사. due 는 "YYYY-MM-DD", 기념일은 "MM-DD". 빈 문자열은 지우기라 참이다.
bool UsageDateValid(const char* text, bool with_year);

class UsageTicker {
public:
    void Create(lv_obj_t* parent, int x, int y, int w, int h);
    void SetDue(const std::string& due);          // "YYYY-MM-DD" 또는 ""
    void SetNote(const char* text);               // 기념일 인사. nullptr 이면 없음
    void SetVisible(bool visible);
    void SetPaused(bool paused);                  // 화면이 꺼진 동안 타이머를 쉰다
    bool Hit(int x, int y) const;
    // 한글 글꼴(assets). 아직 없거나 이 빌드에 안 구웠으면 nullptr. 날씨 판도 같이 쓴다.
    const lv_font_t* font() { LoadAssets(); return font_; }
    void PauseForTap();
    // 만복이 육아일기: 최근 일기 한 줄(「엄마 일기 · …」). "" 이면 없음. fresh 면 다음 줄로 바로 낸다(새 일기).
    void SetDiaryLine(const std::string& line, bool fresh);
    // 지금 흐르는 줄이 일기인가(누르면 일기 판을 연다).
    bool showing_diary() const { return showing_diary_; }
    int due_days() const { return due_days_; }

private:
    struct Tip {
        int week_from;
        int week_to;
        int slot;     // 0 아침, 1 점심, 2 저녁, -1 언제나
        std::string text;
    };

    bool LoadAssets();
    std::string NextLine();
    void StartLine();
    void Refresh();
    static void OnTick(lv_timer_t* timer);

    lv_obj_t* box_ = nullptr;
    lv_obj_t* label_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    const lv_font_t* font_ = nullptr;
    std::vector<Tip> tips_;
    bool tried_assets_ = false;
    bool visible_ = false;
    bool paused_ = false;
    int width_ = 0;
    int x_ = 0;
    int text_w_ = 0;
    int line_n_ = 0;
    int hold_ticks_ = 0;
    int asset_wait_ticks_ = 0;
    int asset_tries_ = 0;
    int area_[4] = {};
    int due_days_ = 0;      // 1970-01-01 부터 센 날. 0 이면 예정일 없음
    std::string note_;
    std::string diary_;
    bool diary_fresh_ = false;
    bool showing_diary_ = false;
};
