#include "weather_panel.h"

#include <esp_log.h>

#include <cstdio>
#include <cstring>

LV_FONT_DECLARE(press_start_8)

// 판 그림(art/weather_icons.c, tools/gen_weather_icons.py). 순서 = UsageWeatherIcon.
extern "C" {
extern const lv_image_dsc_t weather_icon_clear_20_img;
extern const lv_image_dsc_t weather_icon_partly_20_img;
extern const lv_image_dsc_t weather_icon_cloud_20_img;
extern const lv_image_dsc_t weather_icon_rain_20_img;
extern const lv_image_dsc_t weather_icon_snow_20_img;
extern const lv_image_dsc_t weather_icon_moon_20_img;
}

namespace {

constexpr const char* kTag = "WeatherPanel";

// 좌표는 판(화면 8,8 에서 304×224) 기준이다. 시안: 날씨 판 mock3.py.
constexpr int kMargin = 8;
constexpr int kRadius = 8;
constexpr lv_opa_t kOpa = 235;                      // 92%
constexpr uint32_t kBg = 0x0E1222;
constexpr int kTabX = 8, kTabY = 8, kTabW = 69, kTabH = 21, kTabStep = 74, kTabRadius = 5;
constexpr uint32_t kTabOn = 0xF88848, kTabOnText = 0x14141E;
constexpr uint32_t kTabOff = 0x303C4A, kTabOffText = 0xDCDCE6;
constexpr int kCloseX = 272, kCloseW = 25;
constexpr uint32_t kClose = 0xDC503C;
constexpr int kTitleY = 40;
constexpr uint32_t kTitle = 0xC8C8D2;
constexpr int kColLeft = 4, kColSpan = 296;         // 열을 나눌 가로 폭
constexpr int kSepTop = 68, kSepBottom = 198;
constexpr uint32_t kSep = 0x283042;
constexpr int kTopPixelY = 76, kTopKoreanY = 72;   // 열 위 글자: 픽셀 폰트 / 한글
constexpr int kIconPx = 20, kIconY = 104;
constexpr int kTempY = 142, kMinY = 158;
constexpr uint32_t kText = 0xFFFFFF;
constexpr uint32_t kMinText = 0x82AAE6;
constexpr int kDegPx = 4;                           // ° 대신 그리는 네모 고리 한 변
constexpr int kPixelW = 8;                          // press_start_8 은 고정폭 8px(자간 0)
// 손가락 판정은 그림보다 넉넉히 (화면 좌표).
constexpr int kTabHitBottom = kMargin + kTabY + kTabH + 10;
constexpr int kCloseHitLeft = kMargin + kCloseX - 16, kCloseHitBottom = kMargin + kTabY + kTabH + 16;

const lv_image_dsc_t* Icon20(int icon) {
    static const lv_image_dsc_t* const kIcons[kUsageWeatherIconCount] = {
        &weather_icon_clear_20_img, &weather_icon_partly_20_img, &weather_icon_cloud_20_img,
        &weather_icon_rain_20_img,  &weather_icon_snow_20_img,   &weather_icon_moon_20_img,
    };
    return (icon >= 0 && icon < kUsageWeatherIconCount) ? kIcons[icon] : nullptr;
}

lv_obj_t* MakeBox(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return o;
}

// 가로 w 칸 가운데 정렬 한 줄.
lv_obj_t* MakeCenteredLabel(lv_obj_t* parent, int x, int y, int w, const char* text, const lv_font_t* font,
                            uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, w);
    lv_obj_set_pos(label, x, y);
    return label;
}

}  // namespace

UsageWeatherIcon UsageWeatherIconFor(const char* kind, bool night) {
    if (kind == nullptr) return kUsageWeatherNone;
    if (strcmp(kind, "rain") == 0) return kUsageWeatherRain;
    if (strcmp(kind, "snow") == 0) return kUsageWeatherSnow;
    if (strcmp(kind, "clear") == 0) return night ? kUsageWeatherMoon : kUsageWeatherClear;
    if (strcmp(kind, "partly") == 0) return night ? kUsageWeatherCloud : kUsageWeatherPartly;
    if (strcmp(kind, "cloud") == 0) return kUsageWeatherCloud;
    return kUsageWeatherNone;
}

WeatherPanel::WeatherPanel(lv_obj_t* parent, const lv_font_t* korean_font, const WeatherForecast& data, Tab tab)
    : korean_font_(korean_font), data_(data), tab_(tab) {
    const int w = lv_obj_get_width(parent), h = lv_obj_get_height(parent);
    root_ = MakeBox(parent, kMargin, kMargin, w - kMargin * 2, h - kMargin * 2, kBg, kRadius);
    lv_obj_set_style_bg_opa(root_, kOpa, 0);
    BuildTabs();
    lv_obj_t* close = MakeBox(root_, kCloseX, kTabY, kCloseW, kTabH, kClose, kTabRadius);
    MakeCenteredLabel(close, 0, (kTabH - 8) / 2, kCloseW, "X", &press_start_8, kText);
    body_ = lv_obj_create(root_);
    lv_obj_remove_style_all(body_);
    lv_obj_set_size(body_, w - kMargin * 2, h - kMargin * 2);
    lv_obj_remove_flag(body_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    last_input_tick_ = lv_tick_get();
    Rebuild();
    ESP_LOGI(kTag, "open (tab %d, %s)", tab_, data_.valid ? "forecast" : "no forecast yet");
}

WeatherPanel::~WeatherPanel() {
    if (root_ != nullptr) {
        lv_obj_delete(root_);
    }
}

void WeatherPanel::BuildTabs() {
    static const char* const kKorean[kTabCount] = {"시간별", "이번 주"};
    static const char* const kAscii[kTabCount] = {"HOUR", "WEEK"};
    for (int i = 0; i < kTabCount; i++) {
        tabs_[i] = MakeBox(root_, kTabX + i * kTabStep, kTabY, kTabW, kTabH, kTabOff, kTabRadius);
        const bool korean = korean_font_ != nullptr;
        const int text_h = korean ? lv_font_get_line_height(korean_font_) : 8;
        tab_labels_[i] = MakeCenteredLabel(tabs_[i], 0, (kTabH - text_h) / 2, kTabW, korean ? kKorean[i] : kAscii[i],
                                           korean ? korean_font_ : &press_start_8, kTabOffText);
    }
}

void WeatherPanel::SetTab(Tab tab) {
    if (tab >= kTabCount) return;
    tab_ = tab;
    Rebuild();
}

void WeatherPanel::SetData(const WeatherForecast& data) {
    data_ = data;
    Rebuild();
}

void WeatherPanel::Rebuild() {
    for (int i = 0; i < kTabCount; i++) {
        const bool on = i == tab_;
        lv_obj_set_style_bg_color(tabs_[i], lv_color_hex(on ? kTabOn : kTabOff), 0);
        lv_obj_set_style_text_color(tab_labels_[i], lv_color_hex(on ? kTabOnText : kTabOffText), 0);
    }
    lv_obj_clean(body_);
    const int w = lv_obj_get_width(root_);
    const bool korean = korean_font_ != nullptr;
    const lv_font_t* text_font = korean ? korean_font_ : &press_start_8;
    if (!data_.valid) {
        MakeCenteredLabel(body_, 0, 100, w, korean ? "날씨를 받는 중" : "LOADING", text_font, kTitle);
        return;
    }
    const char* title = tab_ == kTabHourly ? (korean ? "오늘 · 1시간마다" : "NEXT HOURS")
                                           : (korean ? "최고 / 최저" : "HIGH / LOW");
    MakeCenteredLabel(body_, 0, kTitleY, w, title, text_font, kTitle);
    if (tab_ == kTabHourly) {
        BuildHourly();
    } else {
        BuildWeekly();
    }
}

void WeatherPanel::BuildHourly() {
    const int n = 1 + data_.hour_count;
    const int cw = kColSpan / n;
    for (int i = 0; i < n; i++) {
        const int cx = kColLeft + cw * i + cw / 2;
        if (i > 0) MakeBox(body_, kColLeft + cw * i, kSepTop, 1, kSepBottom - kSepTop, kSep, 0);
        if (i == 0) {
            BuildColumn(cx, "NOW", false, data_.now_icon, data_.now_temp, false, 0);
            continue;
        }
        const WeatherForecast::Hour& h = data_.hours[i - 1];
        // 12시간제, 오전·오후를 앞에: 「PM3」 「AM12」.
        char top[8];
        snprintf(top, sizeof(top), "%s%d", h.hour < 12 ? "AM" : "PM", h.hour % 12 == 0 ? 12 : h.hour % 12);
        BuildColumn(cx, top, false, h.icon, h.temp, false, 0);
    }
}

void WeatherPanel::BuildWeekly() {
    static const char* const kKorean[7] = {"일", "월", "화", "수", "목", "금", "토"};
    static const char* const kAscii[7] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    const int n = data_.day_count;
    if (n == 0) return;
    const int cw = kColSpan / n;
    const bool korean = korean_font_ != nullptr;
    for (int i = 0; i < n; i++) {
        const int cx = kColLeft + cw * i + cw / 2;
        if (i > 0) MakeBox(body_, kColLeft + cw * i, kSepTop, 1, kSepBottom - kSepTop, kSep, 0);
        const WeatherForecast::Day& d = data_.days[i];
        const int wday = d.wday >= 0 && d.wday < 7 ? d.wday : 0;
        BuildColumn(cx, korean ? kKorean[wday] : kAscii[wday], korean, d.icon, d.tmax, true, d.tmin);
    }
}

void WeatherPanel::BuildColumn(int cx, const char* top, bool top_korean, int icon, int temp, bool has_min,
                               int tmin) {
    const int cw = 48;   // 글자 칸 폭(열 폭보다 좁아도 가운데만 맞으면 된다)
    MakeCenteredLabel(body_, cx - cw / 2, top_korean ? kTopKoreanY : kTopPixelY, cw, top,
                      top_korean ? korean_font_ : &press_start_8, kText);
    const lv_image_dsc_t* dsc = Icon20(icon);
    if (dsc != nullptr) {
        lv_obj_t* img = lv_image_create(body_);
        lv_image_set_src(img, dsc);
        lv_obj_set_pos(img, cx - kIconPx / 2, kIconY);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    }
    BuildTemp(cx, kTempY, temp, kText);
    if (has_min) BuildTemp(cx, kMinY, tmin, kMinText);
}

// 「11°」: 숫자 라벨 + 4px 네모 고리. 픽셀 폰트가 고정폭이라 글자 수로 폭을 안다.
void WeatherPanel::BuildTemp(int cx, int y, int temp, uint32_t color) {
    char text[8];
    snprintf(text, sizeof(text), "%d", temp);
    const int text_w = static_cast<int>(strlen(text)) * kPixelW;
    const int x = cx - (text_w + 1 + kDegPx) / 2;
    lv_obj_t* label = lv_label_create(body_);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &press_start_8, 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    lv_obj_t* deg = lv_obj_create(body_);
    lv_obj_remove_style_all(deg);
    lv_obj_set_pos(deg, x + text_w + 1, y);
    lv_obj_set_size(deg, kDegPx, kDegPx);
    lv_obj_set_style_border_color(deg, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(deg, 1, 0);
    lv_obj_remove_flag(deg, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
}

WeatherPanel::Result WeatherPanel::OnTouch(const Display::TouchEvent& ev) {
    using Kind = Display::TouchEvent;
    Result r;
    last_input_tick_ = lv_tick_get();
    if (ev.kind == Kind::kSwipeLeft || ev.kind == Kind::kSwipeRight) {
        SetTab(ev.kind == Kind::kSwipeLeft ? kTabWeekly : kTabHourly);   // 밀어서 탭 넘기기
        return r;
    }
    if (ev.kind != Kind::kTap) return r;
    if (ev.x0 >= kCloseHitLeft && ev.y0 < kCloseHitBottom) {
        r.close = true;
        return r;
    }
    if (ev.y0 < kTabHitBottom) {
        for (int i = 0; i < kTabCount; i++) {
            const int left = kMargin + kTabX + i * kTabStep;
            if (ev.x0 >= left - 3 && ev.x0 < left + kTabW + 3) {
                if (i != tab_) SetTab(static_cast<Tab>(i));
                break;
            }
        }
    }
    return r;
}
