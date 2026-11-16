#pragma once

// 만복이 메뉴(docs/manbok-diary.md)가 보여 줄 세 갈래. 보드가 일기 앱의 GET /api/board?part=… 에서 받는다
// (diary_fetcher.cc). 그리기는 manbok_panel.cc. 글자는 서버가 이모지를 지워 보낸다(KS X 1001 + ASCII).

#include <string>
#include <vector>

// 갈래마다 받기 상태. kNone = 아직 안 받음, kLoading = 받는 중, kOk = 받음, kFailed = 마지막 받기 실패
// (예전에 받은 값은 그대로 둔다 — 화면은 「못 받았어요」와 마지막 값을 같이 보여 준다).
enum class ManbokPartState : uint8_t { kNone, kLoading, kOk, kFailed };

// 진료 한 번 = 일기 하나. 사진은 .us.jpg 만, 최대 4장.
struct ManbokVisit {
    std::string id;     // "2026-11-16-mom"
    std::string date;   // "2026-11-16"
    int week = -1;      // 25주 4일 → week 25, day 4. 모르면 -1
    int day = 0;
    std::vector<std::string> photos;       // 받을 주소(서명 포함, 2시간)
    std::vector<std::string> photo_keys;   // path=… (캐시 이름)
};

struct ManbokUltrasound {
    bool valid = false;
    int visits = 0;   // 전체 진료 수(부제 「4번 · 15장」)
    int photos = 0;   // 전체 사진 수
    std::vector<ManbokVisit> items;   // 새 날짜 위, 최대 8번
};

struct ManbokMealItem {
    std::string name;
    bool warn = false;   // risk 가 caution·avoid
};

struct ManbokMeal {
    std::string meal;   // 아침·점심·저녁·간식
    std::vector<ManbokMealItem> items;
};

struct ManbokHealth {
    bool valid = false;
    std::string date;     // 오늘(한국 시각) "2026-11-16"
    int kcal = 0;
    int caffeine_mg = 0;
    int count = 0;        // 오늘 먹은 항목 수. 0 이면 「오늘 기록 없음」
    int caffeine_limit = 200;
    bool has_weight = false;
    float weight_kg = 0;
    std::string weight_date;
    float weight_goal = 60;
    std::vector<ManbokMeal> meals;   // 먹은 끼니만, 아침·점심·저녁·간식 순
    int exercise_week = 0;
    std::string exercise_last_date;   // 비면 마지막 운동 없음
    std::string exercise_last_name;
};

struct ManbokPrepItem {
    std::string due;     // "2027-01-01", 없으면 ""
    std::string title;
};

struct ManbokPrep {
    bool valid = false;
    std::string birth;   // "YYYY-MM-DD"
    int dday = 0;
    int open = 0;        // 안 끝난 일 수
    std::vector<ManbokPrepItem> items;   // 마감 가까운 순, 마감 없는 것은 끝. 최대 20개
};

struct ManbokData {
    ManbokUltrasound ultrasound;
    ManbokHealth health;
    ManbokPrep prep;
    ManbokPartState us_state = ManbokPartState::kNone;
    ManbokPartState health_state = ManbokPartState::kNone;
    ManbokPartState prep_state = ManbokPartState::kNone;
};
