#pragma once

// 클로드 코드 사용량 화면 + 캐릭터 상태 화면.
//
// 대기(idle) 화면은 시안 아티팩트의 "대기 — 사용량 화면" 목업 그대로다:
// 어두운 바탕 위에 좌·우 두 블록(라벨 / 큰 퍼센트 / 남은 시간)과 하단
// 막대 두 개. 왼쪽이 5시간 창, 오른쪽이 주간 창이다. 옛 마스코트 얼굴
// (눈·광선 아이콘·4칸 셀·표정 셔플)은 걷어냈다 (PROVENANCE.md 참고).
//
// 폰트는 반드시 플래시에 정적으로 박힌 몬세라트를 쓴다. 에셋 파티션에서
// 받아오는 폰트(LvglCBinFont)를 직접 지정하면, 에셋 재적용 때 그 폰트가
// 해제되면서 이 위젯들만 죽은 포인터를 가리켜 lv_font_get_glyph_width() 에서
// 죽는다. 사용량 화면은 숫자·영문만 쓰므로 한글 폰트가 필요 없다.

#include <lvgl.h>

#include <cstdint>
#include <memory>

#include "art/ui_poke.h"
#include "art/scene_sprites.h"
#include "figure.h"
#include "display.h"
#include "character.h"
#include "character_pack.h"
#include "usage_ticker.h"
#include "bboketer_scenes.h"

// 클로드봇 상태 화면. idle 이면 사용량 화면이고, 나머지는 캐릭터 팩의 상태
// 그림(pack.json map)이 나오는 상태 화면이다. 어느 상태를 보여줄지는
// 호스트가 우선순위 집계를 끝내고 보낸 값이라 펌웨어는 그대로 반영만 한다.
// 시간 복귀(에러 5초→작업 등)는 LcdDisplay::OnIdleTick() 이 굴린다.
enum UsageState : uint8_t {
    kUsageStateIdle = 0,   // 사용량 화면
    kUsageStateThinking,   // 프롬프트 읽는 중
    kUsageStateWorking,    // 도구 실행 중
    kUsageStateJuggling,   // 서브에이전트 저글링
    kUsageStatePermission, // 권한 대기 — 테두리 펄스
    kUsageStateError,      // 도구 실패
    kUsageStateDone,       // 턴 완료
    kUsageStateSleeping,   // 장시간 무활동 — 어둡게
    kUsageStateCount,
};

// 에이전트 선택(docs/agent-badge.md §10). Claude 가 기본. 배지 탭으로 Claude ↔ Codex
// 토글하고 NVS(wardrobe 착장 JSON 의 "agent")에 산다. BOTH 모드는 2026-09-06 사용자
// 결정으로 뺐다 — 보드는 언제나 한쪽만 보여주고 배지는 항상 떠 있다.
// 순서는 NVS 키 표(wardrobe_store kAgentKeys)와 같아야 한다.
enum UsageAgent : uint8_t {
    kUsageAgentClaude = 0,
    kUsageAgentCodex,
    kUsageAgentCount,
};

// 윗줄 시계 칸의 날씨 그림. 순서는 art/weather_icons.c(tools/gen_weather_icons.py ICONS)와 같다.
enum UsageWeatherIcon : int8_t {
    kUsageWeatherNone = -1,   // 아직 모름 — 시계만
    kUsageWeatherClear = 0,   // 맑음
    kUsageWeatherPartly,      // 구름 조금
    kUsageWeatherCloud,       // 흐림 (밤 구름 조금도 이것)
    kUsageWeatherRain,
    kUsageWeatherSnow,
    kUsageWeatherMoon,        // 맑은 밤
    kUsageWeatherIconCount,
};

// 대기 화면 배경. POST {"sky": ...} 나 옷장 BG 탭으로 고른다. 앞 13칸은 옛 하늘 키(NVS·프로토콜
// 값을 잇기 위해 번호만 남았고 그림은 없다), 그 뒤가 장면(assets)과 기념일 배경이다.
enum UsageSky : uint8_t {
    kUsageSkyBrown = 0,  // 마을 · 어두운 갈색 밤 (#161008, 처음 것)
    kUsageSkyNavy,       // 마을 · 남색 밤 (#0E1830)
    kUsageSkyDay,        // 마을 · 낮 하늘색 (#4E8FD6) — 라벨 색도 바뀐다
    kUsageSkySunset,     // 마을 · 노을 (보라→노랑 띠)
    kUsageSkyRoom,       // 방 · 책상(lofi)
    kUsageSkyCamp,       // 캠프파이어
    kUsageSkySpace,      // 우주
    kUsageSkyCafe,       // 카페
    kUsageSkyAlley,      // 고양이 돌담길
    kUsageSkySea,        // 바다속
    kUsageSkySakura,     // 벚꽃길
    kUsageSkyLibrary,    // 도서관
    kUsageSkySite,       // 공사장
    kUsageSkyMoon,        // 우주(달). 2026-11-07 축제 자리에 넣었다(옛 kUsageSkySpace 와 겹치지 않게 moon)
    kUsageSkyFirstScene = kUsageSkyMoon,
    kUsageSkyPub,
    kUsageSkyLpbar,
    kUsageSkyVintage,
    kUsageSkyOffice,
    kUsageSkySalad,
    kUsageSkyScuba,
    kUsageSkyParaglide,
    kUsageSkyDolopath,    // 돌로미티 산길 (여기부터 캐릭터 그림을 기존 장면에서 빌린다)
    kUsageSkyCappadocia,  // 카파도키아
    kUsageSkySeoul,       // 서울 야경
    kUsageSkyCamping,     // 산속 캠핑 (옛 camp·kUsageSkyCamp 와 이름이 겹치지 않게 camping)
    kUsageSkyBeach,       // 해변
    kUsageSkyJeju,        // 제주 귤밭
    kUsageSkyBlossom,     // 벚꽃 공원 (옛 kUsageSkySakura 와 겹치지 않게 blossom)
    kUsageSkyMyroom,      // ny 컴퓨터방(벽에 AI Championship 상장). 11/7 ny 집 사진을 펠트로
    kUsageSkyBedroom,     // ny 침실
    kUsageSkyLiving,      // ny 거실
    kUsageSkyBallpark,    // 광주 기아 야구장
    kUsageSkySubway,      // 만원 지하철
    kUsageSkyLastScene = kUsageSkySubway,   // 장면 끝. europe·dolomeadow·festival 은 뺐다(NVS 이름은 SkyByKey 가 잇는다)
    kUsageSkyBirth,      // 여기부터 옷장에서 고르는 기념일 배경. 몸은 그 기념일 자세(BboketerSkyBody)
    kUsageSkyPepero,
    kUsageSkyXmas,
    kUsageSkyCount,
};

// I8 배경(플래시)을 RGB565 로 1회 디코드해 두는 PSRAM 캔버스.
// lv_image 에 I8 을 직접 물리면 무효화 때마다 전체 이미지를 다시 디코드해서
// (RAM_LOAD) 걷기 애니메이션이 매 프레임 배경 변환 비용을 문다. 여기 담아 두면
// 예전 RGB565 플래시 배열과 그리기 비용이 같다.
class SkyCanvas {
public:
    SkyCanvas() = default;
    ~SkyCanvas();
    SkyCanvas(const SkyCanvas&) = delete;
    SkyCanvas& operator=(const SkyCanvas&) = delete;
    // src(I8) 를 디코드해 캔버스 dsc 를 돌려준다. PSRAM 할당 실패면 src 그대로.
    const lv_image_dsc_t* Render(const lv_image_dsc_t* src);

private:
    uint8_t* buf_ = nullptr;
    lv_image_dsc_t dsc_ = {};
};

// 호스트가 밀어 넣는 값.
//
// **글자는 맥이 만들어 보내고 펌웨어는 뜻을 모른다.** 표기를 바꾸려면 맥 쪽
// 스크립트만 고치면 된다. 대기 화면은 앞의 두 칸(한도)만 그린다:
//     cells[0]  5h   left "42%"  right "2h 41m"   → 그리지 않는다(5시간 한도 없어짐)
//     cells[1]  week left "71%"  right "2d 4h"    → 오른쪽 블록 + 아래 막대
// 막대 값은 left 에서 정수를 파싱한다("42%"→42). right 가 비면(추정치 폴백)
// 남은 시간 줄도 비운다. cells[2..3](오늘/주간 비용)은 받기만 하고 대기
// 화면에는 그리지 않는다.
struct UsageStats {
    static constexpr int kMaxCells = 4;

    struct Cell {
        char name[12] = {};
        char left[16] = {};
        char right[16] = {};
    };

    Cell cells[kMaxCells];
    uint8_t cell_count = 0;
};

// 시각으로 고른 마을 하늘: 07~19시(KST) 낮, 그 외 남색 밤. 시계가 아직 안 맞았으면(SNTP 전) 남색.
// LcdDisplay 의 auto 하늘과 팩 미니카 외출이 같이 쓴다.
UsageSky UsageVillageSkyForClock();
// 밤 화면 시간대(KST 23~07시). 시계가 아직 안 맞았으면 false.
bool UsageIsNightNow();

class UsageScreen {
public:
    // parent 위에 불투명 패널을 만들고 그 안에 그린다.
    // 패널을 따로 두는 이유: SetTheme() 이 container_ 자체의 배경색은
    // 덮어쓰지만 그 자식은 건드리지 않기 때문이다.
    UsageScreen(lv_obj_t* parent, int width, int height);

    // 숫자 갱신. 반드시 DisplayLockGuard 안에서 부를 것.
    void SetStats(const UsageStats& stats);

    // 상태 화면 전환. 반드시 DisplayLockGuard 안에서 부를 것.
    // idle 로 돌아오면 사용량 화면이 복원된다.
    void SetState(UsageState state, const char* ctx);
    UsageState state() const { return state_; }

    // 대기 화면의 걷기 애니메이션을 잠깐 멈추거나 되살린다. 화면을 끈 동안
    // 안 보이는 프레임을 디코드하지 않으려는 것. 반드시 DisplayLockGuard 안에서.
    void PauseIdleWalk(bool paused);

    // 대기 화면 하늘 교체. 반드시 DisplayLockGuard 안에서.
    void SetSky(UsageSky sky);
    UsageSky sky() const { return sky_; }

    // 날씨 이펙트 (0=없음 1=비 2=눈). 호스트 push 발. 대기 화면에만 내린다.
    // 반드시 DisplayLockGuard 안에서.
    void SetWeatherFx(uint8_t fx);

    // 날씨 연출 모드 (kWxItems 인덱스: 0=auto 1=clear 2=rain 3=snow 4=leaf
    // 5=star). auto 가 아니면 push 날씨 파티클을 무시하고 고른 연출만 튼다.
    // 옷 자동 규칙(우산 등)은 건드리지 않는다. 반드시 DisplayLockGuard 안에서.
    void SetWeatherMode(uint8_t wx);

    // 윗줄 왼쪽(배지 오른쪽) 시계 칸. 첫 SetClock 전(시간이 안 맞음)에는 칸째 숨긴다.
    // hour 는 0~23. 반드시 DisplayLockGuard 안에서.
    void SetClock(int hour, int minute);
    // 시계 칸의 날씨 그림·기온. icon 이 kUsageWeatherNone 이고 기온도 없으면 시계만 보인다.
    // 반드시 DisplayLockGuard 안에서.
    void SetWeatherInfo(UsageWeatherIcon icon, bool has_temp, int temp);
    uint8_t weather_mode() const { return wx_mode_; }
    bool WeatherShownHere() const;

    // 대기 화면 위 터치(docs/touch.md). 반드시 DisplayLockGuard 안에서, idle
    // 상태일 때만 부를 것. 몸통(캐릭터 bbox + 8px) 탭 = 놀람, 3초 안 3연타 =
    // 삐짐, 몸통 홀드 = 쓰다듬기. 빈 곳 롱프레스는 화면을 끄라고, 우하단 옷걸이
    // 탭은 옷장을 열라고 돌려준다 — 백라이트와 옷장은 LcdDisplay 가 가지고 있다.
    // kAgentCycled: 좌상 배지 탭으로 선택이 바뀌었다 — LcdDisplay 가 NVS 에 저장한다.
    // kSetCycled: (캐릭터 팩 모드) 옷걸이 탭으로 착장 세트가 바뀌었다 — LcdDisplay 가 NVS 에 저장.
    // kPokeSend: 하트 탭·패널 줄 탭 — LcdDisplay 가 poke_phrase() 를 전송한다.
    // kTripGo·kTripInvite·kTripHome: 패널 오른쪽 칸 「놀러가기·놀러오기·집으로 가기」(docs/trip.md).
    // kOpenWeather: 윗줄 시계·날씨 칸 탭.
    enum class TouchAction : uint8_t {
        kNone, kScreenOff, kOpenWardrobe, kAgentCycled, kSetCycled, kPokeSend, kPokeReply, kTripGo, kTripInvite, kTripHome,
        kOpenWeather,   // 윗줄 시계·날씨 칸 탭 — LcdDisplay 가 날씨 판을 연다
        kOpenDiary,     // 문구 패널 「육아일기 읽기」·흐르는 줄의 일기·새 일기 알림 탭 — diary_open_entry() 참고
        kOpenManbokMenu,   // 일기를 받는 보드에서 하트 꾹 — LcdDisplay 가 만복이 메뉴를 연다(manbok_panel.h)
    };
    TouchAction OnTouch(const Display::TouchEvent& ev);

    // --- 캐릭터 런타임 교체 (docs/character-pack.md "런타임 선택") ---
    // 두 몸(대기 화면·상태 화면)을 새 좌표계로 다시 만들고 지금 화면을 다시 그린다.
    // 옷은 **호출자가** 그 캐릭터 것으로 다시 입힌다 — 아이템 표가 캐릭터마다 달라
    // 인덱스를 물려줄 수 없다(LcdDisplay::SetUsageCharacter 가 NVS 에서 읽어 온다).
    // 팩이 없는 빌드에서 kPack 은 false. 반드시 DisplayLockGuard 안에서.
    bool SetCharacter(Character c);

    // --- 캐릭터 팩 (docs/character-pack.md) ---
    // 착장 세트 전환. key 는 pack.json sets[]. 모르는 키면 false. 반드시 DisplayLockGuard 안에서.
    // toast=true 면 0.8초 세트 이름 토스트 (탭·주입 경로).
    bool SetPackSet(const char* key, bool toast);
    const char* pack_set_key() const { return ActivePack().sets[pack_set_].key; }
    // 옷걸이 탭·BOOT 버튼: 다음 세트로 (세트가 하나면 아무 일 없음).
    void CyclePackSet();
    // 미니카 행동을 지금 시작 — QA·디버그 주입({"act":"ride"})용. 제자리 idle 대기 화면일 때만 true.
    // 반드시 DisplayLockGuard 안에서.
    bool TriggerPackRide();
    // 옷장 미리보기 몸: 지금 세트의 idle 프레임 0 (테이블 상수).
    const lv_image_dsc_t* pack_idle_frame() const {
        return pack_set().states[kUsageStateIdle].frames[0];
    }
    // 미니카 달리는 중 — LcdDisplay 의 시각 하늘 갱신이 외출 배경을 되돌리지 않게.
    bool pack_riding() const { return pack_riding_; }

    // --- 에이전트 배지 (docs/agent-badge.md) ---
    // 선택 = 배지(글자 「CC」/「CX」). 반드시 DisplayLockGuard 안에서. toast=true 면 CLAUDE/CODEX
    // 토스트 (탭·주입 경로).
    void SetAgent(UsageAgent agent, bool toast);
    // agent 의 사용량을 기억하고, 지금 고른 쪽이면 화면에도 그린다.
    void SetAgentStats(UsageAgent agent, const UsageStats& stats);
    UsageAgent agent() const { return agent_; }
    // 탭 경로: Claude ↔ Codex 토글 + 토스트. 새 선택을 돌려준다.
    UsageAgent CycleAgent();
    // 좌상 배지 히트 박스(≈56×40). 상태 화면에서도 쓴다 — 엿보기 대신 전환.
    bool HitBadge(int x, int y) const;

    // --- 콕 찌르기 (docs/poke.md) ---
    // 마지막 kPokeSend 의 문구 번호. 0 = 문구 없는 콕, 1~5 = 문구.
    uint8_t poke_phrase() const { return poke_phrase_; }
    // 기념일 배경을 깔거나(nullptr 이면) 걷는다. 고른 장면은 그대로 기억한다. DisplayLockGuard 안에서.
    void SetSpecialBg(const lv_image_dsc_t* image);
    bool special_bg_active() const { return special_bg_ != nullptr; }
    UsageTicker& ticker() { return ticker_; }
    // --- 만복이 육아일기 (docs/manbok-diary.md) ---
    // kOpenDiary 일 때 바로 열 글. -1 목록(문구 패널), -2 흐르는 줄·알림이 가리키는 글(LcdDisplay 가 id 로 찾음).
    int diary_open_entry() const { return diary_open_entry_; }
    // 일기를 받은 보드만 문구 패널 맨 위에 「육아일기 읽기」 줄이 생긴다. 반드시 DisplayLockGuard 안에서.
    void SetDiaryAvailable(bool available);
    // 일기를 받는 보드(열쇠 있음)는 하트 꾹이 문구 패널 대신 만복이 메뉴(kOpenManbokMenu)다.
    void SetManbokMenu(bool on) { manbok_menu_ = on; }
    // 놀러가기·놀러오기·집으로 가기 중 지금 누를 수 있는 칸(문구 패널·만복이 메뉴 공용).
    void TripRowsOn(bool on[3]) const;
    // 새 일기 알림: 배지 아래 한글 토스트(누르면 그 글). 반드시 DisplayLockGuard 안에서.
    void ShowDiaryToast(const std::string& text);
    void HideDiaryToast();
    // 마지막 kPokeReply 의 답장 번호 1~15 (문구마다 3칸, docs/poke.md).
    uint8_t poke_reply() const { return poke_reply_; }
    // 답장 버튼이 떠 있다. 상태 화면 위에서도 터치를 받아야 한다.
    bool poke_replies_open() const { return poke_replies_open_; }
    // 보낸 쪽 진행 표시 (ASCII 토스트). 반드시 DisplayLockGuard 안에서.
    void ShowPokeStatus(const char* text) { ShowAgentToast(text); }
    // 받은 쪽: 화면 가운데 큰 하트 + 문구 4초. 상태 화면 위에도 뜬다. 대기 화면의 팩 캐릭터는
    // 탭 반응도 같이 한다. 반드시 DisplayLockGuard 안에서.
    void ShowPokePop(uint8_t phrase);
    // 내가 보낸 문구에 답이 왔다: 하트 + 답장 + 「읽음」.
    void ShowPokeReplyPop(uint8_t reply);
    // 10초 안에 서로 콕을 보냈다: 「하이파이브!」 + 캐릭터 반응.
    void ShowHighFivePop();

    // --- 보드 놀러가기 (docs/trip.md, usage_trip.cc). 반드시 DisplayLockGuard 안에서. ---
    // 지금 나가거나 손님을 받을 수 있나: 대기 화면이고 놀러가기가 진행 중이 아니다.
    bool CanTrip() const;
    // 내 캐릭터가 화면 밖으로 나간다. 상대가 받아 준 뒤에 부른다.
    bool StartTripLeave(uint8_t who = 0);
    // 켜진 캐릭터(usage_cast.cc). 장면 화면에서는 세 마리를 각자 그림 한 장으로 그린다.
    bool CastActive() const;
    uint8_t CastDrawMask() const;     // 지금 화면에 있는 캐릭터. 나간 캐릭터는 빠지고 손님은 더해진다
    void CastChanged();               // 옷장에서 켜고 끈 뒤에 부른다
    void TripWave();                  // 상대가 왔는데 이미 다 같이 있다 — 그 자리에서 인사만 한다
    // 손님이 들어온다. character 는 CurrentCharacter() 값(1 = 첫 팩). 못 받으면 false.
    bool StartTripGuest(uint8_t character);
    // 내 캐릭터가 돌아온다. 손님이 떠났다는 신호가 왔거나 시간이 지났을 때.
    void StartTripReturn();
    // 집으로 가기. 손님이 있으면 내보낸다(떠나면 상대에게 「떠났다」가 간다). 손님이 없으면 false.
    bool SendGuestHome();
    // 나가 있는 내 캐릭터를 부를 수 있나(나가는 중·나가 있음). 상대가 손님을 내보내면 「떠났다」로 돌아온다.
    bool TripRecall();

    // 옷(docs/wardrobe.md). 대기 화면 캐릭터에 바로 입힌다. 반드시 DisplayLockGuard 안에서.
    void SetOutfit(const Outfit& outfit);
    const Outfit& outfit() const { return figure_->outfit(); }

private:
    static constexpr int kLimitBlocks = 2;  // 5h · week

    // 보드 놀러가기. Leaving·Away·Returning = 내 캐릭터가 나간 쪽, Guest* = 손님을 받은 쪽.
    enum class Trip : uint8_t { kNone, kLeaving, kAway, kReturning, kGuestEnter, kGuestStay, kGuestExit };
    bool TripBodyGone() const {
        return trip_ == Trip::kLeaving || trip_ == Trip::kAway || trip_ == Trip::kReturning;
    }
    bool TripHasGuest() const {
        return trip_ == Trip::kGuestEnter || trip_ == Trip::kGuestStay || trip_ == Trip::kGuestExit;
    }
    void CastCreate();
    void ApplyCast();
    void CastHide();
    bool CastMoving(int who) const;
    void CastPut(int who, const lv_image_dsc_t* src, int x, int y);
    void CastPlaceState();
    void CastReact(uint8_t mask, uint8_t pose, uint32_t ms);
    void CastReactStop();
    void CastReactTick();
    static void OnCastReactTick(lv_timer_t* timer);
    bool CastHit(int x, int y, int* who, uint8_t* pose) const;
    void CastReactAll(uint32_t ms);
    uint8_t CastGoing(uint8_t asked) const;   // 답으로 온 캐릭터 중 지금 켜진 것(0 이면 아무도 못 간다)
    TouchAction OnCastTouch(const Display::TouchEvent& ev, bool in_hanger);
    void EnsureTripObjects();
    void StopTripSprite();
    void EndTrip();
    void RefreshHostBody();
    void DrawTrip();
    void TripTick();
    static void OnTripTick(lv_timer_t* timer);
    void TripOnStopWalk();
    void TripOnIdleResume();
    bool HitTripGuest(int x, int y) const;
    void OnTripGuestTap();

    // 터치 반응 중인가. 반응 애니(pack_react_anim_)는 OnReactionTick 이 돌린다.
    enum class Reaction : uint8_t { kNone, kPoke };

    void ShowIdle(bool visible);
    void LayoutLimitPlate(int i);   // 글자 폭에 맞춰 뒤 판 크기를 잡는다
    void LayoutClockPlate();        // 시계 칸: 보일지 정하고 글자 폭에 맞춰 놓는다
    bool HitClockPlate(int x, int y) const;
    void StartWalk();
    void StartWalkFrom(int from_x, bool rest_first);
    void StopWalk();
    void StopLegTimer();
    static void OnWalkLegDone(lv_anim_t* anim);
    static void OnWalkLegFrame(lv_timer_t* timer);
    // 우주 하늘의 사인 부유 높이.
    int WalkLift();
    void BuildSceneSprites();
    static void OnSceneSpriteTick(lv_timer_t* timer);
    void BuildParticles();
    static void OnDropTick(lv_timer_t* timer);
    bool HitBody(int x, int y) const;
    void EndReaction();
    static void OnReactionTick(lv_timer_t* timer);
    void StartPermissionPulse();
    void StopPermissionPulse();

    // --- 캐릭터 팩 (docs/character-pack.md): 팩 프레임 재생 ---
    const PackSet& pack_set() const { return ActivePack().sets[pack_set_]; }
    // 대기 화면 몸통 애니: 평소엔 제자리 idle, 미니카(ride) 달리는 동안은 방향별 ride/ride_left.
    // 팩에 ride 가 없으면(count 0) 옛 방식 — idle 프레임 그대로 좌우로 옮긴다.
    const PackAnim& walk_anim() const {
        const PackSet& s = pack_set();
        if (pack_riding_) return (!walk_right_ && s.ride_left.count > 0) ? s.ride_left : s.ride;
        if (IsBboketerScene() && !CharacterBorrowsScenes()) return kBboketerScenePoseAnims[BodyScene()];
        return s.states[kUsageStateIdle];
    }
    // 몸 그림을 고를 때 쓰는 번호(bboketer_scenes.h kBboketerBodyCount). 0~19 = 장면, 20~24 = 기념일 자세.
    // 기념일 배경(자동·옷장)이면 그 기념일 자세, 결혼기념일처럼 자세가 없는 날은 정면 자세(옷방)로 앉는다.
    int BodyScene() const {
        if (special_bg_ != nullptr) {
            const int body = BboketerSpecialBody(special_bg_);
            return body >= 0 ? body : kFrontalScene;
        }
        return BboketerSkyBody(sky_ - kUsageSkyFirstScene);
    }
    static constexpr int kFrontalScene = 3;
    // 집주인 발이 로나(집) 발에서 얼마나 아래인가(라인아트 배치마다 다르다).
    int SceneHostDy() const;
    void ApplySceneProps();
    void RaiseFrontProp();
    bool IsBboketerScene() const {
        return CharacterHasScenes() && sky_ >= kUsageSkyFirstScene && sky_ < kUsageSkyCount;
    }
    // 지금 팩에 미니카가 있는지 — 제자리 대기·자리 소품·미니카는 전부 이 하나로 갈린다
    // (seat()·StartWalk·StartPackRide).
    bool pack_has_ride() const { return pack_set().ride.count > 0; }
    // 미니카 행동(docs/character-pack.md "ride"): 제자리에 앉아 있다가 예약(랜덤 20~60초)이
    // 차면 지금 자리에서 반대 끝까지 ride 애니로 달리고, 닿으면 다시 제자리. 걷기 타이머·anim 을
    // 그대로 재사용한다 — StopLegTimer 가 예약도 같이 지운다(반응·상태 전환·옷장이 끊을 때).
    void ArmPackRide();
    void StartPackRide();
    void StopPackRideTimer();
    static void OnPackRideDue(lv_timer_t* timer);
    bool pack_riding_ = false;
    uint8_t pack_ride_leg_ = 0;                // 0 = 가운데→끝, 1 = 끝→가운데(돌아오는 길)
    lv_timer_t* pack_ride_timer_ = nullptr;
    int walk_x_center() const { return (walk_x_min_ + walk_x_max_) / 2 + SceneDx(); }
    int SceneDx() const;                       // 장면에서 로나를 옮기는 양(스케일 전)
    int SceneWalkY() const;                    // 장면 바닥에 발이 닿는 walk_y_
    void ApplyScenePlace();                    // walk_y_ 와 로찌 그림을 지금 장면에 맞춘다
    static void OnSceneBob(lv_timer_t* timer);
    void SceneBobTick();                       // 장면의 캐릭터들이 번갈아 두둥실거린다
    lv_timer_t* scene_bob_timer_ = nullptr;
    bool scene_bob_up_ = false;
    int rozzi_y_ = 0;
    // 걷기 좌표(walk_x_min_/max_·walk_y_·walk_y_ground_)와 몸 배율을 지금 캐릭터 것으로.
    // 생성자와 SetCharacter 가 같이 쓴다 — 좌표 식이 두 벌로 갈리지 않게.
    void ApplyCharacterGeometry();
    int walk_scale_ = LV_SCALE_NONE;   // 대기 화면 몸 배율 (팩 pack.json scale)
    // 자리 소품(docs/character-pack.md "seats"): 배경(sky_)마다 몸 뒤에 깔리는 lv_image 하나 + raise 만큼
    // 몸 올림(walk_y_ = walk_y_ground_ - raise). 제자리에 앉는(ride 있는) 팩만. 미니카 중엔 자리 없음.
    lv_obj_t* seat_img_ = nullptr;
    lv_obj_t* rozzi_img_ = nullptr;            // 로나 옆에 앉는 로찌(장면에서만)
    lv_obj_t* prop_img_ = nullptr;             // 같이 노는 소품(공·모닥불·축음기) — 캐릭터 뒤
    lv_obj_t* front_img_ = nullptr;            // 캐릭터 앞에 놓이는 소품·앞 탁자(샐러드)
    int walk_y_ground_ = 0;                    // 바닥 기준 프레임 y(스케일 전, 발 밑변 = 길 아랫선)
    const PackSeat* seat() const;              // 지금 하늘의 자리. 없거나 ride 중·비 idle 이면 nullptr
    void ApplySeat(int body_x, bool show);     // walk_y_ 갱신 + seat_img_ 를 body_x 기준에 두거나 숨김
    void RefreshSeat();                        // 하늘이 바뀌었을 때: 앉아 있으면 자리·몸 높이를 새 하늘로
    // 미니카 외출(09-06): 실내(room·cafe·library)에서 출발하면 시각에 맞는 마을로 나갔다가 끝나면 돌아온다.
    UsageSky ride_return_sky_ = kUsageSkyCount;
    void ApplyPackSet();                       // 세트 바뀜: 지금 보이는 프레임을 새 세트로
    // 팩 프레임 한 장을 몸에 물리고 옷장 아이템을 프레임 오프셋만큼 옮긴다(포즈 컷은 아이템 숨김).
    void ShowPackFrame(Figure* fig, const PackAnim& anim, uint8_t i);
    // 상태 화면 캐릭터 — 대기 화면 figure_ 와 같은 Figure 를 상태 배율로. 아이템 오버레이가 같이 간다.
    std::unique_ptr<Figure> state_figure_;
    void BeginPackReaction(const PackAnim* anim, uint32_t ms);
    const PackAnim* ReactionAnim(PackReaction r) const;
    TouchAction OnPackBodyTap(int x, int y);
    static void OnPackStateFrame(lv_timer_t* timer);
    void StopPackState();
    uint8_t pack_set_ = 0;
    const PackAnim* pack_state_anim_ = nullptr;   // 상태 화면에서 도는 애니
    lv_timer_t* pack_state_timer_ = nullptr;
    uint8_t pack_state_frame_ = 0;
    const PackAnim* pack_react_anim_ = nullptr;   // 대기 화면 터치 반응 애니
    uint8_t pack_react_frame_ = 0;
    uint32_t pack_react_left_ms_ = 0;

    // --- 에이전트 배지 ---
    // 배지 그림·표시·위치(대기 = 이름 칸 앞, 상태 = 칩 앞) + 이름 라벨/칩 밀기.
    void UpdateBadge();
    void ShowAgentToast(const char* text);
    static void OnAgentToastDone(lv_timer_t* timer);
    void ShowAgentPop(UsageAgent agent);          // 배지 탭 → 화면 가운데 큰 이름 (09-12, 데모 피드백 "전환 티가 안 남")
    static void OnAgentPopDone(lv_timer_t* timer);

    // --- 콕 찌르기 ---
    bool HitPokeButton(int x, int y) const;
    void OpenPokePanel();
    void ClosePokePanel();
    TouchAction OnPokePanelTouch(const Display::TouchEvent& ev);
    static void OnPokePanelTimeout(lv_timer_t* timer);
    static void OnPokePopDone(lv_timer_t* timer);
    static void OnPokeReadDone(lv_timer_t* timer);
    void StartPokePopTimer(uint32_t ms);
    void LayoutPokeReplies(uint8_t phrase);   // 받은 문구의 답장 3칸 그림·자리
    void SetPokeReplies(bool open);
    TouchAction OnPokeRepliesTouch(const Display::TouchEvent& ev);

    lv_obj_t* panel_ = nullptr;

    // --- 대기 화면: 한도 블록 두 개 + 아래의 캐릭터 ---
    lv_obj_t* limit_plates_[kLimitBlocks] = {};  // 글자 뒤 어두운 판
    lv_obj_t* limit_bar_bg_[kLimitBlocks] = {};  // 판 아래쪽 얇은 막대(빈 쪽)
    lv_obj_t* limit_bar_fill_[kLimitBlocks] = {};  // 막대의 찬 쪽. 길이 = 보이는 % 숫자
    int limit_bar_pct_[kLimitBlocks] = {};
    lv_obj_t* name_labels_[kLimitBlocks] = {};   // "WEEK"
    lv_obj_t* pct_labels_[kLimitBlocks] = {};    // "42%"
    lv_obj_t* left_labels_[kLimitBlocks] = {};   // "4d" — 남은 시간 첫 낱말
    lv_obj_t* left2_labels_[kLimitBlocks] = {};  // "7h" — 나머지. 빈칸 글자(8px) 대신 좁은 틈으로 띄우려고 나눈다
    uint8_t visible_cells_ = 0;  // SetStats 로 들어온 칸 수
    bool limit_present_[kLimitBlocks] = {};   // 값이 온 한도 칸만 그린다(SetStats)
    bool idle_visible_ = false;               // ShowIdle 의 마지막 값

    // --- 윗줄 시계 칸: 「PM 2:05 (날씨) 11°」. 판 안 글자는 판의 자식이라 판만 숨기면 된다 ---
    lv_obj_t* clock_plate_ = nullptr;
    lv_obj_t* clock_ampm_ = nullptr;     // "AM"/"PM"
    lv_obj_t* clock_time_ = nullptr;     // "2:05"
    lv_obj_t* clock_icon_ = nullptr;     // 12×12 날씨 그림
    lv_obj_t* clock_temp_ = nullptr;     // "11"
    lv_obj_t* clock_deg_ = nullptr;      // 픽셀 폰트에 ° 가 없어 작은 네모 고리로 그린다
    bool clock_valid_ = false;           // 시간이 맞은 뒤 첫 SetClock 부터
    int clock_hour_ = -1, clock_minute_ = -1;
    UsageWeatherIcon weather_icon_ = kUsageWeatherNone;
    bool weather_has_temp_ = false;

    // 대기 화면 배경과 캐릭터. 미니카처럼 x 를 옮길 때는 lv_anim 이 컨테이너를 옮긴다.
    lv_obj_t* bg_image_ = nullptr;      // 장면 배경 (RGB565, 대기 화면에서만)
    UsageSky sky_ = kUsageSkyBrown;
    SkyCanvas sky_canvas_;
    std::unique_ptr<Figure> figure_;   // 몸 + 옷 슬롯. 걷기 anim 은 이 컨테이너를 옮긴다
    lv_obj_t* hanger_ = nullptr;           // 우하단 옷걸이 아이콘 (옷장 진입)
    Trip trip_ = Trip::kNone;
    uint8_t trip_away_mask_ = 0;    // 내 보드에서 나간(나가는) 캐릭터
    uint8_t trip_guest_mask_ = 0;   // 내 보드에 온 손님
    lv_obj_t* cast_img_[3] = {};
    const lv_image_dsc_t* cast_src_[3] = {};
    bool cast_visible_ = false;     // 옷장·꺼진 화면에서는 숨긴다
    uint8_t cast_react_pose_[3] = {};
    uint32_t cast_react_start_[3] = {};
    uint32_t cast_react_ms_[3] = {};
    lv_timer_t* cast_react_timer_ = nullptr;
    int8_t cast_tap_who_ = -1;      // 연타는 한 캐릭터를 잇달아 누를 때만 센다
    lv_obj_t* trip_img_ = nullptr;         // 움직이는 몸 한 장. 나가는 내 캐릭터도 손님도 이것으로 그린다
    lv_obj_t* trip_rozzi_img_ = nullptr;   // 로나와 같이 움직이는 로찌
    lv_obj_t* trip_label_ = nullptr;       // 「놀러 갔어요」
    const lv_image_dsc_t* trip_scene_body_ = nullptr;   // 앉은 손님의 장면 자세(같은 활동)
    const lv_image_dsc_t* trip_rozzi_ = nullptr;        // 이번 놀러가기에 같이 움직이는 로찌 그림
    int trip_rozzi_dx_ = 0;                // 몸 왼쪽에서 로찌 왼쪽까지(화면 px)
    int trip_rozzi_dy_ = 0;   // 로찌 발이 손님(로나) 발보다 얼마나 아래인가
    lv_timer_t* trip_timer_ = nullptr;     // 놀러가기가 진행 중일 때만 산다
    const PackAnim* trip_anim_ = nullptr;
    const PackAnim* trip_guest_anim_ = nullptr;
    const PackAnim* trip_greet_anim_ = nullptr;
    const lv_image_dsc_t* trip_src_ = nullptr;
    uint32_t trip_t_ms_ = 0;               // 지금 단계에 들어온 뒤 지난 시간
    uint32_t trip_last_tick_ = 0;
    uint32_t trip_anim_until_ms_ = 0;      // 0 이 아니면 이 시각에 대기 그림으로 돌아간다
    uint16_t trip_scale_ = LV_SCALE_NONE;
    int trip_x_ = 0, trip_y_ = 0, trip_home_x_ = 0;   // 프레임 좌상단, 화면 px
    lv_obj_t* poke_btn_ = nullptr;         // 좌하단 하트 (콕 찌르기). 옷걸이와 같이 보이고 숨는다

    // 에이전트 배지(docs/agent-badge.md). 생성자 맨 끝에 만들어 z 가 가장 위다 —
    // lv_layer_top 에 얹으면 GET /screen 덤프에 안 찍힌다. 로고 그림 대신 글자(CC/CX)다.
    lv_obj_t* badge_ = nullptr;         // 배지 글자
    lv_obj_t* badge_plate_ = nullptr;   // 배지 뒤 어두운 판 — 어느 장면에서나 버튼으로 보이게
    lv_obj_t* toast_ = nullptr;
    lv_timer_t* toast_timer_ = nullptr;
    lv_obj_t* agent_pop_label_ = nullptr;  // 전환 팝: 에이전트 이름 (press_start_24)
    lv_timer_t* agent_pop_timer_ = nullptr;
    lv_obj_t* poke_panel_ = nullptr;       // 문구 고르는 패널. 줄 판정은 좌표로 한다(OnPokePanelTouch)
    lv_timer_t* poke_panel_timer_ = nullptr;
    bool poke_panel_open_ = false;
    int poke_panel_x_ = 0, poke_panel_y_ = 0, poke_panel_w_ = 0;
    uint8_t poke_phrase_ = 0;
    int poke_left_w_ = 0;                  // 왼쪽 칸(문구) 폭. 그 오른쪽은 놀러가기 칸
    lv_obj_t* poke_trip_rows_[3] = {};     // 놀러가기·놀러오기·집으로 가기. 못 누르면 흐리게
    lv_obj_t* poke_phrase_rows_[5] = {};   // 문구 5줄(육아일기 줄이 생기면 한 줄 내려간다)
    lv_obj_t* poke_divider_ = nullptr;
    lv_obj_t* poke_diary_row_ = nullptr;   // 맨 위 「육아일기 읽기」(일기를 받은 보드만)
    lv_obj_t* poke_diary_sep_ = nullptr;
    int poke_top_h_ = 0;                   // 육아일기 줄 높이(0 이면 없음)
    int poke_base_h_ = 0;                  // 문구 5줄 패널 높이
    bool diary_available_ = false;
    bool manbok_menu_ = false;
    int diary_open_entry_ = -1;
    lv_obj_t* diary_toast_ = nullptr;      // 새 일기 알림(한글 글꼴)
    lv_timer_t* diary_toast_timer_ = nullptr;
    void LayoutPokePanel();
    static void OnDiaryToastDone(lv_timer_t* timer);
    bool poke_trip_on_[3] = {};
    lv_obj_t* poke_pop_ = nullptr;         // 받은 쪽 팝: 하트 4×
    lv_obj_t* poke_pop_text_ = nullptr;    // 받은 쪽 팝: 문구 2×
    lv_timer_t* poke_pop_timer_ = nullptr;
    lv_obj_t* poke_reply_btn_[UI_POKE_REPLIES_PER_PHRASE] = {};   // 문구 아래 답장 버튼 3칸
    lv_area_t poke_reply_hit_[UI_POKE_REPLIES_PER_PHRASE] = {};
    uint8_t poke_reply_base_ = 0;          // 지금 뜬 문구의 답장 번호 시작(문구 2 → 3, 첫 칸이 4)
    bool poke_replies_open_ = false;
    uint8_t poke_reply_ = 0;
    lv_obj_t* poke_read_ = nullptr;        // 보낸 쪽 「읽음」
    lv_timer_t* poke_read_timer_ = nullptr;
    UsageAgent agent_ = kUsageAgentClaude;       // 선택 (NVS) = 배지
    UsageAgent badge_shown_ = kUsageAgentCount;  // 지금 그려진 배지 (로그 중복 방지, Count = 아직 없음)
    int pad_x_ = 0, pad_y_ = 0;
    // 에이전트별 마지막 사용량. 배지를 누르면 맥을 기다리지 않고 바로 이 값으로 바꾼다.
    UsageStats agent_stats_[kUsageAgentCount];
    bool agent_stats_valid_[kUsageAgentCount] = {};
    lv_timer_t* walk_leg_timer_ = nullptr; // 대기 애니 프레임 전환 (팩 manifest fps)
    uint8_t walk_leg_frame_ = 0;
    // 우주 두둥실 부유 위상 (WalkLift 가 굴린다)
    uint16_t walk_float_t_ = 0;
    bool walk_right_ = true;
    // 옷장 열림·화면 끄기 동안 true (PauseIdleWalk). ShowIdle(true)/stats 푸시가
    // 가려진 대기 애니를 되살리지 않게 하는 래치.
    bool idle_paused_ = false;
    int walk_x_min_ = 0, walk_x_max_ = 0, walk_y_ = 0;

    // 장면 스프라이트(배경의 움직이는 조각).
    static constexpr int kMaxSceneSprites = 8;
    lv_obj_t* sprite_objs_[kMaxSceneSprites] = {};
    const SceneSprite* sprite_specs_[kMaxSceneSprites] = {};
    uint16_t sprite_acc_[kMaxSceneSprites] = {};
    uint8_t sprite_frame_[kMaxSceneSprites] = {};
    int sprite_count_ = 0;
    lv_timer_t* sprite_timer_ = nullptr;

    // 파티클: 비/눈(호스트 날씨 or WX 선택) + 벚꽃잎(sakura, WX=auto 일 때 상시)
    // + 낙엽·별똥별(WX 선택 전용). 별똥별은 speed<0 을 재등장 대기 카운터로 쓴다.
    static constexpr int kMaxDrops = 48;
    lv_obj_t* drops_[kMaxDrops] = {};
    int16_t drop_speed_[kMaxDrops] = {};
    // 좌표는 위젯에서 되읽지 않고 여기 둔다 — 숨겨진 동안 lv_obj_get_x/y 가
    // 레이아웃 미계산으로 0 을 돌려줘 같은 속도끼리 한 줄로 뭉친다.
    int16_t drop_x_[kMaxDrops] = {};
    int16_t drop_y_[kMaxDrops] = {};
    uint8_t drop_kind_[kMaxDrops] = {};    // 1=비 2=눈 3=꽃잎 4=낙엽 5=별똥별
    int drop_count_ = 0;
    lv_timer_t* drop_timer_ = nullptr;
    uint8_t weather_fx_ = 0;               // push 발 (0/1/2)
    uint8_t wx_mode_ = 0;                  // 옷장 WX 탭 (0=auto)

    // 터치 반응 상태. 타이머가 끝나면 멈춘 자리에서 대기 애니로 돌아간다.
    Reaction reaction_ = Reaction::kNone;
    lv_timer_t* reaction_timer_ = nullptr;

    const lv_image_dsc_t* special_bg_ = nullptr;   // 기념일 배경(assets). 없으면 평소 배경
    UsageTicker ticker_;
    uint32_t pack_tap_ms_[5] = {};   // 팩 몸통 최근 탭 시각 — 삐짐(3초 3번)·어지러움(4.5초 5번)

    // --- 상태 화면 ---
    UsageState state_ = kUsageStateIdle;
    int state_img_x_ = 0, state_img_y_ = 0;         // 상태 그림의 화면 위치
    lv_obj_t* chip_labels_[kLimitBlocks] = {};      // 상단 사용량 칩 (5H/WEEK)
    lv_obj_t* ctx_label_ = nullptr;                 // 좌하: 지금 하는 일
    lv_obj_t* state_label_ = nullptr;               // 우하: 상태 이름
    // SetStats 가 캐시한 칩 글자. name(12)+공백+left(16) 이 잘리지 않는 크기 —
    // 모자라면 -Werror=format-truncation 이 빌드를 깬다.
    char chip_text_[kLimitBlocks][32] = {};
    int width_ = 0, height_ = 0;
};
