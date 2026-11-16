#include "notify_sound.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/task.h>

#include <cstring>
#include <ctime>

#include "settings.h"

#define TAG "NotifySound"

// 코덱이 있는 보드만 소리를 낸다. 다른 보드는 Play() 가 조용히 아무것도 안 한다.
#if CONFIG_BOARD_TYPE_Freenove_ESP32S3_DISPLAY_2_8_LCD
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <es8311_codec.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include "boards/freenove-esp32s3-display-2.8-lcd/config.h"
#define NOTIFY_SOUND_HAS_CODEC 1
#else
#define NOTIFY_SOUND_HAS_CODEC 0
#endif

// tools/import_notify_sounds.py 가 만든 16kHz 모노 s16le. EMBED_FILES 로 들어온다.
#define NOTIFY_SOUND_PCM(name)                                                   \
    extern const uint8_t name##_pcm_start[] asm("_binary_" #name "_pcm_start"); \
    extern const uint8_t name##_pcm_end[] asm("_binary_" #name "_pcm_end");
NOTIFY_SOUND_PCM(notify_poke)
NOTIFY_SOUND_PCM(notify_guest)
NOTIFY_SOUND_PCM(tap_rona2)
NOTIFY_SOUND_PCM(tap_rozzi2)
NOTIFY_SOUND_PCM(tap_manbok1)
NOTIFY_SOUND_PCM(tap_manbok2)
#undef NOTIFY_SOUND_PCM

namespace {

constexpr uint32_t kSampleRate = 16000;      // gen_notify_sounds.py RATE 와 맞출 것
constexpr const char* kNvsNamespace = "sound";
constexpr uint32_t kTaskStack = 4096;
constexpr UBaseType_t kTaskPriority = 4;
constexpr UBaseType_t kQueueDepth = 2;       // 연달아 오면 둘까지만 잇는다
constexpr int kLeadSamples = kSampleRate * 30 / 1000;    // PA 를 켠 뒤 30ms 쉼 — 켜짐 잡음이 소리에 안 섞이게
constexpr int kTailSamples = kSampleRate * 80 / 1000;    // DMA 에 남은 소리를 다 내보낸 뒤 닫는다

constexpr uint32_t kTapGapMs = 1500;        // 캐릭터 연타는 이 간격에 한 번만 운다

struct Clip {
    const uint8_t* start;
    const uint8_t* end;
};

// Kind 별 소리. 탭은 두 개 중 하나를 무작위로 고른다(콕·손님 소리가 각 고양이의 첫 소리를 겸한다).
// 큐에는 kind 와 고른 번호를 한 바이트로 싣는다: (variant << 4) | kind.
Clip ClipFor(uint8_t item) {
    const uint8_t kind = item & 0x0f;
    const bool alt = (item >> 4) != 0;
    switch (kind) {
        case NotifySound::kGuest: return {notify_guest_pcm_start, notify_guest_pcm_end};
        case NotifySound::kTapRona:
            return alt ? Clip{tap_rona2_pcm_start, tap_rona2_pcm_end} : Clip{notify_poke_pcm_start, notify_poke_pcm_end};
        case NotifySound::kTapRozzi:
            return alt ? Clip{tap_rozzi2_pcm_start, tap_rozzi2_pcm_end}
                       : Clip{notify_guest_pcm_start, notify_guest_pcm_end};
        case NotifySound::kTapManbok:
            return alt ? Clip{tap_manbok2_pcm_start, tap_manbok2_pcm_end}
                       : Clip{tap_manbok1_pcm_start, tap_manbok1_pcm_end};
        default: return {notify_poke_pcm_start, notify_poke_pcm_end};
    }
}

const char* KindName(uint8_t kind) {
    static const char* const kNames[] = {"poke", "guest", "rona", "rozzi", "manbok"};
    return kind < sizeof(kNames) / sizeof(kNames[0]) ? kNames[kind] : "?";
}

bool ValidHour(const cJSON* j) {
    return cJSON_IsNumber(j) && j->valueint >= 0 && j->valueint <= 23;
}

}  // namespace

void NotifySound::Start() {
    if (queue_ != nullptr) {
        return;
    }
    {
        Settings settings(kNvsNamespace);
        vol_ = settings.GetInt("vol", vol_);
        quiet_from_ = settings.GetInt("quiet_from", quiet_from_);
        quiet_to_ = settings.GetInt("quiet_to", quiet_to_);
    }
#if NOTIFY_SOUND_HAS_CODEC
    // I2C 버스는 보드가 터치용으로 이미 열어 둔 것을 빌린다(ES8311 이 같은 버스에 있다).
    i2c_master_bus_handle_t i2c_bus = nullptr;
    if (i2c_master_get_bus_handle(AUDIO_CODEC_I2C_NUM, &i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus not ready");
        return;
    }

    // 출력만 쓰므로 TX 채널 하나. 켜고 끄기(enable/disable)는 esp_codec_dev 가 열고 닫을 때 한다.
    i2s_chan_handle_t tx = nullptr;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear = true;   // 데이터가 끊기면 0 을 내보낸다 — 끝에 웅 소리가 안 남는다
    if (i2s_new_channel(&chan_cfg, &tx, nullptr) != ESP_OK) {
        ESP_LOGE(TAG, "i2s channel failed");
        return;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_GPIO_MCLK,
            .bclk = AUDIO_I2S_GPIO_BCLK,
            .ws = AUDIO_I2S_GPIO_WS,
            .dout = AUDIO_I2S_GPIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {},
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2s std init failed");
        i2s_del_channel(tx);
        return;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = I2S_NUM_0;
    i2s_cfg.tx_handle = tx;
    const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port = AUDIO_CODEC_I2C_NUM;
    i2c_cfg.addr = AUDIO_CODEC_ES8311_ADDR;
    i2c_cfg.bus_handle = i2c_bus;
    const audio_codec_ctrl_if_t* ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es_cfg = {};
    es_cfg.ctrl_if = ctrl_if;
    es_cfg.gpio_if = gpio_if;
    es_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es_cfg.pa_pin = AUDIO_CODEC_PA_PIN;   // 열 때 켜고 닫을 때 끈다 — 쉬는 동안 스피커 잡음·전류 없음
    es_cfg.pa_reverted = true;            // 이 보드 앰프는 LOW 가 켜짐(제조사 보드 코드도 pa_inverted=true)
    es_cfg.use_mclk = true;
    es_cfg.hw_gain.pa_voltage = 5.0;
    es_cfg.hw_gain.codec_dac_voltage = 3.3;
    const audio_codec_if_t* codec_if = data_if && ctrl_if && gpio_if ? es8311_codec_new(&es_cfg) : nullptr;
    if (codec_if == nullptr) {
        ESP_LOGE(TAG, "es8311 init failed");
        return;
    }
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    output_dev_ = esp_codec_dev_new(&dev_cfg);
    if (output_dev_ == nullptr) {
        ESP_LOGE(TAG, "codec dev failed");
        return;
    }

    queue_ = xQueueCreate(kQueueDepth, sizeof(uint8_t));
    if (queue_ == nullptr || xTaskCreate(TaskEntry, "notify_sound", kTaskStack, this, kTaskPriority, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "task failed");
        return;
    }
    ESP_LOGI(TAG, "ready vol=%d quiet=%d~%d", vol_.load(), quiet_from_.load(), quiet_to_.load());
#endif
}

bool NotifySound::InQuietHours() const {
    const int from = quiet_from_, to = quiet_to_;
    if (from == to) {
        return false;   // 같으면 조용한 시간 없음
    }
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year < 2024 - 1900) {
        return false;   // SNTP 전 — 몇 시인지 모르니 울린다
    }
    const int h = tm_now.tm_hour;
    return from < to ? (h >= from && h < to) : (h >= from || h < to);
}

bool NotifySound::Enqueue(Kind kind) {
    uint8_t item = kind;
    if (kind >= kTapRona) {
        item |= static_cast<uint8_t>((esp_random() & 1) << 4);
    }
    return queue_ != nullptr && xQueueSend(queue_, &item, 0) == pdTRUE;
}

void NotifySound::Play(Kind kind) {
    if (vol_ <= 0 || InQuietHours()) {
        return;
    }
    Enqueue(kind);
}

void NotifySound::PlayTap(int who) {
    if (who < 0 || who > 2 || queue_ == nullptr || vol_ <= 0 || InQuietHours()) {
        return;
    }
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const uint32_t last = last_tap_ms_;
    if (last != 0 && now - last < kTapGapMs) {
        return;
    }
    if (uxQueueMessagesWaiting(queue_) != 0) {
        return;   // 알림 소리가 줄 서 있으면 탭 소리는 끼우지 않는다
    }
    last_tap_ms_ = now == 0 ? 1 : now;
    Enqueue(static_cast<Kind>(kTapRona + who));
}

bool NotifySound::ApplyJson(const cJSON* root) {
    bool handled = false;
    // {"sound":{"vol":0~100,"quiet_from":0~23,"quiet_to":0~23}}. 범위 밖 값은 버린다.
    const cJSON* js = cJSON_GetObjectItem(root, "sound");
    if (cJSON_IsObject(js)) {
        Settings settings(kNvsNamespace, true);
        const cJSON* jvol = cJSON_GetObjectItem(js, "vol");
        if (cJSON_IsNumber(jvol) && jvol->valueint >= 0 && jvol->valueint <= 100) {
            vol_ = jvol->valueint;
            settings.SetInt("vol", vol_);
            handled = true;
        }
        const cJSON* jfrom = cJSON_GetObjectItem(js, "quiet_from");
        if (ValidHour(jfrom)) {
            quiet_from_ = jfrom->valueint;
            settings.SetInt("quiet_from", quiet_from_);
            handled = true;
        }
        const cJSON* jto = cJSON_GetObjectItem(js, "quiet_to");
        if (ValidHour(jto)) {
            quiet_to_ = jto->valueint;
            settings.SetInt("quiet_to", quiet_to_);
            handled = true;
        }
        ESP_LOGI(TAG, "settings vol=%d quiet=%d~%d", vol_.load(), quiet_from_.load(), quiet_to_.load());
    }
    // 시험: {"sound_test":"poke"|"guest"|"rona"|"rozzi"|"manbok"} 는 조용한 시간·탭 간격을 무시하고
    // 바로 울린다(vol 0 이면 무음). 캐릭터 이름은 두 소리 중 하나를 무작위로 낸다.
    // 코덱이 없거나 큐가 차면 handled 로 안 친다 — 단독 push 가 400 이면 소리 길이 죽은 것이다.
    const cJSON* jtest = cJSON_GetObjectItem(root, "sound_test");
    if (cJSON_IsString(jtest)) {
        for (uint8_t k = kPoke; k <= kTapManbok; ++k) {
            if (strcmp(jtest->valuestring, KindName(k)) == 0) {
                handled = Enqueue(static_cast<Kind>(k)) || handled;
                break;
            }
        }
    }
    return handled;
}

void NotifySound::TaskEntry(void* arg) {
    static_cast<NotifySound*>(arg)->Run();
}

void NotifySound::Run() {
    uint8_t item;
    while (true) {
        if (xQueueReceive(queue_, &item, portMAX_DELAY) == pdTRUE) {
            PlayNow(item);
        }
    }
}

void NotifySound::PlayNow(uint8_t item) {
#if NOTIFY_SOUND_HAS_CODEC
    const Clip clip = ClipFor(item);
    const uint8_t* start = clip.start;
    const uint8_t* end = clip.end;
    const int vol = vol_;
    if (vol <= 0) {
        return;
    }
    auto dev = static_cast<esp_codec_dev_handle_t>(output_dev_);
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = kSampleRate,
        .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "open failed");
        return;
    }
    esp_codec_dev_set_out_vol(dev, vol);
    // 플래시의 소리를 램 조각으로 옮겨 쓴다 — 코덱이 소프트 볼륨을 쓰면 버퍼를 제자리에서 고친다.
    int16_t buf[256];
    auto write_chunks = [&](const uint8_t* src, int bytes) {
        while (bytes > 0) {
            const int n = bytes < static_cast<int>(sizeof(buf)) ? bytes : static_cast<int>(sizeof(buf));
            if (src != nullptr) {
                memcpy(buf, src, n);
                src += n;
            } else {
                memset(buf, 0, n);
            }
            esp_codec_dev_write(dev, buf, n);
            bytes -= n;
        }
    };
    write_chunks(nullptr, kLeadSamples * 2);
    write_chunks(start, end - start);
    write_chunks(nullptr, kTailSamples * 2);
    esp_codec_dev_close(dev);
    ESP_LOGI(TAG, "played %s/%d vol=%d", KindName(item & 0x0f), item >> 4, vol);
#else
    (void)item;
#endif
}
