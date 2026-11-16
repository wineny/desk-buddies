#include "manbok_panel.h"

#include <esp_log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "art/ui_poke.h"

LV_FONT_DECLARE(press_start_8)

namespace {

constexpr const char* kTag = "ManbokPanel";

// 좌표는 판(화면 8,8 에서 304×224) 기준이다. 시안: 클로드봇-메뉴-시안2.png · 클로드봇-초음파-시안.png.
constexpr int kMargin = 8;
constexpr int kW = 304, kH = 224;
// 정보 판(초음파·엄마 건강·출산 준비) — 일기 판과 같은 틀
constexpr uint32_t kBg = 0x0E1222;
constexpr lv_opa_t kOpa = 235;
// 메뉴·메시지·놀러가기 — 옛 문구 패널 색(구운 문구 그림의 판 색 0x161008 과 같다)
constexpr uint32_t kMenuBg = 0x161008, kMenuLine = 0x4B4430, kCellBg = 0x1A1914, kCellSub = 0xA99F80;
constexpr uint32_t kRowLine = 0x2B2A20, kMenuTitle = 0xF08A8A;
constexpr int kBoxY = 8, kBoxH = 21, kBoxRadius = 5;
constexpr int kCloseX = 272, kCloseW = 25;
constexpr int kBackX = 8, kBackW = 25;
constexpr uint32_t kClose = 0xDC503C, kBack = 0x3B4A86;
constexpr uint32_t kButton = 0x303C4A, kButtonOff = 0x1E2430;
constexpr uint32_t kText = 0xFFFFFF, kSoft = 0xC8C8D2, kDim = 0x9AA3C7, kDate = 0x82AAE6, kFail = 0xFF8C7A;
constexpr uint32_t kYellow = 0xF3D27A, kGreen = 0x9FE0A5, kSep = 0x283042, kArrowInk = 0xDCDCE6, kArrowOff = 0x6E768C;
constexpr uint32_t kCardLine = 0x33406E, kCardBg = 0x16204A, kOver = 0xFF7A6E;
constexpr int kTitleX = kBackX + kBackW + 7, kTitleY = 11;
// 오른쪽 넘김 단추
constexpr int kArrowX = 272, kArrowW = 24, kArrowH = 40;
// 메뉴 6칸
constexpr int kCellPad = 6, kCellW = (kW - 3 * kCellPad) / 2, kCellH = (kH - 4 * kCellPad) / 3;
// 초음파 목록
constexpr int kUsTop = 50, kUsRowH = 78, kUsPerPage = 2, kUsThumbDy = 18, kThumbW = 60, kThumbH = 45, kThumbGap = 4;
constexpr int kUsX = 12;
// 엄마 건강
constexpr int kCardY = 34, kCardH = 47, kCardW = 94, kCardGap = 3;
constexpr int kHealthTop = 89, kHealthRowH = 21, kHealthRows = 6, kLabelX = 12, kContentX = 48, kContentRight = 264;
// 출산 준비
constexpr int kPrepTop = 50, kPrepRowH = 26, kPrepRows = 6, kDueX = 12, kPrepTitleX = 52;
// 메시지·놀러가기
constexpr int kListTop = 30, kMsgRowH = 38, kVisitRowH = 64, kTextScale = 2;
// 손가락 판정(판 좌표)
constexpr int kTopHit = kBoxY + kBoxH + 16;
constexpr int kCloseHitLeft = kCloseX - 16;
constexpr int kBackHitRight = kBackX + kBackW + 12;
constexpr int kArrowHitLeft = kArrowX - 6;

const lv_image_dsc_t* const kMenuTitles[6] = {&ui_manbok_menu_1_img, &ui_manbok_menu_2_img,
                                              &ui_manbok_menu_3_img, &ui_manbok_menu_4_img,
                                              &ui_manbok_menu_5_img, &ui_manbok_menu_6_img};
const lv_image_dsc_t* const kPhrases[UI_POKE_PHRASE_COUNT] = {
    &ui_poke_phrase_1_img, &ui_poke_phrase_2_img, &ui_poke_phrase_3_img,
    &ui_poke_phrase_4_img, &ui_poke_phrase_5_img};
const lv_image_dsc_t* const kTripRows[3] = {&ui_poke_trip_img, &ui_poke_invite_img, &ui_poke_home_img};

lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius) {
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

// 네모를 쌓은 도트 삼각형(갈무리 글꼴에 ▲▼◀▶ 가 없다). diary_panel.cc 와 같은 모양.
void Tri(lv_obj_t* box, int bw, int bh, char dir, uint32_t color) {
    constexpr int kRows = 5, kStep = 2;
    const bool vertical = dir == 'u' || dir == 'd';
    const bool grow = dir == 'u' || dir == 'l';
    for (int i = 0; i < kRows; i++) {
        const int len = 2 + 4 * (grow ? i : kRows - 1 - i);
        if (vertical) {
            Box(box, (bw - len) / 2, (bh - kRows * kStep) / 2 + i * kStep, len, kStep, color, 0);
        } else {
            Box(box, (bw - kRows * kStep) / 2 + i * kStep, (bh - len) / 2, kStep, len, color, 0);
        }
    }
}

void XMark(lv_obj_t* box) {
    lv_obj_t* x = lv_label_create(box);
    lv_obj_set_style_text_font(x, &press_start_8, 0);
    lv_obj_set_style_text_color(x, lv_color_hex(kText), 0);
    lv_label_set_text(x, "X");
    lv_obj_center(x);
}

int TextW(const lv_font_t* font, const char* s) {
    lv_point_t p;
    lv_text_get_size(&p, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

size_t U8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

size_t FitPrefix(const lv_font_t* font, const std::string& s, int width) {
    size_t best = 0, i = 0;
    while (i < s.size()) {
        const size_t next = i + U8Len(static_cast<unsigned char>(s[i]));
        if (TextW(font, s.substr(0, next).c_str()) > width) break;
        best = next;
        i = next;
    }
    return best > 0 ? best : std::min(s.size(), U8Len(static_cast<unsigned char>(s[0])));
}

int DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// "2026-11-16" → 1970-01-01 부터 센 날. 못 읽으면 INT32_MIN 대신 -1000000.
int DateDays(const std::string& date) {
    int y = 0, m = 0, d = 0;
    if (sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return -1000000;
    return DaysFromCivil(y, m, d);
}

int TodayDays() {
    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year < 2024 - 1900) return -1000000;
    return DaysFromCivil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

// "2026-11-16" → "11/16"
std::string ShortDate(const std::string& date) {
    int y = 0, m = 0, d = 0;
    if (sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return date;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d/%d", m, d);
    return buf;
}

// "2026-11-16" → "월"
const char* DayName(const std::string& date) {
    static const char* const kNames[7] = {"일", "월", "화", "수", "목", "금", "토"};
    const int days = DateDays(date);
    if (days == -1000000) return "";
    return kNames[((days + 4) % 7 + 7) % 7];   // 1970-01-01 은 목요일
}

// 1294 → "1,294"
std::string Comma(int v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", v < 0 ? -v : v);
    std::string s = buf, out;
    for (size_t i = 0; i < s.size(); i++) {
        if (i > 0 && (s.size() - i) % 3 == 0) out += ',';
        out += s[i];
    }
    return v < 0 ? "-" + out : out;
}

std::string WeekText(int week, int day) {
    if (week < 0) return "";
    char buf[24];
    snprintf(buf, sizeof(buf), "%d주 %d일", week, day);
    return buf;
}

bool Loading(ManbokPartState s) { return s == ManbokPartState::kNone || s == ManbokPartState::kLoading; }

}  // namespace

ManbokPanel::ManbokPanel(lv_obj_t* parent, const lv_font_t* font, const ManbokData& data, int diary_count,
                         const std::vector<DiaryPhotoPtr>& photos, Hooks hooks, Screen screen)
    : parent_(parent), font_(font), data_(data), diary_count_(diary_count), hooks_(std::move(hooks)),
      photos_(photos) {
    last_input_tick_ = lv_tick_get();
    Open(screen);
    ESP_LOGI(kTag, "open %s", ViewName().c_str());
}

ManbokPanel::~ManbokPanel() {
    if (full_ != nullptr) lv_obj_delete(full_);
    if (root_ != nullptr) {
        auto* outer = static_cast<lv_obj_t*>(lv_obj_get_user_data(root_));   // 메뉴 쪽 금색 테두리 상자
        lv_obj_delete(outer != nullptr ? outer : root_);
    }
}

std::string ManbokPanel::ViewName() const {
    char buf[24];
    switch (mode_) {
        case Mode::kMenu: return "menu";
        case Mode::kUsList: snprintf(buf, sizeof(buf), "us:%d", page_); break;
        case Mode::kUsPhoto: snprintf(buf, sizeof(buf), "usphoto:%d", photo_); break;
        case Mode::kHealth: snprintf(buf, sizeof(buf), "health:%d", page_); break;
        case Mode::kPrep: snprintf(buf, sizeof(buf), "prep:%d", page_); break;
        case Mode::kPrepItem: snprintf(buf, sizeof(buf), "prepitem:%d", prep_item_); break;
        case Mode::kMessage: return "message";
        case Mode::kVisit: return "visit";
    }
    return buf;
}

void ManbokPanel::Open(Screen screen) {
    page_ = 0;
    switch (screen) {
        case Screen::kMenu: mode_ = Mode::kMenu; break;
        case Screen::kUltrasound: mode_ = Mode::kUsList; break;
        case Screen::kHealth: mode_ = Mode::kHealth; break;
        case Screen::kPrep: mode_ = Mode::kPrep; break;
        case Screen::kMessage: mode_ = Mode::kMessage; break;
        case Screen::kVisit:
            mode_ = Mode::kVisit;
            if (hooks_.trip_on) hooks_.trip_on(trip_on_);
            break;
    }
    if (mode_ == Mode::kMenu || mode_ == Mode::kUsList || mode_ == Mode::kHealth || mode_ == Mode::kPrep) {
        if (hooks_.want_parts) hooks_.want_parts();
    }
    Rebuild();
}

lv_obj_t* ManbokPanel::MakeLabel(lv_obj_t* parent, int x, int y, const char* text, uint32_t color, int width) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font_, 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (width > 0) {
        // 높이를 한 줄로 묶어야 DOTS 가 말줄임을 한다(diary_panel.cc 와 같다).
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(label, width, lv_font_get_line_height(font_));
    }
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    return label;
}

lv_obj_t* ManbokPanel::MakeCentered(lv_obj_t* parent, int y, const char* text, uint32_t color) {
    lv_obj_t* l = MakeLabel(parent, 0, y, text, color);
    lv_obj_set_width(l, kW);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

lv_obj_t* ManbokPanel::MakeImage(lv_obj_t* parent, int x, int y, const DiaryImage& im) {
    dscs_.push_back(std::make_unique<lv_image_dsc_t>());
    lv_image_dsc_t* dsc = dscs_.back().get();
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = im.w;
    dsc->header.h = im.h;
    dsc->header.stride = im.w * 2;
    dsc->data_size = im.w * im.h * 2;
    dsc->data = im.pixels;
    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, dsc);
    lv_obj_set_pos(img, x, y);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

// 구운 문구 그림을 2배로 (cx, cy) 가운데에. dim 이면 판 색으로 덧칠해 흐리게(옛 문구 패널과 같다).
void ManbokPanel::MakeScaled(const lv_image_dsc_t* dsc, int cx, int cy, bool dim) {
    const int w = static_cast<int>(dsc->header.w), h = static_cast<int>(dsc->header.h);
    lv_obj_t* img = lv_image_create(root_);
    lv_image_set_antialias(img, false);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(img, dsc);
    lv_image_set_pivot(img, w / 2, h / 2);
    lv_image_set_scale(img, LV_SCALE_NONE * kTextScale);
    lv_obj_set_pos(img, cx - w / 2, cy - h / 2);
    if (dim) {
        lv_obj_set_style_image_recolor(img, lv_color_hex(kMenuBg), 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_70, 0);
    }
}

void ManbokPanel::MakeArrows(int page, int count, int up_y, int down_y) {
    if (count <= 1) return;
    const bool up = page > 0, down = page < count - 1;
    lv_obj_t* u = Box(root_, kArrowX, up_y, kArrowW, kArrowH, up ? kButton : kButtonOff, kBoxRadius);
    lv_obj_t* d = Box(root_, kArrowX, down_y, kArrowW, kArrowH, down ? kButton : kButtonOff, kBoxRadius);
    Tri(u, kArrowW, kArrowH, 'u', up ? kArrowInk : kArrowOff);
    Tri(d, kArrowW, kArrowH, 'd', down ? kArrowInk : kArrowOff);
    const int mid = (up_y + kArrowH + down_y) / 2 - 16;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", page + 1);
    lv_obj_t* a = MakeLabel(root_, kArrowX, mid, buf, kSoft);
    lv_obj_set_width(a, kArrowW);
    lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_CENTER, 0);
    snprintf(buf, sizeof(buf), "/%d", count);
    lv_obj_t* b = MakeLabel(root_, kArrowX, mid + 16, buf, kArrowOff);
    lv_obj_set_width(b, kArrowW);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
}

// 정보 판은 일기 판과 같은 남색 둥근 판, 메뉴 쪽은 옛 문구 패널 색에 금색 테두리.
void ManbokPanel::BuildFrame(bool menu_style) {
    if (menu_style) {
        // 금색 테두리 상자 안에 판. 그리기는 안쪽 판(화면 8,8)에, 지울 때는 테두리째(user_data).
        lv_obj_t* line = Box(parent_, kMargin - 2, kMargin - 2, kW + 4, kH + 4, kMenuLine, 0);
        root_ = Box(line, 2, 2, kW, kH, kMenuBg, 0);
        lv_obj_set_user_data(root_, line);
    } else {
        root_ = Box(parent_, kMargin, kMargin, kW, kH, kBg, 8);
        lv_obj_set_style_bg_opa(root_, kOpa, 0);
    }
}

void ManbokPanel::BuildHeader(const char* title, bool back, bool close) {
    if (back) {
        lv_obj_t* b = Box(root_, kBackX, kBoxY, kBackW, kBoxH, kBack, kBoxRadius);
        lv_obj_t* lt = MakeLabel(b, 0, 0, "<", kText);
        lv_obj_center(lt);
    }
    if (close) {
        XMark(Box(root_, kCloseX, kBoxY, kCloseW, kBoxH, kClose, kBoxRadius));
    }
    if (title != nullptr) MakeLabel(root_, kTitleX, kTitleY, title, kText, kCloseX - 6 - kTitleX);
}

void ManbokPanel::Rebuild() {
    if (full_ != nullptr) {
        lv_obj_delete(full_);
        full_ = nullptr;
    }
    if (root_ != nullptr) {
        auto* outer = static_cast<lv_obj_t*>(lv_obj_get_user_data(root_));
        lv_obj_delete(outer != nullptr ? outer : root_);
        root_ = nullptr;
    }
    dscs_.clear();   // 그림을 가리키던 lv_image 를 다 지운 뒤에 서술자를 버린다
    switch (mode_) {
        case Mode::kMenu: BuildMenu(); break;
        case Mode::kUsList: BuildUsList(); break;
        case Mode::kUsPhoto:   // 화면 가득이라 목록은 짓지 않는다(목록 썸네일을 다시 부탁하지 않게)
            BuildUsPhoto();
            if (mode_ == Mode::kUsList) BuildUsList();
            break;
        case Mode::kHealth: BuildHealth(); break;
        case Mode::kPrep: BuildPrep(); break;
        case Mode::kPrepItem: BuildPrepItem(); break;
        case Mode::kMessage: BuildMessage(); break;
        case Mode::kVisit: BuildVisit(); break;
    }
}

// --- ① 메뉴 ------------------------------------------------------------------------

void ManbokPanel::BuildMenu() {
    BuildFrame(true);
    std::string subs[6];
    subs[0] = diary_count_ < 0 ? "불러오는 중" : "최근 " + std::to_string(diary_count_) + "개";
    const ManbokUltrasound& us = data_.ultrasound;
    if (us.valid) {
        subs[1] = std::to_string(us.visits) + "번 · " + std::to_string(us.photos) + "장";
    } else {
        subs[1] = data_.us_state == ManbokPartState::kFailed ? "못 받았어요" : "불러오는 중";
    }
    const ManbokHealth& h = data_.health;
    if (h.valid) {
        subs[2] = h.count > 0 ? "오늘 " + Comma(h.kcal) + "kcal" : "오늘 기록 없음";
    } else {
        subs[2] = data_.health_state == ManbokPartState::kFailed ? "못 받았어요" : "불러오는 중";
    }
    const ManbokPrep& p = data_.prep;
    if (p.valid) {
        char buf[48];
        if (p.dday >= 0) {
            snprintf(buf, sizeof(buf), "D-%d · 할 일 %d", p.dday, p.open);
        } else {
            snprintf(buf, sizeof(buf), "D+%d · 할 일 %d", -p.dday, p.open);
        }
        subs[3] = buf;
    } else {
        subs[3] = data_.prep_state == ManbokPartState::kFailed ? "못 받았어요" : "불러오는 중";
    }
    subs[4] = "밥 먹자 · 사랑해 …";
    subs[5] = "가기 · 부르기";
    for (int i = 0; i < 6; i++) {
        const int x = kCellPad + (i % 2) * (kCellW + kCellPad);
        const int y = kCellPad + (i / 2) * (kCellH + kCellPad);
        lv_obj_t* edge = Box(root_, x, y, kCellW, kCellH, kMenuLine, 7);
        lv_obj_t* cell = Box(edge, 2, 2, kCellW - 4, kCellH - 4, kCellBg, 5);
        const lv_image_dsc_t* t = kMenuTitles[i];
        const int tw = static_cast<int>(t->header.w), th = static_cast<int>(t->header.h);
        const int lh = lv_font_get_line_height(font_);
        const int top = (kCellH - 4 - (th + 6 + lh)) / 2;
        lv_obj_t* img = lv_image_create(cell);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_image_set_src(img, t);
        lv_obj_set_pos(img, (kCellW - 4 - tw) / 2, top);
        lv_obj_t* s = MakeLabel(cell, 0, top + th + 6, subs[i].c_str(), kCellSub);
        lv_obj_set_width(s, kCellW - 4);
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    }
}

// --- ③④ 초음파 ------------------------------------------------------------------------

int ManbokPanel::PhotoTotal() const {
    int n = 0;
    for (const ManbokVisit& v : data_.ultrasound.items) n += static_cast<int>(v.photos.size());
    return n;
}

bool ManbokPanel::FlatToVisit(int flat, int* visit, int* index) const {
    const auto& items = data_.ultrasound.items;
    for (size_t v = 0; v < items.size(); v++) {
        const int n = static_cast<int>(items[v].photos.size());
        if (flat < n) {
            *visit = static_cast<int>(v);
            *index = flat;
            return true;
        }
        flat -= n;
    }
    return false;
}

const DiaryPhoto* ManbokPanel::PhotoAt(int flat) {
    int v = 0, i = 0;
    if (flat < 0 || !FlatToVisit(flat, &v, &i)) return nullptr;
    const ManbokVisit& visit = data_.ultrasound.items[v];
    const std::string& key = visit.photo_keys[i];
    for (const DiaryPhotoPtr& p : photos_) {
        if (p && p->key == key) return p.get();
    }
    if (std::find(asked_.begin(), asked_.end(), key) == asked_.end()) {
        asked_.push_back(key);
        if (hooks_.need_photo) hooks_.need_photo(visit.photos[i], key);
    }
    return nullptr;
}

void ManbokPanel::BuildUsList() {
    BuildFrame(false);
    BuildHeader("초음파", true, true);
    const ManbokUltrasound& us = data_.ultrasound;
    std::string sub;
    if (us.valid) {
        sub = std::to_string(us.visits) + "번 · 사진 " + std::to_string(us.photos) + "장";
        if (data_.us_state == ManbokPartState::kFailed) sub += " · 못 받았어요";
    }
    if (!sub.empty()) MakeCentered(root_, 30, sub.c_str(), data_.us_state == ManbokPartState::kFailed ? kFail : kSoft);
    if (!us.valid || us.items.empty()) {
        const char* msg = us.valid ? "초음파 사진이 없어요"
                                   : (data_.us_state == ManbokPartState::kFailed ? "못 받았어요" : "불러오는 중");
        MakeCentered(root_, 110, msg, kDim);
        return;
    }
    const int pages = PageCount();
    page_ = std::clamp(page_, 0, pages - 1);
    int flat0 = 0;
    for (int v = 0; v < page_ * kUsPerPage && v < static_cast<int>(us.items.size()); v++) {
        flat0 += static_cast<int>(us.items[v].photos.size());
    }
    for (int r = 0; r < kUsPerPage; r++) {
        const int vi = page_ * kUsPerPage + r;
        if (vi >= static_cast<int>(us.items.size())) break;
        const ManbokVisit& v = us.items[vi];
        const int y = kUsTop + r * kUsRowH;
        if (r > 0) Box(root_, kUsX, y - 6, 254, 1, kSep, 0);
        int x = kUsX;
        const std::string week = WeekText(v.week, v.day);
        if (!week.empty()) {
            MakeLabel(root_, x, y, week.c_str(), kText);
            x += TextW(font_, week.c_str()) + 6;
        }
        const std::string date = ShortDate(v.date) + " " + DayName(v.date);
        MakeLabel(root_, x, y, date.c_str(), kDate);
        for (size_t i = 0; i < v.photos.size() && i < 4; i++) {
            const int tx = kUsX + static_cast<int>(i) * (kThumbW + kThumbGap), ty = y + kUsThumbDy;
            Box(root_, tx - 1, ty - 1, kThumbW + 2, kThumbH + 2, 0x46506A, 0);
            Box(root_, tx, ty, kThumbW, kThumbH, 0x000000, 0);
            const DiaryPhoto* p = PhotoAt(flat0 + static_cast<int>(i));
            if (p != nullptr && p->thumb.pixels != nullptr) {
                MakeImage(root_, tx + (kThumbW - p->thumb.w) / 2, ty + (kThumbH - p->thumb.h) / 2, p->thumb);
            } else {
                Box(root_, tx, ty, kThumbW, kThumbH, 0x1E2430, 0);
            }
        }
        flat0 += static_cast<int>(v.photos.size());
    }
    MakeArrows(page_, pages, 64, 174);
}

void ManbokPanel::BuildUsPhoto() {
    const int n = PhotoTotal();
    int vi = 0, idx = 0;
    if (n == 0 || !FlatToVisit(std::clamp(photo_, 0, n - 1), &vi, &idx)) {
        mode_ = Mode::kUsList;
        return;
    }
    photo_ = std::clamp(photo_, 0, n - 1);
    const ManbokVisit& v = data_.ultrasound.items[vi];
    const DiaryPhoto* p = PhotoAt(photo_);
    if (photo_ + 1 < n) PhotoAt(photo_ + 1);   // 다음 장을 미리 부탁해 둔다
    const int w = lv_obj_get_width(parent_), h = lv_obj_get_height(parent_);
    full_ = Box(parent_, 0, 0, w, h, 0x000000, 0);
    if (p != nullptr && p->big.pixels != nullptr) {
        MakeImage(full_, (w - p->big.w) / 2, (h - p->big.h) / 2, p->big);
    } else {
        lv_obj_t* l = MakeLabel(full_, 0, h / 2 - 8, "사진 받는 중…", kArrowOff);
        lv_obj_set_width(l, w);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    XMark(Box(full_, w - 34, 8, kCloseW, kBoxH, kClose, kBoxRadius));
    std::string cap = WeekText(v.week, v.day);
    if (!cap.empty()) cap += " · ";
    char cnt[32];
    snprintf(cnt, sizeof(cnt), " · %d/%d", idx + 1, static_cast<int>(v.photos.size()));
    cap += ShortDate(v.date) + cnt;
    lv_obj_t* plate = Box(full_, 8, h - 26, TextW(font_, cap.c_str()) + 12, 19, 0x161008, 4);
    lv_obj_set_style_bg_opa(plate, LV_OPA_80, 0);
    MakeLabel(plate, 6, (19 - lv_font_get_line_height(font_)) / 2, cap.c_str(), 0xF0E7DD);
    for (int side = 0; side < 2; side++) {
        const bool can = side == 0 ? photo_ > 0 : photo_ < n - 1;
        if (!can) continue;
        lv_obj_t* a = Box(full_, side == 0 ? 6 : w - 6 - kArrowW, (h - kArrowH) / 2, kArrowW, kArrowH, 0x161008,
                          kBoxRadius);
        lv_obj_set_style_bg_opa(a, LV_OPA_60, 0);
        Tri(a, kArrowW, kArrowH, side == 0 ? 'l' : 'r', 0xF0E7DD);
    }
}

void ManbokPanel::TurnPhoto(int delta) {
    const int n = PhotoTotal();
    const int next = std::clamp(photo_ + delta, 0, std::max(0, n - 1));
    if (next == photo_) return;
    photo_ = next;
    Rebuild();
}

// --- ② 엄마 건강 ------------------------------------------------------------------------

// 끼니별 음식을 「·」로 이어 줄에 담는다. 넘치면 다음 줄로(이름표 없이), 한 이름이 줄보다 길면 말줄임.
void ManbokPanel::LayoutHealth() {
    health_lines_.clear();
    const ManbokHealth& h = data_.health;
    const int sep_w = TextW(font_, " · ");
    constexpr int kBadgeW = 12;
    auto group = [&](const char* label, const std::vector<std::pair<std::string, std::pair<uint32_t, bool>>>& segs) {
        health_lines_.push_back({label, kGreen, {}, false});
        int x = kContentX;
        for (size_t i = 0; i < segs.size(); i++) {
            const std::string& text = segs[i].first;
            const uint32_t color = segs[i].second.first;
            const bool badge = segs[i].second.second;
            const bool is_sep = text == " · ";
            int w = is_sep ? sep_w : TextW(font_, text.c_str()) + (badge ? kBadgeW : 0);
            if (x + w > kContentRight && x > kContentX) {
                auto& prev = health_lines_.back().parts;   // 줄 끝에 「·」를 남기지 않는다
                if (!prev.empty() && prev.back().text == " · ") prev.pop_back();
                health_lines_.push_back({"", kGreen, {}, false});
                x = kContentX;
            }
            if (is_sep && x == kContentX) continue;   // 줄 머리에 「·」를 두지 않는다
            w = std::min(w, kContentRight - x);
            health_lines_.back().parts.push_back({x, w, text, color, badge});
            x += w;
        }
        health_lines_.back().group_end = true;
    };
    using Segs = std::vector<std::pair<std::string, std::pair<uint32_t, bool>>>;
    if (h.count <= 0 || h.meals.empty()) {
        group("식단", Segs{{"오늘 기록 없음", {kDim, false}}});
    }
    for (const ManbokMeal& m : h.meals) {
        Segs segs;
        for (size_t i = 0; i < m.items.size(); i++) {
            if (i > 0) segs.push_back({" · ", {kDim, false}});
            segs.push_back({m.items[i].name, {m.items[i].warn ? kYellow : kText, m.items[i].warn}});
        }
        group(m.meal.c_str(), segs);
    }
    Segs ex{{"이번 주 " + std::to_string(h.exercise_week) + "번", {kText, false}}};
    if (!h.exercise_last_date.empty()) {
        ex.push_back({" · ", {kDim, false}});
        ex.push_back({"마지막 " + ShortDate(h.exercise_last_date) + " " + h.exercise_last_name, {kDim, false}});
    }
    group("운동", ex);
}

void ManbokPanel::BuildHealth() {
    BuildFrame(false);
    const ManbokHealth& h = data_.health;
    std::string title = "엄마 건강";
    if (h.valid && !h.date.empty()) title += " · " + ShortDate(h.date) + " " + DayName(h.date);
    BuildHeader(title.c_str(), true, true);
    if (!h.valid) {
        MakeCentered(root_, 110, data_.health_state == ManbokPartState::kFailed ? "못 받았어요" : "불러오는 중", kDim);
        return;
    }
    if (data_.health_state == ManbokPartState::kFailed) {
        lv_obj_t* f = MakeLabel(root_, 0, kTitleY, "못 받았어요", kFail);
        lv_obj_set_x(f, kCloseX - 6 - TextW(font_, "못 받았어요"));
    }
    // 위 카드 3개
    char value[24], head[32], foot[32];
    for (int k = 0; k < 3; k++) {
        const int x = kMargin + k * (kCardW + kCardGap);
        lv_obj_t* edge = Box(root_, x, kCardY, kCardW, kCardH, kCardLine, 5);
        lv_obj_t* card = Box(edge, 1, 1, kCardW - 2, kCardH - 2, kCardBg, 4);
        uint32_t vcolor = kText;
        const char* unit = nullptr;
        if (k == 0) {
            snprintf(head, sizeof(head), "칼로리");
            snprintf(value, sizeof(value), "%s", Comma(h.kcal).c_str());
            unit = " kcal";
            snprintf(foot, sizeof(foot), "%s", h.count > 0 ? "오늘 먹은 양" : "오늘 기록 없음");
        } else if (k == 1) {
            snprintf(head, sizeof(head), "카페인");
            snprintf(value, sizeof(value), "%dmg", h.caffeine_mg);
            vcolor = h.caffeine_mg > h.caffeine_limit ? kOver : kYellow;
            snprintf(foot, sizeof(foot), "하루 %d 아래", h.caffeine_limit);
        } else {
            if (h.has_weight) {
                snprintf(head, sizeof(head), "몸무게 %s", ShortDate(h.weight_date).c_str());
                snprintf(value, sizeof(value), "%.1fkg", static_cast<double>(h.weight_kg));
            } else {
                snprintf(head, sizeof(head), "몸무게");
                snprintf(value, sizeof(value), "—");
            }
            snprintf(foot, sizeof(foot), "목표 %g 미만", static_cast<double>(h.weight_goal));
        }
        MakeLabel(card, 4, 2, head, kDim, kCardW - 8);
        MakeLabel(card, 4, 16, value, vcolor);
        if (unit != nullptr) MakeLabel(card, 4 + TextW(font_, value), 16, unit, kDim);
        MakeLabel(card, 4, 30, foot, kDim, kCardW - 8);
    }
    // 아래 끼니별 줄
    LayoutHealth();
    const int pages = PageCount();
    page_ = std::clamp(page_, 0, pages - 1);
    const int first = page_ * kHealthRows;
    for (int r = 0; r < kHealthRows; r++) {
        const int i = first + r;
        if (i >= static_cast<int>(health_lines_.size())) break;
        const HLine& ln = health_lines_[i];
        const int y = kHealthTop + r * kHealthRowH;
        if (!ln.label.empty()) MakeLabel(root_, kLabelX, y + 3, ln.label.c_str(), ln.label_color);
        for (const Placed& p : ln.parts) {
            const int tw = p.badge ? p.w - 12 : p.w;
            MakeLabel(root_, p.x, y + 3, p.text.c_str(), p.color, TextW(font_, p.text.c_str()) > tw ? tw : 0);
            if (p.badge) {
                lv_obj_t* b = Box(root_, p.x + tw + 2, y + 4, 9, 13, kYellow, 1);
                lv_obj_t* l = MakeLabel(b, 0, 0, "!", 0x14141E);
                lv_obj_center(l);
            }
        }
        if (ln.group_end && r < kHealthRows - 1 && i + 1 < static_cast<int>(health_lines_.size())) {
            Box(root_, kLabelX, y + kHealthRowH - 1, kContentRight - kLabelX, 1, kSep, 0);
        }
    }
    MakeArrows(page_, pages, kHealthTop, 174);
}

// --- ③ 출산 준비 ------------------------------------------------------------------------

void ManbokPanel::BuildPrep() {
    BuildFrame(false);
    BuildHeader("출산 준비", true, true);
    const ManbokPrep& p = data_.prep;
    if (!p.valid) {
        MakeCentered(root_, 110, data_.prep_state == ManbokPartState::kFailed ? "못 받았어요" : "불러오는 중", kDim);
        return;
    }
    // 「출산 M/D 요일 · D-N」 — D-day 만 노랗게
    const std::string left = "출산 " + ShortDate(p.birth) + " " + DayName(p.birth) + " · ";
    char dd[24];
    snprintf(dd, sizeof(dd), p.dday >= 0 ? "D-%d" : "D+%d", p.dday >= 0 ? p.dday : -p.dday);
    const int lw = TextW(font_, left.c_str()), dw = TextW(font_, dd);
    const int x0 = (kW - lw - dw) / 2;
    MakeLabel(root_, x0, 30, left.c_str(), kText);
    MakeLabel(root_, x0 + lw, 30, dd, kYellow);
    if (data_.prep_state == ManbokPartState::kFailed) {
        lv_obj_t* f = MakeLabel(root_, 0, kTitleY, "못 받았어요", kFail);
        lv_obj_set_x(f, kCloseX - 6 - TextW(font_, "못 받았어요"));
    }
    if (p.items.empty()) {
        MakeCentered(root_, 110, "남은 준비가 없어요", kDim);
        return;
    }
    const int pages = PageCount();
    page_ = std::clamp(page_, 0, pages - 1);
    for (int r = 0; r < kPrepRows; r++) {
        const int i = page_ * kPrepRows + r;
        if (i >= static_cast<int>(p.items.size())) break;
        const ManbokPrepItem& it = p.items[i];
        const int y = kPrepTop + r * kPrepRowH;
        if (it.due.empty()) {
            MakeLabel(root_, kDueX, y + 5, "—", kDim);
        } else {
            MakeLabel(root_, kDueX, y + 5, ShortDate(it.due).c_str(), kYellow);
        }
        MakeLabel(root_, kPrepTitleX, y + 5, it.title.c_str(), kText, kContentRight - kPrepTitleX);
        if (r < kPrepRows - 1 && i + 1 < static_cast<int>(p.items.size())) {
            Box(root_, kDueX, y + kPrepRowH - 1, kContentRight - kDueX, 1, kSep, 0);
        }
    }
    MakeArrows(page_, pages, 56, 174);
}

void ManbokPanel::BuildPrepItem() {
    const ManbokPrep& p = data_.prep;
    if (prep_item_ < 0 || prep_item_ >= static_cast<int>(p.items.size())) {   // 새 목록에서 줄이 사라졌다
        mode_ = Mode::kPrep;
        BuildPrep();
        return;
    }
    BuildFrame(false);
    BuildHeader("출산 준비", true, true);
    const ManbokPrepItem& it = p.items[prep_item_];
    std::string due;
    if (it.due.empty()) {
        due = "마감 없음";
    } else {
        due = "마감 " + ShortDate(it.due) + " " + DayName(it.due);
        const int today = TodayDays();
        if (today != -1000000) {
            const int left = DateDays(it.due) - today;
            char buf[32];
            if (left > 0) {
                snprintf(buf, sizeof(buf), " · D-%d", left);
            } else if (left == 0) {
                snprintf(buf, sizeof(buf), " · 오늘");
            } else {
                snprintf(buf, sizeof(buf), " · %d일 지남", -left);
            }
            due += buf;
        }
    }
    MakeLabel(root_, 12, 38, due.c_str(), kYellow);
    // 제목 전체를 낱말 단위로 줄바꿈
    constexpr int kWrapW = 280, kLineH = 18;
    int y = 62;
    std::string line;
    auto emit = [&](const std::string& s) {
        if (y + kLineH > kH - 6) return;
        MakeLabel(root_, 12, y, s.c_str(), kText);
        y += kLineH;
    };
    size_t w0 = 0;
    const std::string& t = it.title;
    while (w0 < t.size()) {
        size_t w1 = t.find(' ', w0);
        if (w1 == std::string::npos) w1 = t.size();
        std::string word = t.substr(w0, w1 - w0);
        w0 = w1 + 1;
        if (word.empty()) continue;
        const std::string cand = line.empty() ? word : line + " " + word;
        if (TextW(font_, cand.c_str()) <= kWrapW) {
            line = cand;
            continue;
        }
        if (!line.empty()) emit(line);
        line.clear();
        while (TextW(font_, word.c_str()) > kWrapW) {
            const size_t cut = FitPrefix(font_, word, kWrapW);
            emit(word.substr(0, cut));
            word = word.substr(cut);
        }
        line = word;
    }
    if (!line.empty()) emit(line);
}

// --- ④⑤ 메시지 보내기 · 놀러가기 ----------------------------------------------------------

void ManbokPanel::BuildMessage() {
    BuildFrame(true);
    BuildHeader(nullptr, true, true);
    MakeCentered(root_, 9, "메시지 보내기", kMenuTitle);
    for (int i = 0; i < UI_POKE_PHRASE_COUNT; i++) {
        const int y = kListTop + i * kMsgRowH;
        Box(root_, 0, y, kW, 1, kRowLine, 0);
        MakeScaled(kPhrases[i], kW / 2, y + kMsgRowH / 2, false);
    }
}

void ManbokPanel::BuildVisit() {
    BuildFrame(true);
    BuildHeader(nullptr, true, true);
    MakeCentered(root_, 9, "놀러가기", kMenuTitle);
    for (int i = 0; i < 3; i++) {
        const int y = kListTop + i * kVisitRowH;
        Box(root_, 0, y, kW, 1, kRowLine, 0);
        MakeScaled(kTripRows[i], kW / 2, y + kVisitRowH / 2, !trip_on_[i]);
    }
}

// --- 넘기기 · 손가락 ---------------------------------------------------------------------

int ManbokPanel::PageCount() const {
    int n = 0, per = 1;
    switch (mode_) {
        case Mode::kUsList:
        case Mode::kUsPhoto:
            n = static_cast<int>(data_.ultrasound.items.size());
            per = kUsPerPage;
            break;
        case Mode::kHealth:
            n = static_cast<int>(health_lines_.size());
            per = kHealthRows;
            break;
        case Mode::kPrep:
        case Mode::kPrepItem:
            n = static_cast<int>(data_.prep.items.size());
            per = kPrepRows;
            break;
        default: return 1;
    }
    return std::max(1, (n + per - 1) / per);
}

void ManbokPanel::Turn(int delta) {
    if (mode_ != Mode::kUsList && mode_ != Mode::kHealth && mode_ != Mode::kPrep) return;
    const int next = std::clamp(page_ + delta, 0, PageCount() - 1);
    if (next == page_) return;
    page_ = next;
    Rebuild();
}

void ManbokPanel::SetData(const ManbokData& data) {
    data_ = data;
    Rebuild();   // 보던 쪽·사진은 Build* 가 범위 안으로 맞춘다
}

void ManbokPanel::SetDiaryCount(int n) {
    if (n == diary_count_) return;
    diary_count_ = n;
    if (mode_ == Mode::kMenu) Rebuild();
}

void ManbokPanel::SetPhoto(const DiaryPhotoPtr& photo) {
    if (!photo) return;
    for (DiaryPhotoPtr& p : photos_) {
        if (p && p->key == photo->key) {
            p = photo;
            if (mode_ == Mode::kUsList || mode_ == Mode::kUsPhoto) Rebuild();
            return;
        }
    }
    photos_.push_back(photo);
    if (photos_.size() > 12) {   // 목록 한 쪽(8) + 크게 보기 앞뒤. 버린 사진은 다시 부탁할 수 있게
        const std::string dropped = photos_.front() ? photos_.front()->key : std::string();
        photos_.erase(photos_.begin());
        asked_.erase(std::remove(asked_.begin(), asked_.end(), dropped), asked_.end());
    }
    if (mode_ == Mode::kUsList || mode_ == Mode::kUsPhoto) Rebuild();
}

void ManbokPanel::PhotoFailed(const std::string& key) {
    asked_.erase(std::remove(asked_.begin(), asked_.end(), key), asked_.end());
}

ManbokPanel::Result ManbokPanel::OnTouch(const Display::TouchEvent& ev) {
    using Kind = Display::TouchEvent;
    Result r;
    last_input_tick_ = lv_tick_get();
    if (mode_ == Mode::kUsPhoto) {
        // 왼쪽·오른쪽 1/4 을 누르거나 옆으로 밀면 앞뒤 장(다른 날까지 이어서), 가운데·X·아래로 밀기는 목록.
        const int sw = lv_obj_get_width(parent_);
        const bool on_close = ev.x0 >= sw - 50 && ev.y0 < 44;
        if (ev.kind == Kind::kSwipeLeft) {
            TurnPhoto(+1);
        } else if (ev.kind == Kind::kSwipeRight) {
            TurnPhoto(-1);
        } else if (ev.kind == Kind::kTap && !on_close && (ev.x0 < sw / 4 || ev.x0 >= sw - sw / 4)) {
            TurnPhoto(ev.x0 < sw / 4 ? -1 : +1);
        } else if (ev.kind == Kind::kTap || ev.kind == Kind::kSwipeDown) {
            int vi = 0, idx = 0;
            if (FlatToVisit(photo_, &vi, &idx)) page_ = vi / kUsPerPage;
            mode_ = Mode::kUsList;
            Rebuild();
        }
        return r;
    }
    if (ev.kind == Kind::kSwipeUp) {
        Turn(+1);
        return r;
    }
    if (ev.kind == Kind::kSwipeDown) {
        Turn(-1);
        return r;
    }
    if (ev.kind != Kind::kTap) return r;
    const int x = ev.x0 - kMargin, y = ev.y0 - kMargin;
    if (mode_ == Mode::kMenu) {
        const int col = (x - kCellPad) / (kCellW + kCellPad), row = (y - kCellPad) / (kCellH + kCellPad);
        const int cx = x - kCellPad - col * (kCellW + kCellPad), cy = y - kCellPad - row * (kCellH + kCellPad);
        const bool in = x >= kCellPad && y >= kCellPad && col < 2 && row < 3 && cx < kCellW && cy < kCellH;
        if (!in) {   // 칸 밖(틈·가장자리)을 누르면 닫는다 — 옛 문구 패널과 같다
            r.close = true;
            return r;
        }
        const int cell = row * 2 + col;
        ESP_LOGI(kTag, "menu pick %d", cell);
        static constexpr Screen kScreens[6] = {Screen::kMenu, Screen::kUltrasound, Screen::kHealth,
                                               Screen::kPrep, Screen::kMessage, Screen::kVisit};
        if (cell == 0) {
            r.open_diary = true;
        } else {
            Open(kScreens[cell]);
        }
        return r;
    }
    // 메시지·놀러가기는 첫 줄이 y=30 부터라 머리 단추 판정을 그 위로 줄인다.
    const int top_hit = (mode_ == Mode::kMessage || mode_ == Mode::kVisit) ? kListTop : kTopHit;
    if (x >= kCloseHitLeft && y < top_hit) {
        r.close = true;
        return r;
    }
    if (x < kBackHitRight && y < top_hit) {
        if (mode_ == Mode::kPrepItem) {
            mode_ = Mode::kPrep;
            page_ = prep_item_ / kPrepRows;
            Rebuild();
        } else {
            Open(Screen::kMenu);
        }
        return r;
    }
    if (mode_ == Mode::kMessage) {
        const int row = (y - kListTop) / kMsgRowH;
        if (y >= kListTop && row < UI_POKE_PHRASE_COUNT) {
            ESP_LOGI(kTag, "message pick %d", row + 1);
            r.poke_phrase = row + 1;
        }
        return r;
    }
    if (mode_ == Mode::kVisit) {
        const int row = (y - kListTop) / kVisitRowH;
        if (y >= kListTop && row < 3 && trip_on_[row]) {   // 흐린 줄은 아무것도 안 한다
            ESP_LOGI(kTag, "visit pick %d", row);
            r.trip = row;
        }
        return r;
    }
    if (x >= kArrowHitLeft && PageCount() > 1 &&
        (mode_ == Mode::kUsList || mode_ == Mode::kHealth || mode_ == Mode::kPrep)) {
        const int up_y = mode_ == Mode::kHealth ? kHealthTop : (mode_ == Mode::kPrep ? 56 : 64);
        if (y >= up_y - 8 && y < up_y + kArrowH + 8) Turn(-1);
        if (y >= 174 - 8 && y < 174 + kArrowH + 8) Turn(+1);
        return r;
    }
    if (mode_ == Mode::kUsList) {
        const ManbokUltrasound& us = data_.ultrasound;
        const int row = (y - kUsTop) / kUsRowH;
        const int vi = page_ * kUsPerPage + row;
        if (y < kUsTop || row >= kUsPerPage || vi >= static_cast<int>(us.items.size())) return r;
        const int ty = y - kUsTop - row * kUsRowH - kUsThumbDy;
        const int i = (x - kUsX + kThumbGap / 2) / (kThumbW + kThumbGap);
        if (ty < -6 || ty > kThumbH + 6 || x < kUsX - 2 || i < 0 || i >= static_cast<int>(us.items[vi].photos.size())) {
            return r;
        }
        int flat = 0;
        for (int v = 0; v < vi; v++) flat += static_cast<int>(us.items[v].photos.size());
        photo_ = flat + i;
        mode_ = Mode::kUsPhoto;
        Rebuild();
        return r;
    }
    if (mode_ == Mode::kPrep) {
        const int row = (y - kPrepTop) / kPrepRowH;
        const int i = page_ * kPrepRows + row;
        if (y >= kPrepTop && row < kPrepRows && i < static_cast<int>(data_.prep.items.size())) {
            prep_item_ = i;
            mode_ = Mode::kPrepItem;
            Rebuild();
        }
        return r;
    }
    return r;
}
