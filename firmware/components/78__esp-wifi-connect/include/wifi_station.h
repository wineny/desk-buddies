#ifndef _WIFI_STATION_H_
#define _WIFI_STATION_H_

#include <string>
#include <vector>
#include <functional>

#include <esp_event.h>
#include <esp_timer.h>
#include "ssid_manager.h"
#include <mutex>
#include <deque>
#include <esp_netif.h>
#include <esp_wifi_types_generic.h>

// WiFi power save level enumeration
enum class WifiPowerSaveLevel {
    LOW_POWER,    // Maximum power saving (WIFI_PS_MAX_MODEM)
    BALANCED,     // Minimum power saving (WIFI_PS_MIN_MODEM)
    PERFORMANCE,  // No power saving (WIFI_PS_NONE) - full power
};

struct WifiApRecord {
    std::string ssid;
    std::string password;
    int channel;
    wifi_auth_mode_t authmode;
    uint8_t bssid[6];
};

/**
 * WifiStation - WiFi station mode handler
 * 
 * This class handles connecting to WiFi access points in station mode.
 * Note: WiFi driver must be initialized before using this class.
 */
class WifiStation {
public:
    WifiStation();
    ~WifiStation();

    // Delete copy constructor and assignment operator
    WifiStation(const WifiStation&) = delete;
    WifiStation& operator=(const WifiStation&) = delete;

    void AddAuth(const std::string &&ssid, const std::string &&password);
    void Start();
    void Stop();
    bool IsConnected();
    bool WaitForConnected(int timeout_ms = 10000);
    int8_t GetRssi();
    std::string GetSsid() const { return ssid_; }
    std::string GetIpAddress() const { return ip_address_; }
    uint8_t GetChannel();
    void SetPowerSaveLevel(WifiPowerSaveLevel level);

    void OnConnect(std::function<void(const std::string& ssid)> on_connect);
    void OnConnected(std::function<void(const std::string& ssid)> on_connected);
    void OnDisconnected(std::function<void(int reason)> on_disconnected);
    void OnScanBegin(std::function<void()> on_scan_begin);
    void SetScanIntervalRange(int min_interval_seconds, int max_interval_seconds);
    void SetHostname(const std::string& hostname) { hostname_ = hostname; }
    // 클로드봇: 연결된 채로 저장된 다른 와이파이로 옮긴다(시험용). 와이파이 이벤트 작업에 넘겨 순서대로 돈다.
    // 못 붙으면 원래 와이파이로 돌아간다. 넘기기에 성공하면 true(실제로 옮겼는지는 GET /wifi 기록).
    bool SwitchTo(const std::string& ssid);
    // 클로드봇: 와이파이 기록(최근 30줄)과 지금 상태를 JSON 으로. GET /wifi 에서 쓴다.
    std::string DebugJson();

    // How many times to retry the strongest same-SSID AP before falling back to
    // a weaker one. Only effective when remember_bssid is off (default). A value
    // of 3 means the driver will attempt the best AP up to 3 extra times before
    // giving up and trying the next-best BSSID.
    void SetFailureRetryCnt(uint8_t cnt) { failure_retry_cnt_ = cnt; }

private:
    EventGroupHandle_t event_group_;
    esp_timer_handle_t timer_handle_ = nullptr;
    esp_event_handler_instance_t instance_any_id_ = nullptr;
    esp_event_handler_instance_t instance_got_ip_ = nullptr;
    esp_event_handler_instance_t instance_cmd_ = nullptr;   // 클로드봇: SwitchTo 를 이벤트 작업에서 돌린다
    bool DoSwitchTo(const std::string& ssid);
    esp_netif_t* station_netif_ = nullptr;
    std::string ssid_;
    std::string password_;
    std::string ip_address_;
    std::string hostname_;
    int8_t max_tx_power_;
    uint8_t remember_bssid_;
    uint8_t failure_retry_cnt_ = 3;  // Retries on strongest AP before falling back
    int reconnect_count_ = 0;

    // Exponential backoff for scan interval
    int scan_min_interval_microseconds_ = 10 * 1000 * 1000;   // Default 10 seconds
    int scan_max_interval_microseconds_ = 300 * 1000 * 1000;  // Default 5 minutes
    int scan_current_interval_microseconds_ = 10 * 1000 * 1000;  // Current interval
    std::function<void(const std::string& ssid)> on_connect_;
    std::function<void(const std::string& ssid)> on_connected_;
    std::function<void(int reason)> on_disconnected_;
    std::function<void()> on_scan_begin_;
    std::vector<WifiApRecord> connect_queue_;
    bool was_connected_ = false;  // Track if we were connected before disconnection

    // 클로드봇: 우선순위가 낮은 와이파이(핫스팟)에 붙어 있으면 가끔 둘러보고, 앞 순위가
    // 보이면 그쪽으로 옮긴다. 우선순위 = SsidManager 목록 순서(0 이 가장 앞).
    static constexpr int64_t kRoamScanIntervalUs = 90LL * 1000 * 1000;
    static constexpr int64_t kRoamMaxIntervalUs = 30LL * 60 * 1000 * 1000;   // 옮기기가 계속 실패하면 30분까지 늘린다
    static constexpr int8_t kRoamMinRssi = -75;   // 이보다 약하면 옮기지 않는다
    esp_timer_handle_t roam_timer_ = nullptr;
    int64_t roam_interval_us_ = kRoamScanIntervalUs;
    bool roam_scanning_ = false;   // 연결된 채로 둘러보는 중
    bool roaming_ = false;         // 앞 순위로 옮기려고 일부러 끊는 중
    bool roam_attempt_ = false;    // 옮기기를 시도했다(다음에 붙은 곳으로 성공·실패를 판단)
    bool roam_notify_ = false;     // 옮기다 실패하면 앱에 끊김을 한 번 알린다
    int connected_rank_ = -1;      // 지금 붙은 와이파이의 순위(-1 = 모름)
    // 순위: 저장 순서. 핫스팟 표시가 있으면 kHotspotRank 를 더해 항상 뒤로 간다. -1 = 저장 안 됨.
    static constexpr int kHotspotRank = 100;
    static int RankOf(const std::vector<SsidItem>& list, const std::string& ssid);
    void BeginRoam(std::vector<WifiApRecord>&& queue, bool upward);   // upward = 핫스팟에서 앞 순위로
    // 다시 붙은 직후 「내 주소는 여기」(gratuitous ARP)를 몇 번 바로 보낸다. 안 보내면 맥이 보드를
    // 응답 없는 기기로 묶어 둔 채 lwIP 의 60초 주기 알림이 올 때까지 1~2분 못 찾았다(폰은 13초, 11/7 실측).
    esp_timer_handle_t garp_timer_ = nullptr;
    int garp_left_ = 0;
    void SendGarp();
    std::mutex note_mutex_;
    std::deque<std::string> notes_;
    void Note(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void HandleRoamScan();

    void HandleScanResult();
    void StartConnect();
    void UpdateScanInterval();  // Exponential backoff for scan interval
    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
};

#endif // _WIFI_STATION_H_
