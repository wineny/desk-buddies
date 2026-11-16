#pragma once

// 호스트(맥)가 밀어 넣는 클로드 사용량을 받는 작은 HTTP 서버.
//
// 왜 서버인가: 호스트가 "응답이 끝날 때마다" 밀어 넣는 구조라 보드가 받는 쪽이
// 되어야 한다. MCP 는 클라우드가 LLM 판단으로 호출하는 구조라 푸시 경로가
// 아니다. esp_http_server 는 이 저장소의 다른 보드들이 이미 쓰고 있어 새
// 의존성이 없다.
//
// 받는 형식 (POST /usage, application/json). 두 필드 모두 선택이고 최소
// 하나는 있어야 한다:
//   {
//     "cells": [                          // 사용량 칸 4개 (push_usage.py)
//       {"name": "5h", "left": "27%", "right": "2h 40m"}, ...
//     ],
//     "state": "working",                 // 상태 화면 (claude_state.py)
//     "ctx": "Bash"                       // 좌하에 찍을 한 줄 (ASCII)
//   }
// state: idle|thinking|working|juggling|permission|error|done.
// NVS 에는 cells 만 저장한다 — 재부팅 때 낡은 상태가 되살아나면 안 된다.

#include <esp_err.h>
#include <esp_http_server.h>

class UsageReceiver {
public:
    static UsageReceiver& GetInstance() {
        static UsageReceiver instance;
        return instance;
    }

    // 네트워크가 붙은 뒤에 부른다. 두 번 불러도 안전하다.
    void Start();
    void Stop();

    // 부팅 직후 NVS 에 저장해 둔 마지막 값을 화면에 되살린다.
    // 호스트가 한 번도 안 보냈으면 아무것도 하지 않는다.
    static void RestoreLastStats();

    // 맥 연결 안내(2단계)를 이번 부팅 동안 다시 띄우지 않는다 — 안내 화면을 탭해 건너뛸 때.
    static void SkipMacSetupGuide();

    // 날씨 한 건 반영 — POST {"weather":…} 와 같은 길(파티클·AUTO 옷·시계 칸 그림).
    // 보드가 직접 받아 온 날씨(weather_fetcher.cc)가 쓴다. 아무 태스크에서 불러도 된다.
    static void ApplyWeather(const char* kind, bool has_temp, int temp, bool night);

private:
    UsageReceiver() = default;
    ~UsageReceiver();
    UsageReceiver(const UsageReceiver&) = delete;
    UsageReceiver& operator=(const UsageReceiver&) = delete;

    static esp_err_t HandlePost(httpd_req_t* req);
    static esp_err_t HandleScreen(httpd_req_t* req);   // GET /screen — 화면 덤프 (QA)
    // 받은 JSON 문자열을 화면에 반영한다. 성공하면 true.
    static bool ApplyJson(const char* json, bool persist);

    httpd_handle_t server_ = nullptr;
};
