#pragma once

// 만복이 육아일기(docs/manbok-diary.md)를 보드가 직접 받아 온다. 일기 앱의
// GET /api/board 를 10분마다 부르고, 일기 판이 부탁한 사진은 GET /api/board/photo 로 받아 풀어 준다.
// 맥이 꺼져 있어도 돈다(날씨와 같은 방식, weather_fetcher.h).
//
// 설정은 NVS "diary": url(기본 CONFIG_DIARY_DEFAULT_URL, 비면 끔)·token(보드 열쇠)·me("mom"|"dad").
// POST /usage {"diary":{"token":"…","me":"mom"}} 로 넣는다. token 이 없으면 아무것도 받지 않는다.
// 목록에 처음 보는 일기가 있고 쓴 사람이 me 가 아니면 알림(글 + 만복이 옹알이)을 띄운다. 본 일기 id 는
// NVS "seen"(쉼표로 이은 최근 30개)에 둔다 — 같은 날 상대 글·지난 날짜 글도 잡고, 숨겼다 되살린 글은
// 다시 울리지 않는다. 처음 켰을 때는 조용히 기억만 한다. me 가 비어 있으면 알리지 않는다.
//
// 만복이 메뉴(초음파·엄마 건강·출산 준비)는 GET /api/board?part=ultrasound|health|prep 로 따로 받는다.
// 10분 주기에는 섞지 않고 메뉴·칸을 열 때만(RequestParts) 받는다. 5분 안에 받은 갈래는 건너뛴다.

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>

#include "display/usage/manbok_data.h"

struct cJSON;

class DiaryFetcher {
public:
    static DiaryFetcher& GetInstance() {
        static DiaryFetcher instance;
        return instance;
    }

    // 네트워크가 붙은 뒤에 부른다. 여러 번 불려도 안전하다.
    void Start();
    // 일기 판이 사진을 부탁한다(url 은 앱 주소 뒤 경로, key 는 캐시 이름). 막지 않는다.
    void RequestPhoto(const std::string& url, const std::string& key);
    // 지금 다시 받기(시험·설정 바뀜).
    void Refresh();
    // 만복이 메뉴 세 갈래를 받아 달라. force 면 5분 규칙을 건너뛴다. 막지 않는다.
    void RequestParts(bool force = false);
    // POST /usage 의 "diary"(설정)·"diary_fetch"(지금 받기)·"diary_test_new"(알림 시험)·
    // "manbok_fetch"(메뉴 세 갈래 지금 받기). 받아들였으면 true.
    bool ApplyJson(const cJSON* root);

private:
    DiaryFetcher() = default;
    DiaryFetcher(const DiaryFetcher&) = delete;
    DiaryFetcher& operator=(const DiaryFetcher&) = delete;

    static void TaskEntry(void* arg);
    void Run();
    bool FetchList();
    void FetchPhoto(const std::string& url, const std::string& key);
    void FetchParts(bool force);
    bool FetchPart(int part);   // 0 ultrasound · 1 health · 2 prep. 받은 값은 parts_ 에

    std::atomic<bool> parts_wanted_{false};
    std::atomic<bool> parts_force_{false};
    // 아래는 받기 태스크만 만진다
    ManbokData parts_;
    uint32_t part_ok_tick_[3] = {};
    bool part_ever_ok_[3] = {};

    std::atomic<TaskHandle_t> task_{nullptr};
    std::atomic<bool> last_ok_{false};
    std::atomic<bool> list_wanted_{false};   // 설정·시험·재접속이 「목록 지금」을 부탁했다
    std::mutex mutex_;
    std::deque<std::pair<std::string, std::string>> photo_queue_;   // (url, key)
};
