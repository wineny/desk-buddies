#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <functional>

#include "device_state.h"
#include "device_state_machine.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_CLOCK_TICK           (1 << 1)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 2)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 3)
#define MAIN_EVENT_STATE_CHANGED        (1 << 4)

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }

    // 유휴 시계를 0 으로 되돌린다. 상태 전환은 이미 자동으로 이렇게 하지만,
    // 화면만 깨우는 터치처럼 상태가 바뀌지 않는 활동은 이걸 직접 불러야
    // 한다. 안 그러면 다음 1초 틱에서 곧바로 다시 잠든다.
    void ResetIdleClock() { clock_ticks_ = 0; }

    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message and emotion
     */
    void Alert(const char* status, const char* message, const char* emotion = "");
    void DismissAlert();

    void Reboot();
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    int clock_ticks_ = 0;

    std::function<void(const std::string&)> mcp_broadcast_callback_;

    // Event handlers
    void HandleStateChangedEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
