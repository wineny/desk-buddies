#include "usage_ticker.h"

#include <cJSON.h>
#include <cbin_font.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>

#include "assets.h"

namespace {

constexpr const char* kTag = "UsageTicker";
constexpr const char* kFontAsset = "font_galmuri11_12_1.bin";
constexpr const char* kTipsAsset = "tips.json";
constexpr uint32_t kTickMs = 50;
constexpr int kStepPx = 2;                  // 40px/초
constexpr int kTapHoldTicks = 4000 / kTickMs;
constexpr uint32_t kBoxColor = 0x0E0C10;
constexpr uint32_t kTextColor = 0xF0E7DD;
constexpr uint32_t kDueColor = 0xFFD878;
constexpr uint32_t kDiaryColor = 0xFFBEC8;   // 일기 줄은 분홍(문구 패널 「육아일기 읽기」와 같은 계열)
constexpr lv_opa_t kBoxOpa = 210;
constexpr int kPregnancyDays = 280;         // 예정일 기준 40주
constexpr int kAssetRetryTicks = 1000 / kTickMs;   // 글꼴을 1초마다 다시 찾는다
constexpr int kAssetRetryMax = 30;                 // 30초 찾아도 없으면 글꼴 없는 빌드로 보고 쉰다

// 설·추석 양력 날짜. 한국천문연구원 음력 표(2029·2030 은 월력요항 발표 전 계산값) 기준.
struct LunarDay {
    int16_t year;
    int8_t seol_m, seol_d, chu_m, chu_d;
};
constexpr LunarDay kLunarDays[] = {
    {2026, 2, 17, 9, 25}, {2027, 2, 7, 9, 15}, {2028, 1, 27, 10, 3},
    {2029, 2, 13, 9, 22}, {2030, 2, 3, 9, 12},
};

// 그레고리력 날짜 → 1970-01-01 부터 센 날(시간대와 무관하게 계산한다).
int DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool LocalNow(struct tm* out) {
    const time_t now = time(nullptr);
    localtime_r(&now, out);
    return out->tm_year >= 2024 - 1900;   // SNTP 전이면 거짓
}

int SlotOfHour(int hour) {
    if (hour >= 5 && hour < 11) return 0;
    if (hour >= 11 && hour < 17) return 1;
    return 2;
}

int SlotByName(const char* name) {
    if (name == nullptr) return -1;
    if (strcmp(name, "morning") == 0) return 0;
    if (strcmp(name, "noon") == 0) return 1;
    if (strcmp(name, "evening") == 0) return 2;
    return -1;
}

}  // namespace

bool UsageDateValid(const char* text, bool with_year) {
    if (text == nullptr) return false;
    if (text[0] == '\0') return true;
    int y = 2000, m = 0, d = 0, used = 0;
    const int n = with_year ? sscanf(text, "%4d-%2d-%2d%n", &y, &m, &d, &used)
                            : sscanf(text, "%2d-%2d%n", &m, &d, &used);
    return n == (with_year ? 3 : 2) && text[used] == '\0' && m >= 1 && m <= 12 && d >= 1 && d <= 31 &&
           y >= 2000 && y < 2100;
}

UsageSpecialDay UsageSpecialDayToday(const std::string& birthday, const std::string& birthday2,
                                     const std::string& anniversary) {
    struct tm now;
    if (!LocalNow(&now)) return UsageSpecialDay::kNone;
    const int month = now.tm_mon + 1, day = now.tm_mday, year = now.tm_year + 1900;
    char today[16];
    snprintf(today, sizeof(today), "%02d-%02d", month, day);
    if (birthday == today || birthday2 == today) return UsageSpecialDay::kBirthday;
    if (anniversary == today) return UsageSpecialDay::kAnniversary;
    if (month == 11 && day == 11) return UsageSpecialDay::kPepero;
    if (month == 12 && day == 25) return UsageSpecialDay::kChristmas;
    for (const LunarDay& l : kLunarDays) {
        if (l.year != year) continue;
        if (l.seol_m == month && l.seol_d == day) return UsageSpecialDay::kSeollal;
        if (l.chu_m == month && l.chu_d == day) return UsageSpecialDay::kChuseok;
    }
    return UsageSpecialDay::kNone;
}

void UsageTicker::Create(lv_obj_t* parent, int x, int y, int w, int h) {
    width_ = w;
    area_[0] = x;
    area_[1] = y;
    area_[2] = x + w;
    area_[3] = y + h;
    box_ = lv_obj_create(parent);
    lv_obj_remove_style_all(box_);
    lv_obj_set_pos(box_, x, y);
    lv_obj_set_size(box_, w, h);
    lv_obj_set_style_bg_color(box_, lv_color_hex(kBoxColor), 0);
    lv_obj_set_style_bg_opa(box_, kBoxOpa, 0);
    lv_obj_set_style_clip_corner(box_, false, 0);
    lv_obj_remove_flag(box_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(box_, LV_OBJ_FLAG_HIDDEN);
    label_ = lv_label_create(box_);
    lv_label_set_text(label_, "");
    lv_label_set_long_mode(label_, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_color(label_, lv_color_hex(kTextColor), 0);
}

bool UsageTicker::LoadAssets() {
    if (tried_assets_) return font_ != nullptr;
    void* data = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData(kFontAsset, data, size) || data == nullptr) {
        return false;   // assets 가 늦게 올라올 수 있다 — 다음에 또 본다
    }
    tried_assets_ = true;
    font_ = cbin_font_create(static_cast<uint8_t*>(data));
    if (font_ == nullptr) {
        ESP_LOGW(kTag, "font load failed");
        return false;
    }
    lv_obj_set_style_text_font(label_, font_, 0);
    if (Assets::GetInstance().GetAssetData(kTipsAsset, data, size) && data != nullptr) {
        cJSON* root = cJSON_ParseWithLength(static_cast<const char*>(data), size);
        const int n = cJSON_IsArray(root) ? cJSON_GetArraySize(root) : 0;
        for (int i = 0; i < n; i++) {
            const cJSON* item = cJSON_GetArrayItem(root, i);
            const cJSON* text = cJSON_GetObjectItem(item, "text");
            const cJSON* from = cJSON_GetObjectItem(item, "week_from");
            const cJSON* to = cJSON_GetObjectItem(item, "week_to");
            const cJSON* slot = cJSON_GetObjectItem(item, "slot");
            if (!cJSON_IsString(text) || !cJSON_IsNumber(from) || !cJSON_IsNumber(to)) continue;
            tips_.push_back({from->valueint, to->valueint,
                             SlotByName(cJSON_IsString(slot) ? slot->valuestring : nullptr), text->valuestring});
        }
        cJSON_Delete(root);
    }
    ESP_LOGI(kTag, "font loaded, %u tips", static_cast<unsigned>(tips_.size()));
    return true;
}

void UsageTicker::SetDue(const std::string& due) {
    int y = 0, m = 0, d = 0;
    due_days_ = (!due.empty() && sscanf(due.c_str(), "%4d-%2d-%2d", &y, &m, &d) == 3) ? DaysFromCivil(y, m, d) : 0;
    line_n_ = 0;
    Refresh();
}

void UsageTicker::SetNote(const char* text) {
    note_ = text != nullptr ? text : "";
    line_n_ = 0;
    Refresh();
}

void UsageTicker::SetVisible(bool visible) {
    if (visible == visible_ && (timer_ != nullptr || tried_assets_)) {
        return;   // SetStats 가 대기 화면을 자주 다시 그린다 — 흐르던 줄을 처음으로 돌리지 않는다
    }
    visible_ = visible;
    Refresh();
}

void UsageTicker::SetPaused(bool paused) {
    paused_ = paused;
    if (timer_ != nullptr) {
        paused ? lv_timer_pause(timer_) : lv_timer_resume(timer_);
    }
}

bool UsageTicker::Hit(int x, int y) const {
    return box_ != nullptr && !lv_obj_has_flag(box_, LV_OBJ_FLAG_HIDDEN) && x >= area_[0] && x < area_[2] &&
           y >= area_[1] && y < area_[3];
}

void UsageTicker::PauseForTap() { hold_ticks_ = kTapHoldTicks; }

void UsageTicker::SetDiaryLine(const std::string& line, bool fresh) {
    if (line == diary_ && !fresh) return;
    diary_ = line;
    diary_fresh_ = fresh && !line.empty();
    if (diary_fresh_ && visible_ && font_ != nullptr && !showing_diary_) StartLine();   // 새 일기는 지금 흐르는 줄을 끊고 바로
}

// 예정일 줄과 팁을 번갈아 낸다. 기념일 인사가 있으면 맨 앞. 낼 것이 없으면 빈 문자열.
std::string UsageTicker::NextLine() {
    struct tm now;
    if (!LocalNow(&now)) return note_;
    const int today = DaysFromCivil(now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
    const int n = line_n_++;
    lv_obj_set_style_text_color(label_, lv_color_hex(kTextColor), 0);
    showing_diary_ = false;
    // 일기: 새로 온 일기는 바로, 그 밖에는 네 줄에 한 번(D-day·팁 사이에 낀다).
    if (!diary_.empty() && (diary_fresh_ || n % 4 == 3)) {
        diary_fresh_ = false;
        showing_diary_ = true;
        lv_obj_set_style_text_color(label_, lv_color_hex(kDiaryColor), 0);
        return diary_;
    }
    if (!note_.empty() && n % 3 == 0) return note_;
    const bool want_due = n % 2 == 0;
    if (due_days_ != 0 && want_due && today <= due_days_) {
        char text[64];
        snprintf(text, sizeof(text), "만복이 만나기까지 D-%d", due_days_ - today);
        lv_obj_set_style_text_color(label_, lv_color_hex(kDueColor), 0);
        return text;
    }
    if (due_days_ == 0 || today > due_days_) return note_;
    const int week = (kPregnancyDays - (due_days_ - today)) / 7;
    const int slot = SlotOfHour(now.tm_hour);
    std::vector<const Tip*> found;
    for (const Tip& t : tips_) {
        if (week >= t.week_from && week <= t.week_to && (t.slot == slot || t.slot == -1)) {
            found.push_back(&t);
        }
    }
    if (found.empty()) {
        char text[64];
        snprintf(text, sizeof(text), "만복이 만나기까지 D-%d", due_days_ - today);
        lv_obj_set_style_text_color(label_, lv_color_hex(kDueColor), 0);
        return text;
    }
    return found[static_cast<size_t>(today + n / 2) % found.size()]->text;
}

void UsageTicker::StartLine() {
    const std::string line = NextLine();
    if (line.empty()) {
        lv_obj_add_flag(box_, LV_OBJ_FLAG_HIDDEN);
        text_w_ = 0;
        x_ = 0;
        return;
    }
    lv_label_set_text(label_, line.c_str());
    lv_point_t size;
    lv_text_get_size(&size, line.c_str(), font_, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    text_w_ = size.x;
    lv_obj_set_width(label_, text_w_);
    x_ = width_;
    lv_obj_set_pos(label_, x_, (area_[3] - area_[1] - size.y) / 2);
    lv_obj_remove_flag(box_, LV_OBJ_FLAG_HIDDEN);
}

void UsageTicker::Refresh() {
    if (box_ == nullptr) return;
    if (!visible_) {
        lv_obj_add_flag(box_, LV_OBJ_FLAG_HIDDEN);
        if (timer_ != nullptr) {
            lv_timer_delete(timer_);
            timer_ = nullptr;
        }
        return;
    }
    if (timer_ == nullptr) {
        timer_ = lv_timer_create(OnTick, kTickMs, this);
        if (paused_) lv_timer_pause(timer_);
    }
    if (!LoadAssets()) {          // 화면이 assets 보다 먼저 뜬다. OnTick 이 글꼴이 올 때까지 다시 찾는다
        lv_obj_add_flag(box_, LV_OBJ_FLAG_HIDDEN);
        asset_wait_ticks_ = 0;
        asset_tries_ = 0;
        return;
    }
    StartLine();
}

void UsageTicker::OnTick(lv_timer_t* timer) {
    auto* self = static_cast<UsageTicker*>(lv_timer_get_user_data(timer));
    if (self->font_ == nullptr) {
        if (++self->asset_wait_ticks_ < kAssetRetryTicks) return;
        self->asset_wait_ticks_ = 0;
        if (self->LoadAssets()) {
            self->StartLine();
        } else if (self->tried_assets_ || ++self->asset_tries_ >= kAssetRetryMax) {
            self->tried_assets_ = true;   // 글꼴 없는 빌드다. SetVisible 이 다시 찾지 않게 한다
            lv_timer_delete(timer);
            self->timer_ = nullptr;
        }
        return;
    }
    if (self->hold_ticks_ > 0) {
        self->hold_ticks_--;
        return;
    }
    if (self->text_w_ == 0) {          // 낼 줄이 없었다. 다음 날·새 예정일을 천천히 기다린다
        static constexpr int kIdleWaitTicks = 60000 / kTickMs;
        if (++self->x_ >= kIdleWaitTicks) self->StartLine();
        return;
    }
    self->x_ -= kStepPx;
    if (self->x_ + self->text_w_ <= 0) {
        self->StartLine();
        return;
    }
    lv_obj_set_x(self->label_, self->x_);
}
