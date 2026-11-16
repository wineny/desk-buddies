#include "diary_fetcher.h"

#include <cJSON.h>
#include <http.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

#include "board.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "diary_data.h"
#include "notify_sound.h"
#include "settings.h"

#define TAG "Diary"

namespace {

// ml307 HttpClient::ReadAll() 은 본문이 8KB(MAX_BODY_CHUNKS_SIZE)를 넘으면 멈춘다:
// 받는 쪽(OnTcpData)이 mutex_ 를 쥔 채 빈자리를 기다리고, ReadAll 은 같은 mutex_ 로 끝을 기다리며
// 아무도 꺼내 가지 않는다. 일기 목록(≈17KB)·사진(≈20KB)은 Read() 로 조금씩 꺼내 읽는다.
bool ReadBody(Http* http, size_t max_bytes, std::string* out) {
    out->clear();
    const size_t expect = http->GetBodyLength();
    if (expect > 0) out->reserve(std::min(expect, max_bytes));
    char buf[1024];
    for (;;) {
        const int n = http->Read(buf, sizeof(buf));
        if (n < 0) return false;
        if (n == 0) break;
        if (out->size() + static_cast<size_t>(n) > max_bytes) return false;
        out->append(buf, static_cast<size_t>(n));
    }
    return expect == 0 || out->size() == expect;
}

constexpr const char* kNvsNamespace = "diary";
constexpr const char* kDefaultUrl = CONFIG_DIARY_DEFAULT_URL;   // 비어 있으면 일기 끔(Kconfig)
constexpr uint32_t kFirstDelayMs = 8 * 1000;               // 날씨(5초) 뒤에
constexpr uint32_t kPeriodMs = 10 * 60 * 1000;             // 10분마다
constexpr uint32_t kRetryMs = 3 * 60 * 1000;               // 실패하면 3분 뒤
constexpr uint32_t kTimeWaitMs = 30 * 1000;
constexpr int kHttpTimeoutMs = 15 * 1000;
constexpr size_t kMaxListBytes = 200 * 1024;               // 서버 상한(10개 × 3,000자)보다 넉넉히
constexpr size_t kMaxPhotoBytes = 300 * 1024;              // 썸네일은 20KB 안팎
constexpr int kMaxPhotosPerEntry = 10;                     // 서버 BOARD_PHOTO_LIMIT 와 같게
constexpr size_t kMaxPartBytes = 32 * 1024;               // 갈래마다 6KB 안팎(서버 목표)
constexpr uint32_t kPartFreshMs = 5 * 60 * 1000;           // 5분 안에 받은 갈래는 다시 안 받는다
constexpr const char* const kPartNames[3] = {"ultrasound", "health", "prep"};
constexpr uint32_t kTaskStack = 8192;                      // TLS 핸드셰이크 + JPEG 풀기
constexpr UBaseType_t kTaskPriority = 2;

bool TimeSynced() {
    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    return t.tm_year >= 2025 - 1900;
}

LcdDisplay* Lcd() { return dynamic_cast<LcdDisplay*>(Board::GetInstance().GetDisplay()); }

std::string Str(const cJSON* obj, const char* key) {
    const cJSON* j = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(j) ? j->valuestring : "";
}

int Int(const cJSON* obj, const char* key, int fallback = 0) {
    const cJSON* j = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(j) ? j->valueint : fallback;
}

// [25, 4] → week 25, day 4
void WeekOf(const cJSON* obj, int* week, int* day) {
    const cJSON* w = cJSON_GetObjectItem(obj, "week");
    if (cJSON_IsArray(w) && cJSON_GetArraySize(w) >= 2 && cJSON_IsNumber(cJSON_GetArrayItem(w, 0))) {
        *week = cJSON_GetArrayItem(w, 0)->valueint;
        *day = cJSON_IsNumber(cJSON_GetArrayItem(w, 1)) ? cJSON_GetArrayItem(w, 1)->valueint : 0;
    }
}

// "/api/board/photo?path=diary/media/x.jpg&exp=…" → "diary/media/x.jpg"
std::string PhotoKeyOf(const std::string& url) {
    const size_t p = url.find("path=");
    if (p == std::string::npos) return "";
    const size_t start = p + 5;
    const size_t end = url.find('&', start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// (sw, sh) 를 (bw, bh) 안에 비율을 지켜 맞춘 크기. 키우지는 않는다.
void FitInside(int sw, int sh, int bw, int bh, int* dw, int* dh) {
    if (sw <= bw && sh <= bh) {
        *dw = sw;
        *dh = sh;
        return;
    }
    if (static_cast<int64_t>(sw) * bh > static_cast<int64_t>(sh) * bw) {
        *dw = bw;
        *dh = std::max(1, static_cast<int>(static_cast<int64_t>(sh) * bw / sw));
    } else {
        *dh = bh;
        *dw = std::max(1, static_cast<int>(static_cast<int64_t>(sw) * bh / sh));
    }
}

// RGB565 상자 평균 축소. 작은 사진(24×32)도 뭉개지지 않게 원본 칸을 평균 낸다.
bool Shrink(const uint8_t* src, int sw, int sh, int sstride, int bw, int bh, DiaryImage* out) {
    int dw = 0, dh = 0;
    FitInside(sw, sh, bw, bh, &dw, &dh);
    auto* dst = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(dw) * dh * 2, MALLOC_CAP_SPIRAM));
    if (dst == nullptr) return false;
    for (int y = 0; y < dh; y++) {
        const int y0 = y * sh / dh, y1 = std::max(y0 + 1, (y + 1) * sh / dh);
        for (int x = 0; x < dw; x++) {
            const int x0 = x * sw / dw, x1 = std::max(x0 + 1, (x + 1) * sw / dw);
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const auto* row = reinterpret_cast<const uint16_t*>(src + static_cast<size_t>(yy) * sstride);
                for (int xx = x0; xx < x1; xx++) {
                    const uint16_t p = row[xx];
                    r += p >> 11;
                    g += (p >> 5) & 0x3F;
                    b += p & 0x1F;
                    n++;
                }
            }
            reinterpret_cast<uint16_t*>(dst)[y * dw + x] =
                static_cast<uint16_t>(((r / n) << 11) | ((g / n) << 5) | (b / n));
        }
    }
    out->pixels = dst;
    out->w = dw;
    out->h = dh;
    return true;
}

}  // namespace

void DiaryFetcher::Start() {
    TaskHandle_t task = task_.load();
    if (task != nullptr) {
        if (!last_ok_) Refresh();
        return;
    }
    if (xTaskCreate(TaskEntry, "diary", kTaskStack, this, kTaskPriority, &task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return;
    }
    task_ = task;
}

void DiaryFetcher::Refresh() {
    list_wanted_ = true;
    TaskHandle_t task = task_.load();
    if (task != nullptr) xTaskNotifyGive(task);
}

void DiaryFetcher::RequestParts(bool force) {
    if (force) parts_force_ = true;
    parts_wanted_ = true;
    TaskHandle_t task = task_.load();
    if (task != nullptr) xTaskNotifyGive(task);
}

void DiaryFetcher::RequestPhoto(const std::string& url, const std::string& key) {
    if (url.empty() || key.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& q : photo_queue_) {
            if (q.second == key) return;
        }
        photo_queue_.emplace_back(url, key);
    }
    TaskHandle_t task = task_.load();   // 사진만 — 목록은 제 시각에
    if (task != nullptr) xTaskNotifyGive(task);
}

bool DiaryFetcher::ApplyJson(const cJSON* root) {
    bool handled = false;
    const cJSON* cfg = cJSON_GetObjectItem(root, "diary");
    if (cJSON_IsObject(cfg)) {
        Settings settings(kNvsNamespace, true);
        // url 이 먼저다: 바뀌면 열쇠를 지운다(같은 와이파이의 누가 url 만 바꿔 열쇠를 빼 가지 못하게).
        for (const char* key : {"url", "token", "me"}) {
            const cJSON* j = cJSON_GetObjectItem(cfg, key);
            if (!cJSON_IsString(j)) continue;
            if (strcmp(key, "me") == 0 && strcmp(j->valuestring, "mom") != 0 && strcmp(j->valuestring, "dad") != 0) {
                ESP_LOGW(TAG, "me must be mom|dad");
                continue;
            }
            if (strcmp(key, "url") == 0) {
                if (strncmp(j->valuestring, "https://", 8) != 0) {
                    ESP_LOGW(TAG, "url must be https");
                    continue;
                }
                if (settings.GetString("url", kDefaultUrl) != j->valuestring) settings.SetString("token", "");
            }
            settings.SetString(key, j->valuestring);
            handled = true;
            ESP_LOGI(TAG, "config %s saved", key);   // 값(열쇠)은 로그에 남기지 않는다
        }
        if (handled) Refresh();
    }
    if (cJSON_IsNumber(cJSON_GetObjectItem(root, "diary_fetch"))) {
        Refresh();
        handled = true;
    }
    if (cJSON_IsNumber(cJSON_GetObjectItem(root, "manbok_fetch"))) {
        RequestParts(true);
        handled = true;
    }
    // 시험: 최신 일기로 새 일기 알림을 띄운다(쓴 사람과 상관없이, 저장 없음).
    if (cJSON_IsNumber(cJSON_GetObjectItem(root, "diary_test_new"))) {
        if (auto* lcd = Lcd()) handled = lcd->ShowDiaryNewTest();
    }
    return handled;
}

void DiaryFetcher::TaskEntry(void* arg) { static_cast<DiaryFetcher*>(arg)->Run(); }

void DiaryFetcher::Run() {
    vTaskDelay(pdMS_TO_TICKS(kFirstDelayMs));
    uint32_t next_list_tick = 0;   // 0 = 지금 받기
    for (;;) {
        // 사진 부탁이 먼저다 — 사람이 판을 보고 기다리고 있다.
        for (;;) {
            std::pair<std::string, std::string> job;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (photo_queue_.empty()) break;
                job = photo_queue_.front();
                photo_queue_.pop_front();
            }
            FetchPhoto(job.first, job.second);
        }
        // 그다음 만복이 메뉴 세 갈래 — 메뉴를 연 사람이 기다린다. 시각이 맞기 전엔(TLS) 미룬다.
        if (TimeSynced() && parts_wanted_.exchange(false)) FetchParts(parts_force_.exchange(false));
        uint32_t wait_ms = kPeriodMs;
        const uint32_t now = xTaskGetTickCount();
        if (list_wanted_.exchange(false)) next_list_tick = 0;
        if (!TimeSynced()) {
            wait_ms = kTimeWaitMs;
        } else if (next_list_tick == 0 || static_cast<int32_t>(now - next_list_tick) >= 0) {
            last_ok_ = FetchList();
            wait_ms = last_ok_ ? kPeriodMs : kRetryMs;
            next_list_tick = now + pdMS_TO_TICKS(wait_ms);
            if (next_list_tick == 0) next_list_tick = 1;
        } else {
            wait_ms = pdTICKS_TO_MS(next_list_tick - now);
        }
        // 사진 부탁·설정·재접속이 깨운다. 목록은 list_wanted_ 가 섰을 때만 앞당긴다.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

bool DiaryFetcher::FetchList() {
    std::string url, token, me, seen;
    {
        Settings settings(kNvsNamespace, false);
        url = settings.GetString("url", kDefaultUrl);
        token = settings.GetString("token");
        me = settings.GetString("me");
        seen = settings.GetString("seen");
    }
    LcdDisplay* lcd_cfg = Lcd();
    // 열쇠와 주소가 다 있는 보드만 만복이 메뉴
    if (lcd_cfg != nullptr) lcd_cfg->SetManbokMenuEnabled(!token.empty() && !url.empty());
    if (!token.empty() && url.empty()) {
        ESP_LOGW(TAG, "token set but no url (CONFIG_DIARY_DEFAULT_URL empty) - send {\"diary\":{\"url\":...}}");
    }
    if (token.empty() || url.empty()) {
        return true;   // 설정 안 된 보드 — 조용히 쉰다
    }
    auto http = Board::GetInstance().GetNetwork()->CreateHttp();
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Authorization", "Bearer " + token);
    if (!http->Open("GET", url + "/api/board")) {
        ESP_LOGW(TAG, "open failed (err %d)", http->GetLastError());
        return false;
    }
    const int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGW(TAG, "list status %d", status);
        http->Close();
        return false;
    }
    if (http->GetBodyLength() > kMaxListBytes) {
        ESP_LOGW(TAG, "list too big (%u)", static_cast<unsigned>(http->GetBodyLength()));
        http->Close();
        return false;
    }
    std::string body;
    const bool read_ok = ReadBody(http.get(), kMaxListBytes, &body);
    http->Close();
    if (!read_ok) {   // 끊김·시간 초과·상한 넘음(chunked 면 길이를 미리 모른다)
        ESP_LOGW(TAG, "list read failed (%u bytes)", static_cast<unsigned>(body.size()));
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    const cJSON* arr = cJSON_GetObjectItem(root, "entries");
    if (!cJSON_IsArray(arr)) {
        ESP_LOGW(TAG, "bad body (%u bytes)", static_cast<unsigned>(body.size()));
        cJSON_Delete(root);
        return false;
    }
    DiaryData data;
    data.valid = true;
    const int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        const cJSON* it = cJSON_GetArrayItem(arr, i);
        DiaryEntry e;
        e.id = Str(it, "id");
        e.date = Str(it, "date");
        e.writer = Str(it, "writer");
        e.body = Str(it, "body");
        e.photo = Str(it, "photo");
        e.photo_key = PhotoKeyOf(e.photo);
        const cJSON* photos = cJSON_GetObjectItem(it, "photos");
        for (int k = 0; cJSON_IsArray(photos) && k < cJSON_GetArraySize(photos) && k < kMaxPhotosPerEntry; k++) {
            const cJSON* s = cJSON_GetArrayItem(photos, k);
            if (!cJSON_IsString(s)) continue;
            const std::string key = PhotoKeyOf(s->valuestring);
            if (key.empty()) continue;
            e.photos.emplace_back(s->valuestring);
            e.photo_keys.push_back(key);
        }
        if (e.photos.empty() && !e.photo.empty() && !e.photo_key.empty()) {   // 옛 서버: 첫 사진만
            e.photos.push_back(e.photo);
            e.photo_keys.push_back(e.photo_key);
        }
        if (!e.photos.empty()) {   // 첫 사진은 photos 의 첫 장과 같게 맞춘다
            e.photo = e.photos.front();
            e.photo_key = e.photo_keys.front();
        }
        const cJSON* pc = cJSON_GetObjectItem(it, "photo_count");
        e.photo_count = std::max(cJSON_IsNumber(pc) ? pc->valueint : 0, static_cast<int>(e.photos.size()));
        const cJSON* mood = cJSON_GetObjectItem(it, "mood");
        for (int m = 0; cJSON_IsArray(mood) && m < cJSON_GetArraySize(mood); m++) {
            const cJSON* s = cJSON_GetArrayItem(mood, m);
            if (cJSON_IsString(s)) e.mood.emplace_back(s->valuestring);
        }
        if (!e.id.empty()) data.entries.push_back(std::move(e));
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "list %u entries (%u bytes, stack left %u)", static_cast<unsigned>(data.entries.size()),
             static_cast<unsigned>(body.size()), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    LcdDisplay* lcd = Lcd();
    if (lcd != nullptr) lcd->SetDiary(data);

    // 새 일기 알림: 처음 보는 id 중 상대가 쓴 가장 최신 글(목록은 최신순).
    std::vector<std::string> seen_ids;
    for (size_t a = 0; a < seen.size();) {
        size_t b = seen.find(',', a);
        if (b == std::string::npos) b = seen.size();
        if (b > a) seen_ids.push_back(seen.substr(a, b - a));
        a = b + 1;
    }
    auto known = [&](const std::string& id) { return std::find(seen_ids.begin(), seen_ids.end(), id) != seen_ids.end(); };
    const DiaryEntry* fresh = nullptr;
    std::vector<std::string> merged;
    for (const DiaryEntry& e : data.entries) {
        if (!known(e.id)) {
            if (fresh == nullptr && !me.empty() && e.writer != me) fresh = &e;
            merged.push_back(e.id);
        }
    }
    if (!merged.empty()) {
        // 새 id 를 앞에, 옛 id 를 뒤에 — 최근 30개만(숨긴 글을 되살려도 30개 안이면 다시 안 울린다).
        for (const std::string& id : seen_ids) {
            if (merged.size() >= 30) break;
            merged.push_back(id);
        }
        std::string joined;
        for (const std::string& id : merged) joined += (joined.empty() ? "" : ",") + id;
        {
            Settings settings(kNvsNamespace, true);
            settings.SetString("seen", joined);
        }
        // 처음 받는 보드는 기억만 한다.
        if (!seen.empty() && fresh != nullptr && lcd != nullptr) {
            ESP_LOGI(TAG, "new diary %s by %s", fresh->id.c_str(), fresh->writer.c_str());
            lcd->ShowDiaryNew(*fresh);
            NotifySound::GetInstance().Play(NotifySound::kTapManbok);
        }
    }
    return true;
}

void DiaryFetcher::FetchPhoto(const std::string& path, const std::string& key) {
    std::string url, token;
    {
        Settings settings(kNvsNamespace, false);
        url = settings.GetString("url", kDefaultUrl);
        token = settings.GetString("token");
    }
    LcdDisplay* lcd = Lcd();
    auto fail = [&](const char* why) {
        ESP_LOGW(TAG, "photo failed: %s", why);
        if (lcd != nullptr) lcd->DiaryPhotoFailed(key);   // 판이 다음에 다시 부탁하게
    };
    if (token.empty() || url.empty()) return fail("no token");
    auto http = Board::GetInstance().GetNetwork()->CreateHttp();
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Authorization", "Bearer " + token);
    if (!http->Open("GET", url + path)) return fail("open");
    const int status = http->GetStatusCode();
    if (status != 200 || http->GetBodyLength() > kMaxPhotoBytes) {
        ESP_LOGW(TAG, "photo status %d (%u bytes)", status, static_cast<unsigned>(http->GetBodyLength()));
        http->Close();
        return fail("status");
    }
    std::string jpeg;
    const bool read_ok = ReadBody(http.get(), kMaxPhotoBytes, &jpeg);
    http->Close();
    if (!read_ok || jpeg.empty()) return fail("read");

    uint8_t* rgb = nullptr;
    size_t len = 0, w = 0, h = 0, stride = 0;
    if (jpeg_to_image(reinterpret_cast<const uint8_t*>(jpeg.data()), jpeg.size(), &rgb, &len, &w, &h, &stride) !=
            ESP_OK ||
        rgb == nullptr) {
        ESP_LOGW(TAG, "photo decode failed (%u bytes)", static_cast<unsigned>(jpeg.size()));
        return fail("decode");
    }
    auto photo = std::make_shared<DiaryPhoto>();
    photo->key = key;
    const bool ok = Shrink(rgb, w, h, stride, 320, 240, &photo->big) &&
                    Shrink(rgb, w, h, stride, 96, 128, &photo->mid) && Shrink(rgb, w, h, stride, 24, 32, &photo->mini) &&
                    Shrink(rgb, w, h, stride, 60, 45, &photo->thumb);
    heap_caps_free(rgb);
    if (!ok) return fail("out of memory");
    ESP_LOGI(TAG, "photo %ux%u -> big %dx%d (stack left %u, internal largest %u)", static_cast<unsigned>(w),
             static_cast<unsigned>(h), photo->big.w, photo->big.h,
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    if (lcd != nullptr) lcd->SetDiaryPhoto(photo);
}

// --- 만복이 메뉴 세 갈래 (docs/manbok-diary.md) ----------------------------------------

void DiaryFetcher::FetchParts(bool force) {
    LcdDisplay* lcd = Lcd();
    ManbokPartState* states[3] = {&parts_.us_state, &parts_.health_state, &parts_.prep_state};
    bool want[3] = {};
    const uint32_t now = xTaskGetTickCount();
    for (int p = 0; p < 3; p++) {
        want[p] = force || !part_ever_ok_[p] || *states[p] == ManbokPartState::kFailed ||
                  now - part_ok_tick_[p] >= pdMS_TO_TICKS(kPartFreshMs);
        if (want[p]) *states[p] = ManbokPartState::kLoading;
    }
    if (!want[0] && !want[1] && !want[2]) return;
    if (lcd != nullptr) lcd->SetManbok(parts_);
    for (int p = 0; p < 3; p++) {
        if (!want[p]) continue;
        const bool ok = FetchPart(p);
        *states[p] = ok ? ManbokPartState::kOk : ManbokPartState::kFailed;
        if (ok) {
            part_ever_ok_[p] = true;
            part_ok_tick_[p] = xTaskGetTickCount();
        }
        if (lcd != nullptr) lcd->SetManbok(parts_);
    }
}

bool DiaryFetcher::FetchPart(int part) {
    std::string url, token;
    {
        Settings settings(kNvsNamespace, false);
        url = settings.GetString("url", kDefaultUrl);
        token = settings.GetString("token");
    }
    if (token.empty() || url.empty()) return false;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp();
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Authorization", "Bearer " + token);
    if (!http->Open("GET", url + "/api/board?part=" + kPartNames[part])) {
        ESP_LOGW(TAG, "part %s open failed (err %d)", kPartNames[part], http->GetLastError());
        return false;
    }
    const int status = http->GetStatusCode();
    if (status != 200 || http->GetBodyLength() > kMaxPartBytes) {
        ESP_LOGW(TAG, "part %s status %d (%u bytes)", kPartNames[part], status,
                 static_cast<unsigned>(http->GetBodyLength()));
        http->Close();
        return false;
    }
    std::string body;
    const bool read_ok = ReadBody(http.get(), kMaxPartBytes, &body);
    http->Close();
    if (!read_ok) {
        ESP_LOGW(TAG, "part %s read failed (%u bytes)", kPartNames[part], static_cast<unsigned>(body.size()));
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    // 옛 서버는 part 를 모르고 일기 목록을 준다 — part 가 맞지 않으면 실패로 친다.
    if (root == nullptr || Str(root, "part") != kPartNames[part]) {
        ESP_LOGW(TAG, "part %s bad body (%u bytes)", kPartNames[part], static_cast<unsigned>(body.size()));
        cJSON_Delete(root);
        return false;
    }
    if (part == 0) {
        ManbokUltrasound us;
        us.valid = true;
        us.visits = Int(root, "visits");
        us.photos = Int(root, "photos");
        const cJSON* items = cJSON_GetObjectItem(root, "items");
        for (int i = 0; cJSON_IsArray(items) && i < cJSON_GetArraySize(items) && i < 8; i++) {
            const cJSON* it = cJSON_GetArrayItem(items, i);
            ManbokVisit v;
            v.id = Str(it, "id");
            v.date = Str(it, "date");
            WeekOf(it, &v.week, &v.day);
            const cJSON* photos = cJSON_GetObjectItem(it, "photos");
            for (int k = 0; cJSON_IsArray(photos) && k < cJSON_GetArraySize(photos) && k < 4; k++) {
                const cJSON* ps = cJSON_GetArrayItem(photos, k);
                if (!cJSON_IsString(ps)) continue;
                const std::string key = PhotoKeyOf(ps->valuestring);
                if (key.empty()) continue;
                v.photos.emplace_back(ps->valuestring);
                v.photo_keys.push_back(key);
            }
            if (!v.photos.empty()) us.items.push_back(std::move(v));
        }
        parts_.ultrasound = std::move(us);
    } else if (part == 1) {
        ManbokHealth h;
        h.valid = true;
        h.date = Str(root, "date");
        const cJSON* today = cJSON_GetObjectItem(root, "today");
        auto amount = [&](const char* key) {   // 0~99999 로 묶는다(NaN·큰 값에서 int 변환이 터지지 않게)
            const cJSON* j = cJSON_GetObjectItem(today, key);
            const double v = cJSON_IsNumber(j) ? j->valuedouble : 0;
            return v >= 0 && v < 99999 ? static_cast<int>(v + 0.5) : (v >= 99999 ? 99999 : 0);
        };
        h.kcal = amount("kcal");
        h.caffeine_mg = amount("caffeine_mg");
        h.count = Int(today, "count");
        h.caffeine_limit = Int(cJSON_GetObjectItem(root, "limits"), "caffeine_mg", 200);
        const cJSON* weight = cJSON_GetObjectItem(root, "weight");
        const cJSON* kg = cJSON_GetObjectItem(weight, "kg");
        if (cJSON_IsNumber(kg)) {
            h.has_weight = true;
            h.weight_kg = static_cast<float>(kg->valuedouble);
            h.weight_date = Str(weight, "date");
        }
        const cJSON* goal = cJSON_GetObjectItem(weight, "goal_max_kg");
        if (cJSON_IsNumber(goal)) h.weight_goal = static_cast<float>(goal->valuedouble);
        const cJSON* meals = cJSON_GetObjectItem(root, "meals");
        for (int i = 0; cJSON_IsArray(meals) && i < cJSON_GetArraySize(meals); i++) {
            const cJSON* m = cJSON_GetArrayItem(meals, i);
            ManbokMeal meal;
            meal.meal = Str(m, "meal");
            const cJSON* items = cJSON_GetObjectItem(m, "items");
            for (int k = 0; cJSON_IsArray(items) && k < cJSON_GetArraySize(items); k++) {
                const cJSON* it = cJSON_GetArrayItem(items, k);
                const std::string risk = Str(it, "risk");
                meal.items.push_back({Str(it, "name"), risk == "caution" || risk == "avoid"});
            }
            if (!meal.items.empty()) h.meals.push_back(std::move(meal));
        }
        const cJSON* ex = cJSON_GetObjectItem(root, "exercise");
        h.exercise_week = Int(ex, "week_count");
        const cJSON* last = cJSON_GetObjectItem(ex, "last");
        if (cJSON_IsObject(last)) {
            h.exercise_last_date = Str(last, "date");
            h.exercise_last_name = Str(last, "name");
        }
        parts_.health = std::move(h);
    } else {
        ManbokPrep pr;
        pr.valid = true;
        pr.birth = Str(root, "birth");
        pr.dday = Int(root, "dday");
        pr.open = Int(root, "open");
        const cJSON* items = cJSON_GetObjectItem(root, "items");
        for (int i = 0; cJSON_IsArray(items) && i < cJSON_GetArraySize(items) && i < 20; i++) {
            const cJSON* it = cJSON_GetArrayItem(items, i);
            pr.items.push_back({Str(it, "due"), Str(it, "title")});
        }
        parts_.prep = std::move(pr);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "part %s ok (%u bytes, stack left %u)", kPartNames[part], static_cast<unsigned>(body.size()),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    return true;
}
