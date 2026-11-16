#pragma once

// 날씨 판: 윗줄 시계·날씨 칸을 누르면 뜬다. 시간별(지금 + 다음 5시간)·이번 주(오늘부터 7일) 두 탭.
// 옷장처럼 열 때 만들고 닫을 때 통째로 지운다 — LcdDisplay 가 unique_ptr 로 들고 있다.
// 여기서는 그리기와 판정만 하고, 닫기는 Result 로 돌려준다. 20초 무입력 닫기는 LcdDisplay 의 1초 틱이 본다.
// 예보는 보드가 직접 받는다(weather_fetcher.cc). 맥 push {"weather":…} 는 시계 칸만 바꾼다.

#include <lvgl.h>

#include <cstdint>

#include "display.h"
#include "usage_screen.h"

// Open-Meteo 예보 한 벌. 기온은 반올림한 ℃.
struct WeatherForecast {
    static constexpr int kHours = 5;   // 「NOW」 다음 시간 수
    static constexpr int kDays = 7;
    struct Hour {
        int8_t hour = 0;               // 0~23 (보드 시간대)
        int8_t icon = kUsageWeatherNone;
        int8_t temp = 0;
    };
    struct Day {
        int8_t wday = 0;               // 0 = 일요일 … 6 = 토요일
        int8_t icon = kUsageWeatherNone;
        int8_t tmax = 0;
        int8_t tmin = 0;
    };
    bool valid = false;
    int8_t now_icon = kUsageWeatherNone;
    int8_t now_temp = 0;
    uint8_t hour_count = 0;
    uint8_t day_count = 0;
    Hour hours[kHours];
    Day days[kDays];
};

// kind(clear|partly|cloud|rain|snow) + 밤 → 그림. 모르는 kind 는 kUsageWeatherNone.
UsageWeatherIcon UsageWeatherIconFor(const char* kind, bool night);

class WeatherPanel {
public:
    enum Tab : uint8_t { kTabHourly = 0, kTabWeekly, kTabCount };

    struct Result {
        bool close = false;
    };

    // parent 위를 덮는 판을 만든다. korean_font 가 nullptr 이면(글꼴 assets 가 없는 빌드) 영어로 쓴다.
    // 반드시 DisplayLockGuard 안에서.
    WeatherPanel(lv_obj_t* parent, const lv_font_t* korean_font, const WeatherForecast& data, Tab tab);
    ~WeatherPanel();

    Result OnTouch(const Display::TouchEvent& ev);
    void SetTab(Tab tab);
    void SetData(const WeatherForecast& data);   // 열린 채로 새 예보가 오면 다시 그린다
    // 마지막 입력 뒤 흐른 ms.
    uint32_t idle_ms() const { return lv_tick_elaps(last_input_tick_); }

private:
    void BuildTabs();
    void Rebuild();
    void BuildHourly();
    void BuildWeekly();
    // 열 하나: 위 글자(시각·요일) + 그림 + 기온(최저는 아래 줄). cx 는 판 기준 가운데.
    void BuildColumn(int cx, const char* top, bool top_korean, int icon, int temp, bool has_min, int tmin);
    void BuildTemp(int cx, int y, int temp, uint32_t color);

    lv_obj_t* root_ = nullptr;
    lv_obj_t* tabs_[kTabCount] = {};
    lv_obj_t* tab_labels_[kTabCount] = {};
    lv_obj_t* body_ = nullptr;   // 탭 아래 내용. 탭·예보가 바뀌면 비우고 다시 짓는다
    const lv_font_t* korean_font_ = nullptr;
    WeatherForecast data_;
    Tab tab_ = kTabHourly;
    uint32_t last_input_tick_ = 0;
};
