#include "wifi_station.h"
#include <cstring>
#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_system.h>
#include "ssid_manager.h"
#include <cstdarg>
#include "esp_netif_net_stack.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"

#define TAG "WifiStation"
#define WIFI_EVENT_CONNECTED BIT0
#define WIFI_EVENT_STOPPED BIT1
#define WIFI_EVENT_SCAN_DONE_BIT BIT2
#define MAX_RECONNECT_COUNT 5

// 클로드봇: 시험용 옮기기(SwitchTo)를 와이파이 이벤트 작업에서 돌리려고 쓰는 이벤트
ESP_EVENT_DEFINE_BASE(CLAUDEBOT_WIFI_CMD);
enum { kCmdSwitchTo = 1 };

WifiStation::WifiStation() {
    // Create the event group
    event_group_ = xEventGroupCreate();

    // 读取配置
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        max_tx_power_ = 0;
        remember_bssid_ = 0;
    } else {
        err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
        if (err != ESP_OK) {
            max_tx_power_ = 0;
        }
        err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
        if (err != ESP_OK) {
            remember_bssid_ = 0;
        }
        nvs_close(nvs);
    }
}

WifiStation::~WifiStation() {
    Stop();
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void WifiStation::AddAuth(const std::string &&ssid, const std::string &&password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

void WifiStation::Stop() {
    ESP_LOGI(TAG, "Stopping WiFi station");
    
    // Unregister event handlers FIRST to prevent scan done from triggering connect
    if (instance_any_id_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }
    if (instance_cmd_ != nullptr) {
        esp_event_handler_instance_unregister(CLAUDEBOT_WIFI_CMD, kCmdSwitchTo, instance_cmd_);
        instance_cmd_ = nullptr;
    }

    if (roam_timer_ != nullptr) {
        esp_timer_stop(roam_timer_);
        esp_timer_delete(roam_timer_);
        roam_timer_ = nullptr;
    }
    if (garp_timer_ != nullptr) {
        esp_timer_stop(garp_timer_);
        esp_timer_delete(garp_timer_);
        garp_timer_ = nullptr;
    }
    roaming_ = false;
    roam_scanning_ = false;
    connected_rank_ = -1;

    // Stop timer
    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
    }

    // Now safe to stop scan, disconnect and stop WiFi (no event callbacks will fire)
    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    esp_wifi_stop();

    if (station_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(station_netif_);
        station_netif_ = nullptr;
    }
    
    // Reset was_connected_ flag to prevent stale state from affecting subsequent sessions
    was_connected_ = false;

    // Clear connected bit
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
    
    // Set stopped event AFTER cleanup is complete to unblock WaitForConnected
    // This ensures no race condition with subsequent WiFi operations
    xEventGroupSetBits(event_group_, WIFI_EVENT_STOPPED);
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}

void WifiStation::OnDisconnected(std::function<void(int reason)> on_disconnected) {
    on_disconnected_ = on_disconnected;
}

void WifiStation::Start() {
    // Note: esp_netif_init() and esp_wifi_init() should be called once before calling this method
    // WiFi driver is initialized by WifiManager::Initialize() and kept alive
    
    // Clear stopped event bit so WaitForConnected works properly
    // Clear scan done bit so Stop() can wait for scan to complete
    xEventGroupClearBits(event_group_, WIFI_EVENT_STOPPED | WIFI_EVENT_SCAN_DONE_BIT);
    
    // Create the default WiFi station interface
    station_netif_ = esp_netif_create_default_wifi_sta();
    if (!hostname_.empty()) {
        esp_err_t err = esp_netif_set_hostname(station_netif_, hostname_.c_str());
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Station DHCP hostname: %s", hostname_.c_str());
        } else {
            ESP_LOGW(TAG, "Failed to set station DHCP hostname: %s", esp_err_to_name(err));
        }
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiStation::WifiEventHandler,
                                                        this,
                                                        &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WifiStation::IpEventHandler,
                                                        this,
                                                        &instance_got_ip_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(CLAUDEBOT_WIFI_CMD, kCmdSwitchTo,
        [](void* arg, esp_event_base_t, int32_t, void* data) {
            auto* this_ = static_cast<WifiStation*>(arg);
            const std::string target(static_cast<const char*>(data));
            if (!this_->DoSwitchTo(target)) this_->Note("switch_to %s refused", target.c_str());
        }, this, &instance_cmd_));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (max_tx_power_ != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(max_tx_power_));
    }

    // Setup the timer to scan WiFi.
    // skip_unhandled_events = false so the timer can wake the CPU from light
    // sleep on its own; otherwise an idle device that failed to connect would
    // never retry the scan, because esp_timer_get_next_alarm_for_wake_up
    // (components/esp_timer/src/esp_timer.c) excludes timers with this flag
    // from light-sleep wakeup sources.
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto* this_ = static_cast<WifiStation*>(arg);
            if (this_->IsConnected()) {
                this_->Note("scan timer fired while connected - skip");
                return;
            }
            esp_wifi_scan_start(nullptr, false);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiScanTimer",
        .skip_unhandled_events = false
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));

    esp_timer_create_args_t roam_args = {
        .callback = [](void* arg) {
            auto* this_ = static_cast<WifiStation*>(arg);
            if (!this_->IsConnected() || this_->connected_rank_ < kHotspotRank || this_->roaming_) return;
            this_->roam_scanning_ = true;
            if (esp_wifi_scan_start(nullptr, false) != ESP_OK) this_->roam_scanning_ = false;
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiRoamTimer",
        .skip_unhandled_events = true
    };
    ESP_ERROR_CHECK(esp_timer_create(&roam_args, &roam_timer_));

    esp_timer_create_args_t garp_args = {
        .callback = [](void* arg) {
            auto* this_ = static_cast<WifiStation*>(arg);
            this_->SendGarp();
            if (--this_->garp_left_ <= 0) esp_timer_stop(this_->garp_timer_);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "WiFiGarpTimer",
        .skip_unhandled_events = true
    };
    ESP_ERROR_CHECK(esp_timer_create(&garp_args, &garp_timer_));
}

static void GarpOnTcpip(void* ctx) {
    auto* nif = static_cast<struct netif*>(ctx);
    if (nif != nullptr && netif_is_up(nif) && !ip4_addr_isany_val(*netif_ip4_addr(nif))) {
        etharp_gratuitous(nif);
    }
}

void WifiStation::SendGarp() {
    if (station_netif_ == nullptr || !IsConnected()) return;
    auto* nif = static_cast<struct netif*>(esp_netif_get_netif_impl(station_netif_));
    if (nif != nullptr) tcpip_callback(GarpOnTcpip, nif);
}

bool WifiStation::SwitchTo(const std::string& ssid) {
    char buf[33] = {0};
    if (ssid.empty() || ssid.size() >= sizeof(buf) || instance_cmd_ == nullptr) return false;
    memcpy(buf, ssid.data(), ssid.size());
    return esp_event_post(CLAUDEBOT_WIFI_CMD, kCmdSwitchTo, buf, sizeof(buf), pdMS_TO_TICKS(100)) == ESP_OK;
}

bool WifiStation::DoSwitchTo(const std::string& ssid) {
    if (!IsConnected() || remember_bssid_ || ssid == ssid_ || roaming_) return false;
    const auto list = SsidManager::GetInstance().GetSsidList();
    for (const auto& item : list) {
        if (item.ssid != ssid) continue;
        WifiApRecord record = {.ssid = item.ssid, .password = item.password, .channel = 0, .authmode = WIFI_AUTH_OPEN, .bssid = {0}};
        Note("switch_to %s -> %s", ssid_.c_str(), ssid.c_str());
        BeginRoam({record}, false);
        return true;
    }
    return false;
}

// 일부러 끊고 큐(옮길 곳)로 붙는다. 끊김 처리에서 roaming_ 을 보고 옛 와이파이로 다시 붙지 않는다.
void WifiStation::BeginRoam(std::vector<WifiApRecord>&& queue, bool upward) {
    connect_queue_ = std::move(queue);
    // 옮길 곳에 못 붙으면 스캔을 기다리지 않고 원래 와이파이로 바로 돌아간다
    WifiApRecord back = {.ssid = ssid_, .password = password_, .channel = 0, .authmode = WIFI_AUTH_OPEN, .bssid = {0}};
    connect_queue_.push_back(back);
    roaming_ = true;
    roam_attempt_ = upward;   // 시험용 옮기기(SwitchTo)는 실패로 세지 않는다
    roam_notify_ = true;
    if (roam_timer_ != nullptr) esp_timer_stop(roam_timer_);
    esp_wifi_disconnect();
}

void WifiStation::Note(const char* fmt, ...) {
    char body[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof(body)) {   // 잘렸으면 끝의 덜 끝난 UTF-8 글자를 버린다
        size_t len = strlen(body), i = len;
        while (i > 0 && (((unsigned char)body[i - 1]) & 0xC0) == 0x80) i--;
        if (i > 0 && (((unsigned char)body[i - 1]) & 0x80)) {
            const unsigned char lead = body[i - 1];
            const size_t need = (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
            if (len - (i - 1) < need) body[i - 1] = '\0';
        }
    }
    ESP_LOGI(TAG, "%s", body);
    char line[180];
    snprintf(line, sizeof(line), "%lld %s", (long long)(esp_timer_get_time() / 1000000), body);
    std::lock_guard<std::mutex> lock(note_mutex_);
    notes_.push_back(line);
    while (notes_.size() > 30) notes_.pop_front();
}

static void JsonEscapeAppend(std::string& out, const std::string& s) {
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((unsigned char)c < 0x20) out += ' ';
        else out += c;
    }
}

std::string WifiStation::DebugJson() {
    std::string ssid_copy;
    {
        std::lock_guard<std::mutex> lock(note_mutex_);
        ssid_copy = ssid_;
    }
    std::string out = "{\"ssid\":\"";
    JsonEscapeAppend(out, ssid_copy);
    char buf[160];
    snprintf(buf, sizeof(buf), "\",\"connected\":%d,\"rank\":%d,\"rssi\":%d,\"roam_interval_s\":%d,\"roaming\":%d,\"uptime_s\":%lld,\"saved\":[",
             IsConnected() ? 1 : 0, connected_rank_, (int)GetRssi(), (int)(roam_interval_us_ / 1000000), roaming_ ? 1 : 0,
             (long long)(esp_timer_get_time() / 1000000));
    out += buf;
    const auto list = SsidManager::GetInstance().GetSsidList();
    for (size_t i = 0; i < list.size(); i++) {
        out += i ? ",\"" : "\"";
        JsonEscapeAppend(out, list[i].ssid);
        out += "\"";
    }
    out += "],\"log\":[";
    std::lock_guard<std::mutex> lock(note_mutex_);
    for (size_t i = 0; i < notes_.size(); i++) {
        out += i ? ",\"" : "\"";
        JsonEscapeAppend(out, notes_[i]);
        out += "\"";
    }
    out += "]}";
    return out;
}

int WifiStation::RankOf(const std::vector<SsidItem>& list, const std::string& ssid) {
    for (int i = 0; i < (int)list.size(); i++) {
        if (list[i].ssid == ssid) return i + (list[i].hotspot ? kHotspotRank : 0);
    }
    return -1;
}

// 연결된 채로 둘러본 결과. 지금보다 앞 순위 와이파이가 충분히 세게 보이면 그쪽으로 옮긴다.
void WifiStation::HandleRoamScan() {
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) { esp_wifi_clear_ap_list(); return; }
    wifi_ap_record_t* ap_records = (wifi_ap_record_t*)malloc(ap_num * sizeof(wifi_ap_record_t));
    if (ap_records == nullptr) { esp_wifi_clear_ap_list(); return; }
    esp_wifi_scan_get_ap_records(&ap_num, ap_records);
    std::vector<WifiApRecord> better;
    const auto list = SsidManager::GetInstance().GetSsidList();
    // 핫스팟이 아닌 저장 와이파이를 저장 순서대로 본다
    for (int r = 0; r < (int)list.size(); r++) {
        if (list[r].hotspot) continue;
        // 같은 와이파이의 AP 가 여럿이면 센 것부터
        std::vector<wifi_ap_record_t> same;
        for (int i = 0; i < ap_num; i++) {
            if (list[r].ssid != (const char*)ap_records[i].ssid) continue;
            if (ap_records[i].rssi >= kRoamMinRssi) same.push_back(ap_records[i]);
            else Note("roam: %s#%d seen but weak %d dBm", list[r].ssid.c_str(), r, ap_records[i].rssi);
        }
        std::sort(same.begin(), same.end(), [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) { return a.rssi > b.rssi; });
        for (auto& ap : same) {
            WifiApRecord record = {.ssid = list[r].ssid, .password = list[r].password, .channel = ap.primary, .authmode = ap.authmode, .bssid = {0}};
            memcpy(record.bssid, ap.bssid, 6);
            better.push_back(record);
        }
    }
    free(ap_records);
    Note("roam scan: rank %d, %d APs, better=%d (%s)", connected_rank_, (int)ap_num, (int)better.size(),
         better.empty() ? "-" : better.front().ssid.c_str());
    if (better.empty() || !IsConnected()) return;
    Note("roam %s#%d -> %s", ssid_.c_str(), connected_rank_, better.front().ssid.c_str());
    BeginRoam(std::move(better), true);
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    // Wait for either connected or stopped event
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED, 
                                    pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    // Return true only if connected (not if stopped)
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::HandleScanResult() {
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(ap_num * sizeof(wifi_ap_record_t));
    esp_wifi_scan_get_ap_records(&ap_num, ap_records);
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    // 클로드봇: 저장 순서(우선순위)가 먼저, 같은 와이파이 안에서는 신호 센 쪽. 원래는 신호만
    // 봐서 집에서 폰을 보드 옆에 두면 핫스팟에 붙었다.
    std::stable_sort(ap_records, ap_records + ap_num, [&ssid_list](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        const int ra = RankOf(ssid_list, (const char*)a.ssid), rb = RankOf(ssid_list, (const char*)b.ssid);
        const int ka = ra < 0 ? 1000 : ra, kb = rb < 0 ? 1000 : rb;
        if (ka != kb) return ka < kb;
        return a.rssi > b.rssi;
    });

    for (int i = 0; i < ap_num; i++) {
        auto ap_record = ap_records[i];
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [ap_record](const SsidItem& item) {
            return strcmp((char *)ap_record.ssid, item.ssid.c_str()) == 0;
        });
        if (it != ssid_list.end()) {
            ESP_LOGI(TAG, "Found AP: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x, RSSI: %d, Channel: %d, Authmode: %d",
                (char *)ap_record.ssid, 
                ap_record.bssid[0], ap_record.bssid[1], ap_record.bssid[2],
                ap_record.bssid[3], ap_record.bssid[4], ap_record.bssid[5],
                ap_record.rssi, ap_record.primary, ap_record.authmode);
            WifiApRecord record = {
                .ssid = it->ssid,
                .password = it->password,
                .channel = ap_record.primary,
                .authmode = ap_record.authmode,
                .bssid = {0}
            };
            memcpy(record.bssid, ap_record.bssid, 6);
            connect_queue_.push_back(record);
        }
    }
    free(ap_records);
    {
        std::string q;
        for (auto& r : connect_queue_) { char b[48]; snprintf(b, sizeof(b), "%s#%d ", r.ssid.c_str(), RankOf(ssid_list, r.ssid)); q += b; }
        Note("scan: %d APs, queue: %s", (int)ap_num, q.empty() ? "(none)" : q.c_str());
    }

    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "No AP found, next scan in %d seconds", scan_current_interval_microseconds_ / 1000 / 1000);
        esp_timer_start_once(timer_handle_, scan_current_interval_microseconds_);
        UpdateScanInterval();
        return;
    }

    StartConnect();
}

void WifiStation::StartConnect() {
    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());
    {
        std::lock_guard<std::mutex> lock(note_mutex_);   // DebugJson(httpd 작업)이 ssid_ 를 읽는다
        ssid_ = ap_record.ssid;
    }
    password_ = ap_record.password;
    Note("connect %s ch %d (queue left %d)", ssid_.c_str(), ap_record.channel, (int)connect_queue_.size());

    if (on_connect_) {
        on_connect_(ssid_);
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    strcpy((char *)wifi_config.sta.ssid, ap_record.ssid.c_str());
    strcpy((char *)wifi_config.sta.password, ap_record.password.c_str());

    if (remember_bssid_) {
        // Explicit opt-in: pin to this exact AP (BSSID + channel) for the fastest
        // reconnect. This intentionally disables roaming between same-SSID APs.
        wifi_config.sta.channel = ap_record.channel;
        memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
        wifi_config.sta.bssid_set = true;
    } else {
        // Default: do not lock a BSSID. Ask the driver to scan every channel and
        // connect to the same-SSID AP with the strongest signal, instead of the
        // first match found by the default fast scan (which may be a weaker
        // same-name hotspot). Because no BSSID is pinned, a later reconnect (e.g.
        // after moving to another room) will scan again and roam to whichever AP
        // is strongest at that time.
        wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        // Retry the strongest AP this many times before falling back to a weaker
        // same-SSID AP. Requires WIFI_ALL_CHANNEL_SCAN (set above). Without this
        // the driver gives up after a single auth failure and immediately moves on,
        // which is why we saw it connect to a -79 dBm AP even though a -73 dBm one
        // was available (the stronger AP rejected the first attempt).
        wifi_config.sta.failure_retry_cnt = failure_retry_cnt_;
    }
    wifi_config.sta.listen_interval = 10;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    reconnect_count_ = 0;
    ESP_ERROR_CHECK(esp_wifi_connect());
}

int8_t WifiStation::GetRssi() {
    // Check if connected first
    if (!IsConnected()) {
        return 0;  // Return 0 if not connected
    }
    
    // Get station info
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get AP info: %s", esp_err_to_name(err));
        return 0;
    }
    return ap_info.rssi;
}

uint8_t WifiStation::GetChannel() {
    // Check if connected first
    if (!IsConnected()) {
        return 0;  // Return 0 if not connected
    }
    
    // Get station info
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get AP info: %s", esp_err_to_name(err));
        return 0;
    }
    return ap_info.primary;
}

bool WifiStation::IsConnected() {
    return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

void WifiStation::SetScanIntervalRange(int min_interval_seconds, int max_interval_seconds) {
    scan_min_interval_microseconds_ = min_interval_seconds * 1000 * 1000;
    scan_max_interval_microseconds_ = max_interval_seconds * 1000 * 1000;
    scan_current_interval_microseconds_ = scan_min_interval_microseconds_;
}


void WifiStation::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    wifi_ps_type_t ps_type;
    switch (level) {
        case WifiPowerSaveLevel::LOW_POWER:
            ps_type = WIFI_PS_MAX_MODEM;  // Maximum power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: LOW_POWER (MAX_MODEM)");
            break;
        case WifiPowerSaveLevel::BALANCED:
            ps_type = WIFI_PS_MIN_MODEM;  // Minimum power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: BALANCED (MIN_MODEM)");
            break;
        case WifiPowerSaveLevel::PERFORMANCE:
        default:
            ps_type = WIFI_PS_NONE;       // No power saving
            ESP_LOGI(TAG, "Setting WiFi power save level: PERFORMANCE (NONE)");
            break;
    }
    ESP_ERROR_CHECK(esp_wifi_set_ps(ps_type));
}

void WifiStation::UpdateScanInterval() {
    // Apply exponential backoff: double the interval, up to max
    if (scan_current_interval_microseconds_ < scan_max_interval_microseconds_) {
        scan_current_interval_microseconds_ *= 2;
        if (scan_current_interval_microseconds_ > scan_max_interval_microseconds_) {
            scan_current_interval_microseconds_ = scan_max_interval_microseconds_;
        }
    }
}

// Static event handler functions
void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_scan_start(nullptr, false);
        if (this_->on_scan_begin_) {
            this_->on_scan_begin_();
        }
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(this_->event_group_, WIFI_EVENT_SCAN_DONE_BIT);
        if (this_->roam_scanning_) {
            // 둘러보기 결과는 평소 연결 경로로 보내지 않는다. 그사이 끊겼으면 끊김 처리가 다시 붙는다.
            this_->roam_scanning_ = false;
            if (this_->IsConnected()) {
                this_->HandleRoamScan();
            } else {
                esp_wifi_clear_ap_list();
            }
            return;
        }
        if (this_->IsConnected()) {
            // 연결된 뒤에 늦게 끝난 평소 스캔. 다시 연결 경로로 보내면 붙어 있던 곳을 끊고 옮긴다.
            this_->Note("scan done while connected - ignore");
            esp_wifi_clear_ap_list();
            return;
        }
        this_->HandleScanResult();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
        
        // Notify disconnected callback only once when transitioning from connected to disconnected
        bool was_connected = this_->was_connected_;
        this_->was_connected_ = false;
        wifi_event_sta_disconnected_t* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        this_->Note("disconnected %s reason %d (roaming=%d, retry=%d, queue=%d)", this_->ssid_.c_str(), event->reason,
                    this_->roaming_ ? 1 : 0, this_->reconnect_count_, (int)this_->connect_queue_.size());
        if (this_->roam_timer_ != nullptr) esp_timer_stop(this_->roam_timer_);
        this_->connected_rank_ = -1;
        if (this_->roaming_) {
            // 앞 순위로 옮기려고 일부러 끊었다. 옛 와이파이로 다시 붙지 않고 큐(앞 순위)로 간다.
            // 화면에는 끊김을 알리지 않는다. 큐가 다 실패하면 아래 평소 스캔으로 돌아간다.
            this_->roaming_ = false;
            if (!this_->connect_queue_.empty()) {
                this_->StartConnect();
                return;
            }
        } else if ((was_connected || this_->roam_notify_) && this_->on_disconnected_) {
            // 옮기다 실패한 끊김도 한 번은 앱에 알린다
            this_->roam_notify_ = false;
            this_->on_disconnected_(event->reason);
        }
        
        if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
            esp_wifi_connect();
            this_->reconnect_count_++;
            ESP_LOGI(TAG, "Reconnecting %s (attempt %d / %d)", this_->ssid_.c_str(), this_->reconnect_count_, MAX_RECONNECT_COUNT);
            return;
        }

        if (!this_->connect_queue_.empty()) {
            this_->StartConnect();
            return;
        }
        
        ESP_LOGI(TAG, "No more AP to connect, next scan in %d seconds", 
                 this_->scan_current_interval_microseconds_ / 1000 / 1000);
        esp_timer_start_once(this_->timer_handle_, this_->scan_current_interval_microseconds_);
        this_->UpdateScanInterval();
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);

    char ip_address[16];
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
    this_->ip_address_ = ip_address;
    ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());
    if (this_->timer_handle_ != nullptr) esp_timer_stop(this_->timer_handle_);   // 남은 평소 스캔 예약을 지운다
    xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);   // SendGarp 가 연결됨을 보게 먼저 세운다
    this_->SendGarp();
    if (this_->garp_timer_ != nullptr) {
        this_->garp_left_ = 3;   // 1·2·3초 뒤에 한 번씩 더
        esp_timer_stop(this_->garp_timer_);
        esp_timer_start_periodic(this_->garp_timer_, 1000 * 1000);
    }
    
    xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
    this_->was_connected_ = true;  // Mark as connected for disconnect notification
    if (this_->on_connected_) {
        this_->on_connected_(this_->ssid_);
    }
    this_->connect_queue_.clear();
    this_->reconnect_count_ = 0;
    
    // Reset scan interval to minimum for fast reconnect if disconnected later
    this_->scan_current_interval_microseconds_ = this_->scan_min_interval_microseconds_;

    // 앞 순위가 아닌 와이파이(핫스팟)에 붙었으면 가끔 둘러본다
    this_->roaming_ = false;
    this_->roam_notify_ = false;
    this_->connected_rank_ = RankOf(SsidManager::GetInstance().GetSsidList(), this_->ssid_);
    if (this_->connected_rank_ < kHotspotRank) {
        this_->roam_interval_us_ = kRoamScanIntervalUs;
    } else if (this_->roam_attempt_) {
        // 앞 순위로 옮기려다 못 붙고 돌아왔다(비밀번호 바뀜·약한 신호). 매번 끊기지 않게 간격을 늘린다.
        this_->roam_interval_us_ = std::min(this_->roam_interval_us_ * 2, kRoamMaxIntervalUs);
    }
    this_->roam_attempt_ = false;
    this_->Note("got ip %s on %s rank %d rssi %d", this_->ip_address_.c_str(), this_->ssid_.c_str(), this_->connected_rank_, (int)this_->GetRssi());
    if (this_->roam_timer_ != nullptr) {
        esp_timer_stop(this_->roam_timer_);
        if (this_->connected_rank_ >= kHotspotRank) {
            this_->Note("roam timer every %d s", (int)(this_->roam_interval_us_ / 1000000));
            esp_timer_start_periodic(this_->roam_timer_, this_->roam_interval_us_);
        }
    }
}
