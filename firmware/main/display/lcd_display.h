#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "lvgl_display.h"
#include "usage/usage_screen.h"
#include "usage/wardrobe_screen.h"
#include "usage/weather_panel.h"
#include "usage/diary_panel.h"
#include "usage/manbok_panel.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000

class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles

    // 클로드 사용량 화면. container_ 위에 얹히며, 유휴 표정·화면 끔도 여기서
    // 관리한다. 화면을 안 쓰는 파생 클래스에서는 null 로 남는다.
    std::unique_ptr<UsageScreen> usage_screen_ = nullptr;
    // 상태 화면의 시간 복귀용. 마지막으로 상태를 받고 흐른 초다.
    int usage_state_age_s_ = 0;
    // 탭으로 대기 화면을 엿보는 중이면 남은 초. 그동안 들어온 상태는 화면에
    // 안 그리고 usage_pending_* 에만 적어 뒀다가 끝나면 복원한다.
    int usage_peek_left_s_ = 0;
    UsageState usage_pending_state_ = kUsageStateIdle;
    char usage_pending_ctx_[32] = {};
    // 마지막으로 호스트가 보낸 ctx. 엿보기를 시작할 때 복원용으로 들고 간다.
    char usage_last_ctx_[32] = {};
    // 수면(10분)으로 백라이트를 낮춰 둔 상태. 화면 끔(usage_backlight_off_)과
    // 별개로 관리한다 — 복구는 둘 다 RestoreBrightness() 하나로 된다.
    bool usage_dimmed_ = false;
    // 하늘을 시각으로 고른다(07~19시 낮, 그 외 남색). POST 로 색을 못 박으면
    // 꺼지고, "auto" 를 받으면 다시 켜진다.
    bool usage_sky_auto_ = true;
    // 시계 전용 라벨. 원래는 상태 텍스트(status_label_)와 한 자리를 번갈아
    // 썼는데, 시계는 왼쪽 위 · 상태 텍스트는 화면 아래로 떼어 놓았다.
    lv_obj_t* usage_clock_label_ = nullptr;
    bool usage_backlight_off_ = false;
    // 빈 배경 롱프레스로 사람이 직접 끈 화면. 유휴 시계와 무관하게 다음 터치까지
    // 꺼져 있다 — 훅이 계속 상태를 밀어 넣어 유휴가 안 쌓여도 끌 수 있게.
    bool usage_manual_off_ = false;
    // 옷장(docs/wardrobe.md). 열려 있는 동안만 산다. 옷·잠금은 닫을 때 여기로
    // 옮겨 두고 대기 화면 캐릭터에 입힌다. (NVS 저장은 다음 단계)
    std::unique_ptr<WardrobeScreen> wardrobe_ = nullptr;
    std::unique_ptr<WeatherPanel> weather_panel_ = nullptr;   // 시계·날씨 칸 탭으로 여는 예보 판
    WeatherForecast usage_forecast_;                          // 보드가 받은 마지막 예보 (weather_fetcher.cc)
    std::unique_ptr<DiaryPanel> diary_panel_ = nullptr;       // 만복이 일기 판 (docs/manbok-diary.md)
    DiaryData usage_diary_;                                   // 보드가 받은 마지막 일기 목록 (diary_fetcher.cc)
    std::vector<DiaryPhotoPtr> diary_photos_;                 // 받은 사진 몇 장(판을 다시 열어도 다시 안 받게)
    std::string diary_focus_id_;                              // 흐르는 줄·알림을 누르면 열 글
    std::unique_ptr<ManbokPanel> manbok_panel_ = nullptr;     // 만복이 메뉴 (docs/manbok-diary.md)
    ManbokData usage_manbok_;                                 // 보드가 받은 마지막 세 갈래 (diary_fetcher.cc)
    bool diary_from_menu_ = false;                            // 지금 일기 판은 메뉴에서 열었다(< = 메뉴로)
    // 옷장이 열려 있는 동안 받은 콕의 문구 번호. 닫힐 때 보여 준다. -1 = 없음.
    // 옷장이 열려 있어 못 보여 준 콕. -1 없음, 0~5 문구, 101~103 답장, 200 하이파이브.
    int usage_poke_pending_ = -1;
    void FlushPokePendingLocked();
    bool SendTripLocked();   // 락을 잡은 채 놀러가기를 묻는다(SendTrip·AcceptTripInvite 공용)
    void LoadUsageDatesLocked();
    void UpdateSpecialDayLocked();
    std::string usage_birthday_, usage_birthday2_, usage_anniversary_;
    int usage_special_shown_ = -1;     // 지금 깔린 기념일(UsageSpecialDay). -1 없음
    int usage_special_test_ = -1;      // 시험 주입. -1 이면 실제 날짜로 판정
    int usage_special_skip_day_ = 0;   // 이 날(mday*100+mon)은 옷장에서 고른 배경이 우선
    bool usage_dates_loaded_ = false;
    // 옷장이 마지막으로 닫힌 시각(lv_tick). 닫힌 직후 그레이스 창 동안 대기 화면
    // 터치를 무시한다 — 자동/푸시 닫힘을 모르는 사용자의 탭이 ACT 를 끊지 않게.
    uint32_t wardrobe_closed_tick_ = 0;
    Outfit usage_outfit_;
    bool usage_outfit_locked_ = false;
    // 마지막 날씨 푸시의 강수 (0=없음 1=비 2=눈). AUTO 세트의 강수 우선 판정용.
    uint8_t usage_precip_ = 0;

    // WiFi 설정 모드 온보딩 가이드 패널. 활성 스크린 최상단 전체 화면 오버레이라
    // 사용량 화면·상태바 위에 덮인다. 설정 모드 진입/이탈 때만 산다.
    lv_obj_t* wifi_guide_panel_ = nullptr;
    lv_obj_t* mac_guide_panel_ = nullptr;   // 2단계 「맥 연결」 안내 (ShowMacSetupGuide)
    // 가이드 위 캐릭터 (마중 구도). 해제 순서 중요 — 패널보다 먼저 reset 해야
    // Figure 소멸자가 산 lv_obj 를 지운다.
    std::unique_ptr<Figure> wifi_guide_figure_ = nullptr;

    void InitializeLcdThemes();
    // 옷장 열기/닫기. 둘 다 DisplayLockGuard 안에서 부를 것.
    void OpenWardrobeLocked();
    void CloseWardrobeLocked();
    void OpenWeatherPanelLocked(WeatherPanel::Tab tab);
    void CloseWeatherPanelLocked();
    void OpenDiaryPanelLocked(int entry);   // entry < 0 이면 목록
    void CloseDiaryPanelLocked();
    void OpenManbokPanelLocked(ManbokPanel::Screen screen);
    void CloseManbokPanelLocked();
    // AUTO 세트(docs/alive.md E절): 지금 하늘의 세트를 옷+wx 로 입힌다.
    // LOCK 이면 아무것도 안 한다. RAM 만 — NVS 저장 없음. DisplayLockGuard 안에서.
    void ApplyAutoSetLocked();
    // 지금 착장·하늘·wx·에이전트를 NVS 에 저장. DisplayLockGuard 안에서.
    void SaveOutfitLocked();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
               int height);

public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;

    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);

    // 사용량 숫자 갱신 (3단계에서 호스트가 밀어 넣는 값)
    void SetUsageStats(UsageAgent agent, const UsageStats& stats);

    // 클로드 상태 화면 전환. 호스트가 우선순위 집계를 끝낸 값을 그대로 반영한다.
    // state: idle|thinking|working|juggling|permission|error|done (모르는 값은 무시)
    void SetUsageState(const char* state, const char* ctx);

    // 대기 화면 배경 하늘: auto|brown|navy|day (모르는 값은 무시)
    void SetUsageSky(const char* sky);
    void SetUsageWeatherFx(const char* kind);   // "rain"/"snow" 만 이펙트, 나머지 끔
    // 윗줄 시계 칸의 날씨 그림·기온. kind = clear|partly|cloud|rain|snow, night 면 맑음이 달.
    void SetUsageWeatherInfo(const char* kind, bool has_temp, int temp, bool night);
    // 보드가 받은 예보(시간별·이번 주). 날씨 판이 열려 있으면 다시 그린다. 맥 push 는 부르지 않는다.
    void SetUsageForecast(const WeatherForecast& forecast);
    // 시험 주입 {"weather_panel":0|1|-1}: 0 시간별, 1 이번 주로 열기, -1 닫기. 대기 화면이 아니면 false.
    bool ShowWeatherPanel(int tab);
    // --- 만복이 육아일기 (docs/manbok-diary.md). diary_fetcher.cc 의 태스크가 부른다 ---
    void SetDiary(const DiaryData& data);
    void SetDiaryPhoto(const DiaryPhotoPtr& photo);
    void DiaryPhotoFailed(const std::string& key);   // 받기·풀기 실패 — 판이 다음에 다시 부탁하게
    void ShowDiaryNew(const DiaryEntry& entry);   // 알림 토스트 + 흐르는 줄에 바로
    bool ShowDiaryNewTest();                      // 시험 {"diary_test_new":1}: 맨 위 일기로 알림
    // 시험 {"diary_panel":n}: 0 목록, n>0 n번째 글, -1 닫기. 대기 화면이 아니면 false.
    bool ShowDiaryPanel(int n);
    // --- 만복이 메뉴 (docs/manbok-diary.md) ---
    void SetManbok(const ManbokData& data);        // diary_fetcher 의 태스크가 부른다
    void SetManbokMenuEnabled(bool enabled);       // 열쇠가 있는 보드만 하트 꾹 = 메뉴
    // 시험 {"manbok_menu":1}·{"manbok_panel":"ultrasound"|…}: 대기 화면이 아니면 false. screen < 0 이면 닫기.
    bool ShowManbokPanel(int screen);

    // 1초마다 호출된다. idle_seconds 는 대기 상태로 들어간 뒤 흐른 초이고,
    // 대화 중이거나 다른 상태면 0 이다. 0 이 오면 깨우기로 친다.
    virtual void OnIdleTick(int idle_seconds) override;
    virtual bool WakeIfScreenOff(bool by_touch = true) override;
    virtual bool PeekIdleScreen() override;
    virtual void OnTouch(const TouchEvent& ev) override;
    // BOOT 버튼: 대기 화면이면 옷장 열기/닫기, 화면이 꺼져 있으면 켜기만, 상태
    // 화면이면 30초 엿보기. 아무 태스크에서나 불러도 된다 (안에서 락을 잡는다).
    void ToggleWardrobe();
    // 열린 옷장을 커밋하고 닫는다 (닫혀 있으면 아무 일 없음). 호스트 outfit push 가
    // 부분 갱신을 얹기 전에 부른다 — state push 닫힘과 같은 "고르던 것 커밋" 정책.
    // 아무 태스크에서나 불러도 된다 (안에서 락을 잡는다).
    void CommitWardrobe();

    // 옷 적용 (docs/wardrobe.md ③). locked/sky 는 -1 = 지금 값 유지.
    // persist=true 면 NVS 에 저장한다(호스트 outfit·옷장 닫기). 날씨 자동 옷은
    // persist=false 로 온다 — 재부팅 뒤엔 다음 날씨 푸시가 다시 입힌다.
    // 아무 태스크에서나 불러도 된다 (안에서 락을 잡는다).
    // wx = kWxItems 인덱스 (0=auto). -1 이면 지금 값 유지.
    void SetUsageOutfit(const Outfit& outfit, int locked, int sky, bool persist, int wx = -1);
    Outfit GetUsageOutfit();
    bool IsUsageOutfitLocked();
    // AUTO 세트 적용 (usage_receiver 의 weather 필드). LOCK 이면 무시.
    // 아무 태스크에서나 불러도 된다 (안에서 락을 잡는다).
    void ApplyUsageAutoSet();
    // 행동 주입 — 지금은 {"act":"ride"}(미니카)만. QA·디버그용.
    bool TriggerUsageActivity(const char* key);
    // 콕 찌르기(docs/poke.md). phrase 0 = 문구 없는 콕, 1~5 = 문구. 범위 밖이면 false.
    // 아무 태스크에서나 불러도 된다 (안에서 락).
    // SendPoke: 토스트를 띄우고 PokeLink 큐에 넣는다. 하트 탭·패널·{"poke"} 주입 공용.
    bool SendPoke(int phrase);
    // 보낸 콕의 결과: ACK 를 받았으면 true, 시간 초과면 false.
    void ShowPokeResult(bool delivered);
    // 받은 콕을 화면에 띄운다. 꺼진 화면은 깨우고, 옷장이 열려 있으면 닫힐 때까지 들고 있다.
    bool ShowPokeReceived(int phrase);
    // SendPokeReply: 답장 버튼·{"poke_reply"} 주입 공용. reply 1~15.
    bool SendPokeReply(int reply);
    // ShowPokeReply: 상대가 답했다 — 「읽음」과 답장을 띄운다. reply 1~15.
    bool ShowPokeReply(int reply);
    // ShowPokeHighFive: 10초 안에 서로 콕을 보냈다.
    void ShowPokeHighFive();
    // 보드 놀러가기(docs/trip.md). SendTrip: 패널 줄·{"trip"} 주입 공용. 지금 캐릭터로 상대에게 묻는다.
    bool SendTrip();
    // CanHostTrip: 손님을 받을 수 있나. 꺼진 화면은 깨운 뒤에 본다 — 사람이 보낸 방문이다.
    bool CanHostTrip();
    // ShowTripAnswer: 상대 답. answered=false 면 답이 없었다. 받아 주면 내 캐릭터가 나간다.
    // 실제로 출발했으면 true.
    // value: 1~7 = 건너갈 캐릭터(받아 줌), kTripAnswerBusy, kTripAnswerSame = 이미 다 같이 있음.
    bool ShowTripAnswer(bool answered, uint8_t value);
    // TripNewGuests: 상대가 보낸 캐릭터 중 내 화면에 없는 것. 0 이면 이미 다 같이 있다.
    uint8_t TripNewGuests(uint8_t mask);
    // TripWave: 이미 다 같이 있어서 건너오지 않을 때 그 자리에서 인사한다.
    void TripWave();
    // SetUsageCast: 켜진 캐릭터를 바꾸고 저장한다({"cast":n} 주입).
    bool SetUsageCast(int mask);
    // ShowTripGuest: 손님이 들어온다. character 는 CurrentCharacter() 값. 못 받으면 false.
    bool ShowTripGuest(int character);
    // ShowTripHome: 상대 보드의 손님이 떠났다 — 내 캐릭터가 돌아온다.
    void ShowTripHome();
    // 놀러오기(패널 오른쪽 칸·{"trip_invite"} 주입): 상대 캐릭터를 부른다.
    bool SendTripInvite();
    // AcceptTripInvite: 상대가 불렀다 — 내 캐릭터로 평소 놀러가기를 묻는다. 못 가면 false.
    bool AcceptTripInvite();
    // TripGoHome(패널·{"trip_go_home"} 주입): 손님은 내보내고, 나가 있는 내 캐릭터는 불러온다.
    bool TripGoHome();
    // SendGuestHome: 상대가 자기 캐릭터를 불렀다 — 내 보드의 손님을 내보낸다.
    bool SendGuestHome();
    // 날짜 저장: key = "due"(YYYY-MM-DD)·"birthday"·"birthday2"·"anniversary"(MM-DD). 빈 값은 지우기. 형식이 틀리면 false
    bool SetUsageDate(const char* key, const char* value);
    void SetSpecialDayTest(int kind);   // 시험 주입 {"special":n}
    // 에이전트 선택(docs/agent-badge.md): "claude"|"codex". persist=true 면
    // NVS 저장 + 토스트 (배지 탭과 같은 경로 — {"agent_select"} 주입). 부팅 복원은
    // persist=false. 모르는 키면 false. 아무 태스크에서나 불러도 된다 (안에서 락).
    bool SetUsageAgent(const char* key, bool persist);
    // 지금 선택된 에이전트 — 프로토콜 필터·POST /usage 응답용 (안에서 락). 화면 없으면 claude.
    UsageAgent GetUsageAgent();
    // 캐릭터 팩 착장 세트(docs/character-pack.md): {"set": "<key>"} 주입·부팅 복원.
    // persist=true 면 NVS 저장 + 토스트. 팩 모드가 아니거나 모르는 키면 false.
    // 아무 태스크에서나 불러도 된다 (안에서 락).
    bool SetUsageSet(const char* key, bool persist);
    // 지금 세트 키. 팩 캐릭터가 아니면 nullptr (응답 JSON 이 필드를 뺀다).
    const char* GetUsageSet();
    // 캐릭터 런타임 교체(docs/character-pack.md "런타임 선택"): "pack" | 팩 이름.
    // 두 몸을 다시 만들고 그 캐릭터의 착장(NVS "outfit_c"/"outfit_p")·팩 세트를 다시 입힌다.
    // 옷장이 열려 있으면 먼저 닫는다. persist=true 면 NVS "char" 에 저장.
    // 모르는 키거나 팩이 없는 빌드의 "pack" 이면 false. 아무 태스크에서나 (안에서 락).
    bool SetUsageCharacter(const char* key, bool persist);
    static constexpr uint8_t kTripAnswerBusy = 8;
    static constexpr uint8_t kTripAnswerSame = 9;
    // 같은 것. 이미 DisplayLockGuard 를 쥔 경로(옷장 CHAR 탭)용 — 옷장은 닫힌 뒤여야 한다.
    bool SetUsageCharacterLocked(const char* key, bool persist);
    // 지금 캐릭터 키 — 응답 JSON·QA 용 (안에서 락 없음, 전역 상태 읽기).
    const char* GetUsageCharacter();
    virtual void SetClock(const char* time_str) override;
    virtual void SetStatus(const char* status) override;
    // WiFi 설정 모드 온보딩 가이드. 아무 태스크에서나 불러도 된다 (안에서 락).
    virtual void ShowWifiConfigGuide(const char* ap_ssid, const char* web_url) override;
    virtual void HideWifiConfigGuide() override;
    virtual void ShowMacSetupGuide(const char* board_ip) override;
    virtual void HideMacSetupGuide() override;
    virtual bool MacSetupGuideShown() const override { return mac_guide_panel_ != nullptr; }
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
};

#endif  // LCD_DISPLAY_H
