#include "weather_fetcher.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include "board.h"
#include "display/lcd_display.h"
#include "settings.h"
#include "usage_receiver.h"
#include "weather_panel.h"

#define TAG "Weather"

namespace {

constexpr const char* kNvsNamespace = "weather";
constexpr int32_t kGeoScale = 10000;                       // 1e-4 도(약 11m) 단위 정수로 저장
constexpr int32_t kDefaultLat = 375665, kDefaultLon = 1269780;   // 서울 — 맥 push_weather.py 기본값과 같다
constexpr uint32_t kFirstDelayMs = 5 * 1000;               // 네트워크가 붙고 DNS 가 자리 잡을 틈
constexpr uint32_t kPeriodMs = 30 * 60 * 1000;             // 맥 launchd 와 같은 30분
constexpr uint32_t kRetryMs = 5 * 60 * 1000;               // 실패하면 5분 뒤
constexpr uint32_t kTimeWaitMs = 30 * 1000;                // 시간이 안 맞았으면 30초 뒤 다시 본다
constexpr int kHttpTimeoutMs = 10 * 1000;
constexpr uint32_t kTaskStack = 4096;
constexpr UBaseType_t kTaskPriority = 2;                   // 화면·콕(3)·메인(10)보다 낮게

// WMO 날씨 코드 → kind. 맥 push_weather.py 와 같은 표에서 2(구름 조금)만 "partly" 로 나눴다.
const char* KindOf(int code) {
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code == 95 || code == 96 || code == 99) {
        return "rain";
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return "snow";
    }
    if (code <= 1) return "clear";
    if (code == 2) return "partly";
    return "cloud";   // 3·45·48 과 그 밖
}

// "2026-11-09" → 요일(0 = 일요일). Sakamoto 공식.
int WeekdayOf(const char* date) {
    int y = 0, m = 0, d = 0;
    if (date == nullptr || sscanf(date, "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12) return 0;
    static const int kT[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + kT[m - 1] + d) % 7;
}

int8_t RoundTemp(const cJSON* v) {
    return static_cast<int8_t>(cJSON_IsNumber(v) ? LV_CLAMP(-99, lround(v->valuedouble), 99) : 0);
}

// 시간별·이번 주 예보를 뽑는다. 본문은 1KB 남짓이라 cJSON 으로 통째로 읽는다.
// 시간별 첫 칸은 지금 시각이 든 시간(예 14:30 이면 14:00)이라 건너뛰고 다음 5시간을 쓴다.
void ParseForecast(const cJSON* root, WeatherForecast* out) {
    const cJSON* hourly = cJSON_GetObjectItem(root, "hourly");
    const cJSON* htime = cJSON_GetObjectItem(hourly, "time");
    const cJSON* htemp = cJSON_GetObjectItem(hourly, "temperature_2m");
    const cJSON* hcode = cJSON_GetObjectItem(hourly, "weather_code");
    const cJSON* hday = cJSON_GetObjectItem(hourly, "is_day");
    const time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    const int hn = cJSON_GetArraySize(htime);
    for (int i = 0; i < hn && out->hour_count < WeatherForecast::kHours; i++) {
        const cJSON* t = cJSON_GetArrayItem(htime, i);
        const cJSON* code = cJSON_GetArrayItem(hcode, i);
        if (!cJSON_IsString(t) || strlen(t->valuestring) < 13 || !cJSON_IsNumber(code)) continue;
        const int hour = atoi(t->valuestring + 11);   // "YYYY-MM-DDTHH:MM"
        if (i == 0 && hour == tm_now.tm_hour) continue;
        const cJSON* day = cJSON_GetArrayItem(hday, i);
        WeatherForecast::Hour& h = out->hours[out->hour_count++];
        h.hour = static_cast<int8_t>(hour);
        h.icon = UsageWeatherIconFor(KindOf(code->valueint), cJSON_IsNumber(day) && day->valueint == 0);
        h.temp = RoundTemp(cJSON_GetArrayItem(htemp, i));
    }
    const cJSON* daily = cJSON_GetObjectItem(root, "daily");
    const cJSON* dtime = cJSON_GetObjectItem(daily, "time");
    const cJSON* dcode = cJSON_GetObjectItem(daily, "weather_code");
    const cJSON* dmax = cJSON_GetObjectItem(daily, "temperature_2m_max");
    const cJSON* dmin = cJSON_GetObjectItem(daily, "temperature_2m_min");
    const int dn = cJSON_GetArraySize(dtime);
    for (int i = 0; i < dn && out->day_count < WeatherForecast::kDays; i++) {
        const cJSON* t = cJSON_GetArrayItem(dtime, i);
        const cJSON* code = cJSON_GetArrayItem(dcode, i);
        if (!cJSON_IsString(t) || !cJSON_IsNumber(code)) continue;
        WeatherForecast::Day& d = out->days[out->day_count++];
        d.wday = static_cast<int8_t>(WeekdayOf(t->valuestring));
        d.icon = UsageWeatherIconFor(KindOf(code->valueint), false);   // 하루 예보는 낮 그림
        d.tmax = RoundTemp(cJSON_GetArrayItem(dmax, i));
        d.tmin = RoundTemp(cJSON_GetArrayItem(dmin, i));
    }
}

// lvgl_display 의 시계 표시와 같은 기준 — SNTP 전에는 1970년이다.
bool TimeSynced() {
    const time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    return tm_now.tm_year >= 2025 - 1900;
}

// 1e-4 도 정수 → "37.5665" / "-0.1278". 부동소수 printf 를 안 써서 스택을 아낀다.
void FormatScaled(char* out, size_t len, int32_t v) {
    const int32_t a = std::abs(v);
    snprintf(out, len, "%s%ld.%04ld", v < 0 ? "-" : "", static_cast<long>(a / kGeoScale),
             static_cast<long>(a % kGeoScale));
}

}  // namespace

void WeatherFetcher::Start() {
    TaskHandle_t task = task_.load();
    if (task != nullptr) {
        if (!last_ok_) {
            xTaskNotifyGive(task);   // 끊겼다 다시 붙었다 — 5분 기다리지 않고 바로
        }
        return;
    }
    if (xTaskCreate(TaskEntry, "weather", kTaskStack, this, kTaskPriority, &task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return;
    }
    task_ = task;
}

bool WeatherFetcher::SetGeo(double lat, double lon) {
    if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180) {
        return false;
    }
    {
        Settings settings(kNvsNamespace, true);
        settings.SetInt("lat", static_cast<int32_t>(lround(lat * kGeoScale)));
        settings.SetInt("lon", static_cast<int32_t>(lround(lon * kGeoScale)));
    }
    ESP_LOGI(TAG, "geo saved");
    TaskHandle_t task = task_.load();
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
    return true;
}

void WeatherFetcher::TaskEntry(void* arg) {
    static_cast<WeatherFetcher*>(arg)->Run();
}

void WeatherFetcher::Run() {
    vTaskDelay(pdMS_TO_TICKS(kFirstDelayMs));
    for (;;) {
        uint32_t wait_ms = kPeriodMs;
        if (!TimeSynced()) {
            wait_ms = kTimeWaitMs;
        } else {
            last_ok_ = FetchOnce();
            if (!last_ok_) {
                wait_ms = kRetryMs;
            }
        }
        // 좌표가 바뀌거나 다시 붙으면 깨운다(SetGeo·Start).
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

bool WeatherFetcher::FetchOnce() {
    int32_t lat, lon;
    {
        Settings settings(kNvsNamespace, false);
        lat = settings.GetInt("lat", kDefaultLat);
        lon = settings.GetInt("lon", kDefaultLon);
    }
    char lat_text[16], lon_text[16];
    FormatScaled(lat_text, sizeof(lat_text), lat);
    FormatScaled(lon_text, sizeof(lon_text), lon);
    // https 는 인증서 묶음·메모리가 더 들어 평문 http 로 받는다. 날씨라 가로채여도 잃을 게 없다.
    std::string url = "http://api.open-meteo.com/v1/forecast?latitude=";
    url += lat_text;
    url += "&longitude=";
    url += lon_text;
    url += "&current=temperature_2m,weather_code,is_day";
    // 날씨 판(시간별·이번 주). 시간대를 보드 시계(KST)와 맞춰야 시각·요일이 맞는다.
    url += "&hourly=temperature_2m,weather_code,is_day&daily=weather_code,temperature_2m_max,temperature_2m_min"
           "&timezone=Asia%2FSeoul&forecast_days=7&forecast_hours=6";

    auto http = Board::GetInstance().GetNetwork()->CreateHttp();
    http->SetTimeout(kHttpTimeoutMs);
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "open failed (err %d)", http->GetLastError());
        return false;
    }
    const int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGW(TAG, "status %d", status);
        http->Close();
        return false;
    }
    const std::string body = http->ReadAll();
    http->Close();

    cJSON* root = cJSON_Parse(body.c_str());
    const cJSON* cur = cJSON_GetObjectItem(root, "current");
    const cJSON* jtemp = cJSON_GetObjectItem(cur, "temperature_2m");
    const cJSON* jcode = cJSON_GetObjectItem(cur, "weather_code");
    const cJSON* jday = cJSON_GetObjectItem(cur, "is_day");
    if (!cJSON_IsNumber(jtemp) || !cJSON_IsNumber(jcode)) {
        ESP_LOGW(TAG, "bad body (%u bytes)", static_cast<unsigned>(body.size()));
        cJSON_Delete(root);
        return false;
    }
    const char* kind = KindOf(jcode->valueint);
    const int temp = static_cast<int>(lround(jtemp->valuedouble));
    const bool night = cJSON_IsNumber(jday) && jday->valueint == 0;
    const int code = jcode->valueint;
    WeatherForecast forecast;
    forecast.valid = true;
    forecast.now_icon = UsageWeatherIconFor(kind, night);
    forecast.now_temp = static_cast<int8_t>(LV_CLAMP(-99, temp, 99));
    ParseForecast(root, &forecast);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "code %d -> %s %d%s (stack left %u)", code, kind, temp, night ? " night" : "",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    UsageReceiver::ApplyWeather(kind, true, temp, night);
    // 예보는 보드가 받은 것만 판에 간다 — 맥 push {"weather":…} 는 시계 칸만 바꾼다.
    auto* display = dynamic_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
    if (display != nullptr) {
        display->SetUsageForecast(forecast);
    }
    ESP_LOGI(TAG, "forecast %u hours, %u days", forecast.hour_count, forecast.day_count);
    return true;
}
