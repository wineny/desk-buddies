#ifndef SSID_MANAGER_H
#define SSID_MANAGER_H

#include <string>
#include <vector>
#include <mutex>

struct SsidItem {
    std::string ssid;
    std::string password;
    bool hotspot = false;   // 클로드봇: 폰 핫스팟 = 최후 수단. 다른 저장 와이파이가 보이면 그쪽이 먼저
};

class SsidManager {
public:
    static SsidManager& GetInstance() {
        static SsidManager instance;
        return instance;
    }

    // hotspot: 1/0 = 그 값으로, -1 = 이미 있으면 그대로 두고 새것은 new_hotspot_default
    void AddSsid(const std::string& ssid, const std::string& password, int hotspot = -1, bool new_hotspot_default = false);
    void RemoveSsid(int index);
    void SetDefaultSsid(int index);
    void Clear();
    // 클로드봇: 맥에서 wifi_add 로 HTTP 작업이 목록을 바꾸는 동안 와이파이 이벤트 작업이 읽을 수
    // 있어서, 잠그고 사본을 돌려준다.
    std::vector<SsidItem> GetSsidList() const { std::lock_guard<std::mutex> lock(mutex_); return ssid_list_; }

private:
    SsidManager();
    ~SsidManager();

    void LoadFromNvs();
    void SaveToNvs();

    std::vector<SsidItem> ssid_list_;
    mutable std::mutex mutex_;
};

#endif // SSID_MANAGER_H
