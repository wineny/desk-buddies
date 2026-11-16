#include "usage_receiver.h"
#include "lcd_display.h"

#include "usage/character.h"
#include "usage/outfit_sets.h"
#include "usage/wardrobe_store.h"
#include "usage/art/ui_poke.h"
#include "poke_link.h"
#include "diary_fetcher.h"
#include "assets/lang_config.h"
#include "gif/lvgl_gif.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <src/misc/cache/lv_cache.h>
#include <algorithm>
#include <cstring>
#include <vector>

#include "board.h"

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);
LV_FONT_DECLARE(press_start_8);  // WiFi 가이드용 픽셀 폰트 (ASCII 전용)

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    // light theme — 클로드봇 팔레트(주황 바탕 + 남색 요소)를 여기에 박는다.
    // SetTheme() 이 container_ 배경색을 무조건 덮어쓰므로 위젯 쪽에서 색을
    // 지정해봤자 무효다. 덮어쓸 때 쓰는 값 자체를 바꾸는 편이 확실하다.
    //
    // 주의: "claudebot" 같은 새 이름의 테마를 만들면 안 된다. LvglDisplay::
    // SetTextFont() 이 "light"/"dark" 두 이름만 찾아 에셋의 한글 폰트를 꽂기
    // 때문에, 새 테마는 그 갱신에서 영원히 빠져 상태바 한글이 두부로 뜬다.
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xF06828));
    light_theme->set_text_color(lv_color_hex(0x111820));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);
    light_theme->set_emoji_font(emoji_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                       int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback =
            [](void* arg) {
                LcdDisplay* display = static_cast<LcdDisplay*>(arg);
                display->SetPreviewImage(nullptr);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .swap_bytes = 1,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .swap_bytes = 0,
                .full_refresh = 1,
                .direct_mode = 1,
            },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {.flags = {
                                                     .bb_mode = true,
                                                     .avoid_tearing = true,
                                                 }};

    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = true,
            },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {.flags = {
                                                     .avoid_tearing = false,
                                                 }};
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);

    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void LcdDisplay::Unlock() { lvgl_port_unlock(); }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(),
                              0);  // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);

    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0);  // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0,
                 text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);
}
#if CONFIG_IDF_TARGET_ESP32P4
#define MAX_MESSAGES 40
#else
#define MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called "
                     "but container not created)",
                     role, content);
        }
        return;
    }

    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }

    // Collapse system messages (if it's a system message, check if the last message is also a
    // system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) &&
                lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr &&
                        strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered AI logo
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if (strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);

    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;

    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);

    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;

    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }

    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);

        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);

        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }

    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);

    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);

    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);

    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;   // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100;  // 50% of screen height

    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld",
                 img_width, img_height, max_width, max_height);
    }

    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;

    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256)
        zoom = 256;

    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);

    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release();  // Release ownership of smart pointer
    lv_obj_add_event_cb(
        preview_image,
        [](lv_event_t* e) {
            LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
            if (img != nullptr) {
                delete img;  // Properly release memory by deleting LvglImage object
            }
        },
        LV_EVENT_DELETE, (void*)raw_image);

    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;

    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);

    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);

    // Center the image within the bubble
    lv_obj_center(preview_image);

    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);

    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;

    // Show the centered AI logo (emoji_label_) again
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI(TAG, "Chat messages cleared");
}
#else
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container - used as background */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Bottom layer: emoji_box_ - centered display */
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    /* Middle layer: preview_image_ - centered display */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    /* Bottom bar - auto height, grows upward with wrapped text */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_width(bottom_bar_, LV_HOR_RES);
    lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, multiline wrapped display */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#else
    /* Top layer: Bottom bar - fixed height at bottom */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + lvgl_theme->spacing(8));
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_left(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, single-line horizontal scroll */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

    // Start scrolling after a delay (short text won't scroll)
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000),
                                   LV_PART_MAIN);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#endif

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);

    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    /* --- 클로드 사용량 화면 -------------------------------------------
     * 기존 위젯은 지우지 않는다. emoji_box_ 는 SetPreviewImage() 와
     * SetTheme() 에서 널 체크 없이 역참조되므로 생성은 그대로 두고
     * 숨기기만 한다. SetEmotion() 은 자식(emoji_label_/emoji_image_)만
     * 건드리므로 부모를 숨기면 재출현 경로가 모두 막힌다.
     *
     * 사용량 화면은 container_ 에 얹는다. container_ 는 screen 의 첫 자식
     * 이라 항상 맨 아래에 그려지고, 따라서 top_bar_(WiFi·배터리) 와
     * status_bar_(듣는 중/말하는 중) 가 그 위에 남는다.
     */
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);

    /* 자막 끄기. hide_subtitle_ 는 SetChatMessage() 에서만 읽힌다.
     * SetHideSubtitle() 을 부르면 DisplayLockGuard 가 중첩되므로 직접 처리. */
    hide_subtitle_ = true;
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_scrollbar_mode(container_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(container_, LV_OBJ_FLAG_SCROLLABLE);

    /* 얼굴·숫자는 UsageScreen 이 자기 패널 위에 그린다. 패널을 따로 두는
     * 이유는 SetTheme() 이 container_ 자체의 배경색만 덮어쓰고 그 자식은
     * 건드리지 않기 때문이다. 폰트도 그 안에서 정적 폰트만 쓴다. */
    usage_screen_ = std::make_unique<UsageScreen>(container_, width_, height_);

    /* --- 상단 바 제거 -------------------------------------------------
     * 한동안 왼쪽에 시계, 오른쪽에 WiFi 만 남겨 뒀는데, 사용량·표정 화면이
     * 정보를 다 갖고 있어 바가 자리만 차지했다. 통째로 숨긴다(2026-08-29).
     * UpdateStatusBar() 가 숨은 위젯을 갱신해도 무해하고, 시계 라벨을
     * 안 만들면 SetClock() 은 널 가드로 조용히 빠진다. */
    lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);

    /* --- 상태 텍스트를 화면 아래로 -------------------------------------
     * status_bar_ 는 원래 top_bar_ 와 겹치게 놓여 있었다. 아래로 내리면
     * 시계 자리와 겹칠 일이 없어진다. 얼굴 레이아웃의 맨 아래 요소는
     * y=200 근처에서 끝나므로 하단 바와 부딪히지 않는다.
     */
    lv_obj_align(status_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* 글자를 70% 로. 상태 텍스트는 한글이라 에셋 폰트를 써야 하는데 그걸
     * 위젯에 직접 박으면 테마 갱신 때 죽는다. 그래서 폰트는 그대로 두고
     * 위젯 자체를 축소한다. LV_SCALE_NONE(256) 이 100% 다. */
    /* 상태 텍스트는 영문만 쓴다(SetStatus 에서 갈아끼운다). 한글을 안 쓰니
     * 에셋 폰트가 필요 없고, 플래시에 박힌 정적 폰트를 직접 지정할 수 있다.
     * 덕분에 위젯 축소(transform)를 쓰지 않아도 되는데, 그 방식은 그릴 때마다
     * 별도 레이어를 만들어 비쌌다. */
    lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_14, 0);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);  // 대기 중에는 숨김
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        /* 사용량 화면을 쓰는 동안에는 이모지를 되살리지 않는다. 되살리면
         * "이번 주 보여줘" 같은 미리보기가 5초 뒤 사라질 때마다 로봇 이모지가
         * 얼굴 위로 튀어나온다. SetupUI() 에서 한 번 숨긴 것만으로는 부족하다. */
        if (usage_screen_ == nullptr) {
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 0.5
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: chat_message_label_ is nullptr (SetupUI() "
                     "was called but label not created)",
                     role, content);
        }
        return;
    }
    lv_anim_delete(chat_message_label_, nullptr);
    lv_label_set_text(chat_message_label_, content);
    // Show bottom_bar_ only when there is content (and subtitle is not globally hidden)
    if (bottom_bar_ != nullptr) {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    // Re-align bottom_bar_ after text change so it stays anchored to the bottom
    // as its height adapts to the wrapped content.
    if (bottom_bar_ != nullptr) {
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
#endif
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    // In non-wechat mode, just clear the chat message label and hide the bar
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

void LcdDisplay::SetEmotion(const char* emotion) {
    if (usage_screen_ != nullptr) {
        // 사용량 화면이 전면을 덮고 있어 이모지는 어차피 안 보인다. 여기서
        // 끊지 않으면 안 보이는 GIF 디코더가 만들어져 PSRAM 과 CPU 만 쓴다.
        return;
    }
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!",
                 emotion);
    }
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetEmotion('%s') failed: emoji_image_ is nullptr (SetupUI() was called but "
                     "emoji image not created)",
                     emotion);
        }
        return;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        const char* utf8 = noto_emoji_get_utf8(emotion);
        const lv_font_t* emotion_font = lvgl_theme->emoji_font()->font();
        if (utf8 == nullptr) {
            utf8 = material_symbols_get_utf8(emotion);
            emotion_font = lvgl_theme->large_icon_font()->font();
        }
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            if (gif_controller_) {
                gif_controller_->Stop();
                gif_controller_.reset();
            }
            lv_obj_set_style_text_font(emoji_label_, emotion_font, 0);
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    DisplayLockGuard lock(this);
    // Stop any running GIF animation in the same lock scope as setting new image
    // to prevent LVGL from accessing freed image data between operations
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());

        if (gif_controller_->IsLoaded()) {
            // Set up frame update callback
            gif_controller_->SetFrameCallback(
                [this]() { lv_image_set_src(emoji_image_, gif_controller_->image_dsc()); });

            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();

            // Show GIF, hide others
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }

        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);

    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }

    // Update top bar background color with 50% opacity
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }

    // Update status bar elements
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr)
            continue;

        lv_obj_t* bubble = nullptr;

        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }

        if (bubble == nullptr)
            continue;

        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);

            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0);
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }

            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);

            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "child[%lu] Bubble type is not found", i);
        }
    }
#else
    // Simple UI mode - just update the main chat message
    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    }

    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }

    // Update bottom bar background color with 50% opacity
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    }
#endif

    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;

    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Only show if there is actual content to display
            const char* text =
                (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

// --- 사용량 화면: 유휴 상태 기계 ------------------------------------------
//
// 새 타이머를 만들지 않는다. application.cc 의 1초 틱이 OnIdleTick() 을 부르고,
// 거기 넘어오는 idle_seconds 는 대기 상태로 들어간 뒤 흐른 초다. 호출어·듣는
// 중·말하는 중 등 상태가 바뀌면 그쪽에서 0 이 넘어오므로, "말을 걸면 깨어난다"
// 가 별도 배선 없이 성립한다.

namespace {
constexpr int kUsageSleepAfterS = 10 * 60;        // 수면 — 어둡게
constexpr int kUsageScreenOffAfterS = 30 * 60;    // 화면 끔
constexpr int kUsageNightDimAfterS = 60;          // 밤 화면: 이만큼 손대지 않으면 어둡게
constexpr int kUsageSleepBrightness = 12;         // 수면 중 백라이트 (%)

// 상태 화면의 시간 복귀 규칙 (시안 확정값)
constexpr int kUsageErrorHoldS = 5;    // 에러 → 작업
constexpr int kUsageDoneHoldS = 8;     // 완료 → 대기
constexpr int kUsageWorkingHoldS = 60; // 생각/작업/저글링 무이벤트 → 대기
// 권한 대기는 원래 다음 이벤트로만 풀리지만, 호스트가 죽으면(프롬프트에서
// Ctrl-C, WiFi 단절) 풀어줄 이벤트가 영영 안 온다. 그대로 두면 풀브라이트
// 펄스가 영구 지속돼 상시 켜둔 기기의 수명을 갉아먹으므로 상한을 둔다.
constexpr int kUsagePermissionHoldS = 15 * 60;
constexpr int kUsagePeekS = 30;  // 탭으로 대기 화면을 엿보는 시간
constexpr uint32_t kWardrobeIdleMs = 20 * 1000;  // 옷장 무입력 닫기 (WX 탭까지 훑기엔 10초가 짧았다)
constexpr uint32_t kDiaryIdleMs = 60 * 1000;     // 일기 판 무입력 닫기 (긴 글을 읽는 시간)
constexpr size_t kDiaryPhotoCache = 4;           // 받아 둔 사진 수(한 장 ≈ 112KB, PSRAM). 판은 이 포인터를 나눠 쓴다
// 옷장이 닫힌 직후의 탭 무시 창 — 자동 닫힘·푸시 닫힘을 모르는 사용자의 탭이
// 대기 화면으로 새서 ACT 모드를 끊던 것(빈 곳/몸통 탭 판정)을 막는다.
constexpr uint32_t kWardrobeCloseGraceMs = 400;
// 지금 시각으로 고른 auto 하늘(UsageVillageSkyForClock: 07~19시 낮, 그 외 남색).
// 팩(제자리에 앉는 캐릭터)은 평시 실내 — 낮 카페, 밤 방. 마을은 미니카 외출 때만
// (UsageScreen::StartPackRide) 잠깐 나갔다 온다. (09-06 "길바닥에 덩그러니" 개선)
UsageSky SkyForNow() {
    const UsageSky village = UsageVillageSkyForClock();
    if (CharacterHasScenes()) return kUsageSkyFirstScene;   // 장면 팩의 기본 장면
    return village == kUsageSkyDay ? kUsageSkyCafe : kUsageSkyRoom;
}

// auto 가 고르는 하늘인가 — 옷장에서 이 중 하나를 AUTO 로 고르면 시각을 따른다.
bool IsAutoSky(UsageSky sky) {
    if (CharacterHasScenes()) return false;   // 장면은 손으로 고른다
    return sky == kUsageSkyCafe || sky == kUsageSkyRoom;
}

// 지금 캐릭터가 쓸 수 있는 배경인가. 장면 팩은 장면 20개 + 기념일 3개, 다른 팩은 기본 13개.
bool SkyFitsCharacter(UsageSky sky) {
    const bool scene = sky >= kUsageSkyFirstScene && sky < kUsageSkyCount;
    return CharacterHasScenes() ? scene : !scene;
}
}  // namespace

void LcdDisplay::OnIdleTick(int idle_seconds) {
    if (usage_screen_ == nullptr) {
        return;
    }
    auto backlight = Board::GetInstance().GetBacklight();
    // HTTP 태스크의 SetUsageState() 와 겨루지 않게 틱 전체를 한 락으로 덮는다.
    // 락 없이 state() 를 읽고 재사용하면, 호스트가 방금 민 상태를 여기 수면
    // 판정이 덮어쓸 수 있다.
    DisplayLockGuard lock(this);

    // 사람이 롱프레스로 끈 화면은 다음 터치(WakeIfScreenOff)까지 그대로 둔다.
    // 아래 2) 의 "유휴가 짧으면 다시 켜기" 가 바로 되살리는 것을 막는다.
    if (usage_manual_off_) {
        return;
    }

    // 일기 판은 읽는 시간이 길어 무입력 60초. 그동안 유휴 연출은 쉰다.
    if (diary_panel_ != nullptr) {
        if (diary_panel_->idle_ms() >= kDiaryIdleMs) {
            ESP_LOGI(TAG, "Diary panel: idle %" PRIu32 "ms -> close", diary_panel_->idle_ms());
            CloseDiaryPanelLocked();
        }
        return;
    }

    // 만복이 메뉴도 일기 판과 같은 무입력 60초.
    if (manbok_panel_ != nullptr) {
        if (manbok_panel_->idle_ms() >= kDiaryIdleMs) {
            ESP_LOGI(TAG, "Manbok panel: idle %" PRIu32 "ms -> close", manbok_panel_->idle_ms());
            CloseManbokPanelLocked();
        }
        return;
    }

    // 날씨 판도 옷장처럼 무입력 20초만 재고 유휴 연출은 쉰다.
    if (weather_panel_ != nullptr) {
        if (weather_panel_->idle_ms() >= kWardrobeIdleMs) {
            ESP_LOGI(TAG, "Weather panel: idle %" PRIu32 "ms -> close", weather_panel_->idle_ms());
            CloseWeatherPanelLocked();
        }
        return;
    }

    // 옷장이 열려 있으면 무입력 10초만 재고 유휴 연출(하늘·엿보기·수면·끔)은 쉰다.
    if (wardrobe_ != nullptr) {
        if (wardrobe_->idle_ms() >= kWardrobeIdleMs) {
            ESP_LOGI(TAG, "Wardrobe: idle %" PRIu32 "ms -> close", wardrobe_->idle_ms());
            CloseWardrobeLocked();
        }
        return;
    }

    LoadUsageDatesLocked();
    UpdateSpecialDayLocked();

    // 0) 시각에 따른 하늘. SetSky 는 같은 값이면 사실상 공짜다(이미지 src 재지정).
    //    팩 미니카 외출 중엔 건드리지 않는다 — 돌아오면 usage_screen 이 실내로 되돌린다.
    if (usage_sky_auto_ && !usage_screen_->pack_riding()) {
        const UsageSky want = SkyForNow();
        if (want != usage_screen_->sky()) {
            ESP_LOGI(TAG, "Usage screen: sky -> %d by clock", want);
            usage_screen_->SetSky(want);
            ApplyAutoSetLocked();  // 시각 세트 (E절). 강수 중이면 강수 세트 유지.
        }
    }

    // 0) 탭 엿보기. 끝나면 그동안 밀린 상태를 되살린다. 나이(age)는 계속
    //    세어서, 엿보는 사이 만료됐을 상태(작업 60초 등)는 다음 틱이 바로 접는다.
    if (usage_peek_left_s_ > 0) {
        usage_peek_left_s_--;
        usage_state_age_s_++;
        if (usage_peek_left_s_ > 0) {
            return;
        }
        if (usage_pending_state_ != kUsageStateIdle) {
            ESP_LOGI(TAG, "Usage screen: peek over, restoring state %d", usage_pending_state_);
            usage_screen_->SetState(usage_pending_state_, usage_pending_ctx_);
        }
        usage_pending_state_ = kUsageStateIdle;
    }

    // 1) 상태 화면의 시간 복귀. 호스트는 이벤트가 있을 때만 보내므로
    //    "몇 초 보여주고 돌아가기"는 이 1초 틱이 굴린다.
    UsageState st = usage_screen_->state();
    if (st != kUsageStateIdle && st != kUsageStateSleeping) {
        usage_state_age_s_++;
        if (st == kUsageStateError && usage_state_age_s_ >= kUsageErrorHoldS) {
            usage_screen_->SetState(kUsageStateWorking, nullptr);
            usage_state_age_s_ = 0;
        } else if (st == kUsageStateDone && usage_state_age_s_ >= kUsageDoneHoldS) {
            usage_screen_->SetState(kUsageStateIdle, nullptr);
        } else if ((st == kUsageStateThinking || st == kUsageStateWorking ||
                    st == kUsageStateJuggling) &&
                   usage_state_age_s_ >= kUsageWorkingHoldS) {
            usage_screen_->SetState(kUsageStateIdle, nullptr);
        } else if (st == kUsageStatePermission &&
                   usage_state_age_s_ >= kUsagePermissionHoldS) {
            ESP_LOGW(TAG, "Usage screen: permission held %ds, giving up",
                     usage_state_age_s_);
            usage_screen_->SetState(kUsageStateIdle, nullptr);
        }
        st = usage_screen_->state();
        if (st != kUsageStateIdle) {
            return;  // 상태 화면 표시 중 — 유휴 연출(표정·수면·끔)은 쉰다
        }
    }

    // 2) 화면 끔 (30분)
    if (idle_seconds >= kUsageScreenOffAfterS) {
        if (!usage_backlight_off_) {
            // permanent=false 라 NVS 에 저장되지 않는다. 사용자의 밝기 설정을
            // 덮어쓰지 않고 껐다가 그대로 복구할 수 있다.
            ESP_LOGI(TAG, "Usage screen: turning screen off (idle %ds)", idle_seconds);
            if (backlight != nullptr) {
                backlight->SetBrightness(0, false);
            }
            usage_backlight_off_ = true;
            usage_screen_->PauseIdleWalk(true);  // 안 보이는 프레임은 디코드하지 않는다
        }
        return;  // 꺼진 뒤에는 아무것도 돌리지 않는다
    }
    if (usage_backlight_off_) {  // 유휴 시계가 줄었다 = 활동이 있었다
        if (backlight != nullptr) {
            backlight->RestoreBrightness();
        }
        usage_backlight_off_ = false;
        usage_screen_->PauseIdleWalk(false);
    }

    // 2.5) 밤 화면: 23~07시에 1분 동안 손대지 않으면 어둡게. 수면 전까지 화면 내용은 그대로다.
    // 깨우기는 수면과 같은 길이다 — 터치는 WakeIfScreenOff, 콕은 유휴 시계 초기화 + WakeIfScreenOff.
    if (idle_seconds >= kUsageNightDimAfterS && idle_seconds < kUsageSleepAfterS && UsageIsNightNow()) {
        if (!usage_dimmed_) {
            ESP_LOGI(TAG, "Usage screen: night dim (idle %ds)", idle_seconds);
            if (backlight != nullptr) {
                backlight->SetBrightness(kUsageSleepBrightness, false);
            }
            usage_dimmed_ = true;
        }
        if (st == kUsageStateSleeping) {
            usage_screen_->SetState(kUsageStateIdle, nullptr);
        }
        return;
    }

    // 3) 수면 (10분): 캐릭터가 자고 화면이 어두워진다
    if (idle_seconds >= kUsageSleepAfterS) {
        if (st != kUsageStateSleeping) {
            ESP_LOGI(TAG, "Usage screen: sleeping (idle %ds)", idle_seconds);
            usage_screen_->SetState(kUsageStateSleeping, nullptr);
            if (backlight != nullptr) {
                backlight->SetBrightness(kUsageSleepBrightness, false);
            }
            usage_dimmed_ = true;
        }
        return;
    }
    if (st == kUsageStateSleeping) {  // 기상
        ESP_LOGI(TAG, "Usage screen: waking from sleep");
        usage_screen_->SetState(kUsageStateIdle, nullptr);
    }
    if (usage_dimmed_) {
        if (backlight != nullptr) {
            backlight->RestoreBrightness();
        }
        usage_dimmed_ = false;
    }

    // 그 아래(10분 미만)는 사용량 화면 그대로다. 옛 5분 표정 셔플은 걷어냈다 —
    // 수면(캐릭터가 자는 화면)이 그 역할을 한다.
}

namespace {

// 상태 텍스트를 영문으로 바꾼다. 빈 문자열을 돌려주면 아예 숨긴다.
//
// strcmp 로 비교하는 이유: Lang::Strings 는 헤더의 constexpr 포인터라 번역
// 단위마다 주소가 다를 수 있어 포인터 비교가 성립하지 않는다. 대신 한글
// 문자열을 여기 직접 적지 않고 Lang::Strings 를 참조하므로, 나중에 언어
// 설정을 바꿔도 이 표는 그대로 동작한다.
const char* UsageStatusText(const char* status) {
    if (status == nullptr || status[0] == '\0') {
        return "";
    }
    struct StatusMap {
        const char* from;
        const char* to;
    };
    static const StatusMap kMap[] = {
        // 평소(대기)에는 아무것도 띄우지 않는다 — 화면이 깔끔한 편이 낫다.
        {Lang::Strings::STANDBY, ""},
        // 네트워크 붙는 동안의 문구는 전부 숨긴다 — 마을 화면이 대신 떠 있다.
        {Lang::Strings::CONNECTING, ""},
        {Lang::Strings::LISTENING, "Listening..."},
        {Lang::Strings::SPEAKING, "Speaking..."},
        {Lang::Strings::ERROR, "Error"},
        {Lang::Strings::PLEASE_WAIT, "Please wait..."},
        {Lang::Strings::INITIALIZING, ""},
        {Lang::Strings::REGISTERING_NETWORK, ""},
        {Lang::Strings::DETECTING_MODULE, ""},
        // WiFi 를 못 잡아 AP 설정 모드로 떨어졌을 때만은 알려야 한다.
        {Lang::Strings::WIFI_CONFIG_MODE, "WiFi setup mode - join the board's AP"},
        {Lang::Strings::CHECKING_NEW_VERSION, "Checking for new version..."},
        {Lang::Strings::ACTIVATION, "Activation"},
        {Lang::Strings::LOADING_PROTOCOL, "Logging in..."},
        {Lang::Strings::UPGRADING, "System is upgrading..."},
    };
    for (const auto& m : kMap) {
        if (m.from != nullptr && strcmp(status, m.from) == 0) {
            return m.to;
        }
    }
    // 표에 없는 문자열은 한글일 수 있다. 상태 라벨은 영문 전용 정적 폰트를
    // 쓰므로 그대로 띄우면 네모(두부)가 되니 차라리 숨긴다.
    return "";
}

}  // namespace

void LcdDisplay::SetStatus(const char* status) {
    if (usage_screen_ == nullptr) {
        LvglDisplay::SetStatus(status);
        return;
    }
    const char* text = UsageStatusText(status);
    LvglDisplay::SetStatus(text);
    // 빈 문자열이면 라벨 자체를 숨긴다. 스크롤 애니메이션까지 멈춰서
    // 대기 중에는 그리기 비용도 들지 않는다.
    DisplayLockGuard lock(this);
    if (status_label_ != nullptr) {
        if (text[0] == '\0') {
            lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LcdDisplay::SetClock(const char* time_str) {
    if (usage_screen_ != nullptr && usage_clock_label_ == nullptr) {
        // 사용량 화면 구성에서는 상단 바를 걷어냈다. status_label_ 에 쓰면 상태 텍스트가
        // 매 틱 "HH:MM" 으로 덮이므로, 사용량 화면 윗줄의 시계 칸에 넘긴다.
        // 시간이 맞기 전에는 lvgl_display 의 tm_year 검사가 여기까지 오지 않게 막는다.
        int hour = 0, minute = 0;
        if (time_str == nullptr || sscanf(time_str, "%d:%d", &hour, &minute) != 2) {
            return;
        }
        DisplayLockGuard lock(this);
        usage_screen_->SetClock(hour, minute);
        return;
    }
    DisplayLockGuard lock(this);
    if (usage_clock_label_ == nullptr) {
        // 사용량 화면이 없는 구성에서는 예전처럼 상태 텍스트 자리에 띄운다.
        if (status_label_ != nullptr) {
            lv_label_set_text(status_label_, time_str);
        }
        return;
    }
    lv_label_set_text(usage_clock_label_, time_str);
}

bool LcdDisplay::WakeIfScreenOff(bool by_touch) {
    // 화면이 꺼져 있거나(30분) 수면으로 어두울 때(10분) 입력을 가로챈다.
    // 표정 모드(5~10분)는 화면이 밝은 상태라 가로채지 않는다.
    if (usage_screen_ == nullptr) {
        return false;
    }
    if (usage_manual_off_ && !by_touch) {
        return false;  // 사람이 끈 화면은 데이터가 와도 그대로 둔다 (터치·권한/에러만 켠다)
    }
    const bool asleep = usage_screen_->state() == kUsageStateSleeping;
    if (!usage_backlight_off_ && !usage_dimmed_ && !asleep) {
        return false;
    }

    ESP_LOGI(TAG, "Usage screen: waking by touch (screen was %s)",
             usage_backlight_off_ ? "off" : "asleep");
    auto backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr) {
        backlight->RestoreBrightness();
    }
    usage_backlight_off_ = false;
    usage_manual_off_ = false;
    usage_dimmed_ = false;

    // 켜자마자 자는 얼굴만 덩그러니 있으면 이상하니 숫자도 같이 되살린다.
    DisplayLockGuard lock(this);
    if (asleep) {
        usage_screen_->SetState(kUsageStateIdle, nullptr);
    }
    usage_screen_->PauseIdleWalk(false);  // 화면 끔으로 멈췄던 걷기 재개
    return true;
}

void LcdDisplay::SetUsageStats(UsageAgent agent, const UsageStats& stats) {
    DisplayLockGuard lock(this);
    if (usage_screen_ == nullptr) {
        return;
    }
    usage_screen_->SetAgentStats(agent, stats);
}

void LcdDisplay::SetUsageState(const char* state, const char* ctx) {
    if (usage_screen_ == nullptr || state == nullptr) {
        return;
    }
    // sleeping 은 보드가 스스로 들어가는 상태라 표에 없다.
    struct NameMap {
        const char* name;
        UsageState state;
    };
    static const NameMap kMap[] = {
        {"idle", kUsageStateIdle},         {"thinking", kUsageStateThinking},
        {"working", kUsageStateWorking},   {"juggling", kUsageStateJuggling},
        {"permission", kUsageStatePermission}, {"error", kUsageStateError},
        {"done", kUsageStateDone},
    };
    for (const auto& m : kMap) {
        if (strcmp(state, m.name) == 0) {
            // 사람이 끈 화면이라도 권한 대기·에러는 알려야 하니 켠다.
            if (usage_manual_off_ &&
                (m.state == kUsageStatePermission || m.state == kUsageStateError)) {
                ESP_LOGI(TAG, "Usage screen: %s while manually off -> waking", m.name);
                WakeIfScreenOff(true);
            }
            DisplayLockGuard lock(this);
            strncpy(usage_last_ctx_, ctx != nullptr ? ctx : "", sizeof(usage_last_ctx_) - 1);
            usage_last_ctx_[sizeof(usage_last_ctx_) - 1] = '\0';
            usage_state_age_s_ = 0;
            if (usage_peek_left_s_ > 0) {
                // 엿보는 중 — 화면은 두고 마지막 값만 기억한다.
                usage_pending_state_ = m.state;
                strncpy(usage_pending_ctx_, usage_last_ctx_, sizeof(usage_pending_ctx_));
                return;
            }
            if (wardrobe_ != nullptr && m.state != kUsageStateIdle) {
                ESP_LOGI(TAG, "Wardrobe: closed by state %s", m.name);
                CloseWardrobeLocked();  // 권한 대기·에러가 언제나 이긴다
            }
            if (weather_panel_ != nullptr && m.state != kUsageStateIdle) {
                CloseWeatherPanelLocked();
            }
            if (diary_panel_ != nullptr && m.state != kUsageStateIdle) {
                CloseDiaryPanelLocked();
            }
            if (manbok_panel_ != nullptr && m.state != kUsageStateIdle) {
                CloseManbokPanelLocked();
            }
            if (m.state != kUsageStateIdle) usage_screen_->HideDiaryToast();
            usage_screen_->SetState(m.state, ctx);
            return;
        }
    }
    ESP_LOGW(TAG, "Unknown usage state: %s", state);
}

bool LcdDisplay::PeekIdleScreen() {
    if (usage_screen_ == nullptr) {
        return false;
    }
    DisplayLockGuard lock(this);
    const UsageState st = usage_screen_->state();
    if (usage_peek_left_s_ > 0) {
        usage_peek_left_s_ = kUsagePeekS;  // 또 탭하면 연장
        return true;
    }
    if (st == kUsageStateIdle || st == kUsageStateSleeping) {
        return false;  // 이미 대기 화면 (수면은 WakeIfScreenOff 가 처리)
    }
    ESP_LOGI(TAG, "Usage screen: peeking idle for %ds (state %d)", kUsagePeekS, st);
    usage_pending_state_ = st;
    strncpy(usage_pending_ctx_, usage_last_ctx_, sizeof(usage_pending_ctx_));
    usage_peek_left_s_ = kUsagePeekS;
    usage_screen_->SetState(kUsageStateIdle, nullptr);
    return true;
}

void LcdDisplay::OnTouch(const TouchEvent& ev) {
    if (usage_screen_ == nullptr) {
        return;
    }
    // 맥 연결 안내가 떠 있으면 탭 한 번 = 건너뛰기(맥 없이 보드만 쓰는 사람). 다음 부팅까지 안 뜬다.
    if (mac_guide_panel_ != nullptr) {
        if (ev.kind == TouchEvent::kTap) {
            HideMacSetupGuide();
            UsageReceiver::SkipMacSetupGuide();
        }
        return;
    }
    {
        DisplayLockGuard lock(this);
        bool screen_off = false;
        if (manbok_panel_ != nullptr) {
            const ManbokPanel::Result r = manbok_panel_->OnTouch(ev);
            if (r.close || r.open_diary || r.poke_phrase > 0 || r.trip >= 0) CloseManbokPanelLocked();
            if (r.open_diary) {
                diary_from_menu_ = true;
                OpenDiaryPanelLocked(-1);
            } else if (r.poke_phrase > 0) {
                SendPoke(r.poke_phrase);   // 옛 문구 패널 줄 탭과 같다
            } else if (r.trip == 0) {
                SendTrip();
            } else if (r.trip == 1) {
                SendTripInvite();
            } else if (r.trip == 2) {
                TripGoHome();
            }
            return;
        }
        if (diary_panel_ != nullptr) {
            const DiaryPanel::Result r = diary_panel_->OnTouch(ev);
            if (r.close || r.menu) {
                CloseDiaryPanelLocked();
            }
            if (r.menu) OpenManbokPanelLocked(ManbokPanel::Screen::kMenu);
            return;
        }
        if (weather_panel_ != nullptr) {
            if (weather_panel_->OnTouch(ev).close) {
                CloseWeatherPanelLocked();
            }
            return;
        }
        if (wardrobe_ != nullptr) {
            const WardrobeScreen::Result r = wardrobe_->OnTouch(ev);
            if (r.close) {
                CloseWardrobeLocked();   // 지금 캐릭터의 옷을 그 캐릭터 키에 확정한다
                if (r.character != nullptr) {
                    // CHAR 탭: 옷장을 닫고 캐릭터를 갈아탄다 + NVS 저장. 이미 락 안이라
                    // 락을 다시 잡지 않는 SetUsageCharacterLocked 를 쓴다.
                    SetUsageCharacterLocked(r.character, /*persist=*/true);
                }
            }
            screen_off = r.screen_off;
        } else if (usage_screen_->state() == kUsageStateIdle || usage_screen_->poke_replies_open()) {
            if (lv_tick_elaps(wardrobe_closed_tick_) < kWardrobeCloseGraceMs) {
                return;  // 옷장이 방금 닫혔다 — 옷장 UI 를 노리던 탭이 새 들어온 것
            }
            // 대기 화면(엿보기 중 포함): 놀람·쓰다듬기·화면 끄기·옷장은 UsageScreen 이 판정.
            const UsageScreen::TouchAction action = usage_screen_->OnTouch(ev);
            if (action == UsageScreen::TouchAction::kOpenWardrobe) {
                OpenWardrobeLocked();
                return;
            }
            if (action == UsageScreen::TouchAction::kOpenWeather) {
                OpenWeatherPanelLocked(WeatherPanel::kTabHourly);
                return;
            }
            if (action == UsageScreen::TouchAction::kOpenDiary) {
                OpenDiaryPanelLocked(usage_screen_->diary_open_entry());
                return;
            }
            if (action == UsageScreen::TouchAction::kOpenManbokMenu) {
                OpenManbokPanelLocked(ManbokPanel::Screen::kMenu);
                return;
            }
            if (action == UsageScreen::TouchAction::kAgentCycled) {
                SaveOutfitLocked();   // 배지 탭 = 선택 바뀜 → 재부팅해도 유지
                return;
            }
            if (action == UsageScreen::TouchAction::kPokeSend) {
                SendPoke(usage_screen_->poke_phrase());
                return;
            }
            if (action == UsageScreen::TouchAction::kTripGo) {
                SendTrip();
                return;
            }
            if (action == UsageScreen::TouchAction::kTripInvite) {
                SendTripInvite();
                return;
            }
            if (action == UsageScreen::TouchAction::kTripHome) {
                TripGoHome();
                return;
            }
            if (action == UsageScreen::TouchAction::kPokeReply) {
                SendPokeReply(usage_screen_->poke_reply());
                return;
            }
            if (action == UsageScreen::TouchAction::kSetCycled) {
                WardrobeSaveSet(usage_screen_->pack_set_key());   // 옷걸이 탭 = 세트 바뀜
                return;
            }
            screen_off = (action == UsageScreen::TouchAction::kScreenOff);
        } else {
            goto state_screen;
        }
        if (screen_off) {
            ESP_LOGI(TAG, "Usage screen: screen off by long press");
            auto backlight = Board::GetInstance().GetBacklight();
            if (backlight != nullptr) {
                backlight->SetBrightness(0, false);  // permanent=false: 밝기 설정은 안 건드린다
            }
            usage_backlight_off_ = true;
            usage_manual_off_ = true;
            usage_screen_->PauseIdleWalk(true);
        }
        return;
    }
state_screen:
    // 상태 화면: 탭은 예전처럼 30초 엿보기. 몸통 "바쁨" 눈은 상태 GIF 마다
    // 크기가 달라 1차에서 뺐다(docs/wardrobe-impl.md 3절).
    // 좌상 배지 영역만 예외 — 엿보기 대신 에이전트 전환 (docs/agent-badge.md §2).
    if (ev.kind == TouchEvent::kTap) {
        if (usage_screen_->HitBadge(ev.x0, ev.y0)) {
            DisplayLockGuard lock(this);
            usage_screen_->CycleAgent();
            SaveOutfitLocked();
            return;
        }
        PeekIdleScreen();
    }
}

bool LcdDisplay::SendPoke(int phrase) {
    if (usage_screen_ == nullptr || phrase < 0 || phrase > UI_POKE_PHRASE_COUNT) {
        return false;
    }
    if (!PokeLink::GetInstance().Send(static_cast<uint8_t>(phrase))) {
        ESP_LOGW(TAG, "poke not queued (phrase %d)", phrase);
        DisplayLockGuard lock(this);
        usage_screen_->ShowPokeStatus("NO PEER");   // 탭에 아무 반응이 없으면 고장처럼 보인다
        return false;
    }
    DisplayLockGuard lock(this);
    usage_screen_->ShowPokeStatus("POKE!");
    return true;
}

void LcdDisplay::ShowPokeResult(bool delivered) {
    if (usage_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    usage_screen_->ShowPokeStatus(delivered ? "SENT" : "NO PEER");
}

bool LcdDisplay::ShowPokeReceived(int phrase) {
    if (usage_screen_ == nullptr || phrase < 0 || phrase > UI_POKE_PHRASE_COUNT) {
        return false;
    }
    WakeIfScreenOff(true);   // 사람이 끈 화면도 켠다 — 사람이 보낸 하트다 (안에서 락)
    DisplayLockGuard lock(this);
    if (wardrobe_ != nullptr) {
        usage_poke_pending_ = phrase;
        return true;
    }
    CloseWeatherPanelLocked();   // 하트는 판 밑에 숨지 않게 판을 걷고 보인다
    CloseDiaryPanelLocked();
    CloseManbokPanelLocked();
    usage_screen_->ShowPokePop(static_cast<uint8_t>(phrase));
    return true;
}

bool LcdDisplay::SendPokeReply(int reply) {
    if (usage_screen_ == nullptr || reply < 1 || reply > UI_POKE_REPLY_COUNT) {
        return false;
    }
    const bool queued = PokeLink::GetInstance().SendReply(static_cast<uint8_t>(reply));
    DisplayLockGuard lock(this);
    usage_screen_->ShowPokeStatus(queued ? "POKE!" : "NO PEER");
    return queued;
}

bool LcdDisplay::ShowPokeReply(int reply) {
    if (usage_screen_ == nullptr || reply < 1 || reply > UI_POKE_REPLY_COUNT) {
        return false;
    }
    WakeIfScreenOff(true);
    DisplayLockGuard lock(this);
    if (wardrobe_ != nullptr) {
        usage_poke_pending_ = 100 + reply;
        return true;
    }
    CloseWeatherPanelLocked();   // 하트는 판 밑에 숨지 않게 판을 걷고 보인다
    CloseDiaryPanelLocked();
    CloseManbokPanelLocked();
    usage_screen_->ShowPokeReplyPop(static_cast<uint8_t>(reply));
    return true;
}

void LcdDisplay::ShowPokeHighFive() {
    if (usage_screen_ == nullptr) {
        return;
    }
    WakeIfScreenOff(true);
    DisplayLockGuard lock(this);
    if (wardrobe_ != nullptr) {
        usage_poke_pending_ = 200;
        return;
    }
    CloseWeatherPanelLocked();   // 하트는 판 밑에 숨지 않게 판을 걷고 보인다
    CloseDiaryPanelLocked();
    CloseManbokPanelLocked();
    usage_screen_->ShowHighFivePop();
}

bool LcdDisplay::SendTrip() {
    if (usage_screen_ == nullptr) {
        return false;
    }
    DisplayLockGuard lock(this);
    return SendTripLocked();
}

bool LcdDisplay::SendTripLocked() {
    if (PokeLink::GetInstance().TripAsking()) {
        usage_screen_->ShowPokeStatus("ASKING");   // 이미 묻는 중이다. 시한을 늘리지 않는다
        return true;
    }
    if (!usage_screen_->CanTrip()) {
        usage_screen_->ShowPokeStatus("BUSY");
        return false;
    }
    const uint8_t going = BboketerCastMask();
    const bool queued = PokeLink::GetInstance().SendTrip(going);
    usage_screen_->ShowPokeStatus(queued ? "ASKING" : "NO PEER");
    return queued;
}

bool LcdDisplay::SendTripInvite() {
    if (usage_screen_ == nullptr) {
        return false;
    }
    DisplayLockGuard lock(this);
    if (PokeLink::GetInstance().InviteWaiting()) {
        usage_screen_->ShowPokeStatus("CALLING");   // 이미 불렀다. 또 보내지 않는다
        return true;
    }
    if (wardrobe_ != nullptr || !usage_screen_->CanTrip() || PokeLink::GetInstance().TripAsking()) {
        usage_screen_->ShowPokeStatus("BUSY");   // 손님을 받을 수 없는 상태면 부르지 않는다
        return false;
    }
    const bool queued = PokeLink::GetInstance().SendTripInvite();
    usage_screen_->ShowPokeStatus(queued ? "CALLING" : "NO PEER");
    return queued;
}

bool LcdDisplay::AcceptTripInvite() {
    if (usage_screen_ == nullptr) {
        return false;
    }
    WakeIfScreenOff(true);   // 사람이 부른 것이다
    DisplayLockGuard lock(this);
    if (wardrobe_ != nullptr || !usage_screen_->CanTrip() || PokeLink::GetInstance().TripAsking()) {
        return false;
    }
    return SendTripLocked();
}

bool LcdDisplay::TripGoHome() {
    if (usage_screen_ == nullptr) {
        return false;
    }
    DisplayLockGuard lock(this);
    if (usage_screen_->SendGuestHome()) {
        usage_screen_->ShowPokeStatus("BYE!");
        return true;
    }
    if (!usage_screen_->TripRecall()) {
        return false;   // 아무도 놀러 가 있지 않다
    }
    PokeLink::GetInstance().SendTripCallHome();
    usage_screen_->ShowPokeStatus("COME HOME");
    return true;
}

bool LcdDisplay::SendGuestHome() {
    if (usage_screen_ == nullptr) {
        return false;
    }
    DisplayLockGuard lock(this);
    return usage_screen_->SendGuestHome();
}

bool LcdDisplay::CanHostTrip() {
    if (usage_screen_ == nullptr) {
        return false;
    }
    WakeIfScreenOff(true);
    DisplayLockGuard lock(this);
    return wardrobe_ == nullptr && usage_screen_->CanTrip();
}

uint8_t LcdDisplay::TripNewGuests(uint8_t mask) {
    if (usage_screen_ == nullptr) {
        return 0;
    }
    DisplayLockGuard lock(this);
    return mask & ~usage_screen_->CastDrawMask();
}

void LcdDisplay::TripWave() {
    if (usage_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    usage_screen_->TripWave();
}

bool LcdDisplay::SetUsageCast(int mask) {
    if (usage_screen_ == nullptr || mask < 1 || mask > 7) {
        return false;
    }
    DisplayLockGuard lock(this);
    BboketerSetCastMask(static_cast<uint8_t>(mask));
    BboketerCastSave();
    usage_screen_->CastChanged();
    return true;
}

bool LcdDisplay::ShowTripAnswer(bool answered, uint8_t value) {
    if (usage_screen_ == nullptr) {
        return false;
    }
    DisplayLockGuard lock(this);
    if (!answered) {
        usage_screen_->ShowPokeStatus("NO PEER");
        return false;
    }
    if (value == kTripAnswerSame) {
        usage_screen_->TripWave();
        return false;
    }
    if (value < 1 || value > 7) {
        usage_screen_->ShowPokeStatus("BUSY");
        return false;
    }
    if (!usage_screen_->StartTripLeave(value)) {
        usage_screen_->ShowPokeStatus("BUSY");   // 답을 기다리는 사이 내 화면이 대기 화면을 벗어났다
        return false;
    }
    usage_screen_->ShowPokeStatus("BYE!");
    return true;
}

bool LcdDisplay::ShowTripGuest(int character) {
    if (usage_screen_ == nullptr || character < 1 || character > 7) {
        return false;
    }
    DisplayLockGuard lock(this);
    if (wardrobe_ != nullptr) {
        return false;
    }
    CloseWeatherPanelLocked();   // 손님이 판 밑에 와 있지 않게
    CloseDiaryPanelLocked();
    CloseManbokPanelLocked();
    return usage_screen_->StartTripGuest(static_cast<uint8_t>(character));
}

void LcdDisplay::ShowTripHome() {
    if (usage_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    usage_screen_->StartTripReturn();
}

namespace {
int TodayKey() {
    const time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    return tm_now.tm_mday * 100 + tm_now.tm_mon;
}
}  // namespace

bool LcdDisplay::SetUsageDate(const char* key, const char* value) {
    if (usage_screen_ == nullptr || key == nullptr || value == nullptr) {
        return false;
    }
    const bool with_year = strcmp(key, "due") == 0;
    if (!UsageDateValid(value, with_year)) {
        ESP_LOGW(TAG, "bad date for %s", key);
        return false;
    }
    WardrobeSaveDate(key, value);
    DisplayLockGuard lock(this);
    usage_dates_loaded_ = false;
    LoadUsageDatesLocked();
    return true;
}

void LcdDisplay::LoadUsageDatesLocked() {
    if (usage_dates_loaded_) {
        return;
    }
    usage_dates_loaded_ = true;
    usage_birthday_ = WardrobeLoadDate("birthday");
    usage_birthday2_ = WardrobeLoadDate("birthday2");
    usage_anniversary_ = WardrobeLoadDate("anniversary");
    usage_screen_->ticker().SetDue(WardrobeLoadDate("due"));
    usage_special_shown_ = -2;   // 다음 판정이 다시 그리게
}

void LcdDisplay::SetSpecialDayTest(int kind) {
    if (usage_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    usage_special_test_ = kind;
    UpdateSpecialDayLocked();
}

void LcdDisplay::UpdateSpecialDayLocked() {
    static const char* const kNotes[6] = {"생일 축하해", "결혼기념일 축하해", "새해 복 많이 받아", "풍성한 한가위",
                                          "달콤한 빼빼로데이", "메리 크리스마스"};
    int kind = static_cast<int>(UsageSpecialDayToday(usage_birthday_, usage_birthday2_, usage_anniversary_));
    if (usage_special_test_ != -1) {
        kind = usage_special_test_;   // 시험 주입 {"special":n}. -2 는 기념일 없는 날처럼 그린다
        if (kind < -1) kind = -1;
    } else if (kind >= 0 && usage_special_skip_day_ == TodayKey()) {
        kind = -1;   // 오늘은 옷장에서 고른 배경을 쓴다
    }
    if (kind == usage_special_shown_) {
        return;
    }
    usage_special_shown_ = kind;
    usage_screen_->SetSpecialBg(kind >= 0 ? UsageSpecialBgImage(kind) : nullptr);
    usage_screen_->ticker().SetNote(kind >= 0 ? kNotes[kind] : nullptr);
}

void LcdDisplay::FlushPokePendingLocked() {
    const int p = usage_poke_pending_;
    usage_poke_pending_ = -1;
    if (p == 200) {
        usage_screen_->ShowHighFivePop();
    } else if (p > 100) {
        usage_screen_->ShowPokeReplyPop(static_cast<uint8_t>(p - 100));
    } else if (p >= 0) {
        usage_screen_->ShowPokePop(static_cast<uint8_t>(p));
    }
}

void LcdDisplay::SaveOutfitLocked() {
    WardrobeSaveOutfit(usage_outfit_, usage_outfit_locked_, usage_screen_->sky(),
                       usage_screen_->weather_mode(), usage_screen_->agent());
}

bool LcdDisplay::SetUsageAgent(const char* key, bool persist) {
    if (usage_screen_ == nullptr || key == nullptr) {
        return false;
    }
    UsageAgent agent;
    if (!AgentByKey(key, &agent)) {
        ESP_LOGW(TAG, "agent select rejected: %s", key);
        return false;
    }
    DisplayLockGuard lock(this);
    usage_screen_->SetAgent(agent, /*toast=*/persist);
    if (persist) {
        SaveOutfitLocked();
    }
    return true;
}

UsageAgent LcdDisplay::GetUsageAgent() {
    if (usage_screen_ == nullptr) {
        return kUsageAgentClaude;
    }
    DisplayLockGuard lock(this);
    return usage_screen_->agent();
}

bool LcdDisplay::SetUsageSet(const char* key, bool persist) {
    if (usage_screen_ == nullptr || key == nullptr) {
        return false;
    }
    DisplayLockGuard lock(this);
    if (!usage_screen_->SetPackSet(key, /*toast=*/persist)) {
        return false;
    }
    if (persist) {
        WardrobeSaveSet(usage_screen_->pack_set_key());
    }
    return true;
}

const char* LcdDisplay::GetUsageSet() {
    if (usage_screen_ == nullptr) {
        return nullptr;
    }
    DisplayLockGuard lock(this);
    return usage_screen_->pack_set_key();   // 테이블 상수 문자열 — 락 밖에서 읽어도 안전
}

bool LcdDisplay::SetUsageCharacter(const char* key, bool persist) {
    if (usage_screen_ == nullptr) {
        return false;
    }
    DisplayLockGuard lock(this);
    if (wardrobe_ != nullptr) {
        CloseWardrobeLocked();   // 지금 캐릭터의 옷을 먼저 그 캐릭터 키에 확정한다
    }
    return SetUsageCharacterLocked(key, persist);
}

// 락을 이미 쥔 경로용(옷장 CHAR 탭). 옷장은 호출 전에 닫혀 있어야 한다 —
// 미리보기 몸도 아이템 표도 옛 캐릭터에 묶여 있다.
bool LcdDisplay::SetUsageCharacterLocked(const char* key, bool persist) {
    Character c = Character::kPack;
    if (!CharacterByKey(key, &c)) {
        ESP_LOGW(TAG, "unknown character: %s", key != nullptr ? key : "?");
        return false;
    }
    if (usage_screen_ == nullptr) {
        return false;
    }
    if (!usage_screen_->SetCharacter(c)) {
        return false;
    }

    // 새 캐릭터의 착장을 **그 캐릭터 키**에서 읽어 입힌다 — 아이템 표가 달라 인덱스를
    // 물려줄 수 없다. bg·wx·agent 는 지금 것을 유지한다(캐릭터와 무관한 값이고, 여기서
    // 하늘까지 갈아치우면 전환이 배경 변경까지 겸하게 된다).
    Outfit outfit;
    bool locked = false;
    UsageSky stored_sky = kUsageSkyNavy;
    uint8_t stored_wx = 0;
    UsageAgent stored_agent = kUsageAgentClaude;
    const bool has_record =
        WardrobeLoadOutfit(&outfit, &locked, &stored_sky, &stored_wx, &stored_agent);
    if (has_record) {
        usage_outfit_ = outfit;
        usage_outfit_locked_ = locked;
    } else {
        usage_outfit_ = Outfit{};
        usage_outfit_locked_ = false;
    }
    // 지금 배경이 이 캐릭터에 없으면 옮긴다 — 저장된 배경이 맞으면 그것, 아니면 기본 배경.
    if (!SkyFitsCharacter(usage_screen_->sky())) {
        const bool use_stored = has_record && SkyFitsCharacter(stored_sky);
        const UsageSky next = use_stored ? stored_sky : SkyForNow();
        usage_sky_auto_ = !usage_outfit_locked_ && IsAutoSky(next);
        usage_screen_->SetSky(next);
    }
    const bool bare = usage_outfit_.head == 0 && usage_outfit_.face == 0 &&
                      usage_outfit_.hand == 0 && usage_outfit_.fx == 0;
    if (bare && !usage_outfit_locked_) {
        // 처음 온 캐릭터(또는 캐릭터를 나누기 전 기록에 이 캐릭터의 슬롯이 없던 경우)는
        // 벗은 채로 두지 않고 지금 배경의 AUTO 세트를 입힌다.
        ApplyAutoSetLocked();
    } else {
        usage_screen_->SetOutfit(usage_outfit_);
    }
    // 저장된 착장 세트를 되살린다 (SetCharacter 는 pack.json 기본값으로 둔다).
    const std::string set_key = WardrobeLoadSet();
    if (!set_key.empty()) {
        usage_screen_->SetPackSet(set_key.c_str(), /*toast=*/false);
    }
    if (persist) {
        WardrobeSaveCharacter(CharacterKey(c));
    }
    ESP_LOGI(TAG, "Usage character: %s (head=%u face=%u hand=%u fx=%u %s%s)", CharacterKey(c),
             usage_outfit_.head, usage_outfit_.face, usage_outfit_.hand, usage_outfit_.fx,
             usage_outfit_locked_ ? "LOCK" : "AUTO", persist ? " (saved)" : "");
    return true;
}

const char* LcdDisplay::GetUsageCharacter() { return CharacterKey(CurrentCharacter()); }

void LcdDisplay::SetUsageWeatherFx(const char* kind) {
    if (usage_screen_ == nullptr || kind == nullptr) {
        return;
    }
    uint8_t fx = 0;
    if (strcmp(kind, "rain") == 0) fx = 1;
    else if (strcmp(kind, "snow") == 0) fx = 2;
    DisplayLockGuard lock(this);
    usage_precip_ = fx;   // AUTO 세트의 강수 우선 판정용 (ApplyAutoSetLocked)
    usage_screen_->SetWeatherFx(fx);
}

void LcdDisplay::SetUsageWeatherInfo(const char* kind, bool has_temp, int temp, bool night) {
    if (usage_screen_ == nullptr || kind == nullptr) {
        return;
    }
    const UsageWeatherIcon icon = UsageWeatherIconFor(kind, night);   // 모르는 kind 는 그림 없이 기온만
    DisplayLockGuard lock(this);
    usage_screen_->SetWeatherInfo(icon, has_temp, temp);
}

void LcdDisplay::SetUsageForecast(const WeatherForecast& forecast) {
    if (usage_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    usage_forecast_ = forecast;
    if (weather_panel_ != nullptr) {
        weather_panel_->SetData(usage_forecast_);
    }
}

bool LcdDisplay::ShowWeatherPanel(int tab) {
    if (usage_screen_ == nullptr || tab < -1 || tab >= WeatherPanel::kTabCount) {
        return false;
    }
    DisplayLockGuard lock(this);
    if (tab < 0) {
        CloseWeatherPanelLocked();
        return true;
    }
    if (weather_panel_ != nullptr) {
        weather_panel_->SetTab(static_cast<WeatherPanel::Tab>(tab));
        return true;
    }
    if (wardrobe_ != nullptr || usage_screen_->state() != kUsageStateIdle) {
        return false;
    }
    OpenWeatherPanelLocked(static_cast<WeatherPanel::Tab>(tab));
    return weather_panel_ != nullptr;
}

// 옷장과 같은 틀: 걷기·배지를 멈추고 판을 container_ 맨 위에 덮는다.
void LcdDisplay::OpenWeatherPanelLocked(WeatherPanel::Tab tab) {
    if (usage_screen_ == nullptr || weather_panel_ != nullptr || wardrobe_ != nullptr || diary_panel_ != nullptr ||
        manbok_panel_ != nullptr) {
        return;
    }
    if (usage_screen_->state() != kUsageStateIdle) {
        return;
    }
    usage_screen_->PauseIdleWalk(true);
    weather_panel_ = std::make_unique<WeatherPanel>(container_, usage_screen_->ticker().font(), usage_forecast_, tab);
}

void LcdDisplay::CloseWeatherPanelLocked() {
    if (weather_panel_ == nullptr) {
        return;
    }
    weather_panel_.reset();
    ESP_LOGI(TAG, "Weather panel: closed");
    wardrobe_closed_tick_ = lv_tick_get();   // 직후 탭 무시 창 (OnTouch 그레이스)
    usage_screen_->PauseIdleWalk(false);
}

// --- 만복이 육아일기 (docs/manbok-diary.md) ------------------------------------------

void LcdDisplay::SetDiary(const DiaryData& data) {
    if (usage_screen_ == nullptr) return;
    DisplayLockGuard lock(this);
    usage_diary_ = data;
    const bool has = !usage_diary_.entries.empty();
    usage_screen_->SetDiaryAvailable(has);
    // 흐르는 줄은 맨 위 글이다. 새 일기 알림이 오면(ShowDiaryNew, 이 뒤에 불림) 그 글로 바뀐다.
    diary_focus_id_ = has ? usage_diary_.entries.front().id : std::string();
    usage_screen_->ticker().SetDiaryLine(has ? DiaryTickerLine(usage_diary_.entries.front()) : std::string(), false);
    if (diary_panel_ != nullptr) diary_panel_->SetData(usage_diary_);
    if (manbok_panel_ != nullptr) manbok_panel_->SetDiaryCount(static_cast<int>(usage_diary_.entries.size()));
}

void LcdDisplay::SetDiaryPhoto(const DiaryPhotoPtr& photo) {
    if (usage_screen_ == nullptr || !photo) return;
    DisplayLockGuard lock(this);
    diary_photos_.erase(std::remove_if(diary_photos_.begin(), diary_photos_.end(),
                                       [&](const DiaryPhotoPtr& p) { return !p || p->key == photo->key; }),
                        diary_photos_.end());
    diary_photos_.push_back(photo);
    if (diary_photos_.size() > kDiaryPhotoCache) diary_photos_.erase(diary_photos_.begin());
    if (diary_panel_ != nullptr) diary_panel_->SetPhoto(photo);
    if (manbok_panel_ != nullptr) manbok_panel_->SetPhoto(photo);
}

void LcdDisplay::DiaryPhotoFailed(const std::string& key) {
    if (usage_screen_ == nullptr) return;
    DisplayLockGuard lock(this);
    if (diary_panel_ != nullptr) diary_panel_->PhotoFailed(key);
    if (manbok_panel_ != nullptr) manbok_panel_->PhotoFailed(key);
}

void LcdDisplay::ShowDiaryNew(const DiaryEntry& entry) {
    if (usage_screen_ == nullptr) return;
    DisplayLockGuard lock(this);
    diary_focus_id_ = entry.id;   // 알림·흐르는 줄을 누르면 이 글
    usage_screen_->ticker().SetDiaryLine(DiaryTickerLine(entry), true);
    if (usage_screen_->state() == kUsageStateIdle && wardrobe_ == nullptr && weather_panel_ == nullptr &&
        diary_panel_ == nullptr && manbok_panel_ == nullptr) {
        usage_screen_->ShowDiaryToast(std::string(DiaryWriterName(entry.writer)) + "가 일기를 썼어요");
    }
}

bool LcdDisplay::ShowDiaryNewTest() {
    DiaryEntry top;
    {
        DisplayLockGuard lock(this);
        if (usage_diary_.entries.empty()) return false;
        top = usage_diary_.entries.front();
    }
    ShowDiaryNew(top);
    return true;
}

bool LcdDisplay::ShowDiaryPanel(int n) {
    if (usage_screen_ == nullptr) return false;
    DisplayLockGuard lock(this);
    if (n < 0) {
        CloseDiaryPanelLocked();
        return true;
    }
    CloseDiaryPanelLocked();
    CloseManbokPanelLocked();
    if (wardrobe_ != nullptr || weather_panel_ != nullptr || usage_screen_->state() != kUsageStateIdle) return false;
    OpenDiaryPanelLocked(n - 1);
    return diary_panel_ != nullptr;
}

void LcdDisplay::OpenDiaryPanelLocked(int entry) {
    const bool from_menu = diary_from_menu_;
    diary_from_menu_ = false;
    if (usage_screen_ == nullptr || diary_panel_ != nullptr || weather_panel_ != nullptr || wardrobe_ != nullptr ||
        manbok_panel_ != nullptr) {
        return;
    }
    if (usage_screen_->state() != kUsageStateIdle) return;
    const lv_font_t* font = usage_screen_->ticker().font();
    if (font == nullptr) return;   // 한글 글꼴이 없는 빌드
    if (entry == -2) {   // 흐르는 줄·새 일기 알림: 그 글을 id 로 찾는다
        entry = usage_diary_.entries.empty() ? -1 : 0;
        for (size_t i = 0; i < usage_diary_.entries.size(); i++) {
            if (usage_diary_.entries[i].id == diary_focus_id_) entry = static_cast<int>(i);
        }
    }
    usage_screen_->PauseIdleWalk(true);
    diary_panel_ = std::make_unique<DiaryPanel>(
        container_, font, usage_diary_, usage_screen_->ticker().due_days(), entry, diary_photos_,
        [](const std::string& url, const std::string& key) { DiaryFetcher::GetInstance().RequestPhoto(url, key); },
        from_menu);
}

void LcdDisplay::CloseDiaryPanelLocked() {
    if (diary_panel_ == nullptr) return;
    diary_panel_.reset();
    ESP_LOGI(TAG, "Diary panel: closed");
    wardrobe_closed_tick_ = lv_tick_get();   // 직후 탭 무시 창 (OnTouch 그레이스)
    usage_screen_->PauseIdleWalk(false);
}

// --- 만복이 메뉴 (docs/manbok-diary.md) ------------------------------------------------

void LcdDisplay::SetManbok(const ManbokData& data) {
    if (usage_screen_ == nullptr) return;
    DisplayLockGuard lock(this);
    usage_manbok_ = data;
    if (manbok_panel_ != nullptr) manbok_panel_->SetData(usage_manbok_);
}

void LcdDisplay::SetManbokMenuEnabled(bool enabled) {
    if (usage_screen_ == nullptr) return;
    DisplayLockGuard lock(this);
    usage_screen_->SetManbokMenu(enabled);
}

bool LcdDisplay::ShowManbokPanel(int screen) {
    if (usage_screen_ == nullptr) return false;
    DisplayLockGuard lock(this);
    CloseManbokPanelLocked();
    if (screen < 0) return true;
    CloseDiaryPanelLocked();
    if (wardrobe_ != nullptr || weather_panel_ != nullptr || usage_screen_->state() != kUsageStateIdle) return false;
    OpenManbokPanelLocked(static_cast<ManbokPanel::Screen>(screen));
    return manbok_panel_ != nullptr;
}

void LcdDisplay::OpenManbokPanelLocked(ManbokPanel::Screen screen) {
    if (usage_screen_ == nullptr || manbok_panel_ != nullptr || diary_panel_ != nullptr || weather_panel_ != nullptr ||
        wardrobe_ != nullptr) {
        return;
    }
    if (usage_screen_->state() != kUsageStateIdle) return;
    const lv_font_t* font = usage_screen_->ticker().font();
    if (font == nullptr) return;   // 한글 글꼴이 없는 빌드
    usage_screen_->PauseIdleWalk(true);
    ManbokPanel::Hooks hooks;
    hooks.need_photo = [](const std::string& url, const std::string& key) {
        DiaryFetcher::GetInstance().RequestPhoto(url, key);
    };
    hooks.want_parts = []() { DiaryFetcher::GetInstance().RequestParts(); };
    UsageScreen* screen_ptr = usage_screen_.get();
    hooks.trip_on = [screen_ptr](bool on[3]) { screen_ptr->TripRowsOn(on); };
    manbok_panel_ = std::make_unique<ManbokPanel>(container_, font, usage_manbok_,
                                                  usage_diary_.valid ? static_cast<int>(usage_diary_.entries.size()) : -1,
                                                  diary_photos_, std::move(hooks), screen);
}

void LcdDisplay::CloseManbokPanelLocked() {
    if (manbok_panel_ == nullptr) return;
    manbok_panel_.reset();
    ESP_LOGI(TAG, "Manbok panel: closed");
    wardrobe_closed_tick_ = lv_tick_get();   // 직후 탭 무시 창 (OnTouch 그레이스)
    usage_screen_->PauseIdleWalk(false);
}

void LcdDisplay::ApplyAutoSetLocked() {
    if (usage_screen_ == nullptr || usage_outfit_locked_) {
        return;
    }
    uint8_t wx = 0;
    AutoOutfitSet(usage_screen_->sky(), usage_precip_, &usage_outfit_, &wx);
    usage_screen_->SetOutfit(usage_outfit_);
    usage_screen_->SetWeatherMode(wx);
    ESP_LOGI(TAG, "Auto set: sky=%d precip=%u -> head=%u face=%u hand=%u fx=%u wx=%u",
             static_cast<int>(usage_screen_->sky()), usage_precip_, usage_outfit_.head,
             usage_outfit_.face, usage_outfit_.hand, usage_outfit_.fx, wx);
}

void LcdDisplay::ApplyUsageAutoSet() {
    if (usage_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    ApplyAutoSetLocked();
}

void LcdDisplay::SetUsageSky(const char* sky) {
    if (usage_screen_ == nullptr || sky == nullptr) {
        return;
    }
    if (strcmp(sky, "auto") == 0) {
        DisplayLockGuard lock(this);
        usage_sky_auto_ = true;
        usage_screen_->SetSky(SkyForNow());
        return;
    }
    // 키 문자열 ↔ enum 은 옷장 BG 테이블이 정본이다 (12종 전부).
    UsageSky parsed;
    if (SkyByKey(sky, &parsed)) {
        if (!SkyFitsCharacter(parsed)) {
            ESP_LOGW(TAG, "sky %s is not for this character", sky);
            return;
        }
        DisplayLockGuard lock(this);
        usage_sky_auto_ = false;
        usage_screen_->SetSky(parsed);
        return;
    }
    ESP_LOGW(TAG, "Unknown usage sky: %s", sky);
}

// --- 옷장 -------------------------------------------------------------------

void LcdDisplay::OpenWardrobeLocked() {
    if (usage_screen_ == nullptr || wardrobe_ != nullptr || diary_panel_ != nullptr || manbok_panel_ != nullptr) {
        return;
    }
    if (usage_screen_->state() != kUsageStateIdle) {
        return;  // 상태 화면 위에는 안 연다 — ToggleWardrobe 가 엿보기로 돌린다
    }
    usage_screen_->PauseIdleWalk(true);  // 가려진 걷기 프레임을 디코드하지 않는다
    // 미리보기 몸 = 팩 idle 프레임 0.
    const lv_image_dsc_t* body = usage_screen_->pack_idle_frame();
    wardrobe_ = std::make_unique<WardrobeScreen>(container_, width_, height_, usage_outfit_,
                                                 usage_screen_->sky(), usage_outfit_locked_,
                                                 usage_screen_->weather_mode(), body);
}

void LcdDisplay::CloseWardrobeLocked() {
    if (wardrobe_ == nullptr) {
        return;
    }
    usage_outfit_ = wardrobe_->outfit();
    usage_outfit_locked_ = wardrobe_->locked();
    const bool cast_changed = wardrobe_->cast_changed();
    const UsageSky sky = wardrobe_->sky();
    const uint8_t wx = wardrobe_->wx();
    wardrobe_.reset();
    if (usage_screen_->special_bg_active() && sky != usage_screen_->sky()) {
        usage_special_skip_day_ = TodayKey();   // 기념일이어도 직접 고른 배경이 우선이다
    }
    wardrobe_closed_tick_ = lv_tick_get();   // 직후 탭 무시 창 (OnTouch 그레이스)
    usage_screen_->SetOutfit(usage_outfit_);
    usage_screen_->SetWeatherMode(wx);
    // LOCK 이면 고른 하늘을 못 박는다. AUTO 면 (E절) auto 하늘(IsAutoSky: 마을 4종, 팩은
    // 카페·방)은 시각을 따르고 나머지 장면은 고른 그대로 두되, 어느 쪽이든 그 하늘의 세트를 통째로 입힌다.
    if (usage_outfit_locked_) {
        usage_sky_auto_ = false;
        usage_screen_->SetSky(sky);
    } else if (IsAutoSky(sky)) {
        usage_sky_auto_ = true;
        usage_screen_->SetSky(SkyForNow());
    } else {
        usage_sky_auto_ = false;
        usage_screen_->SetSky(sky);
    }
    ESP_LOGI(TAG, "Wardrobe: closed (head=%u face=%u hand=%u fx=%u sky=%d wx=%u %s)", usage_outfit_.head,
             usage_outfit_.face, usage_outfit_.hand, usage_outfit_.fx, static_cast<int>(sky), wx,
             usage_outfit_locked_ ? "LOCK" : "AUTO");
    WardrobeSaveOutfit(usage_outfit_, usage_outfit_locked_, sky, wx,
                       usage_screen_->agent());  // 재부팅해도 입은 채로
    BboketerCastSave();
    if (cast_changed) {
        usage_screen_->CastChanged();
    }
    // AUTO 세트는 옷장 **안**에서 BG 픽·AUTO 토글 때 이미 선택에 반영됐다
    // (WardrobeScreen::ApplyAutoSetPreview). 여기서 또 입히면 사용자가 세트
    // 위에 개별 수정한 슬롯을 덮어버린다 — 닫기에서는 적용하지 않는다.
    usage_screen_->PauseIdleWalk(false);  // idle 이 아니면 안에서 무시된다
    FlushPokePendingLocked();
}

void LcdDisplay::CommitWardrobe() {
    DisplayLockGuard lock(this);
    CloseWardrobeLocked();
}

void LcdDisplay::SetUsageOutfit(const Outfit& outfit, int locked, int sky, bool persist, int wx) {
    if (usage_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    if (wx >= 0 && wx < kWxItemCount) {
        usage_screen_->SetWeatherMode(static_cast<uint8_t>(wx));
    }
    if (wardrobe_ != nullptr) {
        // 안전망 — outfit push 경로는 CommitWardrobe 로 이미 닫고 온다. 다른
        // 경로로 옷장이 열린 채 들어오면 고르던 것은 버리고 닫는다.
        wardrobe_.reset();
        BboketerCastLoad();                      // 고르던 캐릭터·옷도 저장값으로 되돌린다
        wardrobe_closed_tick_ = lv_tick_get();   // 직후 탭 무시 창 (OnTouch 그레이스)
        usage_screen_->PauseIdleWalk(false);
    }
    usage_outfit_ = outfit;
    if (locked >= 0) {
        usage_outfit_locked_ = (locked != 0);
    }
    usage_screen_->SetOutfit(usage_outfit_);
    if (sky >= 0 && sky < kUsageSkyCount && SkyFitsCharacter(static_cast<UsageSky>(sky))) {
        usage_sky_auto_ = false;
        usage_screen_->SetSky(static_cast<UsageSky>(sky));
    } else if (locked == 0) {
        usage_sky_auto_ = true;  // AUTO 로 풀면 하늘도 시각을 따라간다 (옷장 닫기와 같은 규칙)
    }
    if (persist) {
        SaveOutfitLocked();
    }
    ESP_LOGI(TAG, "Usage outfit: head=%u face=%u hand=%u fx=%u %s%s", usage_outfit_.head,
             usage_outfit_.face, usage_outfit_.hand, usage_outfit_.fx,
             usage_outfit_locked_ ? "LOCK" : "AUTO", persist ? " (saved)" : "");
}

Outfit LcdDisplay::GetUsageOutfit() {
    DisplayLockGuard lock(this);
    return usage_outfit_;
}

bool LcdDisplay::IsUsageOutfitLocked() {
    DisplayLockGuard lock(this);
    return usage_outfit_locked_;
}

bool LcdDisplay::TriggerUsageActivity(const char* key) {
    if (usage_screen_ == nullptr || key == nullptr || strcmp(key, "ride") != 0) {
        return false;
    }
    DisplayLockGuard lock(this);
    return usage_screen_->TriggerPackRide();
}


void LcdDisplay::ToggleWardrobe() {
    if (usage_screen_ == nullptr) {
        return;
    }
    if (WakeIfScreenOff(true)) {
        return;  // 꺼진 화면은 켜는 데만 쓴다
    }
    {
        DisplayLockGuard lock(this);
        if (weather_panel_ != nullptr) {
            CloseWeatherPanelLocked();   // BOOT 은 덮인 판부터 닫는다
            return;
        }
        if (diary_panel_ != nullptr) {
            CloseDiaryPanelLocked();
            return;
        }
        if (manbok_panel_ != nullptr) {
            CloseManbokPanelLocked();
            return;
        }
        if (wardrobe_ != nullptr) {
            ESP_LOGI(TAG, "Wardrobe: close (BOOT)");
            CloseWardrobeLocked();
            return;
        }
        if (usage_screen_->state() == kUsageStateIdle) {
            ESP_LOGI(TAG, "Wardrobe: open (BOOT)");
            OpenWardrobeLocked();
            return;
        }
    }
    PeekIdleScreen();  // 상태 화면이면 30초 엿보기 (안에서 락을 잡는다)
}

// ---------------------------------------------------------------------------
// WiFi 설정 모드 온보딩 가이드 — 모르는 망(자격증명 없음)에 떨어져
// WIFI_CONFIG_MODE 로 들어갔을 때의 첫-연결 안내 (docs/alive.md 참고).
// 활성 스크린 최상단 전체 화면 오버레이라 기존 Alert·사용량 화면을 덮는다.
// 픽셀 폰트는 ASCII 전용이므로 카피는 전부 영문. 팔레트는 usage_screen 과 동일.
// ---------------------------------------------------------------------------

namespace {
constexpr uint32_t kGuideBg = 0x161008;       // 대기 화면과 같은 어두운 바탕
constexpr uint32_t kGuideAccent = 0xFF8A4C;   // 제목·스텝 번호 칩
constexpr uint32_t kGuideStepLabel = 0x9C7B5E;
constexpr uint32_t kGuideValue = 0xF0E7DD;
constexpr uint32_t kGuideFoot = 0xB08A6E;
constexpr uint32_t kGuideChipBg = 0x0C0804;  // 말풍선 배경 (상태 칩과 동일)

void GuideStylePlain(lv_obj_t* obj, uint32_t color) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t* GuidePixelLabel(lv_obj_t* parent, const char* text, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &press_start_8, 0);
    lv_obj_set_style_text_letter_space(label, 1, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}
}  // namespace

void LcdDisplay::ShowWifiConfigGuide(const char* ap_ssid, const char* web_url) {
    WakeIfScreenOff(false);  // 꺼진/어두운 화면이면 먼저 켠다 (안에서 락)

    DisplayLockGuard lock(this);
    wifi_guide_figure_.reset();  // 패널보다 먼저 — 소멸자가 산 lv_obj 를 지운다
    if (wifi_guide_panel_ != nullptr) {
        lv_obj_del(wifi_guide_panel_);
        wifi_guide_panel_ = nullptr;
    }

    // lv_layer_top() 이 아니라 활성 스크린에 얹는다 — QA 화면 덤프(GET /screen)가
    // lv_screen_active() 만 스냅샷하므로 top layer 는 덤프에 안 찍힌다.
    lv_obj_t* panel = lv_obj_create(lv_screen_active());
    lv_obj_set_size(panel, width_, height_);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
    GuideStylePlain(panel, kGuideBg);
    lv_obj_move_foreground(panel);

    lv_obj_t* title = GuidePixelLabel(panel, "FIRST TIME SETUP", kGuideAccent);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 12);

    // 캐릭터 마중 구도 (docs/mockups/wifi_guide.html B-2 확정안): 스텝은 왼쪽
    // 컬럼(3번은 짧게 두 줄), 캐릭터는 우측 끝 바닥, 말풍선은 머리 위.
    struct Step {
        const char* label;
        const char* value;
    };
    const Step steps[3] = {
        {"JOIN WIFI:", ap_ssid},
        {"OPEN IN BROWSER:", web_url},
        {"PICK YOUR WIFI", "+ SAVE"},
    };

    const int row_x = 46;
    const int row_y0 = 36;
    const int row_h = 50;
    for (int i = 0; i < 3; ++i) {
        const int y = row_y0 + i * row_h;

        // 번호 칩
        lv_obj_t* chip = lv_obj_create(panel);
        lv_obj_set_size(chip, 22, 22);
        GuideStylePlain(chip, kGuideAccent);
        lv_obj_set_style_radius(chip, 4, 0);
        lv_obj_align(chip, LV_ALIGN_TOP_LEFT, 14, y + 6);
        char num[2] = {static_cast<char>('1' + i), '\0'};
        lv_obj_t* num_label = GuidePixelLabel(chip, num, kGuideBg);
        lv_obj_center(num_label);

        lv_obj_t* label = GuidePixelLabel(panel, steps[i].label, kGuideStepLabel);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, row_x, y);
        lv_obj_t* value = GuidePixelLabel(panel, steps[i].value, kGuideValue);
        lv_obj_align(value, LV_ALIGN_TOP_LEFT, row_x, y + 17);
    }

    lv_obj_t* foot = GuidePixelLabel(panel, "WAITING...", kGuideFoot);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_LEFT, 16, -12);

    // 캐릭터 — 지금 입고 있는 옷 그대로 마중 나온다. 0.75× —
    // 실물 확인 결과 원 크기(98×65)는 모자 돌출까지 더하면 3번 스텝을 가린다.
    constexpr int kGuideFigureScale = 192;  // 256 = 1×
    wifi_guide_figure_ = std::make_unique<Figure>(panel, kGuideFigureScale);
    // 팩 캐릭터(idle 프레임 0)가 팩 아이템을 입고 마중 나온다. 반전본이 없어 방향은 그대로.
    wifi_guide_figure_->SetBodySrc(usage_screen_ != nullptr ? usage_screen_->pack_idle_frame() : nullptr);
    wifi_guide_figure_->SetOutfit(usage_outfit_);
    const int margin_r = 8;
    const int margin_b = 4;
    const int body_w = wifi_guide_figure_->scaled_body_w();
    const int body_h = wifi_guide_figure_->scaled_body_h();
    wifi_guide_figure_->SetBodyScreenPos(width_ - margin_r - body_w,
                                        height_ - margin_b - body_h);
    // 착장이 몸 위로 튀어나온 만큼(모자 등, ≤0) — 말풍선을 그 위에 놓는다.
    const int overhang = -wifi_guide_figure_->top_overhang() * kGuideFigureScale / 256;

    // 말풍선
    lv_obj_t* bubble = lv_obj_create(panel);
    GuideStylePlain(bubble, kGuideChipBg);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 6, 0);
    lv_obj_set_style_border_width(bubble, 2, 0);
    lv_obj_set_style_border_color(bubble, lv_color_hex(kGuideFoot), 0);
    lv_obj_set_style_pad_all(bubble, 8, 0);
    lv_obj_t* btext = GuidePixelLabel(bubble, "NEW PLACE!\nHELP ME OUT", kGuideAccent);
    lv_obj_set_style_text_line_space(btext, 5, 0);
    // 캐릭터(우측 끝 바닥) 머리(모자 포함) 위
    lv_obj_align(bubble, LV_ALIGN_BOTTOM_RIGHT, -6, -(margin_b + body_h + overhang + 4));

    wifi_guide_panel_ = panel;
    ESP_LOGI(TAG, "WiFi config guide shown (ap=%s url=%s)", ap_ssid, web_url);
}

void LcdDisplay::ShowMacSetupGuide(const char* board_ip) {
    WakeIfScreenOff(false);
    DisplayLockGuard lock(this);
    if (mac_guide_panel_ != nullptr) {
        lv_obj_del(mac_guide_panel_);
        mac_guide_panel_ = nullptr;
    }
    // WiFi 안내와 같은 바탕·글꼴. 활성 스크린에 얹어 GET /screen 덤프에도 찍힌다.
    lv_obj_t* panel = lv_obj_create(lv_screen_active());
    lv_obj_set_size(panel, width_, height_);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
    GuideStylePlain(panel, kGuideBg);
    lv_obj_move_foreground(panel);

    lv_obj_t* title = GuidePixelLabel(panel, "CONNECT YOUR MAC", kGuideAccent);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 12);

    struct Step {
        const char* label;
        const char* value;
    };
    const Step steps[3] = {
        {"ON THE SAME WIFI,", "OPEN TERMINAL"},
        {"IN CLAUDEBOT FOLDER RUN:", "python3 host/stats_widget/\nsetup.py"},
        {"TALK TO CLAUDE", "ONCE"},
    };
    const int row_x = 46;
    const int row_y[3] = {36, 80, 138};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* chip = lv_obj_create(panel);
        lv_obj_set_size(chip, 22, 22);
        GuideStylePlain(chip, kGuideAccent);
        lv_obj_set_style_radius(chip, 4, 0);
        lv_obj_align(chip, LV_ALIGN_TOP_LEFT, 14, row_y[i] + 6);
        char num[2] = {static_cast<char>('1' + i), '\0'};
        lv_obj_center(GuidePixelLabel(chip, num, kGuideBg));
        lv_obj_t* label = GuidePixelLabel(panel, steps[i].label, kGuideStepLabel);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, row_x, row_y[i]);
        lv_obj_t* value = GuidePixelLabel(panel, steps[i].value, kGuideValue);
        lv_obj_set_style_text_line_space(value, 6, 0);
        lv_obj_align(value, LV_ALIGN_TOP_LEFT, row_x, row_y[i] + 17);
    }

    char ip_line[40];
    snprintf(ip_line, sizeof(ip_line), "BOARD: %s", board_ip != nullptr && board_ip[0] ? board_ip : "?");
    lv_obj_t* ip = GuidePixelLabel(panel, ip_line, kGuideValue);
    lv_obj_align(ip, LV_ALIGN_BOTTOM_LEFT, 16, -30);
    lv_obj_t* foot = GuidePixelLabel(panel, "WAITING FOR MAC...  TAP=SKIP", kGuideFoot);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_LEFT, 16, -12);

    mac_guide_panel_ = panel;
    ESP_LOGI(TAG, "Mac setup guide shown (ip=%s)", board_ip != nullptr ? board_ip : "");
}

void LcdDisplay::HideMacSetupGuide() {
    DisplayLockGuard lock(this);
    if (mac_guide_panel_ == nullptr) {
        return;
    }
    lv_obj_del(mac_guide_panel_);
    mac_guide_panel_ = nullptr;
    ESP_LOGI(TAG, "Mac setup guide hidden");
}

void LcdDisplay::HideWifiConfigGuide() {
    DisplayLockGuard lock(this);
    if (wifi_guide_panel_ == nullptr) {
        return;
    }
    wifi_guide_figure_.reset();
    lv_obj_del(wifi_guide_panel_);
    wifi_guide_panel_ = nullptr;
    ESP_LOGI(TAG, "WiFi config guide hidden");
}
