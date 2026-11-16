#include "usage_receiver.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cmath>
#include <cstring>
#include <string>

#include "application.h"
#include "board.h"
#include "display/lcd_display.h"
#include "usage/wardrobe_store.h"
#include "weather_fetcher.h"
#include "diary_fetcher.h"
#include "notify_sound.h"
#include "lvgl.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "settings.h"
#include "wifi_manager.h"
#include "boards/common/wifi_board.h"
#include <esp_mac.h>
#include <esp_netif.h>
#include <lwip/sockets.h>
#include <lwip/etharp.h>
#include <lwip/prot/etharp.h>
#include <lwip/prot/iana.h>
#include <netif/ethernet.h>
#include <esp_timer.h>
#include <lwip/tcpip.h>
#include "esp_netif_net_stack.h"

#define TAG "UsageReceiver"

namespace {

// 본문 상한. 정상 페이로드는 300바이트 남짓이라 넉넉하다.
constexpr size_t kMaxBodyLen = 2048;
// 마지막 값을 담아 두는 NVS 자리. 부팅 직후 화면을 되살리는 데 쓴다.
constexpr const char* kNvsNamespace = "usage";
constexpr const char* kNvsKey = "last";
constexpr const char* kNvsKeyCodex = "last_codex";   // Codex 쪽 마지막 사용량. 재부팅 뒤에도 전환이 바로 보이게

// JSON 문자열 필드를 고정 길이 버퍼로 옮긴다. 없거나 문자열이 아니면 비운다.
void CopyString(const cJSON* obj, const char* key, char* out, size_t out_len) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        strncpy(out, item->valuestring, out_len - 1);
        out[out_len - 1] = '\0';
    } else {
        out[0] = '\0';
    }
}

// 화면을 가진 보드에서만 사용량 화면이 존재한다.
LcdDisplay* GetLcdDisplay() {
    return dynamic_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
}

// 날씨 한 건을 화면에 얹는다. 맥의 {"weather":…} POST 와 보드가 직접 받아 온 것
// (weather_fetcher.cc)이 이 한 길로 들어온다. 호출자가 DisplayLockGuard 를 잡고 있어야 한다.
// kind = clear|partly|cloud|rain|snow. partly·cloud·clear 는 파티클이 없어 옷·이펙트 규칙에선
// 똑같다(SetUsageWeatherFx 는 rain/snow 만 본다). 다른 점은 시계 칸 그림뿐이다.
void ApplyWeatherLocked(LcdDisplay* display, const char* kind, bool has_temp, int temp, bool night) {
    // 비/눈 파티클은 옷과 달리 LOCK 이어도 내린다 — 날씨는 입는 게 아니라 오는 것.
    display->SetUsageWeatherFx(kind);
    display->SetUsageWeatherInfo(kind, has_temp, temp, night);
    if (display->IsUsageOutfitLocked()) {
        ESP_LOGI(TAG, "weather ignored (outfit LOCK)");
    } else {
        // AUTO 세트(docs/alive.md E절): 슬롯별 규칙표 대신 지금 하늘의 세트를
        // 통째로 입힌다. 마을 하늘에선 강수 세트 > 시각 세트 — 판정은
        // 디스플레이가 방금 캐시한 강수(SetUsageWeatherFx)로 한다.
        display->ApplyUsageAutoSet();
    }
}

// 옷장 ③ 프로토콜 (docs/wardrobe.md). outfit = 강제 적용(+NVS), unlock = 인벤토리
// 비트 + 축하 반짝, weather = LOCK 이 아닐 때 AUTO 세트로 자동 옷 (저장 안 함).
bool ApplyWardrobeJson(const cJSON* root, LcdDisplay* display) {
    bool handled = false;

    // 틱 전체를 한 락으로 덮는다 — 안의 Get/SetUsageOutfit 등이 각자 또 락을
    // 잡지만 lvgl_port_lock 은 재귀라 안전하고, "읽고(GetUsageOutfit) 고쳐서
    // 쓰는(SetUsageOutfit)" 사이에 옷장 터치·OnIdleTick 이 usage_outfit_ 를
    // 바꿔 이 push 가 그 변경을 덮어쓰는 TOCTOU 를 막는다.
    DisplayLockGuard guard(display);

    const cJSON* outfit_json = cJSON_GetObjectItem(root, "outfit");
    if (cJSON_IsObject(outfit_json)) {
        handled = true;
        // 옷장이 열려 있으면 고르던 것을 먼저 커밋하고 닫는다 — state push 가
        // 옷장을 닫을 때와 같은 정책. 부분 갱신은 그 커밋본 위에 얹힌다.
        display->CommitWardrobe();
        Outfit o = display->GetUsageOutfit();
        // 4번째 슬롯은 "neck" (item_tables.h SlotJsonKey)
        const struct { const char* name; OutfitSlot slot; uint8_t* field; } slots[] = {
            {"head", kSlotHead, &o.head}, {"face", kSlotFace, &o.face},
            {"hand", kSlotHand, &o.hand}, {SlotJsonKey(kSlotFx), kSlotFx, &o.fx},
        };
        for (const auto& sl : slots) {
            const cJSON* v = cJSON_GetObjectItem(outfit_json, sl.name);
            if (v == nullptr) continue;                       // 없는 필드는 입던 대로
            if (cJSON_IsNull(v)) { *sl.field = 0; continue; } // null = 벗기
            if (!cJSON_IsString(v) || !OutfitItemIndexByKey(sl.slot, v->valuestring, sl.field)) {
                ESP_LOGW(TAG, "outfit: unknown %s=%s", sl.name,
                         cJSON_IsString(v) ? v->valuestring : "?");
            }
        }
        int sky = -1;
        const cJSON* bg = cJSON_GetObjectItem(outfit_json, "bg");
        UsageSky parsed_sky;
        if (cJSON_IsString(bg) && SkyByKey(bg->valuestring, &parsed_sky)) {
            sky = parsed_sky;
        }
        int wx = -1;   // 없으면 유지
        const cJSON* jwx = cJSON_GetObjectItem(outfit_json, "wx");
        uint8_t parsed_wx;
        if (cJSON_IsString(jwx)) {
            if (WxByKey(jwx->valuestring, &parsed_wx)) {
                wx = parsed_wx;
            } else {
                ESP_LOGW(TAG, "outfit: unknown wx=%s", jwx->valuestring);
            }
        }
        const cJSON* lock = cJSON_GetObjectItem(outfit_json, "lock");
        // lock 을 안 보낸 push 는 지금 모드를 유지한다(-1). 기본을 LOCK 으로 두면
        // 훅의 부분 갱신 한 방에 AUTO 세트가 영구 정지된다(NVS 라 재부팅에도 남는다).
        const int locked = cJSON_IsBool(lock) ? (cJSON_IsTrue(lock) ? 1 : 0) : -1;
        display->SetUsageOutfit(o, locked, sky, /*persist=*/true, wx);
    }

    const cJSON* unlock = cJSON_GetObjectItem(root, "unlock");
    if (cJSON_IsString(unlock)) {
        handled = true;  // 모르는 키도 200 — 호스트의 at-least-once 재전송이 영원히 돌지 않게
        const int bit = UnlockBit(unlock->valuestring);
        if (bit < 0) {
            ESP_LOGW(TAG, "unlock: unknown key %s", unlock->valuestring);
        } else {
            const uint32_t mask = WardrobeLoadUnlock();
            if ((mask & (1u << bit)) == 0) {
                WardrobeSaveUnlock(mask | (1u << bit));
                ESP_LOGI(TAG, "unlock %s (mask 0x%02x)", unlock->valuestring,
                         static_cast<unsigned>(mask | (1u << bit)));
            } else {
                ESP_LOGI(TAG, "unlock %s already owned", unlock->valuestring);
            }
        }
    }

    const cJSON* weather = cJSON_GetObjectItem(root, "weather");
    if (cJSON_IsObject(weather)) {
        handled = true;
        char fx_kind[12];
        CopyString(weather, "kind", fx_kind, sizeof(fx_kind));
        const cJSON* jtemp = cJSON_GetObjectItem(weather, "temp");
        const bool has_temp = cJSON_IsNumber(jtemp);
        const int temp = has_temp ? static_cast<int>(lround(jtemp->valuedouble)) : 0;
        const bool night = cJSON_IsTrue(cJSON_GetObjectItem(weather, "night"));
        ApplyWeatherLocked(display, fx_kind, has_temp, temp, night);
    }
    return handled;
}

}  // namespace

UsageReceiver::~UsageReceiver() { Stop(); }

void UsageReceiver::ApplyWeather(const char* kind, bool has_temp, int temp, bool night) {
    auto display = GetLcdDisplay();
    if (display == nullptr || kind == nullptr) {
        return;
    }
    DisplayLockGuard guard(display);   // POST 와 같은 락 — 옷 읽고 고쳐 쓰는 사이를 막는다
    ApplyWeatherLocked(display, kind, has_temp, temp, night);
}

bool UsageReceiver::ApplyJson(const char* json, bool persist) {
    if (json == nullptr || json[0] == '\0') {
        return false;
    }
    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) {
        ESP_LOGW(TAG, "JSON parse failed");
        return false;
    }

    UsageStats stats;

    // 칸 네 개. 맥이 완성한 글자를 그대로 받는다 — 펌웨어는 뜻을 모르므로
    // 무엇을 보여줄지 바꿔도 다시 구울 필요가 없다.
    // 값을 left/right 로 나눠 받는 이유는 구분자를 세로로 맞추기 위해서다.
    const cJSON* cells = cJSON_GetObjectItem(root, "cells");
    if (cJSON_IsArray(cells)) {
        int count = cJSON_GetArraySize(cells);
        if (count > UsageStats::kMaxCells) {
            count = UsageStats::kMaxCells;
        }
        for (int i = 0; i < count; i++) {
            const cJSON* c = cJSON_GetArrayItem(cells, i);
            if (!cJSON_IsObject(c)) {
                continue;
            }
            CopyString(c, "name", stats.cells[i].name, sizeof(stats.cells[i].name));
            CopyString(c, "left", stats.cells[i].left, sizeof(stats.cells[i].left));
            CopyString(c, "right", stats.cells[i].right, sizeof(stats.cells[i].right));
            stats.cell_count = static_cast<uint8_t>(i + 1);
        }
    }
    // 빈 배열은 "칸 없음"으로 친다 — 화면을 비우고 NVS 복원 블롭까지
    // 덮어쓰는 사고를 막는다.
    const bool has_cells = stats.cell_count > 0;

    // 상태(표정) 화면. 호스트의 훅 집계가 보낸다. cells 없이 state 만 와도 된다.
    char state[16];
    char ctx[32];
    CopyString(root, "state", state, sizeof(state));
    CopyString(root, "ctx", ctx, sizeof(ctx));

    auto display = GetLcdDisplay();
    if (display == nullptr) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "No LCD display; ignoring usage payload");
        return false;
    }

    // 에이전트 필터 (docs/agent-badge.md §3·§10). 페이로드 "agent": "claude"|"codex" 가
    // 보드 선택과 다르면 state/ctx/cells 를 **반영하지 않되 200** — handled 로 쳐서
    // 호스트의 재전송 루프를 막는다. 없거나 모르는 값은 옛 호스트로 보고 반영.
    // 다른 필드(sky·outfit·touch·act…)는 누구 것이라는 개념이 없어 거르지 않는다.
    char agent_key[8];   // 가장 긴 키 "claude" 6자 + NUL. 더 긴 값은 잘려서 unknown 이 된다.
    CopyString(root, "agent", agent_key, sizeof(agent_key));
    UsageAgent payload_agent = kUsageAgentClaude;
    const bool has_agent = agent_key[0] != '\0' && AgentByKey(agent_key, &payload_agent);
    if (agent_key[0] != '\0' && !has_agent) {
        ESP_LOGW(TAG, "agent: unknown %s (treated as none)", agent_key);
    }
    const UsageAgent selected = display->GetUsageAgent();
    const bool filtered = has_agent && payload_agent != selected;
    if (filtered) {
        ESP_LOGW(TAG, "agent filtered: %s (selected %s)", AgentKey(payload_agent),
                 AgentKey(selected));
    }

    if (has_cells) {
        // 걸러진 쪽 값도 기억해 둔다 — 배지를 누르면 맥을 기다리지 않고 바로 보인다.
        // SetUsageStats() 가 안에서 DisplayLockGuard 를 잡는다.
        display->SetUsageStats(has_agent ? payload_agent : selected, stats);
    }
    if (state[0] != '\0' && !filtered) {
        display->SetUsageState(state, ctx);
    }
    // 대기 화면 하늘색: auto|brown|navy|day. NVS 에는 남기지 않는다 — 기본은
    // 시각으로 고르는 auto 이고, 수동 지정은 그 부팅 동안만이다.
    char sky[16];
    CopyString(root, "sky", sky, sizeof(sky));
    if (sky[0] != '\0') {
        display->SetUsageSky(sky);
    }

    const bool has_wardrobe = ApplyWardrobeJson(root, display);

    // 디버그: 행동 주입. {"act": "ride"} — 미니카가 있는 팩이 지금 달린다. 저장하지 않는다.
    char act[12];
    CopyString(root, "act", act, sizeof(act));
    bool has_act = false;
    if (act[0] != '\0') {
        // 모르는 키·발동 불가(비 idle 등)는 handled 로 안 친다 — act 단독 push 가
        // 조용히 버려지고도 200 을 받으면 QA 가 실패를 못 본다.
        has_act = display->TriggerUsageActivity(act);
        if (!has_act) {
            ESP_LOGW(TAG, "act rejected: %s", act);
        }
    }

    // 에이전트 선택 주입 (docs/agent-badge.md §3). {"agent_select": "claude|codex"}
    // — 배지 탭과 같은 경로: 배지 즉시 교체 + 토스트 + NVS 저장. 모르는 값은
    // handled 로 안 친다(단독 push 400). 사용량 캐시(NVS usage)에는 남기지 않는다.
    char agent_select[8];
    CopyString(root, "agent_select", agent_select, sizeof(agent_select));
    bool has_agent_select = false;
    if (agent_select[0] != '\0') {
        has_agent_select = display->SetUsageAgent(agent_select, /*persist=*/true);
    }

    // 캐릭터 런타임 교체 (docs/character-pack.md "런타임 선택"). {"char": "pack"|"<팩 이름>"}
    // — 옷장 CHAR 탭과 같은 경로: 두 몸을 다시 만들고 그 캐릭터의 착장·세트를 입히고 NVS 저장.
    // 아래 "set" 보다 **먼저** 처리한다 — 같은 push 에 둘이 같이 오면 세트가 새 캐릭터 것이어야 한다.
    char char_key[24];
    CopyString(root, "char", char_key, sizeof(char_key));
    bool has_char = false;
    if (char_key[0] != '\0') {
        has_char = display->SetUsageCharacter(char_key, /*persist=*/true);
    }

    // 캐릭터 팩 착장 세트 (docs/character-pack.md). {"set": "<pack.json sets 키>"} —
    // 옷걸이 탭과 같은 경로: 즉시 교체 + 토스트 + NVS(wardrobe:set). 팩 모드가 아니거나
    // 모르는 키는 handled 로 안 친다(단독 push 400). 옛 outfit 필드는 팩 모드에서 무시된다.
    char set_key[24];
    CopyString(root, "set", set_key, sizeof(set_key));
    bool has_set = false;
    if (set_key[0] != '\0') {
        has_set = display->SetUsageSet(set_key, /*persist=*/true);
    }

    // 콕 찌르기 주입 (docs/poke.md). {"poke": 0~5} = 실제 전송 경로로 보내기,
    // {"poke_rx": 0~5} = 받은 것처럼 화면에 표시. 범위 밖은 handled 로 안 친다(단독 push 400).
    // 저장하지 않는다.
    const cJSON* jpoke = cJSON_GetObjectItem(root, "poke");
    const bool has_poke = cJSON_IsNumber(jpoke) && display->SendPoke(jpoke->valueint);
    const cJSON* jpoke_rx = cJSON_GetObjectItem(root, "poke_rx");
    const bool has_poke_rx = cJSON_IsNumber(jpoke_rx) && display->ShowPokeReceived(jpoke_rx->valueint);
    // {"poke_reply": 1~15} = 답장 전송, {"poke_reply_rx": 1~15} = 답장 받은 화면,
    // {"poke_high5": 1} = 하이파이브 화면. 시험용 주입이다.
    const cJSON* jreply = cJSON_GetObjectItem(root, "poke_reply");
    const bool has_poke_reply = cJSON_IsNumber(jreply) && display->SendPokeReply(jreply->valueint);
    const cJSON* jreply_rx = cJSON_GetObjectItem(root, "poke_reply_rx");
    const bool has_poke_reply_rx = cJSON_IsNumber(jreply_rx) && display->ShowPokeReply(jreply_rx->valueint);
    const cJSON* jhigh5 = cJSON_GetObjectItem(root, "poke_high5");
    const bool has_poke_high5 = cJSON_IsNumber(jhigh5);
    if (has_poke_high5) {
        display->ShowPokeHighFive();
    }
    // 놀러가기 시험 주입 (docs/trip.md). {"trip":1} = 실제 전송 경로로 묻기,
    // {"trip_rx":n} = 캐릭터 n 이 온 것처럼 손님 표시, {"trip_home":1} = 손님이 떠난 신호를 받은 것처럼.
    const bool has_trip = cJSON_IsNumber(cJSON_GetObjectItem(root, "trip")) && display->SendTrip();
    const cJSON* jtrip_rx = cJSON_GetObjectItem(root, "trip_rx");
    const bool has_trip_rx = cJSON_IsNumber(jtrip_rx) && display->ShowTripGuest(jtrip_rx->valueint);
    const bool has_trip_home = cJSON_IsNumber(cJSON_GetObjectItem(root, "trip_home"));
    // 켜진 캐릭터 주입(시험용). {"cast":n} n = 로나 1 + 로찌 2 + 만복이 4.
    const cJSON* jcast = cJSON_GetObjectItem(root, "cast");
    const bool has_cast = cJSON_IsNumber(jcast) && display->SetUsageCast(jcast->valueint);
    if (has_trip_home) {
        display->ShowTripHome();
    }
    // {"trip_invite":1} = 패널 「놀러오기」, {"trip_go_home":1} = 패널 「집으로 가기」와 같은 길.
    const bool has_trip_invite = cJSON_IsNumber(cJSON_GetObjectItem(root, "trip_invite"));
    if (has_trip_invite) {
        display->SendTripInvite();
    }
    const bool has_trip_go_home = cJSON_IsNumber(cJSON_GetObjectItem(root, "trip_go_home"));
    if (has_trip_go_home) {
        display->TripGoHome();
    }

    // 날짜 저장 (docs/gift-board-plan.md). {"due":"YYYY-MM-DD"} 만복이 예정일,
    // {"birthday":"MM-DD"}·{"birthday2":"MM-DD"}·{"anniversary":"MM-DD"} 기념일. 빈 문자열은 지우기. 날짜는 NVS 에만 둔다.
    bool has_date = false;
    for (const char* key : {"due", "birthday", "birthday2", "anniversary"}) {
        const cJSON* j = cJSON_GetObjectItem(root, key);
        if (cJSON_IsString(j) && display->SetUsageDate(key, j->valuestring)) {
            has_date = true;
        }
    }

    // 날씨 좌표. {"geo":{"lat":37.5,"lon":127.0}} — NVS 에 저장하고 곧 그 자리 날씨를 다시 받는다
    // (weather_fetcher.cc). 범위 밖·숫자 아님은 handled 로 안 친다(단독 push 400).
    const cJSON* geo = cJSON_GetObjectItem(root, "geo");
    bool has_geo = false;
    if (cJSON_IsObject(geo)) {
        const cJSON* jlat = cJSON_GetObjectItem(geo, "lat");
        const cJSON* jlon = cJSON_GetObjectItem(geo, "lon");
        has_geo = cJSON_IsNumber(jlat) && cJSON_IsNumber(jlon) &&
                  WeatherFetcher::GetInstance().SetGeo(jlat->valuedouble, jlon->valuedouble);
        if (!has_geo) {
            ESP_LOGW(TAG, "geo rejected");
        }
    }

    // 시험 주입: 날씨 판. {"weather_panel":0} 시간별, 1 이번 주, -1 닫기. 저장하지 않는다.
    const cJSON* jwpanel = cJSON_GetObjectItem(root, "weather_panel");
    const bool has_weather_panel = cJSON_IsNumber(jwpanel) && display->ShowWeatherPanel(jwpanel->valueint);

    // 보드를 거꾸로 세워 쓸 때 {"flip180":1}, 되돌리기 {"flip180":0}. NVS 에 저장하고 응답 뒤 재부팅한다.
    const cJSON* jflip = cJSON_GetObjectItem(root, "flip180");
    const bool has_flip = cJSON_IsNumber(jflip);
    if (has_flip) {
        {
            Settings settings("wardrobe", true);
            settings.SetBool("flip180", jflip->valueint != 0);
        }
        Application::GetInstance().Schedule([]() {
            vTaskDelay(pdMS_TO_TICKS(500));
            Application::GetInstance().Reboot();
        });
    }

    // 알림 소리 설정·시험 {"sound":{…}}·{"sound_test":"poke"|"guest"} (notify_sound.h)
    const bool has_sound = NotifySound::GetInstance().ApplyJson(root);

    // 만복이 육아일기 {"diary":{"token":…,"me":"mom"}}·{"diary_fetch":1}·{"diary_test_new":1} (diary_fetcher.h)
    // 시험 {"diary_panel":0} 목록, n>0 n번째 글, -1 닫기. 저장하지 않는다.
    bool has_diary = DiaryFetcher::GetInstance().ApplyJson(root);
    const cJSON* jdpanel = cJSON_GetObjectItem(root, "diary_panel");
    if (cJSON_IsNumber(jdpanel) && display->ShowDiaryPanel(jdpanel->valueint)) has_diary = true;
    // 시험 만복이 메뉴 {"manbok_menu":1} 열기 / 0·-1 닫기, {"manbok_panel":"ultrasound"|"health"|"prep"|"message"|"visit"}
    // 칸 바로 열기, {"manbok_fetch":1} 세 갈래 지금 받기(5분 규칙 건너뜀, DiaryFetcher::ApplyJson). 저장하지 않는다.
    const cJSON* jmmenu = cJSON_GetObjectItem(root, "manbok_menu");
    if (cJSON_IsNumber(jmmenu) && display->ShowManbokPanel(jmmenu->valueint > 0 ? 0 : -1)) has_diary = true;
    const cJSON* jmpanel = cJSON_GetObjectItem(root, "manbok_panel");
    if (cJSON_IsString(jmpanel)) {
        static const char* const kPanels[] = {"menu", "ultrasound", "health", "prep", "message", "visit"};
        for (int i = 0; i < 6; i++) {
            if (strcmp(jmpanel->valuestring, kPanels[i]) == 0 && display->ShowManbokPanel(i)) has_diary = true;
        }
    }

    // 시험 주입: {"special": 0~5} = 그 기념일 화면, -2 = 기념일 없는 날, -1 = 실제 날짜로 복귀. 저장하지 않는다.
    const cJSON* jspecial = cJSON_GetObjectItem(root, "special");
    const bool has_special = cJSON_IsNumber(jspecial);
    if (has_special) {
        display->SetSpecialDayTest(jspecial->valueint);
    }

    // 디버그: WiFi 온보딩 가이드 주입. {"wifiguide": 1} 표시 / {"wifiguide": 0} 해제.
    // 실제 설정 모드(AP)에 들어가면 STA 가 끊겨 화면 덤프(GET /screen)를 못 하므로,
    // 렌더링 QA 는 네트워크를 유지한 채 이 주입으로 한다. 저장하지 않는다.
    const cJSON* jwifiguide = cJSON_GetObjectItem(root, "wifiguide");
    bool has_wifiguide = false;
    if (cJSON_IsNumber(jwifiguide)) {
        if (jwifiguide->valueint != 0) {
            // STA 모드에선 설정 AP 가 없어 GetApSsid/GetApWebUrl 이 빈 값이다.
            // 실제 설정 모드와 같은 값(프리픽스-MAC 뒤 2바이트, 기본 URL)을 합성한다.
            auto& wifi_manager = WifiManager::GetInstance();
            std::string ssid = wifi_manager.GetApSsid();
            std::string url = wifi_manager.GetApWebUrl();
            if (ssid.empty()) {
                // 실제 설정 AP SSID 는 softAP MAC(STA+1) 뒤 2바이트를 쓴다 —
                // 실기기 로그로 확인(STA 58:54 → AP Xiaozhi-5855).
                uint8_t mac[6] = {};
                esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
                char buf[32];
                snprintf(buf, sizeof(buf), "Xiaozhi-%02X%02X", mac[4], mac[5]);
                ssid = buf;
            }
            if (url.empty()) {
                url = "http://192.168.4.1";
            }
            display->ShowWifiConfigGuide(ssid.c_str(), url.c_str());
        } else {
            display->HideWifiConfigGuide();
        }
        has_wifiguide = true;
    }

    // 디버그: 실제 WiFi 설정 모드 진입. {"wificfg": 1} — EnterWifiConfigMode 실경로
    // (STA 정지→설정 AP 기동→가이드 표시)를 검증한다. STA 가 끊기므로 복구는
    // 리셋(esptool) 또는 설정 포털 저장뿐이다. 응답이 나간 뒤 1초 지나 발동한다.
    // 저장하지 않는다.
    const cJSON* jwificfg = cJSON_GetObjectItem(root, "wificfg");
    bool has_wificfg = false;
    if (cJSON_IsNumber(jwificfg) && jwificfg->valueint != 0) {
        static_cast<WifiBoard&>(Board::GetInstance()).EnterWifiConfigMode();
        has_wificfg = true;
    }

    // 와이파이 추가. {"wifi_add": {"ssid": "...", "password": "...", "hotspot": true}} — 폰 핫스팟을
    // 설정 모드 없이 맥에서 넣는 길. hotspot(새로 넣을 때 기본 true, 이미 있으면 주지 않는 한 그대로)이면 최후 수단으로 표시해, 다른 저장 와이파이가
    // 보이면 그쪽이 먼저고 핫스팟에 붙어 있으면 90초마다 돌아갈 곳을 찾는다. 이미 있으면 덮어쓴다.
    // 디버그: {"wifi_try": "ssid"} — 저장된 그 와이파이로 바로 옮긴다(핫스팟 복귀 시험용).
    const cJSON* wifi_add = cJSON_GetObjectItem(root, "wifi_add");
    bool has_wifi = false;
    if (cJSON_IsObject(wifi_add)) {
        char ssid[33] = {0}, pass[65] = {0};
        CopyString(wifi_add, "ssid", ssid, sizeof(ssid));
        CopyString(wifi_add, "password", pass, sizeof(pass));
        if (ssid[0] != '\0') {
            const cJSON* jhs = cJSON_GetObjectItem(wifi_add, "hotspot");
            const int hotspot = cJSON_IsBool(jhs) ? (cJSON_IsTrue(jhs) ? 1 : 0) : -1;   // -1 = 있으면 유지, 새것은 핫스팟
            ESP_LOGI(TAG, "wifi_add %s hotspot=%d", ssid, hotspot);
            WifiManager::GetInstance().AddNetwork(ssid, pass, hotspot);
            has_wifi = true;
        }
    }
    const cJSON* wifi_try = cJSON_GetObjectItem(root, "wifi_try");
    if (cJSON_IsString(wifi_try) && wifi_try->valuestring != nullptr) {
        const std::string target = wifi_try->valuestring;
        has_wifi = true;
        // 메인 작업에서 끊는다(응답이 먼저 나가는 경우가 많지만 보장하지는 않는다)
        Application::GetInstance().Schedule([target]() {
            ESP_LOGI(TAG, "wifi_try %s -> %d", target.c_str(), WifiManager::GetInstance().SwitchTo(target));
        });
    }

    // 디버그: 터치 주입. {"touch": {"kind": "tap|hold|long|swipe_left|swipe_right|swipe_up|swipe_down", "x": 160, "y": 170}}
    // 실기기를 손으로 못 만지는 QA(맥에서 curl)용. 저장하지 않는다.
    const cJSON* touch = cJSON_GetObjectItem(root, "touch");
    bool has_touch = false;
    if (cJSON_IsObject(touch)) {
        char kind[16];
        CopyString(touch, "kind", kind, sizeof(kind));
        const cJSON* jx = cJSON_GetObjectItem(touch, "x");
        const cJSON* jy = cJSON_GetObjectItem(touch, "y");
        static const struct { const char* name; Display::TouchEvent::Kind kind; uint32_t press_ms; } kKinds[] = {
            {"tap", Display::TouchEvent::kTap, 120},          {"hold", Display::TouchEvent::kHold, 800},
            {"long", Display::TouchEvent::kLongPress, 2000},  {"swipe_left", Display::TouchEvent::kSwipeLeft, 200},
            {"swipe_right", Display::TouchEvent::kSwipeRight, 200}, {"swipe_up", Display::TouchEvent::kSwipeUp, 200},
            {"swipe_down", Display::TouchEvent::kSwipeDown, 200},
        };
        for (const auto& k : kKinds) {
            if (strcmp(kind, k.name) == 0 && cJSON_IsNumber(jx) && cJSON_IsNumber(jy)) {
                Display::TouchEvent ev{k.kind, static_cast<uint16_t>(jx->valueint), static_cast<uint16_t>(jy->valueint),
                                       static_cast<uint16_t>(jx->valueint), static_cast<uint16_t>(jy->valueint), k.press_ms};
                ESP_LOGI(TAG, "debug touch %s at (%d,%d)", k.name, jx->valueint, jy->valueint);
                if (!display->WakeIfScreenOff()) {
                    display->OnTouch(ev);
                }
                has_touch = true;
                break;
            }
        }
    }

    if (persist && has_cells && display->MacSetupGuideShown()) {
        display->HideMacSetupGuide();   // 맥에서 첫 사용량이 왔다 — 2단계 안내 끝
    }
    if (persist && has_cells) {
        // blob API 가 없어 문자열로 저장한다. 부팅 때 같은 경로로 되살린다.
        // state/ctx 는 빼고 저장한다 — 재부팅 직후 몇 시간 전의 "작업 중"이
        // 되살아나면 거짓말이 된다. cells 만 남긴다. "agent" 는 남긴다 — 복원 때
        // 그 에이전트 칸으로 돌아간다. 두 에이전트를 키 두 개에 따로 둔다.
        cJSON_DeleteItemFromObject(root, "state");
        cJSON_DeleteItemFromObject(root, "ctx");
        cJSON_DeleteItemFromObject(root, "sky");
        cJSON_DeleteItemFromObject(root, "touch");
        cJSON_DeleteItemFromObject(root, "cast");
        cJSON_DeleteItemFromObject(root, "act");
        cJSON_DeleteItemFromObject(root, "agent_select");
        cJSON_DeleteItemFromObject(root, "wifiguide");
        cJSON_DeleteItemFromObject(root, "wifi_add");
        cJSON_DeleteItemFromObject(root, "wifi_try");
        cJSON_DeleteItemFromObject(root, "wificfg");
        cJSON_DeleteItemFromObject(root, "set");
        cJSON_DeleteItemFromObject(root, "char");
        cJSON_DeleteItemFromObject(root, "poke");
        cJSON_DeleteItemFromObject(root, "poke_rx");
        cJSON_DeleteItemFromObject(root, "trip");
        cJSON_DeleteItemFromObject(root, "trip_rx");
        cJSON_DeleteItemFromObject(root, "trip_home");
        cJSON_DeleteItemFromObject(root, "trip_invite");
        cJSON_DeleteItemFromObject(root, "trip_go_home");
        cJSON_DeleteItemFromObject(root, "due");
        cJSON_DeleteItemFromObject(root, "birthday");
        cJSON_DeleteItemFromObject(root, "birthday2");
        cJSON_DeleteItemFromObject(root, "special");
        cJSON_DeleteItemFromObject(root, "flip180");
        cJSON_DeleteItemFromObject(root, "anniversary");
        cJSON_DeleteItemFromObject(root, "geo");
        cJSON_DeleteItemFromObject(root, "weather_panel");
        cJSON_DeleteItemFromObject(root, "poke_reply");
        cJSON_DeleteItemFromObject(root, "poke_reply_rx");
        cJSON_DeleteItemFromObject(root, "poke_high5");
        cJSON_DeleteItemFromObject(root, "sound");
        cJSON_DeleteItemFromObject(root, "sound_test");
        cJSON_DeleteItemFromObject(root, "diary");          // 열쇠는 NVS "diary" 에만 둔다
        cJSON_DeleteItemFromObject(root, "diary_fetch");
        cJSON_DeleteItemFromObject(root, "diary_test_new");
        cJSON_DeleteItemFromObject(root, "diary_panel");
        cJSON_DeleteItemFromObject(root, "manbok_menu");
        cJSON_DeleteItemFromObject(root, "manbok_panel");
        cJSON_DeleteItemFromObject(root, "manbok_fetch");
        // 옷은 NVS "wardrobe" 에 따로 산다. 날씨는 다음 푸시가 다시 입히므로 안 남긴다.
        cJSON_DeleteItemFromObject(root, "outfit");
        cJSON_DeleteItemFromObject(root, "unlock");
        cJSON_DeleteItemFromObject(root, "weather");
        char* stripped = cJSON_PrintUnformatted(root);
        if (stripped != nullptr) {
            Settings settings(kNvsNamespace, true);
            settings.SetString(has_agent && payload_agent == kUsageAgentCodex ? kNvsKeyCodex : kNvsKey, stripped);
            cJSON_free(stripped);
        }
    }
    cJSON_Delete(root);
    // has_agent: {"agent":"codex"} 단독 = 호스트의 싼 "선택 물어보기" ping (응답 JSON 이 목적).
    return has_cells || state[0] != '\0' || sky[0] != '\0' || has_touch || has_wardrobe ||
           has_act || has_agent_select || has_agent ||
           has_wifiguide || has_wificfg || has_wifi || has_set || has_char || has_poke || has_poke_rx ||
           has_poke_reply || has_poke_reply_rx || has_poke_high5 || has_trip || has_trip_rx ||
           has_trip_home || has_trip_invite || has_trip_go_home || has_cast || has_date || has_special || has_flip ||
           has_geo || has_weather_panel || has_sound || has_diary;
}

void UsageReceiver::RestoreLastStats() {
    auto display = GetLcdDisplay();

    // 캐릭터를 **맨 먼저** (NVS wardrobe:char). 아래 착장·세트가 그 캐릭터의 아이템 표와
    // 슬롯 이름으로 읽혀야 한다 — 순서가 바뀌면 남의 옷을 입고 뜬다.
    // 전역만 바꾸면 안 된다: UsageScreen 은 이미 만들어져 있고 **첫 부팅 기본 캐릭터**
    // (첫 팩)의 좌표계로 몸을 지어 뒀다. 그대로 두면 다른 팩이 첫 팩 배율로 뜬다.
    // SetUsageCharacter 가 몸까지 다시 짓는다. 이 빌드에 없는 옛 키는 첫 팩으로 남는다.
    const std::string char_key = WardrobeLoadCharacter();
    if (!char_key.empty() && display != nullptr) {
        if (display->SetUsageCharacter(char_key.c_str(), /*persist=*/false)) {
            ESP_LOGI(TAG, "Restored character %s", char_key.c_str());
        } else {
            ESP_LOGW(TAG, "stored character unusable: %s", char_key.c_str());
        }
    }

    // 옷·에이전트 선택 (NVS "wardrobe"). LOCK 이었으면 하늘까지 고정해 되살린다.
    Outfit outfit;
    bool locked = false;
    UsageSky sky = kUsageSkyNavy;
    uint8_t wx = 0;
    UsageAgent agent = kUsageAgentClaude;
    const bool has_outfit = WardrobeLoadOutfit(&outfit, &locked, &sky, &wx, &agent);
    if (has_outfit && display != nullptr) {
        // 에이전트 선택을 맨 먼저 — 아래 사용량 캐시 복원이 고른 쪽만 화면에 그리고
        // 다른 쪽은 기억만 해 두게 한다(ApplyJson).
        display->SetUsageAgent(AgentKey(agent), /*persist=*/false);
    }

    Settings settings(kNvsNamespace, false);
    std::string last = settings.GetString(kNvsKey, "");
    if (last.empty()) {
        ESP_LOGI(TAG, "No cached usage to restore");
    } else if (ApplyJson(last.c_str(), false)) {
        ESP_LOGI(TAG, "Restored cached usage (%d bytes)", static_cast<int>(last.size()));
    }
    const std::string last_codex = settings.GetString(kNvsKeyCodex, "");
    if (!last_codex.empty() && ApplyJson(last_codex.c_str(), false)) {
        ESP_LOGI(TAG, "Restored cached codex usage (%d bytes)", static_cast<int>(last_codex.size()));
    }

    if (has_outfit && display != nullptr) {
        display->SetUsageOutfit(outfit, locked ? 1 : 0, locked ? static_cast<int>(sky) : -1,
                                /*persist=*/false, wx);
        ESP_LOGI(TAG, "Restored outfit (lock=%d wx=%u agent=%s)", locked, wx, AgentKey(agent));
    }
    // 캐릭터 팩 착장 세트 (NVS wardrobe:set). 팩 모드가 아니면 SetUsageSet 이 거른다.
    const std::string set_key = WardrobeLoadSet();
    if (!set_key.empty() && display != nullptr) {
        if (display->SetUsageSet(set_key.c_str(), /*persist=*/false)) {
            ESP_LOGI(TAG, "Restored set %s", set_key.c_str());
        }
    }
}

static void RememberHost(httpd_req_t* req);
static void CaptureHostMac(void*);

esp_err_t UsageReceiver::HandlePost(httpd_req_t* req) {
    if (req->content_len == 0 || req->content_len > kMaxBodyLen) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_FAIL;
    }

    RememberHost(req);
    tcpip_callback(CaptureHostMac, nullptr);
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, &body[received], req->content_len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        received += r;
    }

    if (!ApplyJson(body.c_str(), true)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad payload");
        return ESP_FAIL;
    }

    // 데이터가 들어온 것도 활동으로 친다. 화면이 잠들어 있었다면 깨우고,
    // 유휴 시계를 0 으로 되돌려 곧바로 다시 잠들지 않게 한다.
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->WakeIfScreenOff(false);  // 수동으로 끈 화면은 안 켠다
    }
    Application::GetInstance().ResetIdleClock();

    // 응답에 지금 선택을 실어 준다 (docs/agent-badge.md §3 보드→호스트). 호스트가
    // 이걸 캐시해 집계를 고른 쪽으로 좁힌다. 옛 호스트는 본문을 안 읽는다.
    // 캐릭터 팩 모드면 지금 세트도 실어 준다 ("set" 필드는 팩 모드에서만 있다).
    char resp[120];
    auto lcd = GetLcdDisplay();
    const char* set_now = lcd != nullptr ? lcd->GetUsageSet() : nullptr;
    const char* char_now = lcd != nullptr ? lcd->GetUsageCharacter() : "pack";
    if (set_now != nullptr) {
        snprintf(resp, sizeof(resp), "{\"ok\":1,\"agent\":\"%s\",\"char\":\"%s\",\"set\":\"%s\"}",
                 AgentKey(lcd->GetUsageAgent()), char_now, set_now);
    } else {
        snprintf(resp, sizeof(resp), "{\"ok\":1,\"agent\":\"%s\",\"char\":\"%s\"}",
                 AgentKey(lcd != nullptr ? lcd->GetUsageAgent() : kUsageAgentClaude), char_now);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// 디버그: 지금 화면을 RGB565 원시 바이트로 돌려준다 (320×240 → 153600B). 맥에서
//   python3 tools/screen.py out.png
// lv_snapshot 은 실제 그리기 파이프라인으로 렌더하므로 패널에 보이는 것과 같다.
// 렌더는 httpd 태스크(스택 4KB)가 아니라 LVGL 태스크에서 한다 — lv_async_call 로
// 넘기고 세마포어로 기다린다. 버퍼는 PSRAM.
namespace {
struct SnapshotJob {
    lv_draw_buf_t* buf;
    SemaphoreHandle_t done;
    lv_result_t result;
};
void SnapshotAsync(void* arg) {
    auto* job = static_cast<SnapshotJob*>(arg);
    job->result = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, job->buf);
    xSemaphoreGive(job->done);
}
}  // namespace

esp_err_t UsageReceiver::HandleScreen(httpd_req_t* req) {
    auto display = GetLcdDisplay();
    if (display == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no display");
        return ESP_FAIL;
    }
    const uint32_t w = display->width(), h = display->height();
    const uint32_t stride = w * 2;
    auto* data = static_cast<uint8_t*>(heap_caps_malloc(stride * h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no psram");
        return ESP_FAIL;
    }
    lv_draw_buf_t buf;
    lv_draw_buf_init(&buf, w, h, LV_COLOR_FORMAT_RGB565, stride, data, stride * h);
    SnapshotJob job{&buf, xSemaphoreCreateBinary(), LV_RESULT_INVALID};
    {
        DisplayLockGuard lock(display);
        lv_async_call(SnapshotAsync, &job);
    }
    const bool ok = xSemaphoreTake(job.done, pdMS_TO_TICKS(5000)) == pdTRUE && job.result == LV_RESULT_OK;
    vSemaphoreDelete(job.done);
    if (!ok) {
        heap_caps_free(data);
        ESP_LOGW(TAG, "screen snapshot failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snapshot failed");
        return ESP_FAIL;
    }
    char hdr[3][16];
    snprintf(hdr[0], sizeof(hdr[0]), "%u", static_cast<unsigned>(w));
    snprintf(hdr[1], sizeof(hdr[1]), "%u", static_cast<unsigned>(h));
    snprintf(hdr[2], sizeof(hdr[2]), "%u", static_cast<unsigned>(stride));
    httpd_resp_set_hdr(req, "X-Width", hdr[0]);
    httpd_resp_set_hdr(req, "X-Height", hdr[1]);
    httpd_resp_set_hdr(req, "X-Stride", hdr[2]);
    httpd_resp_set_type(req, "application/octet-stream");
    esp_err_t err = ESP_OK;
    const size_t total = stride * h;
    for (size_t off = 0; off < total && err == ESP_OK; off += 8192) {
        const size_t n = (total - off < 8192) ? (total - off) : 8192;
        err = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(data + off), n);
    }
    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, nullptr, 0);
    }
    heap_caps_free(data);
    return err;
}

namespace {
bool g_mac_guide_skipped = false;   // 이번 부팅 동안 맥 연결 안내를 건너뜀(탭)

// WiFi 는 붙었는데 맥에서 사용량이 한 번도 안 왔으면(NVS 캐시 없음) 2단계 안내를 띄운다.
// 첫 cells push 가 저장될 때 ApplyJson 이 내린다. 보드 주소는 자동 찾기가 막혔을 때 쓰라고 보여 준다.
void MaybeShowMacSetupGuide() {
    if (g_mac_guide_skipped) return;
    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) return;
    Settings settings(kNvsNamespace, false);
    if (!settings.GetString(kNvsKey, "").empty() || !settings.GetString(kNvsKeyCodex, "").empty()) return;
    char ip[20] = "";
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    }
    display->ShowMacSetupGuide(ip);
}
}  // namespace

// 마지막으로 사용량을 보낸 맥의 주소(IP·하드웨어). 와이파이를 옮겨 다시 붙으면 보드의 ARP 표가 비는데,
// 맥에 답하려고 보드가 방송으로 맥 주소를 물으면 이 집 공유기에서는 1~2분 답이 없었다(맥은 보드 주소를
// 이미 알아 다시 묻지 않음, 같은 AP 의 폰은 먼저 물어 와서 13초). 그래서 다시 붙자마자 기억해 둔 맥에게
// 방송이 아닌 직접(유니캐스트) ARP 로 묻는다. 맥이 답하면 lwIP 가 보통 줄로 배운다. 맥 주소가 바뀌었으면
// 답이 없을 뿐 틀린 줄이 남지 않는다(고정 줄은 lwIP 가 덮어쓰지 않아 쓰지 않는다).
static uint32_t g_last_host_ip = 0;   // network byte order, 0 = 모름
static struct eth_addr g_last_host_mac = {};
static bool g_last_host_mac_ok = false;
static uint32_t g_mac_for_ip = 0;     // g_last_host_mac 이 어느 IP 의 것인지

static struct netif* StaNetif() {
    esp_netif_t* n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return n ? static_cast<struct netif*>(esp_netif_get_netif_impl(n)) : nullptr;
}

static void RememberHost(httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage addr = {};
    socklen_t len = sizeof(addr);
    if (fd >= 0 && getpeername(fd, (struct sockaddr*)&addr, &len) == 0) {
        if (addr.ss_family == AF_INET) {
            g_last_host_ip = ((struct sockaddr_in*)&addr)->sin_addr.s_addr;
        }
#if LWIP_IPV6
        else if (addr.ss_family == AF_INET6) {
            auto* a6 = (struct sockaddr_in6*)&addr;   // IPv4-mapped (::ffff:a.b.c.d)
            if (a6->sin6_addr.un.u32_addr[0] == 0 && a6->sin6_addr.un.u32_addr[1] == 0 &&
                a6->sin6_addr.un.u32_addr[2] == PP_HTONL(0x0000FFFF)) {
                g_last_host_ip = a6->sin6_addr.un.u32_addr[3];
            }
        }
#endif
    }
}

// tcpip 작업에서: 맥의 하드웨어 주소를 ARP 표에서 읽어 둔다(IP 가 바뀌면 옛 주소는 버린다)
static void CaptureHostMac(void*) {
    struct netif* nif = StaNetif();
    const uint32_t ip_now = g_last_host_ip;
    if (nif == nullptr || ip_now == 0) return;
    if (g_mac_for_ip != ip_now) g_last_host_mac_ok = false;
    ip4_addr_t ip; ip.addr = ip_now;
    struct eth_addr* mac = nullptr; const ip4_addr_t* ipr = nullptr;
    if (etharp_find_addr(nif, &ip, &mac, &ipr) >= 0 && mac != nullptr) {
        g_last_host_mac = *mac;
        g_mac_for_ip = ip_now;
        g_last_host_mac_ok = true;
    }
}

// tcpip 작업에서: 기억해 둔 맥에게 유니캐스트 ARP 요청을 보낸다. 같은 서브넷일 때만.
static void AskHostMac(void*) {
    struct netif* nif = StaNetif();
    if (nif == nullptr || !g_last_host_mac_ok || g_mac_for_ip == 0) return;
    if (!netif_is_up(nif) || ip4_addr_isany_val(*netif_ip4_addr(nif))) return;
    ip4_addr_t ip; ip.addr = g_mac_for_ip;
    if (!ip4_addr_net_eq(&ip, netif_ip4_addr(nif), netif_ip4_netmask(nif))) return;   // 다른 망(핫스팟)
    struct pbuf* p = pbuf_alloc(PBUF_LINK, SIZEOF_ETHARP_HDR, PBUF_RAM);
    if (p == nullptr) return;
    auto* hdr = static_cast<struct etharp_hdr*>(p->payload);
    hdr->hwtype = PP_HTONS(LWIP_IANA_HWTYPE_ETHERNET);
    hdr->proto = PP_HTONS(ETHTYPE_IP);
    hdr->hwlen = ETH_HWADDR_LEN;
    hdr->protolen = sizeof(ip4_addr_t);
    hdr->opcode = PP_HTONS(ARP_REQUEST);
    SMEMCPY(&hdr->shwaddr, nif->hwaddr, ETH_HWADDR_LEN);
    memset(&hdr->dhwaddr, 0, ETH_HWADDR_LEN);
    IPADDR_WORDALIGNED_COPY_FROM_IP4_ADDR_T(&hdr->sipaddr, netif_ip4_addr(nif));
    IPADDR_WORDALIGNED_COPY_FROM_IP4_ADDR_T(&hdr->dipaddr, &ip);
    ethernet_output(nif, p, reinterpret_cast<const struct eth_addr*>(nif->hwaddr), &g_last_host_mac, ETHTYPE_ARP);
    pbuf_free(p);
}

// 다시 붙은 직후 0·1·3초에 한 번씩 묻는다(첫 프레임은 연결 직후라 놓칠 수 있다)
static esp_timer_handle_t g_ask_timer = nullptr;
static int g_ask_left = 0;
static void AskHostMacSoon() {
    if (!g_last_host_mac_ok) return;
    tcpip_callback(AskHostMac, nullptr);
    if (g_ask_timer == nullptr) {
        esp_timer_create_args_t args = {
            .callback = [](void*) {
                tcpip_callback(AskHostMac, nullptr);
                if (--g_ask_left > 0) esp_timer_start_once(g_ask_timer, 2000 * 1000);
            },
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "AskHostMac",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &g_ask_timer) != ESP_OK) return;
    }
    g_ask_left = 2;
    esp_timer_stop(g_ask_timer);
    esp_timer_start_once(g_ask_timer, 1000 * 1000);
}

// GET /wifi — 와이파이 상태와 최근 기록(저장된 이름만, 비밀번호 없음). 핫스팟 시험용.
static esp_err_t HandleWifiGet(httpd_req_t* req) {
    const std::string body = WifiManager::GetInstance().GetDebugJson();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.c_str(), body.size());
}

void UsageReceiver::SkipMacSetupGuide() {
    g_mac_guide_skipped = true;
}

void UsageReceiver::Start() {
    MaybeShowMacSetupGuide();   // 다시 붙을 때(Start 두 번째)도 아직 안 왔으면 띄운다
    AskHostMacSoon();   // 다시 붙을 때 맥에 바로 답할 수 있게
    if (server_ != nullptr) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    // LWIP 소켓 한도 10 = httpd 내부 3 + 세션 5 + 콕 UDP 1 (poke_link.cc) + 날씨 TCP 1 (weather_fetcher.cc).
    // 한도를 넘기면 LRU 가 돌기 전에 accept 가 실패하고, 재부팅 전까지 POST 를 못 받을 수 있다.
    config.max_open_sockets = 5;
    config.max_uri_handlers = 4;
    config.stack_size = 4096;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        server_ = nullptr;
        return;
    }

    static const httpd_uri_t usage_uri = {
        .uri = "/usage",
        .method = HTTP_POST,
        .handler = HandlePost,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server_, &usage_uri);
    static const httpd_uri_t screen_uri = {
        .uri = "/screen",
        .method = HTTP_GET,
        .handler = HandleScreen,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server_, &screen_uri);
    static const httpd_uri_t wifi_uri = {
        .uri = "/wifi",
        .method = HTTP_GET,
        .handler = HandleWifiGet,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server_, &wifi_uri);
    ESP_LOGI(TAG, "Usage receiver listening on POST /usage");
}

void UsageReceiver::Stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}
