#ifndef DISPLAY_H
#define DISPLAY_H

#include "emoji_collection.h"
#include "text_glyph.h"

#ifndef CONFIG_USE_EMOTE_MESSAGE_STYLE
#define HAVE_LVGL 1
#include <lvgl.h>
#endif

#include <esp_log.h>
#include <esp_pm.h>
#include <esp_timer.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

class Theme {
public:
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    inline std::string name() const { return name_; }

private:
    std::string name_;
};

class Display {
public:
    Display();
    virtual ~Display();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000);
    virtual void SetEmotion(const char* emotion);
    virtual void SetChatMessage(const char* role, const char* content);
    virtual void ClearChatMessages();
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    virtual bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) { return false; }
    virtual void ClearTextGlyphs() {}
    virtual void SetEmojiCollection(std::shared_ptr<EmojiCollection>) {}
    virtual void SetupUI() { setup_ui_called_ = true; }

    // 1초마다 호출된다. idle_seconds 는 대기 상태로 들어간 뒤 흐른 초이고,
    // 대화 중이거나 다른 상태면 0 이다. 화면을 가진 구현이 유휴 표정·화면
    // 끔을 여기서 처리한다. 기본은 아무것도 하지 않는다.
    virtual void OnIdleTick(int idle_seconds) {}

    // 화면이 꺼져 있었으면 켜고 true 를 돌려준다. true 가 오면 그 입력은
    // 깨우는 데 쓰인 것이므로 원래 동작(대화 시작 등)으로 넘기지 않는다.
    // 화면이 켜져 있었으면 아무것도 하지 않고 false 를 돌려준다.
    // by_touch=false(데이터 수신 등)는 사람이 롱프레스로 끈 화면은 켜지 않는다 —
    // 훅이 몇 분마다 밀어 넣는 POST 가 수동 끔을 풀어 버리지 않게.
    virtual bool WakeIfScreenOff(bool by_touch = true) { return false; }

    // 짧은 탭: 상태 화면이 떠 있으면 잠깐(30초) 대기 화면을 보여준다. 세션이
    // 여러 개라 권한 대기 같은 상태가 화면을 오래 차지할 때 마을·클로드를 보는
    // 용도. 대기 화면이 이미 떠 있으면 아무것도 안 하고 false.
    virtual bool PeekIdleScreen() { return false; }

    // 터치 제스처. 보드의 터치 태스크가 판정해서 넘긴다(docs/touch.md,
    // docs/wardrobe-impl.md 3절). 좌표는 화면 좌표(가로 320×240 기준).
    //   kTap        뗄 때, 이동 < 10px, 누름 < 800ms
    //   kHold       누른 채 800ms 경과 (한 번만) — 쓰다듬기
    //   kLongPress  누른 채 2000ms 경과 (한 번만) — 화면 끄기
    //   kSwipe*     뗄 때, 이동 ≥ 24px
    // 화면이 꺼져 있을 때의 첫 터치는 WakeIfScreenOff() 가 먹고 여기 안 온다.
    struct TouchEvent {
        enum Kind : uint8_t { kTap, kHold, kLongPress, kSwipeLeft, kSwipeRight, kSwipeUp, kSwipeDown };
        Kind kind;
        uint16_t x0, y0;      // 누른 자리
        uint16_t x, y;        // 마지막 자리(뗀 자리)
        uint32_t press_ms;
    };
    virtual void OnTouch(const TouchEvent& ev) {}

    // 시각(HH:MM) 표시. 상태 텍스트와 같은 자리를 쓰던 것을 떼어내기 위한
    // 훅이다. 기본 구현은 예전처럼 상태 텍스트로 보내므로, 따로 처리하지
    // 않는 화면은 동작이 그대로다.
    virtual void SetClock(const char* time_str) { SetStatus(time_str); }

    // WiFi 설정 모드(핫스팟 프로비저닝) 온보딩 가이드. 설정 AP SSID 와 설정
    // 페이지 URL 을 받아 전체 화면 안내를 띄운다. LCD 가 없는 파생은 무시.
    // 레이아웃은 docs/mockups/wifi_guide.html 의 B-2(우측 컬럼) 확정안.
    virtual void ShowWifiConfigGuide(const char* ap_ssid, const char* web_url) {}
    virtual void HideWifiConfigGuide() {}
    // WiFi 가 붙었는데 맥에서 한 번도 사용량이 안 왔을 때의 2단계 안내(맥에서 setup.py 한 줄).
    virtual void ShowMacSetupGuide(const char* board_ip) {}
    virtual void HideMacSetupGuide() {}
    virtual bool MacSetupGuideShown() const { return false; }

    inline int width() const { return width_; }
    inline int height() const { return height_; }
    inline bool IsSetupUICalled() const { return setup_ui_called_; }

protected:
    int width_ = 0;
    int height_ = 0;
    bool setup_ui_called_ = false;  // Track if SetupUI() has been called

    Theme* current_theme_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};

class DisplayLockGuard {
public:
    DisplayLockGuard(Display* display) : display_(display) {
        if (!display_->Lock(30000)) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() { display_->Unlock(); }

private:
    Display* display_;
};

class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override { return true; }
    virtual void Unlock() override {}
};

#endif
