#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "system_info.h"
#include "usage_receiver.h"
#include "poke_link.h"
#include "notify_sound.h"
#include "weather_fetcher.h"
#include "diary_fetcher.h"

#include <cstdlib>
#include <ctime>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_system.h>

#define TAG "Application"

Application::Application() {
    event_group_ = xEventGroupCreate();

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // NVS 에 남겨 둔 마지막 사용량을 곧바로 되살린다. 호스트가 다음 값을
    // 보내줄 때까지 화면이 비어 있지 않게 하려는 것이다. SetupUI() 가
    // 반환한 뒤라야 한다 — 그 안에서 부르면 DisplayLockGuard 가 재진입된다.
    UsageReceiver::RestoreLastStats();
    NotifySound::GetInstance().Start();   // 콕·손님 알림 소리(코덱 출력만)
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "warning");
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "warning");
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "warning");
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = MAIN_EVENT_SCHEDULE | MAIN_EVENT_CLOCK_TICK |
                                   MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED |
                                   MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

            // 사용량 화면의 유휴 표정(5분)·화면 끔(30분). clock_ticks_ 는
            // HandleStateChangedEvent() 가 상태 전환마다 0 으로 되돌리므로,
            // 대기 상태일 때만 그대로 넘기면 "말을 걸면 깨어난다" 가 별도
            // 배선 없이 성립한다. 화면이 없는 보드에서는 기본 구현이 no-op.
            display->OnIdleTick(GetDeviceState() == kDeviceStateIdle ? clock_ticks_ : 0);

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    // 호스트가 사용량을 밀어 넣을 수 있게 수신구를 연다. 두 번 불려도 안전하다.
    UsageReceiver::GetInstance().Start();
    PokeLink::GetInstance().Start();
    WeatherFetcher::GetInstance().Start();   // 맥 없이도 시계 칸 날씨·날씨 옷 (30분마다)
    DiaryFetcher::GetInstance().Start();     // 만복이 육아일기 (10분마다, 열쇠가 있을 때만)

    // 상태바 시계는 예전에 OTA 서버가 내려주는 시간으로 맞췄다. 서버 대화를
    // 걷어냈으므로 SNTP 로 대신한다. 시간이 맞기 전에는 lvgl_display 의
    // tm_year 검사가 시계 표시를 막아 준다.
    static bool sntp_started = false;
    if (!sntp_started) {
        sntp_started = true;
        // 시간대. 기본은 UTC 라 시계가 9시간 어긋나고 낮/밤 하늘 전환도 틀어진다.
        setenv("TZ", "KST-9", 1);
        tzset();
        esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&sntp_config);
    }

    auto state = GetDeviceState();
    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // 예전에는 여기서 OTA 확인과 서버 프로토콜 초기화(activation)를
        // 거쳐야만 Idle 이 됐다. 지금은 네트워크만 붙으면 바로 대기 상태다.
        SetDeviceState(kDeviceStateIdle);
        auto display = Board::GetInstance().GetDisplay();
        display->SetChatMessage("system", "");
        // WiFi 절전을 끈다. 원본은 LOW_POWER(MAX_MODEM)인데, 그러면 보드가
        // 브로드캐스트 ARP 를 자주 놓쳐 맥의 ARP 캐시가 만료된 뒤(20분쯤 안
        // 쓰면) 훅 5초 안에 주소가 안 풀려 POST 가 한 시간 넘게 연속 실패했다
        // (2026-08-29). USB 상시 전원이라 절전으로 얻는 게 없다.
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    // 화면이 꺼져 있거나 표정 모드였다면 여기서 즉시 깨운다. 1초 틱만
    // 믿으면 최대 1초 늦게 반응한다.
    display->OnIdleTick(0);

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();    // Clear messages first
            display->SetEmotion("neutral");  // Then set emotion (wechat mode checks child count)
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::CanEnterSleepMode() { return GetDeviceState() == kDeviceStateIdle; }

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload]() {
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}
