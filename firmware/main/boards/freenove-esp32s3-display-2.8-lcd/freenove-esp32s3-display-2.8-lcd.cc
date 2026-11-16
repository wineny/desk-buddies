#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <wifi_station.h>
#include "wifi_board.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "adc_battery_monitor.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include "led/single_led.h"
#include "system_reset.h"
#include "settings.h"
#include "esp_lcd_ili9341.h"

#define TAG "FreenoveESP32S3Display"

class TouchDriver {
public:
    TouchDriver() : dev_(nullptr) {}

    bool Init(i2c_master_bus_handle_t bus, uint8_t addr) {
        i2c_device_config_t cfg = {
            .device_address = addr,
            .scl_speed_hz = 400000,
            .scl_wait_us = 0,
        };
        return i2c_master_bus_add_device(bus, &cfg, &dev_) == ESP_OK;
    }

    bool Read(bool &touched, uint16_t &x, uint16_t &y) {
        touched = false;
        x = y = 0;
        if (!dev_) return false;

        uint8_t reg = 0x02;
        uint8_t buf[5];
        if (i2c_master_transmit_receive(dev_, &reg, 1, buf, 5, 50) != ESP_OK) return false;

        uint8_t points = buf[0] & 0x0F;
        if (points == 0) return true;

        touched = true;
        x = ((buf[1] & 0x0F) << 8) | buf[2];
        y = ((buf[3] & 0x0F) << 8) | buf[4];
        return true;
    }

private:
    i2c_master_dev_handle_t dev_;
};

class FreenoveESP32S3Display : public WifiBoard {
private:
    Button boot_button_;
    LcdDisplay *display_;
    i2c_master_bus_handle_t codec_i2c_bus_;
    TouchDriver touch_;
    AdcBatteryMonitor* adc_battery_monitor_;

    void InitializeBatteryMonitor() {
        adc_battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1, ADC_CHANNEL_8, 200000, 200000, GPIO_NUM_NC);
    }

    // FT6336 원시 좌표 → 화면 좌표. 원시 x 가 짧은 축(0~239), y 가 긴 축(0~319)이라
    // 축을 바꾸고, 바꾼 뒤 y 는 뒤집혀 있다(2026-08-29 네 모서리 실측: 좌상이
    // (21,196), 우하가 (313,38) 로 찍혔다). 옷장·놀람 히트박스가 전부 이 좌표를 쓴다.
    static constexpr bool kTouchSwapXY = true;
    static constexpr bool kTouchMirrorX = false;
    static constexpr bool kTouchMirrorY = true;

    // 보드를 거꾸로 세워 쓸 때 화면과 터치를 함께 뒤집는다. NVS wardrobe/flip180, POST {"flip180":1}.
    static inline bool flip180_ = false;

    static void MapTouch(uint16_t rx, uint16_t ry, uint16_t &x, uint16_t &y) {
        int mx = kTouchSwapXY ? ry : rx;
        int my = kTouchSwapXY ? rx : ry;
        if (kTouchMirrorX) mx = DISPLAY_WIDTH - 1 - mx;
        if (kTouchMirrorY) my = DISPLAY_HEIGHT - 1 - my;
        mx = mx < 0 ? 0 : (mx >= DISPLAY_WIDTH ? DISPLAY_WIDTH - 1 : mx);
        my = my < 0 ? 0 : (my >= DISPLAY_HEIGHT ? DISPLAY_HEIGHT - 1 : my);
        if (flip180_) {
            mx = DISPLAY_WIDTH - 1 - mx;
            my = DISPLAY_HEIGHT - 1 - my;
        }
        x = static_cast<uint16_t>(mx);
        y = static_cast<uint16_t>(my);
    }

    // 제스처 판정(docs/wardrobe-impl.md 3절). 50ms 폴링이라 시간 해상도는 50ms.
    static constexpr int kTouchMovePx = 10;      // 이 미만이면 제자리
    static constexpr int kTouchSwipePx = 24;     // 이 이상 움직여 떼면 스와이프
    static constexpr uint32_t kTouchHoldMs = 800;
    static constexpr uint32_t kTouchLongMs = 2000;

    static void TouchTask(void *arg) {
        auto *self = static_cast<FreenoveESP32S3Display*>(arg);
        auto &app = Application::GetInstance();
        using Ev = Display::TouchEvent;

        uint32_t down_start = 0;
        bool down = false;
        // 이번에 누른 터치를 "화면 켜기" 로 써버렸는지. 그랬으면 제스처로 넘기지 않는다.
        bool consumed_to_wake = false;
        uint16_t x0 = 0, y0 = 0, xl = 0, yl = 0;  // 누른 자리 · 마지막 자리
        bool moved = false, hold_sent = false, long_sent = false;
        // 부팅 직후 컨트롤러가 유령 터치(2초 넘게 눌린 것으로 읽힘)를 낸다 —
        // 실기기에서 켜자마자 "롱프레스 화면 끄기"가 발동했다. 부팅 3초가 지나고
        // 한 번이라도 "안 눌림"을 본 뒤부터 받는다.
        constexpr uint32_t kBootGraceMs = 3000;
        bool seen_release = false;

        auto send = [&](Ev::Kind kind, uint32_t press_ms) {
            Ev ev{kind, x0, y0, xl, yl, press_ms};
            self->GetDisplay()->OnTouch(ev);
            // 상태 전환이 아니라 유휴 시계를 직접 되돌린다 — 만지는 동안 자지 않게.
            app.ResetIdleClock();
        };

        while (true) {
            bool t;
            uint16_t rx, ry;
            self->touch_.Read(t, rx, ry);

            uint32_t now = esp_timer_get_time() / 1000;
            if (!seen_release) {
                if (!t && now >= kBootGraceMs) seen_release = true;
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            if (t) {
                uint16_t x, y;
                MapTouch(rx, ry, x, y);
                if (!down) {
                    down = true;
                    down_start = now;
                    x0 = xl = x;
                    y0 = yl = y;
                    moved = hold_sent = long_sent = false;
                    ESP_LOGI(TAG, "touch down raw=(%u,%u) -> (%u,%u)", rx, ry, x, y);
                    // 화면이 꺼져 있었다면 이 터치는 켜는 데만 쓴다. 숫자만
                    // 확인하려던 사람이 의도치 않게 뭔가 누르는 일을 막는다.
                    consumed_to_wake = self->GetDisplay()->WakeIfScreenOff();
                    if (consumed_to_wake) {
                        // 안 그러면 다음 1초 틱에서 바로 다시 꺼진다.
                        app.ResetIdleClock();
                    }
                } else {
                    xl = x;
                    yl = y;
                    const int dx = static_cast<int>(x) - x0, dy = static_cast<int>(y) - y0;
                    if (dx * dx + dy * dy >= kTouchMovePx * kTouchMovePx) moved = true;
                }
                // 누르고 있는 동안 한 번씩: 홀드(쓰다듬기) → 롱프레스(화면 끄기)
                if (!consumed_to_wake && !moved) {
                    const uint32_t press = now - down_start;
                    if (!hold_sent && press >= kTouchHoldMs) {
                        hold_sent = true;
                        send(Ev::kHold, press);
                    }
                    if (!long_sent && press >= kTouchLongMs) {
                        long_sent = true;
                        send(Ev::kLongPress, press);
                    }
                }
            }

            if (!t && down) {
                down = false;

                if (consumed_to_wake) {
                    consumed_to_wake = false;
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }

                const uint32_t press = now - down_start;
                const int dx = static_cast<int>(xl) - x0, dy = static_cast<int>(yl) - y0;
                // 롱탭으로 WiFi 설정 모드에 들어가던 것은 뺐다 — 3초 누르면
                // WiFi 가 끊기는 사고만 났다. 설정 모드는 연결 실패 폴백과
                // 부팅 중 BOOT 버튼으로만 들어간다.
                if (moved) {
                    if (dx >= kTouchSwipePx || dx <= -kTouchSwipePx || dy >= kTouchSwipePx || dy <= -kTouchSwipePx) {
                        const bool horiz = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy);
                        send(horiz ? (dx < 0 ? Ev::kSwipeLeft : Ev::kSwipeRight)
                                   : (dy < 0 ? Ev::kSwipeUp : Ev::kSwipeDown), press);
                    }
                } else if (!hold_sent) {
                    // 짧은 탭. 홀드·롱프레스는 누르는 동안 이미 보냈으니 뗄 때는 조용히.
                    send(Ev::kTap, press);
                }
            }

            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    void InitializeTouch() {
        if (!touch_.Init(codec_i2c_bus_, 0x38)) return;
        xTaskCreatePinnedToCore(TouchTask, "touch_task", 4096, this, 5, nullptr, 0);
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = AUDIO_CODEC_I2C_NUM,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = DISPLAY_MIS0_PIN;
        buscfg.sclk_io_num = DISPLAY_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            // 부팅이 끝난 뒤의 BOOT 는 옷장 열기/닫기 (docs/wardrobe.md)
            if (display_ != nullptr) {
                display_->ToggleWardrobe();
            }
        });
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        ESP_LOGI(TAG, "Install LCD driver ILI9341");
        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        {
            Settings settings("wardrobe");
            flip180_ = settings.GetBool("flip180", false);
        }
        const bool mirror_x = DISPLAY_MIRROR_X != flip180_;
        const bool mirror_y = DISPLAY_MIRROR_Y != flip180_;
        esp_lcd_panel_mirror(panel, mirror_x, mirror_y);
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            mirror_x, mirror_y, DISPLAY_SWAP_XY);
    }

    void InitializeTools() {
    }

public:
    FreenoveESP32S3Display(): boot_button_(BOOT_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializeBatteryMonitor();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->SetBrightness(100);
    }

    virtual Led *GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual Display *GetDisplay() override { return display_; }

    virtual Backlight *GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        charging = adc_battery_monitor_->IsCharging();
        discharging = adc_battery_monitor_->IsDischarging();
        level = adc_battery_monitor_->GetBatteryLevel();
        return true;
    }
};

DECLARE_BOARD(FreenoveESP32S3Display);
