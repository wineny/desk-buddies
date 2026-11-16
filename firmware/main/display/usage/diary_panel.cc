#include "diary_panel.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

LV_FONT_DECLARE(press_start_8)

namespace {

constexpr const char* kTag = "DiaryPanel";

// 좌표는 판(화면 8,8 에서 304×224) 기준이다. 시안: docs/manbok-diary.md, 날씨 판과 같은 틀.
constexpr int kMargin = 8;
constexpr int kRadius = 8;
constexpr lv_opa_t kOpa = 235;
constexpr uint32_t kBg = 0x0E1222;
constexpr int kBoxY = 8, kBoxH = 21, kBoxRadius = 5;
constexpr int kCloseX = 272, kCloseW = 25;
constexpr int kBackX = 8, kBackW = 25;
constexpr uint32_t kClose = 0xDC503C;
constexpr uint32_t kButton = 0x303C4A, kButtonOff = 0x1E2430;
constexpr uint32_t kText = 0xFFFFFF, kSoft = 0xC8C8D2, kDim = 0x6E768C, kDate = 0x82AAE6;
constexpr uint32_t kMom = 0xFF96AA, kDad = 0x78BEFF, kChipInk = 0x14141E, kMoodBg = 0x3C485C, kMoodInk = 0xDCDCE6;
constexpr uint32_t kSep = 0x283042, kBarBg = 0x283042, kBarFill = 0x57C93E;
// 목록
constexpr int kTitleX = 12, kTitleY = 12;
constexpr int kSubY = 32, kBarY = 50, kBarX = 16, kBarW = 248, kBarH = 6;
constexpr int kRowTop = 64, kRowH = 50, kRowsPerPage = 3;
constexpr int kMiniX = 240;
// 글
constexpr int kTextX = 12, kTextTop = 38, kTextBottom = 214, kTextW = 254;
constexpr int kPhotoX = 12, kPhotoY = 38, kPhotoW = 96, kPhotoH = 128;
constexpr int kNarrowX = kPhotoX + kPhotoW + 8, kNarrowW = kTextX + kTextW - kNarrowX;
constexpr int kLineH = 16, kParaGap = 6;
// 오른쪽 넘김 단추
constexpr int kArrowX = 272, kArrowW = 24, kArrowH = 40, kUpY = 64, kDownY = 174, kPageY = 112;
// 손가락 판정은 그림보다 넉넉히 (화면 좌표).
constexpr int kTopHitBottom = kMargin + kBoxY + kBoxH + 16;
constexpr int kCloseHitLeft = kMargin + kCloseX - 16;
constexpr int kBackHitRight = kMargin + kBackX + kBackW + 12;
constexpr int kArrowHitLeft = kMargin + kArrowX - 6;

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

// 네모를 쌓은 도트 삼각형을 (bw×bh) 상자 가운데에 그린다. dir: 'u' 'd' 'l' 'r'.
// 갈무리 글꼴에 ▲▼◀▶ 가 없어서다(실기기 11/9).
void Tri(lv_obj_t* box, int bw, int bh, char dir, uint32_t color) {
    constexpr int kRows = 5, kStep = 2;
    const bool vertical = dir == 'u' || dir == 'd';
    const bool grow = dir == 'u' || dir == 'l';   // 앞쪽이 뾰족하다
    for (int i = 0; i < kRows; i++) {
        const int len = 2 + 4 * (grow ? i : kRows - 1 - i);
        if (vertical) {
            Box(box, (bw - len) / 2, (bh - kRows * kStep) / 2 + i * kStep, len, kStep, color, 0);
        } else {
            Box(box, (bw - kRows * kStep) / 2 + i * kStep, (bh - len) / 2, kStep, len, color, 0);
        }
    }
}

lv_obj_t* Plain(lv_obj_t* parent, int w, int h) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return o;
}

int TextW(const lv_font_t* font, const char* s) {
    lv_point_t p;
    lv_text_get_size(&p, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return p.x;
}

// UTF-8 한 글자 길이(첫 바이트로).
size_t U8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

// s 앞에서 width 안에 드는 가장 긴 바이트 수(글자 경계). 한 글자도 안 들면 첫 글자 하나.
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

int TodayDays() {
    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year < 2024 - 1900) return 0;
    return DaysFromCivil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

// "2026-10-17" → "10/17"
std::string ShortDate(const std::string& date) {
    int y = 0, m = 0, d = 0;
    if (sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return date;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d/%d", m, d);
    return buf;
}

// 첫 문단(빈 줄 전까지)을 한 줄로.
std::string FirstParagraph(const std::string& body) {
    std::string out;
    size_t i = 0;
    while (i < body.size() && (body[i] == '\n' || body[i] == ' ')) i++;
    for (; i < body.size(); i++) {
        if (body[i] == '\n') {
            if (i + 1 < body.size() && body[i + 1] == '\n') break;
            out += ' ';
            continue;
        }
        out += body[i];
    }
    return out;
}

}  // namespace

DiaryPhoto::~DiaryPhoto() {
    for (DiaryImage* im : {&big, &mid, &mini, &thumb}) {
        if (im->pixels != nullptr) heap_caps_free(im->pixels);
        im->pixels = nullptr;
    }
}

std::string DiaryTickerLine(const DiaryEntry& e) {
    std::string text = FirstParagraph(e.body);
    constexpr size_t kMaxBytes = 180;   // 한글 60자 남짓
    if (text.size() > kMaxBytes) {
        size_t cut = kMaxBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) cut--;
        text = text.substr(0, cut) + "…";
    }
    return std::string(DiaryWriterName(e.writer)) + " 일기 · " + text;
}

DiaryPanel::DiaryPanel(lv_obj_t* parent, const lv_font_t* font, const DiaryData& data, int due_days, int open_entry,
                       const std::vector<DiaryPhotoPtr>& photos, NeedPhoto need_photo, bool from_menu)
    : parent_(parent), font_(font), data_(data), due_days_(due_days), need_photo_(std::move(need_photo)),
      photos_(photos), from_menu_(from_menu) {
    const int w = lv_obj_get_width(parent), h = lv_obj_get_height(parent);
    root_ = Box(parent, kMargin, kMargin, w - kMargin * 2, h - kMargin * 2, kBg, kRadius);
    lv_obj_set_style_bg_opa(root_, kOpa, 0);
    // 만든 직후엔 배치 계산 전이라 lv_obj_get_width(root_) 가 0 이다. 바로 글을 열면(알림·흐르는 줄·시험)
    // Rebuild 의 body_ 가 0×0 이 되어 빈 판만 보였다(실기기 11/9). 크기를 지금 계산해 둔다.
    lv_obj_update_layout(root_);
    last_input_tick_ = lv_tick_get();
    if (open_entry >= 0 && open_entry < static_cast<int>(data_.entries.size())) {
        OpenEntry(open_entry);
    } else {
        Rebuild();
    }
    ESP_LOGI(kTag, "open (%u entries) %s", static_cast<unsigned>(data_.entries.size()), ViewName().c_str());
}

DiaryPanel::~DiaryPanel() {
    if (full_ != nullptr) lv_obj_delete(full_);
    if (root_ != nullptr) lv_obj_delete(root_);
}

std::string DiaryPanel::ViewName() const {
    char buf[32];
    switch (mode_) {
        case Mode::kList: snprintf(buf, sizeof(buf), "list:%d", list_page_); break;
        case Mode::kRead: snprintf(buf, sizeof(buf), "read:%d:%d/%d", entry_, page_, PageCount()); break;
        case Mode::kPhoto: snprintf(buf, sizeof(buf), "photo:%d:%d", entry_, photo_index_); break;
    }
    return buf;
}

lv_obj_t* DiaryPanel::MakeLabel(lv_obj_t* parent, int x, int y, const char* text, uint32_t color, int width) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font_, 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (width > 0) {
        // 높이를 한 줄로 묶어야 DOTS 가 말줄임을 한다(안 묶으면 줄을 바꿔 아래 줄을 덮는다).
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(label, width, lv_font_get_line_height(font_));
    }
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    return label;
}

int DiaryPanel::MakeChip(lv_obj_t* parent, int x, int y, const char* text, uint32_t bg, uint32_t fg) {
    const int w = TextW(font_, text) + 10;
    lv_obj_t* chip = Box(parent, x, y, w, 16, bg, 4);
    MakeLabel(chip, 5, (16 - lv_font_get_line_height(font_)) / 2, text, fg);
    return x + w + 4;
}

void DiaryPanel::MakeArrows(int page, int count) {
    if (count <= 1) return;
    const bool up = page > 0, down = page < count - 1;
    lv_obj_t* u = Box(body_, kArrowX, kUpY, kArrowW, kArrowH, up ? kButton : kButtonOff, kBoxRadius);
    lv_obj_t* d = Box(body_, kArrowX, kDownY, kArrowW, kArrowH, down ? kButton : kButtonOff, kBoxRadius);
    // 갈무리 글꼴에 ▲▼ 가 없어(실기기 11/9) 네모를 쌓아 도트 삼각형으로 그린다. 흐린 쪽은 더 못 넘긴다는 뜻.
    Tri(u, kArrowW, kArrowH, 'u', up ? kMoodInk : kDim);
    Tri(d, kArrowW, kArrowH, 'd', down ? kMoodInk : kDim);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", page + 1);
    lv_obj_t* a = MakeLabel(body_, 0, kPageY, buf, kSoft);
    lv_obj_set_width(a, kArrowW);
    lv_obj_set_x(a, kArrowX);
    lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_CENTER, 0);
    snprintf(buf, sizeof(buf), "/%d", count);
    lv_obj_t* b = MakeLabel(body_, 0, kPageY + 16, buf, kDim);
    lv_obj_set_width(b, kArrowW);
    lv_obj_set_x(b, kArrowX);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
}

lv_obj_t* DiaryPanel::MakeImage(lv_obj_t* parent, int x, int y, const uint8_t* pixels, int w, int h,
                                lv_image_dsc_t* dsc) {
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.stride = w * 2;
    dsc->data_size = w * h * 2;
    dsc->data = pixels;
    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, dsc);
    lv_obj_set_pos(img, x, y);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

const DiaryPhoto* DiaryPanel::PhotoAt(const DiaryEntry& e, int index) {
    if (index < 0 || index >= static_cast<int>(e.photos.size())) return nullptr;
    const std::string& key = e.photo_keys[index];
    if (key.empty()) return nullptr;
    for (const DiaryPhotoPtr& p : photos_) {
        if (p && p->key == key) return p.get();
    }
    if (std::find(asked_.begin(), asked_.end(), key) == asked_.end()) {
        asked_.push_back(key);
        if (need_photo_) need_photo_(e.photos[index], key);
    }
    return nullptr;
}

// 작은 사진 귀퉁이의 「+N」. (right, bottom) 이 표의 오른쪽 아래 모서리다.
void DiaryPanel::MakeMoreBadge(lv_obj_t* parent, int right, int bottom, int more) {
    if (more <= 0) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "+%d", std::min(more, 99));
    const int w = static_cast<int>(strlen(buf)) * 8 + 4, h = 12;
    lv_obj_t* b = Box(parent, right - w, bottom - h, w, h, 0x161008, 3);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &press_start_8, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(kText), 0);
    lv_label_set_text(l, buf);
    lv_obj_center(l);
}

void DiaryPanel::TurnPhoto(int delta) {
    if (entry_ < 0 || entry_ >= static_cast<int>(data_.entries.size())) return;
    const int n = static_cast<int>(data_.entries[entry_].photos.size());
    const int next = std::clamp(photo_index_ + delta, 0, std::max(0, n - 1));
    if (next == photo_index_) return;
    photo_index_ = next;
    Rebuild();
}

void DiaryPanel::Rebuild() {
    if (full_ != nullptr) {
        lv_obj_delete(full_);
        full_ = nullptr;
    }
    if (body_ != nullptr) lv_obj_delete(body_);
    dscs_.clear();   // 그림을 가리키던 lv_image 를 다 지운 뒤에 서술자를 버린다
    body_ = Plain(root_, lv_obj_get_width(root_), lv_obj_get_height(root_));
    // 닫기 X — 모든 화면 공통
    lv_obj_t* close = Box(body_, kCloseX, kBoxY, kCloseW, kBoxH, kClose, kBoxRadius);
    lv_obj_t* x = lv_label_create(close);
    lv_obj_set_style_text_font(x, &press_start_8, 0);
    lv_obj_set_style_text_color(x, lv_color_hex(kText), 0);
    lv_label_set_text(x, "X");
    lv_obj_center(x);
    switch (mode_) {
        case Mode::kList: BuildList(); break;
        case Mode::kRead: BuildRead(); break;
        case Mode::kPhoto: BuildRead(); BuildPhoto(); break;
    }
}

void DiaryPanel::BuildList() {
    if (from_menu_) {   // 만복이 메뉴에서 왔다 — < = 메뉴
        lv_obj_t* back = Box(body_, kBackX, kBoxY, kBackW, kBoxH, kButton, kBoxRadius);
        lv_obj_t* lt = MakeLabel(back, 0, 0, "<", kMoodInk);
        lv_obj_center(lt);
    }
    MakeLabel(body_, from_menu_ ? kBackX + kBackW + 7 : kTitleX, kTitleY, "만복이 일기", kText);
    const int today = TodayDays();
    char sub[48] = "";
    int filled = -1;
    if (due_days_ > 0 && today > 0) {
        const int left = due_days_ - today;
        if (left >= 0) {
            const int g = 280 - left;
            if (g >= 0) {
                snprintf(sub, sizeof(sub), "%d주 %d일 · D-%d", g / 7, g % 7, left);
                filled = std::min(g, 280);
            } else {
                snprintf(sub, sizeof(sub), "D-%d", left);
            }
        } else {
            snprintf(sub, sizeof(sub), "예정일 지나고 %d일", -left);
        }
    }
    if (sub[0] != '\0') {
        lv_obj_t* s = MakeLabel(body_, 0, kSubY, sub, kSoft);
        lv_obj_set_width(s, lv_obj_get_width(root_));
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (filled >= 0) {
        Box(body_, kBarX, kBarY, kBarW, kBarH, kBarBg, 3);
        if (filled > 0) Box(body_, kBarX, kBarY, std::max(kBarH, kBarW * filled / 280), kBarH, kBarFill, 3);
    }
    const int n = static_cast<int>(data_.entries.size());
    if (n == 0) {
        lv_obj_t* e = MakeLabel(body_, 0, 120, data_.valid ? "아직 일기가 없어요" : "일기를 받는 중", kDim);
        lv_obj_set_width(e, lv_obj_get_width(root_));
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    const int pages = (n + kRowsPerPage - 1) / kRowsPerPage;
    list_page_ = std::clamp(list_page_, 0, pages - 1);
    for (int r = 0; r < kRowsPerPage; r++) {
        const int i = list_page_ * kRowsPerPage + r;
        if (i >= n) break;
        const DiaryEntry& e = data_.entries[i];
        const int y = kRowTop + r * kRowH;
        Box(body_, kTextX, y - 3, kTextW, 1, kSep, 0);
        const std::string date = ShortDate(e.date);
        MakeLabel(body_, kTextX, y + 2, date.c_str(), kDate);
        int x = kTextX + TextW(font_, date.c_str()) + 6;
        x = MakeChip(body_, x, y + 1, DiaryWriterName(e.writer), e.writer == "dad" ? kDad : kMom, kChipInk);
        for (size_t m = 0; m < e.mood.size() && m < 2; m++) {
            if (x + TextW(font_, e.mood[m].c_str()) + 10 > kMiniX - 4) break;
            x = MakeChip(body_, x, y + 1, e.mood[m].c_str(), kMoodBg, kMoodInk);
        }
        const bool has_photo = !e.photo.empty();
        MakeLabel(body_, kTextX, y + 23, FirstParagraph(e.body).c_str(), kText, has_photo ? kMiniX - kTextX - 6 : kTextW);
        if (has_photo) {
            const DiaryPhoto* p = PhotoFor(e);
            if (p != nullptr && p->mini.pixels != nullptr) {
                dscs_.push_back(std::make_unique<lv_image_dsc_t>());
                MakeImage(body_, kMiniX + (24 - p->mini.w) / 2, y + 4, p->mini.pixels, p->mini.w, p->mini.h,
                          dscs_.back().get());
            } else {
                Box(body_, kMiniX, y + 4, 24, 32, kMoodBg, 2);
            }
            MakeMoreBadge(body_, kMiniX + 28, y + 40, e.photo_count - 1);
        }
    }
    MakeArrows(list_page_, pages);
}

void DiaryPanel::Layout(const DiaryEntry& e) {
    pages_.clear();
    pages_.emplace_back();
    entry_has_photo_ = !e.photo.empty();
    int y = kTextTop;
    auto narrow = [&]() { return entry_has_photo_ && pages_.size() == 1 && y < kPhotoY + kPhotoH + 2; };
    auto width = [&]() { return narrow() ? kNarrowW : kTextW; };
    auto emit = [&](const std::string& text) {
        if (y + kLineH > kTextBottom) {
            pages_.emplace_back();
            y = kTextTop;
        }
        pages_.back().push_back({narrow() ? kNarrowX : kTextX, y, text});
        y += kLineH;
    };
    size_t start = 0;
    const std::string& body = e.body;
    while (start <= body.size()) {
        size_t end = body.find('\n', start);
        if (end == std::string::npos) end = body.size();
        const std::string para = body.substr(start, end - start);
        start = end + 1;
        if (para.empty()) {
            if (y > kTextTop) y += kParaGap;   // 쪽 맨 위에는 문단 틈을 두지 않는다
            if (end >= body.size()) break;
            continue;
        }
        std::string line;
        size_t w0 = 0;
        while (w0 < para.size()) {
            size_t w1 = para.find(' ', w0);
            if (w1 == std::string::npos) w1 = para.size();
            std::string word = para.substr(w0, w1 - w0);
            w0 = w1 + 1;
            if (word.empty()) continue;
            const std::string cand = line.empty() ? word : line + " " + word;
            if (TextW(font_, cand.c_str()) <= width()) {
                line = cand;
                continue;
            }
            if (!line.empty()) {
                emit(line);
                line.clear();
            }
            while (TextW(font_, word.c_str()) > width()) {   // 줄보다 긴 낱말은 글자로 자른다
                const size_t cut = FitPrefix(font_, word, width());
                emit(word.substr(0, cut));
                word = word.substr(cut);
            }
            line = word;
        }
        if (!line.empty()) emit(line);
        if (end >= body.size()) break;
    }
    if (pages_.back().empty() && pages_.size() > 1) pages_.pop_back();
}

int DiaryPanel::PageCount() const {
    if (mode_ == Mode::kList) {
        const int n = static_cast<int>(data_.entries.size());
        return std::max(1, (n + kRowsPerPage - 1) / kRowsPerPage);
    }
    return std::max<int>(1, pages_.size());
}

void DiaryPanel::OpenEntry(int index) {
    if (index < 0 || index >= static_cast<int>(data_.entries.size())) return;
    entry_ = index;
    page_ = 0;
    photo_index_ = 0;
    mode_ = Mode::kRead;
    Layout(data_.entries[index]);
    Rebuild();
}

void DiaryPanel::BuildRead() {
    const DiaryEntry& e = data_.entries[entry_];
    lv_obj_t* back = Box(body_, kBackX, kBoxY, kBackW, kBoxH, kButton, kBoxRadius);
    lv_obj_t* lt = MakeLabel(back, 0, 0, "<", kMoodInk);
    lv_obj_center(lt);
    const std::string date = ShortDate(e.date);
    MakeLabel(body_, kBackX + kBackW + 7, kBoxY + 3, date.c_str(), kDate);
    int x = kBackX + kBackW + 7 + TextW(font_, date.c_str()) + 6;
    x = MakeChip(body_, x, kBoxY + 2, DiaryWriterName(e.writer), e.writer == "dad" ? kDad : kMom, kChipInk);
    for (size_t m = 0; m < e.mood.size() && m < 2; m++) {
        if (x + TextW(font_, e.mood[m].c_str()) + 10 > kCloseX - 4) break;
        x = MakeChip(body_, x, kBoxY + 2, e.mood[m].c_str(), kMoodBg, kMoodInk);
    }
    page_ = std::clamp(page_, 0, PageCount() - 1);
    if (entry_has_photo_ && page_ == 0) {
        const DiaryPhoto* p = PhotoFor(e);
        Box(body_, kPhotoX - 1, kPhotoY - 1, kPhotoW + 2, kPhotoH + 2, 0x46506A, 0);
        if (p != nullptr && p->mid.pixels != nullptr) {
            Box(body_, kPhotoX, kPhotoY, kPhotoW, kPhotoH, 0x000000, 0);
            dscs_.push_back(std::make_unique<lv_image_dsc_t>());
            MakeImage(body_, kPhotoX + (kPhotoW - p->mid.w) / 2, kPhotoY + (kPhotoH - p->mid.h) / 2, p->mid.pixels,
                      p->mid.w, p->mid.h, dscs_.back().get());
        } else {
            Box(body_, kPhotoX, kPhotoY, kPhotoW, kPhotoH, 0x1E2430, 0);
            lv_obj_t* l1 = MakeLabel(body_, kPhotoX, kPhotoY + 50, "사진", kDim);
            lv_obj_set_width(l1, kPhotoW);
            lv_obj_set_style_text_align(l1, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_t* l2 = MakeLabel(body_, kPhotoX, kPhotoY + 66, "받는 중…", kDim);
            lv_obj_set_width(l2, kPhotoW);
            lv_obj_set_style_text_align(l2, LV_TEXT_ALIGN_CENTER, 0);
        }
        MakeMoreBadge(body_, kPhotoX + kPhotoW - 2, kPhotoY + kPhotoH - 2, e.photo_count - 1);
    }
    if (!pages_.empty()) {
        for (const Line& ln : pages_[page_]) MakeLabel(body_, ln.x, ln.y, ln.text.c_str(), kText);
    }
    MakeArrows(page_, PageCount());
}

void DiaryPanel::BuildPhoto() {
    const DiaryEntry& e = data_.entries[entry_];
    const int n = static_cast<int>(e.photos.size());
    if (n == 0) {
        mode_ = Mode::kRead;
        return;
    }
    photo_index_ = std::clamp(photo_index_, 0, n - 1);
    const DiaryPhoto* p = PhotoAt(e, photo_index_);
    if (photo_index_ + 1 < n) PhotoAt(e, photo_index_ + 1);   // 다음 장을 미리 부탁해 둔다
    const int w = lv_obj_get_width(parent_), h = lv_obj_get_height(parent_);
    full_ = Box(parent_, 0, 0, w, h, 0x000000, 0);
    if (p != nullptr && p->big.pixels != nullptr) {
        dscs_.push_back(std::make_unique<lv_image_dsc_t>());
        MakeImage(full_, (w - p->big.w) / 2, (h - p->big.h) / 2, p->big.pixels, p->big.w, p->big.h, dscs_.back().get());
    } else {
        lv_obj_t* l = MakeLabel(full_, 0, h / 2 - 8, "사진 받는 중…", kDim);
        lv_obj_set_width(l, w);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t* close = Box(full_, w - 34, 8, kCloseW, kBoxH, kClose, kBoxRadius);
    lv_obj_t* x = lv_label_create(close);
    lv_obj_set_style_text_font(x, &press_start_8, 0);
    lv_obj_set_style_text_color(x, lv_color_hex(kText), 0);
    lv_label_set_text(x, "X");
    lv_obj_center(x);
    const std::string cap = ShortDate(e.date) + " " + DiaryWriterName(e.writer);
    lv_obj_t* plate = Box(full_, 8, h - 26, TextW(font_, cap.c_str()) + 12, 19, 0x161008, 4);
    lv_obj_set_style_bg_opa(plate, LV_OPA_80, 0);
    MakeLabel(plate, 6, (19 - lv_font_get_line_height(font_)) / 2, cap.c_str(), 0xF0E7DD);
    if (n > 1) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d/%d", photo_index_ + 1, n);
        const int cw = TextW(font_, buf) + 12;
        lv_obj_t* cnt = Box(full_, w - 8 - cw, h - 26, cw, 19, 0x161008, 4);
        lv_obj_set_style_bg_opa(cnt, LV_OPA_80, 0);
        MakeLabel(cnt, 6, (19 - lv_font_get_line_height(font_)) / 2, buf, 0xF0E7DD);
        // 마지막 장인데 일기에 사진이 더 있으면(서버 상한 넘음) 앱에서 보라고 알린다
        if (photo_index_ == n - 1 && e.photo_count > n) {
            char more[48];
            snprintf(more, sizeof(more), "나머지 %d장은 앱에서", e.photo_count - n);
            const int mw = TextW(font_, more) + 12;
            lv_obj_t* mp = Box(full_, 8, 8, mw, 19, 0x161008, 4);
            lv_obj_set_style_bg_opa(mp, LV_OPA_80, 0);
            MakeLabel(mp, 6, (19 - lv_font_get_line_height(font_)) / 2, more, 0xF0E7DD);
        }
        // 넘길 수 있는 쪽에만 화살표
        for (int side = 0; side < 2; side++) {
            const bool can = side == 0 ? photo_index_ > 0 : photo_index_ < n - 1;
            if (!can) continue;
            lv_obj_t* a = Box(full_, side == 0 ? 6 : w - 6 - kArrowW, (h - kArrowH) / 2, kArrowW, kArrowH, 0x161008,
                              kBoxRadius);
            lv_obj_set_style_bg_opa(a, LV_OPA_60, 0);
            Tri(a, kArrowW, kArrowH, side == 0 ? 'l' : 'r', 0xF0E7DD);
        }
    }
}

void DiaryPanel::Turn(int delta) {
    if (mode_ == Mode::kList) {
        const int next = std::clamp(list_page_ + delta, 0, PageCount() - 1);
        if (next == list_page_) return;
        list_page_ = next;
    } else if (mode_ == Mode::kRead) {
        const int next = std::clamp(page_ + delta, 0, PageCount() - 1);
        if (next == page_) return;
        page_ = next;
    } else {
        return;
    }
    Rebuild();
}

void DiaryPanel::SetData(const DiaryData& data) {
    // 보던 글의 id 를 기억해 두고, 새 목록에서 같은 글을 찾아 그대로 보여 준다.
    const std::string open_id = (mode_ != Mode::kList && entry_ >= 0 && entry_ < static_cast<int>(data_.entries.size()))
                                    ? data_.entries[entry_].id
                                    : std::string();
    data_ = data;
    if (!open_id.empty()) {
        int found = -1;
        for (size_t i = 0; i < data_.entries.size(); i++) {
            if (data_.entries[i].id == open_id) found = static_cast<int>(i);
        }
        if (found < 0) {
            mode_ = Mode::kList;
        } else {
            entry_ = found;
            // 같은 글이면 쪽은 그대로 두고 줄만 다시 나눈다(글이 고쳐졌을 수 있다)
            const int keep = page_;
            Layout(data_.entries[entry_]);
            page_ = keep;
        }
    }
    Rebuild();
}

void DiaryPanel::SetPhoto(const DiaryPhotoPtr& photo) {
    if (!photo) return;
    for (DiaryPhotoPtr& p : photos_) {
        if (p && p->key == photo->key) {
            p = photo;
            Rebuild();
            return;
        }
    }
    photos_.push_back(photo);
    if (photos_.size() > 4) {   // 목록 한 쪽(3) + 지금 글. 버린 사진은 다시 부탁할 수 있게
        const std::string dropped = photos_.front() ? photos_.front()->key : std::string();
        photos_.erase(photos_.begin());
        asked_.erase(std::remove(asked_.begin(), asked_.end(), dropped), asked_.end());
    }
    Rebuild();
}

void DiaryPanel::PhotoFailed(const std::string& key) {
    asked_.erase(std::remove(asked_.begin(), asked_.end(), key), asked_.end());
}

DiaryPanel::Result DiaryPanel::OnTouch(const Display::TouchEvent& ev) {
    using Kind = Display::TouchEvent;
    Result r;
    last_input_tick_ = lv_tick_get();
    if (mode_ == Mode::kPhoto) {
        // 여러 장: 왼쪽·오른쪽 1/4 을 누르거나 옆으로 밀면 앞뒤 장, 가운데·X·아래로 밀기는 글로.
        // 한 장이면 예전처럼 어디를 눌러도 글로 돌아간다.
        const int n = static_cast<int>(data_.entries[entry_].photos.size());
        const int sw = lv_obj_get_width(parent_);
        const bool on_close = ev.x0 >= sw - 50 && ev.y0 < 44;
        if (n > 1 && ev.kind == Kind::kSwipeLeft) {
            TurnPhoto(+1);
        } else if (n > 1 && ev.kind == Kind::kSwipeRight && photo_index_ > 0) {
            TurnPhoto(-1);
        } else if (n > 1 && ev.kind == Kind::kTap && !on_close && (ev.x0 < sw / 4 || ev.x0 >= sw - sw / 4)) {
            TurnPhoto(ev.x0 < sw / 4 ? -1 : +1);
        } else if (ev.kind == Kind::kTap || ev.kind == Kind::kSwipeDown || ev.kind == Kind::kSwipeRight) {
            mode_ = Mode::kRead;
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
    if (ev.kind == Kind::kSwipeRight && mode_ == Mode::kRead) {
        mode_ = Mode::kList;
        Rebuild();
        return r;
    }
    if (ev.kind != Kind::kTap) return r;
    const int x = ev.x0, y = ev.y0;
    if (x >= kCloseHitLeft && y < kTopHitBottom) {
        r.close = true;
        return r;
    }
    if (x >= kArrowHitLeft && PageCount() > 1) {
        if (y >= kMargin + kUpY - 8 && y < kMargin + kUpY + kArrowH + 8) Turn(-1);
        if (y >= kMargin + kDownY - 8 && y < kMargin + kDownY + kArrowH + 8) Turn(+1);
        return r;
    }
    if (mode_ == Mode::kList) {
        if (from_menu_ && x < kBackHitRight && y < kTopHitBottom) {
            r.menu = true;
            return r;
        }
        const int ry = y - kMargin - kRowTop + 3;
        if (ry < 0) return r;
        const int row = ry / kRowH;
        const int i = list_page_ * kRowsPerPage + row;
        if (row < kRowsPerPage && i < static_cast<int>(data_.entries.size())) OpenEntry(i);
        return r;
    }
    // 글
    if (x < kBackHitRight && y < kTopHitBottom) {
        mode_ = Mode::kList;
        list_page_ = entry_ / kRowsPerPage;
        Rebuild();
        return r;
    }
    if (entry_has_photo_ && page_ == 0 && x >= kMargin + kPhotoX && x < kMargin + kPhotoX + kPhotoW &&
        y >= kMargin + kPhotoY && y < kMargin + kPhotoY + kPhotoH) {
        const DiaryPhoto* p = PhotoFor(data_.entries[entry_]);
        if (p != nullptr && p->big.pixels != nullptr) {
            mode_ = Mode::kPhoto;
            photo_index_ = 0;
            Rebuild();
        }
    }
    return r;
}
