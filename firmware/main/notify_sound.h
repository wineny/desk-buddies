#pragma once

// 알림 소리: 콕·답장이 오거나 손님 캐릭터가 걸어 들어올 때 짧은 소리를 낸다. 장면의 캐릭터를
// 누르면 그 캐릭터 목소리(두 개 중 하나를 무작위로)를 낸다. 소리는 tools/import_notify_sounds.py 가
// 녹음에서 잘라 만든 16kHz 모노 PCM 이고, ES8311 코덱 출력만 쓴다(마이크·서버 대화는 여전히 없다).
//
// 전용 태스크 하나가 코덱을 혼자 쓴다. Play()·Test() 는 큐에 넣고 바로 돌아오므로 PokeLink·
// httpd·LVGL 어느 태스크에서 불러도 막지 않는다. 코덱은 재생할 때만 열고(PA 도 그때만 켬) 끝나면 닫는다.
//
// 설정은 POST /usage {"sound":{"vol":0~100,"quiet_from":23,"quiet_to":7}} 로 바꾸고 NVS "sound" 에
// 둔다. vol 0 은 음소거. 조용한 시간(기본 23~07시)에는 울리지 않되, 시계가 아직 안 맞았으면 울린다.

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <cstdint>

struct cJSON;

class NotifySound {
public:
    enum Kind : uint8_t {
        kPoke = 0,       // 콕·답장 = 로나 야옹
        kGuest = 1,      // 손님 입장 = 로찌 야옹
        kTapRona = 2,    // 캐릭터 탭 — 그 캐릭터 목소리 둘 중 하나
        kTapRozzi = 3,
        kTapManbok = 4,
    };

    static NotifySound& GetInstance() {
        static NotifySound instance;
        return instance;
    }

    // 부팅 때 한 번. 코덱·I2S 를 준비하고 재생 태스크를 띄운다. 실패하면 소리 없이 지낸다.
    void Start();
    // 음소거·조용한 시간을 보고 울린다. 막지 않는다.
    void Play(Kind kind);
    // 장면에서 캐릭터를 눌렀을 때. who = 장면 순서(0 로나, 1 로찌, 2 만복이). 연타는
    // kTapGapMs 에 한 번만, 다른 소리가 재생·대기 중이면 건너뛴다. 음소거·조용한 시간도 따른다.
    void PlayTap(int who);
    // POST /usage 의 "sound"·"sound_test" 를 처리한다. 하나라도 받아들였으면 true.
    bool ApplyJson(const cJSON* root);

private:
    NotifySound() = default;
    NotifySound(const NotifySound&) = delete;
    NotifySound& operator=(const NotifySound&) = delete;

    static void TaskEntry(void* arg);
    void Run();
    bool Enqueue(Kind kind);
    bool InQuietHours() const;
    void PlayNow(uint8_t item);   // item = (variant << 4) | kind

    QueueHandle_t queue_ = nullptr;
    void* output_dev_ = nullptr;   // esp_codec_dev_handle_t
    std::atomic<int> vol_{50};
    std::atomic<int> quiet_from_{23};
    std::atomic<int> quiet_to_{7};
    std::atomic<uint32_t> last_tap_ms_{0};
};
