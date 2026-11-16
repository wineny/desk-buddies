#pragma once

// 콕 찌르기(docs/poke.md): 같은 WiFi 의 보드끼리 UDP 로 하트를 주고받는다. 중계 서버도 짝
// 맺기도 없다 — 같은 망에서 매직이 맞으면 받는다.
//
// 전용 태스크 하나가 소켓을 혼자 쓴다. 터치 태스크는 Send() 로 큐에 넣고 바로 돌아오고,
// 화면은 LcdDisplay 공개 메서드(안에서 DisplayLockGuard)로만 만진다.

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

class PokeLink {
public:
    static PokeLink& GetInstance() {
        static PokeLink instance;
        return instance;
    }

    // 네트워크가 붙은 뒤에 부른다. 재접속마다 다시 불려도 안전하다 — 소켓은 INADDR_ANY 에
    // 묶여 있어 WiFi 가 끊겼다 붙어도 그대로 산다.
    void Start();
    // phrase 0 = 문구 없는 콕, 1~5 = 문구. 막지 않는다. 큐가 없거나 가득 차면 false.
    bool Send(uint8_t phrase);
    // 받은 문구에 답한다. reply 1~3. 받은 쪽 화면에 답장과 「읽음」이 뜬다.
    bool SendReply(uint8_t reply);
    // 놀러가기(docs/trip.md). character 는 내 보드의 캐릭터 비트 묶음. 상대 답은 LcdDisplay 로 온다.
    bool SendTrip(uint8_t character);
    // 상대 답을 기다리는 중인가. 다시 눌러도 시한이 늘어나지 않게 막는 데 쓴다.
    bool TripAsking() const { return trip_asking_; }
    // 놀러오기를 보내고 상대가 오기를 기다리는 중인가. 다시 눌러도 또 보내지 않는다.
    bool InviteWaiting() const { return invite_waiting_; }
    // 손님이 떠났다 — 상대 보드에 캐릭터를 돌려보낸다. 화면 코드(lvgl 락 안)에서 불러도 막지 않는다.
    bool SendTripHome();
    // 놀러오기: 상대 캐릭터를 부른다. 상대가 받으면 상대 쪽에서 평소 놀러가기(type 3)가 시작된다.
    bool SendTripInvite();
    // 집으로 가기: 내 캐릭터가 상대 보드에 손님으로 가 있다. 상대에게 손님을 내보내 달라고 한다.
    bool SendTripCallHome();

private:
    static constexpr int kSeenCount = 8;

    struct OutItem {
        uint8_t type;
        uint8_t value;
    };

    PokeLink() = default;
    PokeLink(const PokeLink&) = delete;
    PokeLink& operator=(const PokeLink&) = delete;

    static void TaskEntry(void* arg);
    void Run();
    bool OpenSocket();
    void CloseSocket();
    void Transmit(uint8_t type, uint16_t seq, uint8_t phrase, uint32_t ip);
    void TransmitRound();              // 브로드캐스트 2종 + 마지막 상대 유니캐스트 한 벌
    void PumpOutgoing(int64_t now_ms);  // 큐 → 재전송 → 시간 초과
    void Receive();
    void HandlePacket(const uint8_t* buf, size_t len, uint32_t src_ip);
    bool AlreadySeen(const uint8_t* mac, uint16_t seq);  // 처음 보면 기억하고 false
    void HandleTripRequest(uint8_t character);
    void HandleOwnAck(int64_t now_ms);                   // 내가 보낸 패킷의 ACK 가 왔다
    void HandleTripAnswer(uint16_t seq, uint8_t value, uint32_t src_ip);
    void HandleTripInvite();

    QueueHandle_t queue_ = nullptr;
    int sock_ = -1;
    uint8_t own_mac_[6] = {};
    uint32_t peer_ip_ = 0;       // 마지막으로 알게 된 상대 IP (네트워크 바이트 순서, RAM 만)
    uint16_t next_seq_ = 0;

    // 지금 보내는 콕. 새 콕이 오면 덮어쓴다.
    bool out_active_ = false;
    uint16_t out_seq_ = 0;
    uint8_t out_type_ = 0;
    uint8_t out_phrase_ = 0;
    uint8_t out_tries_ = 0;
    int64_t out_next_ms_ = 0;
    int64_t out_deadline_ms_ = 0;

    // 하이파이브: 문구 없는 콕을 10초 안에 서로 보냈는지 본다. 0 = 없음.
    int64_t last_tx_poke_ms_ = 0;
    int64_t last_rx_poke_ms_ = 0;

    // 놀러가기. 0 = 없음.
    int64_t trip_wait_until_ms_ = 0;   // 내가 물었고 상대 답을 기다리는 시한
    uint8_t guest_pending_ = 0;        // 받겠다고 답했고 그 답이 닿기를 기다리는 손님
    std::atomic<bool> trip_asking_{false};   // 터치 태스크도 읽는다
    int64_t invite_wait_until_ms_ = 0;       // 놀러오기를 보냈고 상대의 type 3 을 기다리는 시한
    std::atomic<bool> invite_waiting_{false};   // 터치 태스크도 읽는다
    // 답은 처리 결과를 보고 ACK 한다(HandleTripAnswer). 재전송된 답에 같은 결과를 주려고 기억한다.
    bool answer_seen_ = false;
    bool answer_acked_ = false;
    uint16_t answer_seq_ = 0;

    struct Seen {
        bool used;
        uint8_t mac[6];
        uint16_t seq;
    };
    Seen seen_[kSeenCount] = {};
    uint8_t seen_head_ = 0;
};
