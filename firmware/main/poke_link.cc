#include "poke_link.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <lwip/sockets.h>

#include <cerrno>
#include <cstring>

#include "application.h"
#include "board.h"
#include "display/lcd_display.h"
#include "notify_sound.h"
#include "usage/character_pack.h"
#include "usage/art/ui_poke.h"

#define TAG "PokeLink"

namespace {

constexpr uint16_t kPort = 47801;
constexpr uint8_t kTypePoke = 0;
constexpr uint8_t kTypeAck = 1;
constexpr uint8_t kTypeReply = 2;          // phrase 자리에 답장 번호 1~3 을 싣는다
// 놀러가기(docs/trip.md). 세 패킷 다 phrase 자리에 값을 싣는다.
constexpr uint8_t kTypeTrip = 3;           // 내 캐릭터가 가도 되나. 값 = 내 보드의 캐릭터 비트 묶음(1~7)
constexpr uint8_t kTypeTripAnswer = 4;     // 값 = 건너올 캐릭터(1~7) / kTripBusy / kTripSame
constexpr uint8_t kTypeTripHome = 5;       // 손님이 떠났다. 값 = 1
constexpr uint8_t kTypeTripInvite = 6;     // 놀러 와. 값 = 1. 받은 쪽이 평소 놀러가기(type 3)를 시작한다
constexpr uint8_t kTypeTripInviteNo = 7;   // 놀러오기를 못 받는다. 값 = kTripBusy
constexpr uint8_t kTypeTripCallHome = 8;   // 내 캐릭터를 집으로 보내 줘. 값 = 1
constexpr uint8_t kTripBusy = LcdDisplay::kTripAnswerBusy;
constexpr uint8_t kTripSame = LcdDisplay::kTripAnswerSame;   // 이미 다 같이 있다
// magic 4 + type 1 + MAC 6 + seq 2(LE) + phrase 1
constexpr size_t kPacketLen = 14;
constexpr uint8_t kMagic[4] = {'C', 'B', 'P', '1'};
constexpr size_t kOffType = 4, kOffMac = 5, kOffSeq = 11, kOffPhrase = 13;

constexpr uint32_t kTickMs = 50;           // 수신 대기 한 번. 전송 지연의 상한이기도 하다
constexpr uint32_t kRetryMs = 150;
constexpr uint8_t kMaxTries = 3;
constexpr uint32_t kAckWaitMs = 1500;      // 이 안에 ACK 가 없으면 상대 없음
constexpr int64_t kHighFiveMs = 10000;     // 이 안에 서로 콕을 보내면 하이파이브
constexpr int64_t kTripAnswerMs = 3000;    // 이 안에 답이 없으면 상대 없음
constexpr uint32_t kSocketRetryMs = 1000;
constexpr uint32_t kTaskStack = 4096;
constexpr UBaseType_t kTaskPriority = 3;
constexpr UBaseType_t kQueueDepth = 4;

int64_t NowMs() { return esp_timer_get_time() / 1000; }

LcdDisplay* GetLcdDisplay() {
    return dynamic_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
}

// STA 서브넷 브로드캐스트. 아직 IP 가 없으면 0.
uint32_t SubnetBroadcast() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (netif == nullptr || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return 0;
    }
    return info.ip.addr | ~info.netmask.addr;
}

bool ValueValid(uint8_t type, uint8_t value) {
    switch (type) {
        case kTypePoke: return value <= UI_POKE_PHRASE_COUNT;
        case kTypeAck: return true;   // 받은 패킷의 값을 되돌려 줄 뿐이다
        case kTypeReply: return value >= 1 && value <= UI_POKE_REPLY_COUNT;
        case kTypeTrip: return value >= 1 && value <= 7;
        case kTypeTripAnswer: return (value >= 1 && value <= 7) || value == kTripBusy || value == kTripSame;
        case kTypeTripHome: return value == 1;
        case kTypeTripInvite: return value == 1;
        case kTypeTripInviteNo: return value == kTripBusy;
        case kTypeTripCallHome: return value == 1;
        default: return false;
    }
}

}  // namespace

void PokeLink::Start() {
    if (queue_ != nullptr) {
        return;
    }
    queue_ = xQueueCreate(kQueueDepth, sizeof(OutItem));
    if (queue_ == nullptr) {
        ESP_LOGE(TAG, "queue alloc failed");
        return;
    }
    if (xTaskCreate(TaskEntry, "poke_link", kTaskStack, this, kTaskPriority, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
}

bool PokeLink::Send(uint8_t phrase) {
    if (queue_ == nullptr || phrase > UI_POKE_PHRASE_COUNT) {
        return false;
    }
    const OutItem item = {kTypePoke, phrase};
    return xQueueSend(queue_, &item, 0) == pdTRUE;
}

bool PokeLink::SendReply(uint8_t reply) {
    if (queue_ == nullptr || reply == 0 || reply > UI_POKE_REPLY_COUNT) {
        return false;
    }
    const OutItem item = {kTypeReply, reply};
    return xQueueSend(queue_, &item, 0) == pdTRUE;
}

bool PokeLink::SendTrip(uint8_t character) {
    if (queue_ == nullptr || !ValueValid(kTypeTrip, character)) {
        return false;
    }
    const OutItem item = {kTypeTrip, character};
    trip_asking_ = true;
    if (xQueueSend(queue_, &item, 0) != pdTRUE) {
        trip_asking_ = false;
        return false;
    }
    return true;
}

bool PokeLink::SendTripHome() {
    if (queue_ == nullptr) {
        return false;
    }
    const OutItem item = {kTypeTripHome, 1};
    if (xQueueSend(queue_, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "trip home not queued");   // 상대는 시한이 지나면 혼자 돌아온다
        return false;
    }
    return true;
}

bool PokeLink::SendTripInvite() {
    if (queue_ == nullptr) {
        return false;
    }
    const OutItem item = {kTypeTripInvite, 1};
    invite_waiting_ = true;
    if (xQueueSend(queue_, &item, 0) != pdTRUE) {
        invite_waiting_ = false;
        return false;
    }
    return true;
}

bool PokeLink::SendTripCallHome() {
    if (queue_ == nullptr) {
        return false;
    }
    const OutItem item = {kTypeTripCallHome, 1};
    return xQueueSend(queue_, &item, 0) == pdTRUE;   // 못 보내도 내 보드는 잠시 뒤 혼자 돌아온다
}

void PokeLink::TaskEntry(void* arg) {
    static_cast<PokeLink*>(arg)->Run();
}

bool PokeLink::OpenSocket() {
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock_ < 0) {
        ESP_LOGW(TAG, "socket failed: errno %d", errno);
        return false;
    }
    const int yes = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct timeval tv = {};
    tv.tv_usec = kTickMs * 1000;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "bind failed: errno %d", errno);
        CloseSocket();
        return false;
    }
    ESP_LOGI(TAG, "listening on udp %u", kPort);
    return true;
}

void PokeLink::CloseSocket() {
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
}

void PokeLink::Transmit(uint8_t type, uint16_t seq, uint8_t phrase, uint32_t ip) {
    if (sock_ < 0) {
        return;
    }
    uint8_t buf[kPacketLen];
    memcpy(buf, kMagic, sizeof(kMagic));
    buf[kOffType] = type;
    memcpy(buf + kOffMac, own_mac_, sizeof(own_mac_));
    buf[kOffSeq] = static_cast<uint8_t>(seq & 0xFF);
    buf[kOffSeq + 1] = static_cast<uint8_t>(seq >> 8);
    buf[kOffPhrase] = phrase;

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(kPort);
    dest.sin_addr.s_addr = ip;
    if (sendto(sock_, buf, sizeof(buf), 0, reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest)) < 0) {
        ESP_LOGW(TAG, "sendto failed: errno %d", errno);   // 미연결이면 여기로 온다
    }
}

void PokeLink::TransmitRound() {
    Transmit(out_type_, out_seq_, out_phrase_, htonl(INADDR_BROADCAST));
    const uint32_t subnet = SubnetBroadcast();
    if (subnet != 0 && subnet != htonl(INADDR_BROADCAST)) {
        Transmit(out_type_, out_seq_, out_phrase_, subnet);
    }
    if (peer_ip_ != 0) {
        Transmit(out_type_, out_seq_, out_phrase_, peer_ip_);
    }
}

void PokeLink::PumpOutgoing(int64_t now_ms) {
    // 놀러가기 패킷은 끝날 때까지 다음 패킷이 덮어쓰지 못한다. 덮이면 늦은 ACK 가 무시되어
    // 손님이 안 들어오거나 「떠났다」가 끊긴다. 뒤 패킷은 큐에서 기다린다.
    const bool trip_in_flight = out_active_ && out_type_ >= kTypeTrip;
    OutItem item;
    if (!trip_in_flight && xQueueReceive(queue_, &item, 0) == pdTRUE) {
        out_active_ = true;
        out_seq_ = next_seq_++;
        out_type_ = item.type;
        out_phrase_ = item.value;
        if (item.type == kTypePoke && item.value == 0) {
            last_tx_poke_ms_ = now_ms;
        }
        if (item.type == kTypeTrip) {
            trip_wait_until_ms_ = now_ms + kTripAnswerMs;
        }
        if (item.type == kTypeTripInvite) {
            invite_wait_until_ms_ = now_ms + kTripAnswerMs;
        }
        out_tries_ = 0;
        out_next_ms_ = now_ms;
        out_deadline_ms_ = now_ms + kAckWaitMs;
        ESP_LOGI(TAG, "tx type=%u seq=%u phrase=%u", out_type_, out_seq_, out_phrase_);
    }
    if (trip_wait_until_ms_ != 0 && now_ms >= trip_wait_until_ms_) {
        trip_wait_until_ms_ = 0;
        trip_asking_ = false;
        ESP_LOGI(TAG, "trip: no answer");
        if (auto* display = GetLcdDisplay()) {
            display->ShowTripAnswer(false, 0);
        }
    }
    if (invite_wait_until_ms_ != 0 && now_ms >= invite_wait_until_ms_) {
        invite_wait_until_ms_ = 0;
        invite_waiting_ = false;
        ESP_LOGI(TAG, "invite: no answer");
        if (auto* display = GetLcdDisplay()) {
            display->ShowTripAnswer(false, 0);
        }
    }
    if (!out_active_) {
        return;
    }
    if (out_tries_ < kMaxTries && now_ms >= out_next_ms_) {
        TransmitRound();
        out_tries_++;
        out_next_ms_ = now_ms + kRetryMs;
    }
    if (now_ms >= out_deadline_ms_) {
        out_active_ = false;
        ESP_LOGI(TAG, "no ack seq=%u", out_seq_);
        if (out_type_ == kTypeTripAnswer) {
            guest_pending_ = 0;   // 상대가 답을 못 받았으니 상대 캐릭터는 출발하지 않았다
        } else if (out_type_ == kTypeTripCallHome) {
            // 상대가 「집으로 보내 줘」를 못 받았다(꺼짐·옛 펌웨어). 기다려도 「떠났다」가 안 오니 바로 돌아온다.
            // 받았으면(ACK) 상대가 손님을 내보내고 type 5 를 보낸다 — 그때까지 기다려야 두 곳에 안 나온다.
            if (auto* display = GetLcdDisplay()) {
                display->ShowTripHome();
            }
        } else if (out_type_ == kTypeTripInvite) {
            invite_wait_until_ms_ = 0;
            invite_waiting_ = false;
            if (auto* display = GetLcdDisplay()) {
                display->ShowTripAnswer(false, 0);
            }
        } else if (out_type_ == kTypePoke || out_type_ == kTypeReply) {
            if (auto* display = GetLcdDisplay()) {
                display->ShowPokeResult(false);
            }
        }
    }
}

bool PokeLink::AlreadySeen(const uint8_t* mac, uint16_t seq) {
    for (const Seen& s : seen_) {
        if (s.used && s.seq == seq && memcmp(s.mac, mac, sizeof(s.mac)) == 0) {
            return true;
        }
    }
    Seen& slot = seen_[seen_head_];
    slot.used = true;
    slot.seq = seq;
    memcpy(slot.mac, mac, sizeof(slot.mac));
    seen_head_ = (seen_head_ + 1) % kSeenCount;
    return false;
}

void PokeLink::HandleOwnAck(int64_t now_ms) {
    auto* display = GetLcdDisplay();
    if (display == nullptr) {
        return;
    }
    if (out_type_ == kTypeTripAnswer) {
        // 「와도 돼」가 상대에게 닿았다. 그래야 상대 캐릭터가 출발하므로 이때 손님을 들인다.
        const uint8_t guest = guest_pending_;
        guest_pending_ = 0;
        if (out_phrase_ >= 1 && out_phrase_ <= 7 && guest != 0) {
            if (display->ShowTripGuest(guest)) {
                NotifySound::GetInstance().Play(NotifySound::kGuest);   // 손님이 걸어 들어온다
            } else {
                SendTripHome();   // 그 사이 상태 화면이 됐다. 상대 캐릭터를 바로 돌려보낸다
            }
        }
        return;
    }
    if (out_type_ != kTypePoke && out_type_ != kTypeReply) {
        return;
    }
    // 내 콕이 닿았고 상대 콕을 10초 안에 받았다면 양쪽 다 서로 보낸 것이다.
    const bool high_five = out_type_ == kTypePoke && out_phrase_ == 0 && last_rx_poke_ms_ != 0 &&
                           now_ms - last_rx_poke_ms_ < kHighFiveMs;
    if (high_five) {
        last_tx_poke_ms_ = last_rx_poke_ms_ = 0;
        display->ShowPokeHighFive();
    } else {
        display->ShowPokeResult(true);
    }
}

// B 는 이 ACK 를 받아야 손님을 들인다. 그래서 내 캐릭터가 실제로 출발했거나 「바빠」를 받았을 때만
// ACK 한다. 시한이 지났거나 그 사이 상태 화면이 되어 못 나가면 ACK 하지 않고, B 는 재전송 뒤 포기한다.
void PokeLink::HandleTripAnswer(uint16_t seq, uint8_t value, uint32_t src_ip) {
    if (answer_seen_ && seq == answer_seq_) {
        if (answer_acked_) {
            Transmit(kTypeAck, seq, value, src_ip);   // 재전송 — 처음과 같은 결과를 준다
        }
        return;
    }
    if (trip_wait_until_ms_ == 0) {
        return;
    }
    trip_wait_until_ms_ = 0;
    trip_asking_ = false;
    auto* display = GetLcdDisplay();
    const bool left = display != nullptr && display->ShowTripAnswer(true, value);
    answer_seen_ = true;
    answer_seq_ = seq;
    answer_acked_ = left || value == kTripBusy || value == kTripSame;
    if (answer_acked_) {
        Transmit(kTypeAck, seq, value, src_ip);
    }
}

// 상대가 「놀러오기」를 눌렀다. 내가 놀러가기를 누른 것과 같은 길로 묻는다. 못 가면 바쁘다고 답한다.
void PokeLink::HandleTripInvite() {
    auto* display = GetLcdDisplay();
    if (display != nullptr && trip_wait_until_ms_ == 0 && display->AcceptTripInvite()) {
        return;
    }
    const OutItem item = {kTypeTripInviteNo, kTripBusy};
    xQueueSend(queue_, &item, 0);
}

void PokeLink::HandleTripRequest(uint8_t character) {
    invite_wait_until_ms_ = 0;   // 부른 상대가 오고 있다(부르지 않았어도 지울 뿐이다)
    invite_waiting_ = false;
    auto* display = GetLcdDisplay();
    const bool ok = display != nullptr && display->CanHostTrip() && trip_wait_until_ms_ == 0;
    const uint8_t fresh = ok ? display->TripNewGuests(character) : 0;
    guest_pending_ = fresh;
    if (ok && fresh == 0) {
        display->TripWave();
    }
    const OutItem item = {kTypeTripAnswer, !ok ? kTripBusy : (fresh != 0 ? fresh : kTripSame)};
    if (xQueueSend(queue_, &item, 0) != pdTRUE) {
        guest_pending_ = 0;
    }
}

void PokeLink::HandlePacket(const uint8_t* buf, size_t len, uint32_t src_ip) {
    if (len != kPacketLen || memcmp(buf, kMagic, sizeof(kMagic)) != 0) {
        return;
    }
    const uint8_t type = buf[kOffType];
    const uint8_t* mac = buf + kOffMac;
    const uint16_t seq = static_cast<uint16_t>(buf[kOffSeq] | (buf[kOffSeq + 1] << 8));
    const uint8_t phrase = buf[kOffPhrase];
    if (!ValueValid(type, phrase) || memcmp(mac, own_mac_, sizeof(own_mac_)) == 0) {
        return;   // 브로드캐스트는 내 패킷도 돌아올 수 있다
    }
    const int64_t now_ms = NowMs();
    peer_ip_ = src_ip;

    if (type == kTypeAck) {
        if (out_active_ && seq == out_seq_) {
            out_active_ = false;
            ESP_LOGI(TAG, "ack seq=%u", seq);
            HandleOwnAck(now_ms);
        }
        return;
    }

    if (type == kTypeTripAnswer) {
        HandleTripAnswer(seq, phrase, src_ip);   // 이 ACK 는 「출발했다」는 뜻이라 결과를 본 뒤 보낸다
        return;
    }
    Transmit(kTypeAck, seq, phrase, src_ip);   // 화면보다 먼저 — 표시가 밀려도 ACK 는 바로 나간다
    if (AlreadySeen(mac, seq)) {
        ESP_LOGI(TAG, "duplicate seq=%u", seq);
        return;
    }
    ESP_LOGI(TAG, "rx type=%u seq=%u phrase=%u", type, seq, phrase);
    Application::GetInstance().ResetIdleClock();   // 깨우기 전에 — 사이에 1초 틱이 오면 다시 꺼진다
    auto* display = GetLcdDisplay();
    if (display == nullptr) {
        return;
    }
    if (type == kTypeTrip) {
        HandleTripRequest(phrase);
        return;
    }
    if (type == kTypeTripHome) {
        display->ShowTripHome();
        return;
    }
    if (type == kTypeTripInvite) {
        HandleTripInvite();
        return;
    }
    if (type == kTypeTripInviteNo) {
        if (invite_wait_until_ms_ != 0) {
            invite_wait_until_ms_ = 0;
            invite_waiting_ = false;
            display->ShowTripAnswer(true, kTripBusy);
        }
        return;
    }
    if (type == kTypeTripCallHome) {
        if (!display->SendGuestHome()) {
            guest_pending_ = 0;   // 아직 안 들어온 손님이면 들이지 않는다
            SendTripHome();       // 손님이 없다 — 상대 캐릭터는 바로 돌아가도 된다
        }
        return;
    }
    NotifySound::GetInstance().Play(NotifySound::kPoke);   // 여기부터는 콕·답장이다. 옷장 중이라 표시가 밀려도 소리는 지금
    if (type == kTypeReply) {
        display->ShowPokeReply(phrase);
        return;
    }
    if (phrase != 0) {
        display->ShowPokeReceived(phrase);
        return;
    }
    if (last_tx_poke_ms_ != 0 && now_ms - last_tx_poke_ms_ < kHighFiveMs) {
        last_tx_poke_ms_ = last_rx_poke_ms_ = 0;
        display->ShowPokeHighFive();
        return;
    }
    last_rx_poke_ms_ = now_ms;
    display->ShowPokeReceived(0);
}

void PokeLink::Receive() {
    uint8_t buf[kPacketLen + 1];   // 한 바이트 더 받아 긴 패킷을 길이 검사로 걸러낸다
    struct sockaddr_in src = {};
    socklen_t src_len = sizeof(src);
    const int n = recvfrom(sock_, buf, sizeof(buf), 0, reinterpret_cast<struct sockaddr*>(&src), &src_len);
    if (n >= 0) {
        HandlePacket(buf, static_cast<size_t>(n), src.sin_addr.s_addr);
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "recvfrom failed: errno %d, reopening", errno);
        CloseSocket();
    }
}

void PokeLink::Run() {
    esp_read_mac(own_mac_, ESP_MAC_WIFI_STA);
    // 재부팅 뒤 상대의 중복 판정에 걸리지 않게 무작위에서 시작한다.
    next_seq_ = static_cast<uint16_t>(esp_random());
    while (true) {
        if (sock_ < 0) {
            OpenSocket();
        }
        PumpOutgoing(NowMs());
        if (sock_ < 0) {
            vTaskDelay(pdMS_TO_TICKS(kSocketRetryMs));
            continue;
        }
        Receive();
    }
}
