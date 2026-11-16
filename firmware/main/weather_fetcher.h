#pragma once

// 보드가 직접 날씨를 받아 온다(Open-Meteo, 30분마다). 지금 날씨와 함께 시간별(다음 5시간)·
// 이번 주(7일) 예보도 받아 날씨 판(display/usage/weather_panel.h)에 넘긴다. 맥이 꺼져 있어도, 맥이 없는
// 두 번째 보드에서도 시계 칸 날씨 그림과 날씨 옷이 돈다. 받은 값은 맥의
// POST {"weather":…} 와 같은 길(UsageReceiver::ApplyWeather)로 들어간다.
// 맥도 계속 보낼 수 있다 — 나중에 온 쪽이 이긴다.
//
// 좌표는 NVS "weather" 의 lat/lon(1e-4 도 단위 정수). 없으면 서울.
// POST /usage {"geo":{"lat":37.5,"lon":127.0}} 로 바꾼다.
//
// 전용 태스크 하나(낮은 우선순위)가 HTTP 를 혼자 쓴다. 화면 태스크·사용량 서버를 막지 않고,
// 실패는 로그만 남기고 5분 뒤 다시 해 본다.

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

class WeatherFetcher {
public:
    static WeatherFetcher& GetInstance() {
        static WeatherFetcher instance;
        return instance;
    }

    // 네트워크가 붙은 뒤에 부른다. 재접속마다 다시 불려도 안전하다 — 지난번에 실패했으면 곧 다시 받는다.
    void Start();
    // 좌표를 NVS 에 저장하고 곧 다시 받는다. 위도 ±90·경도 ±180 밖이면 false.
    bool SetGeo(double lat, double lon);

private:
    WeatherFetcher() = default;
    WeatherFetcher(const WeatherFetcher&) = delete;
    WeatherFetcher& operator=(const WeatherFetcher&) = delete;

    static void TaskEntry(void* arg);
    void Run();
    bool FetchOnce();

    std::atomic<TaskHandle_t> task_{nullptr};
    std::atomic<bool> last_ok_{false};
};
