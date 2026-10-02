#include "lcd_display.h"
#include "gif/lvgl_gif.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "assets/lang_config.h"

#include <vector>
#include <algorithm>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <cstring>
#include <src/misc/cache/lv_cache.h>

#include "board.h"
#include "audio_codec.h"

#include <wifi_manager.h>
#include <esp_system.h>
#ifdef CONFIG_USE_AVAGENC_DEVICE_BINDING
#include "device_binding.h"
#endif
#ifdef CONFIG_USE_AVAGENC_USER_PAIRING
#include "user_pairing.h"
#endif

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_awesome_30_4);

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_4);

    // Initialize twemoji_64 emoji collection for both themes
    auto emoji_collection = std::make_shared<Twemoji128>();

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
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
    light_theme->set_emoji_collection(emoji_collection);

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
    dark_theme->set_emoji_collection(emoji_collection);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "dark");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback = [](void* arg) {
            LcdDisplay* display = static_cast<LcdDisplay*>(arg);
            display->SetPreviewImage(nullptr);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);

    // Ambient ("watch face") idle timer
#if CONFIG_AMBIENT_MODE_ENABLE
    ambient_timeout_ms_ = settings.GetInt("ambient_sec", CONFIG_AMBIENT_MODE_TIMEOUT_SEC) * 1000;
#endif
    if (ambient_timeout_ms_ > 0) {
        esp_timer_create_args_t ambient_timer_args = {
            .callback = [](void* arg) {
                static_cast<LcdDisplay*>(arg)->ShowAmbientScreen();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ambient_timer",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&ambient_timer_args, &ambient_timer_);
    }
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
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
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
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
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy)
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
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 0,
            .full_refresh = 1,
            .direct_mode = 1,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        }
    };
    
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
                            int width, int height,  int offset_x, int offset_y,
                            bool mirror_x, bool mirror_y, bool swap_xy)
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
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram =false,
            .sw_rotate = true,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        }
    };
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

    if (ambient_timer_ != nullptr) {
        esp_timer_stop(ambient_timer_);
        esp_timer_delete(ambient_timer_);
    }
    ambient_screen_.reset();

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

bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

void LcdDisplay::ShowQrCode(const std::string& qr_data, const std::string& code, const std::string& caption) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    const lv_font_t* text_font = lvgl_theme ? lvgl_theme->text_font()->font() : nullptr;

    // Recreate from scratch each time so updated codes are reflected.
    if (qr_overlay_ != nullptr) {
        lv_obj_del(qr_overlay_);
        qr_overlay_ = nullptr;
    }

    lv_obj_t* screen = lv_screen_active();
    qr_overlay_ = lv_obj_create(screen);
    lv_obj_remove_style_all(qr_overlay_);
    lv_obj_set_size(qr_overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(qr_overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(qr_overlay_, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_pad_all(qr_overlay_, 12, 0);
    lv_obj_set_style_pad_row(qr_overlay_, 10, 0);
    lv_obj_set_flex_flow(qr_overlay_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(qr_overlay_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(qr_overlay_, LV_SCROLLBAR_MODE_OFF);
    if (text_font != nullptr) {
        lv_obj_set_style_text_font(qr_overlay_, text_font, 0);
    }

    lv_obj_t* title = lv_label_create(qr_overlay_);
    lv_label_set_text(title, "avagenc");
    lv_obj_set_style_text_color(title, lv_color_hex(0xA5F3FC), 0);

    if (!caption.empty()) {
        lv_obj_t* cap = lv_label_create(qr_overlay_);
        lv_label_set_text(cap, caption.c_str());
        lv_obj_set_width(cap, LV_HOR_RES * 0.8);
        lv_label_set_long_mode(cap, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(cap, lv_color_hex(0x9AA6C7), 0);
    }

#if defined(CONFIG_LV_USE_QRCODE)
    int qr_size = (LV_HOR_RES < LV_VER_RES ? LV_HOR_RES : LV_VER_RES) * 0.55;
    lv_obj_t* qr = lv_qrcode_create(qr_overlay_);
    lv_qrcode_set_size(qr, qr_size);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_qrcode_update(qr, qr_data.c_str(), qr_data.length());
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 6, 0);
    lv_obj_set_style_radius(qr, 6, 0);
#else
    // Fallback when the QR library is disabled: show the raw URL as text.
    lv_obj_t* url = lv_label_create(qr_overlay_);
    lv_label_set_text(url, qr_data.c_str());
    lv_obj_set_width(url, LV_HOR_RES * 0.85);
    lv_label_set_long_mode(url, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(url, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(url, lv_color_white(), 0);
#endif

    if (!code.empty()) {
        lv_obj_t* code_label = lv_label_create(qr_overlay_);
        lv_label_set_text(code_label, code.c_str());
        lv_obj_set_style_text_color(code_label, lv_color_white(), 0);
        lv_obj_set_style_text_letter_space(code_label, 3, 0);
    }
}

void LcdDisplay::HideQrCode() {
    DisplayLockGuard lock(this);
    if (qr_overlay_ != nullptr) {
        lv_obj_del(qr_overlay_);
        qr_overlay_ = nullptr;
    }
}

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
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
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
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

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
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_

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
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(), 0); // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    
    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0); // Space between messages

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
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0, text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);

    // Create slider overlay for brightness and volume
    CreateSliderOverlay();
    InitializeAmbientScreen();

    // Register touch input device event for "tap anywhere" to show slider overlay
    lv_indev_t* indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                LcdDisplay* display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
                if (display->HandleTouchWake()) {
                    return;  // Touch only dismissed the ambient screen
                }
                if (display->slider_overlay_ && lv_obj_has_flag(display->slider_overlay_, LV_OBJ_FLAG_HIDDEN)) {
                    display->ShowSliderOverlay();
                }
            }, LV_EVENT_PRESSED, this);
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}
#if CONFIG_IDF_TARGET_ESP32P4
#define  MAX_MESSAGES 40
#else
#define  MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called but container not created)", role, content);
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
    
    // Collapse system messages (if it's a system message, check if the last message is also a system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) && lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr && strcmp((const char*)bubble_type_ptr, "system") == 0) {
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
    if(strlen(content) == 0) {
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
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;  // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100; // 50% of screen height
    
    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld", img_width, img_height, max_width, max_height);
    }
    
    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;
    
    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256) zoom = 256;
    
    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);
    
    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release(); // Release ownership of smart pointer
    lv_obj_add_event_cb(preview_image, [](lv_event_t* e) {
        LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
        if (img != nullptr) {
            delete img; // Properly release memory by deleting LvglImage object
        }
    }, LV_EVENT_DELETE, (void*)raw_image);
    
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
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, -8);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    // Empty at boot — Application::Initialize() immediately shows the wifi.gif
    // emotion; the old FONT_AWESOME_MICROCHIP_AI chip glyph is not wanted.
    lv_label_set_text(emoji_label_, "");

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    CreateStateRing(screen);

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
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
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
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

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
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_

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

    /* Top layer: Bottom bar - chat text centered below emoji */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES - lvgl_theme->spacing(8), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bottom_bar_, 8, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    // Position below the emoji area (roughly 65% from top)
    lv_obj_align(bottom_bar_, LV_ALIGN_CENTER, 0, LV_VER_RES / 4);

    /* chat_message_label_ centered below emoji, multi-line wrap */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(16));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

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

    // Create slider overlay for brightness and volume
    CreateSliderOverlay();
    InitializeAmbientScreen();

    // Register touch input device event for "tap anywhere" to show slider overlay
    lv_indev_t* indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                LcdDisplay* display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
                if (display->HandleTouchWake()) {
                    return;  // Touch only dismissed the ambient screen
                }
                if (display->slider_overlay_ && lv_obj_has_flag(display->slider_overlay_, LV_OBJ_FLAG_HIDDEN)) {
                    display->ShowSliderOverlay();
                }
            }, LV_EVENT_PRESSED, this);
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
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
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetChatMessage('%s', '%s') failed: chat_message_label_ is nullptr (SetupUI() was called but label not created)", role, content);
        }
        return;
    }
    lv_label_set_text(chat_message_label_, content);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    // In non-wechat mode, just clear the chat message label
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
}
#endif

void LcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!", emotion);
    }
    // Stop any running GIF animation
    if (gif_controller_) {
        DisplayLockGuard lock(this);
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetEmotion('%s') failed: emoji_image_ is nullptr (SetupUI() was called but emoji image not created)", emotion);
        }
        return;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        const char* utf8 = font_awesome_get_utf8(emotion);
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    DisplayLockGuard lock(this);
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());
        
        if (gif_controller_->IsLoaded()) {
            // Set up frame update callback
            gif_controller_->SetFrameCallback([this]() {
                lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            });
            
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
        if (obj == nullptr) continue;
        
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
        
        if (bubble == nullptr) continue;
        
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
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LcdDisplay::CreateSliderOverlay() {
    auto screen = lv_screen_active();
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto icon_font = lvgl_theme->icon_font()->font();

    // Create auto-hide timer
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            LcdDisplay* display = static_cast<LcdDisplay*>(arg);
            display->HideSliderOverlay();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "slider_hide_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&timer_args, &slider_hide_timer_);

    // Full-screen overlay with semi-transparent dark background
    slider_overlay_ = lv_obj_create(screen);
    lv_obj_set_size(slider_overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(slider_overlay_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(slider_overlay_, LV_OPA_60, 0);
    lv_obj_set_style_radius(slider_overlay_, 0, 0);
    lv_obj_set_style_border_width(slider_overlay_, 0, 0);
    lv_obj_set_style_pad_all(slider_overlay_, 0, 0);
    lv_obj_set_scrollbar_mode(slider_overlay_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(slider_overlay_, LV_OBJ_FLAG_HIDDEN);

    // ===================== LEFT ARC (Brightness) =====================
    // Left half of circle: bottom (0%) → left → top (100%)
    brightness_arc_ = lv_arc_create(slider_overlay_);
    lv_obj_set_size(brightness_arc_, 280, 280);
    lv_arc_set_rotation(brightness_arc_, 90);           // Start at 6 o'clock (bottom)
    lv_arc_set_bg_angles(brightness_arc_, 0, 180);      // Bottom → left → top
    lv_arc_set_range(brightness_arc_, 1, 100);           // Min 1 to avoid completely dark
    lv_arc_set_value(brightness_arc_, 75);               // Default - will be updated on show
    lv_obj_align(brightness_arc_, LV_ALIGN_CENTER, 0, 0);

    // Arc styling - indicator (active portion)
    lv_obj_set_style_arc_color(brightness_arc_, lv_color_hex(0xFFCC00), LV_PART_INDICATOR);  // Warm yellow
    lv_obj_set_style_arc_width(brightness_arc_, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(brightness_arc_, true, LV_PART_INDICATOR);

    // Arc styling - background (inactive portion)
    lv_obj_set_style_arc_color(brightness_arc_, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(brightness_arc_, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(brightness_arc_, true, LV_PART_MAIN);

    // Knob styling
    lv_obj_set_style_bg_color(brightness_arc_, lv_color_hex(0xFFCC00), LV_PART_KNOB);
    lv_obj_set_style_pad_all(brightness_arc_, 4, LV_PART_KNOB);

    // Transparent background for arc container
    lv_obj_set_style_bg_opa(brightness_arc_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(brightness_arc_, 0, 0);
    lv_obj_add_flag(brightness_arc_, LV_OBJ_FLAG_ADV_HITTEST);  // Only respond to touches near arc path

    // Brightness icon (sun) - left-center
    brightness_icon_ = lv_label_create(slider_overlay_);
    lv_label_set_text(brightness_icon_, FONT_AWESOME_BRIGHTNESS);
    lv_obj_set_style_text_font(brightness_icon_, icon_font, 0);
    lv_obj_set_style_text_color(brightness_icon_, lv_color_hex(0xFFCC00), 0);
    lv_obj_align(brightness_icon_, LV_ALIGN_LEFT_MID, 50, -10);

    // Brightness percentage label
    brightness_label_ = lv_label_create(slider_overlay_);
    lv_label_set_text(brightness_label_, "75%");
    lv_obj_set_style_text_color(brightness_label_, lv_color_white(), 0);
    lv_obj_align(brightness_label_, LV_ALIGN_LEFT_MID, 50, 14);

    // Brightness arc event handler
    lv_obj_add_event_cb(brightness_arc_, [](lv_event_t* e) {
        LcdDisplay* display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
        lv_obj_t* arc = (lv_obj_t*)lv_event_get_current_target(e);
        int val = lv_arc_get_value(arc);

        // Update label
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", val);
        lv_label_set_text(display->brightness_label_, buf);

        // Apply brightness (permanent = true to persist in NVS)
        auto& board = Board::GetInstance();
        board.GetBacklight()->SetBrightness(val, true);

        // Reset auto-hide timer
        if (display->slider_hide_timer_) {
            esp_timer_stop(display->slider_hide_timer_);
            esp_timer_start_once(display->slider_hide_timer_, 3000000); // 3 seconds
        }
    }, LV_EVENT_VALUE_CHANGED, this);

    // ===================== RIGHT ARC (Volume) =====================
    // Right half of circle: top (0%) → right → bottom (100%)
    volume_arc_ = lv_arc_create(slider_overlay_);
    lv_obj_set_size(volume_arc_, 280, 280);
    lv_arc_set_rotation(volume_arc_, 270);              // Start at 12 o'clock (top)
    lv_arc_set_bg_angles(volume_arc_, 0, 180);          // Top → right → bottom
    lv_arc_set_range(volume_arc_, 0, 100);
    lv_arc_set_value(volume_arc_, 70);                    // Default - will be updated on show
    lv_obj_align(volume_arc_, LV_ALIGN_CENTER, 0, 0);

    // Arc styling - indicator (active portion)
    lv_obj_set_style_arc_color(volume_arc_, lv_color_hex(0x00AAFF), LV_PART_INDICATOR);  // Blue
    lv_obj_set_style_arc_width(volume_arc_, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(volume_arc_, true, LV_PART_INDICATOR);

    // Arc styling - background (inactive portion)
    lv_obj_set_style_arc_color(volume_arc_, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(volume_arc_, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(volume_arc_, true, LV_PART_MAIN);

    // Knob styling
    lv_obj_set_style_bg_color(volume_arc_, lv_color_hex(0x00AAFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(volume_arc_, 4, LV_PART_KNOB);

    // Transparent background for arc container
    lv_obj_set_style_bg_opa(volume_arc_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(volume_arc_, 0, 0);
    lv_obj_add_flag(volume_arc_, LV_OBJ_FLAG_ADV_HITTEST);  // Only respond to touches near arc path

    // Volume icon (speaker) - right-center
    volume_icon_ = lv_label_create(slider_overlay_);
    lv_label_set_text(volume_icon_, FONT_AWESOME_VOLUME_HIGH);
    lv_obj_set_style_text_font(volume_icon_, icon_font, 0);
    lv_obj_set_style_text_color(volume_icon_, lv_color_hex(0x00AAFF), 0);
    lv_obj_align(volume_icon_, LV_ALIGN_RIGHT_MID, -50, -10);

    // Volume percentage label
    volume_label_ = lv_label_create(slider_overlay_);
    lv_label_set_text(volume_label_, "70%");
    lv_obj_set_style_text_color(volume_label_, lv_color_white(), 0);
    lv_obj_align(volume_label_, LV_ALIGN_RIGHT_MID, -50, 14);

    // Volume arc event handler
    lv_obj_add_event_cb(volume_arc_, [](lv_event_t* e) {
        LcdDisplay* display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
        lv_obj_t* arc = (lv_obj_t*)lv_event_get_current_target(e);
        int val = lv_arc_get_value(arc);

        // Update label
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", val);
        lv_label_set_text(display->volume_label_, buf);

        // Update volume icon based on value
        if (val == 0) {
            lv_label_set_text(display->volume_icon_, FONT_AWESOME_VOLUME_XMARK);
        } else if (val < 30) {
            lv_label_set_text(display->volume_icon_, FONT_AWESOME_VOLUME_LOW);
        } else if (val < 70) {
            lv_label_set_text(display->volume_icon_, FONT_AWESOME_VOLUME);
        } else {
            lv_label_set_text(display->volume_icon_, FONT_AWESOME_VOLUME_HIGH);
        }

        // Apply volume
        auto& board = Board::GetInstance();
        board.GetAudioCodec()->SetOutputVolume(val);

        // Reset auto-hide timer
        if (display->slider_hide_timer_) {
            esp_timer_stop(display->slider_hide_timer_);
            esp_timer_start_once(display->slider_hide_timer_, 3000000); // 3 seconds
        }
    }, LV_EVENT_VALUE_CHANGED, this);

    // ===================== Quick action buttons =====================
    CreateQuickActionButtons();

    // ===================== Overlay touch event =====================
    // Tapping overlay background (not on arcs) hides the overlay
    lv_obj_add_event_cb(slider_overlay_, [](lv_event_t* e) {
        LcdDisplay* display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
        lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
        // Only hide if clicking the overlay background itself, not the arc controls
        if (target == display->slider_overlay_) {
            display->HideSliderOverlay();
        }
    }, LV_EVENT_CLICKED, this);
}

void LcdDisplay::CreateQuickActionButtons() {
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto icon_font = lvgl_theme->icon_font()->font();

    auto make_button = [&](const char* icon, const char* text, lv_color_t color,
                           lv_obj_t** text_label_out) {
        lv_obj_t* btn = lv_button_create(slider_overlay_);
        lv_obj_set_size(btn, 92, 62);
        lv_obj_set_style_radius(btn, 14, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1f2430), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 4, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* icon_label = lv_label_create(btn);
        lv_obj_set_style_text_font(icon_label, icon_font, 0);
        lv_obj_set_style_text_color(icon_label, color, 0);
        lv_label_set_text(icon_label, icon);

        lv_obj_t* text_label = lv_label_create(btn);
        lv_obj_set_style_text_color(text_label, lv_color_hex(0xd1d5db), 0);
        lv_label_set_text(text_label, text);
        if (text_label_out != nullptr) {
            *text_label_out = text_label;
        }
        return btn;
    };
    (void)make_button; // only used by the logout button (device-binding builds)

    // Reconnect WiFi — circular button with a wifi icon, dead-center on screen.
    wifi_btn_ = lv_button_create(slider_overlay_);
    lv_obj_set_size(wifi_btn_, 76, 76);
    lv_obj_set_style_radius(wifi_btn_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(wifi_btn_, lv_color_hex(0x1f2430), 0);
    lv_obj_set_style_bg_opa(wifi_btn_, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(wifi_btn_, 0, 0);
    lv_obj_set_style_pad_all(wifi_btn_, 0, 0);
    lv_obj_align(wifi_btn_, LV_ALIGN_CENTER, 0, 0);
    {
        lv_obj_t* wifi_icon = lv_label_create(wifi_btn_);
        lv_obj_set_style_text_font(wifi_icon, icon_font, 0);
        lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x22d3ee), 0);
        lv_label_set_text(wifi_icon, FONT_AWESOME_WIFI);
        lv_obj_center(wifi_icon);
    }
    lv_obj_add_event_cb(wifi_btn_, [](lv_event_t* e) {
        LcdDisplay* display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
        display->HideSliderOverlay();
        display->ShowNotification("Menyambung ulang WiFi...");
        auto& wifi_manager = WifiManager::GetInstance();
        wifi_manager.StopStation();
        wifi_manager.StartStation();
    }, LV_EVENT_CLICKED, this);

#if defined(CONFIG_USE_AVAGENC_DEVICE_BINDING) || defined(CONFIG_USE_AVAGENC_USER_PAIRING)
    // Logout: forget the avagenc user/binding and reboot into pairing.
    // Two-tap confirm so a stray touch can't log the account out.
    logout_btn_ = make_button(FONT_AWESOME_POWER_OFF, "Logout", lv_color_hex(0xf87171),
                              &logout_btn_label_);
    lv_obj_align(logout_btn_, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_add_event_cb(logout_btn_, [](lv_event_t* e) {
        LcdDisplay* display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
        int64_t now = esp_timer_get_time();
        if (display->logout_armed_at_us_ != 0 &&
            now - display->logout_armed_at_us_ < 3000000) {
#ifdef CONFIG_USE_AVAGENC_DEVICE_BINDING
            DeviceBinding().ClearBinding();
#endif
#ifdef CONFIG_USE_AVAGENC_USER_PAIRING
            UserPairing().ClearUser();
#endif
            esp_restart();
        }
        display->logout_armed_at_us_ = now;
        lv_label_set_text(display->logout_btn_label_, "Yakin?");
        // Keep the overlay up long enough to confirm
        if (display->slider_hide_timer_) {
            esp_timer_stop(display->slider_hide_timer_);
            esp_timer_start_once(display->slider_hide_timer_, 3000000);
        }
    }, LV_EVENT_CLICKED, this);
#endif
}

void LcdDisplay::ShowSliderOverlay() {
    DisplayLockGuard lock(this);
    if (slider_overlay_ == nullptr) return;

    // Cancel any ongoing fade-out animation and reset opacity
    lv_anim_delete(slider_overlay_, NULL);
    lv_obj_set_style_opa(slider_overlay_, LV_OPA_COVER, 0);

    // Read current brightness and volume values
    auto& board = Board::GetInstance();
    int brightness = board.GetBacklight()->brightness();
    int volume = board.GetAudioCodec()->output_volume();

    // Update arcs with current values
    lv_arc_set_value(brightness_arc_, brightness);
    lv_arc_set_value(volume_arc_, volume);

    // Update labels
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", brightness);
    lv_label_set_text(brightness_label_, buf);

    snprintf(buf, sizeof(buf), "%d%%", volume);
    lv_label_set_text(volume_label_, buf);

    // Update volume icon
    if (volume == 0) {
        lv_label_set_text(volume_icon_, FONT_AWESOME_VOLUME_XMARK);
    } else if (volume < 30) {
        lv_label_set_text(volume_icon_, FONT_AWESOME_VOLUME_LOW);
    } else if (volume < 70) {
        lv_label_set_text(volume_icon_, FONT_AWESOME_VOLUME);
    } else {
        lv_label_set_text(volume_icon_, FONT_AWESOME_VOLUME_HIGH);
    }

    // Reset the logout two-tap confirm state
    logout_armed_at_us_ = 0;
    if (logout_btn_label_ != nullptr) {
        lv_label_set_text(logout_btn_label_, "Logout");
    }

    // Show overlay
    lv_obj_remove_flag(slider_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(slider_overlay_);

    // Start auto-hide timer (3 seconds)
    if (slider_hide_timer_) {
        esp_timer_stop(slider_hide_timer_);
        esp_timer_start_once(slider_hide_timer_, 3000000);
    }
}

void LcdDisplay::HideSliderOverlay() {
    DisplayLockGuard lock(this);
    if (slider_overlay_ == nullptr) return;
    if (lv_obj_has_flag(slider_overlay_, LV_OBJ_FLAG_HIDDEN)) return;

    if (slider_hide_timer_) {
        esp_timer_stop(slider_hide_timer_);
    }

    // Fade out animation (500ms)
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, slider_overlay_);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 500);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t val) {
        lv_obj_set_style_opa((lv_obj_t*)obj, val, 0);
    });
    lv_anim_set_completed_cb(&a, [](lv_anim_t* anim) {
        lv_obj_add_flag((lv_obj_t*)anim->var, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa((lv_obj_t*)anim->var, LV_OPA_COVER, 0);  // Reset for next show
    });
    lv_anim_start(&a);
}

// ============ State ring (listening/speaking feedback) ============

void LcdDisplay::CreateStateRing(lv_obj_t* screen) {
    state_ring_ = lv_arc_create(screen);
    // Slightly larger than the 128 px emoji, same center offset as emoji_box_
    lv_obj_set_size(state_ring_, 176, 176);
    lv_obj_align(state_ring_, LV_ALIGN_CENTER, 0, -8);
    lv_arc_set_rotation(state_ring_, 270);
    lv_arc_set_bg_angles(state_ring_, 0, 360);
    lv_obj_remove_style(state_ring_, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(state_ring_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(state_ring_, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(state_ring_, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(state_ring_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state_ring_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state_ring_, 0, 0);
    lv_obj_add_flag(state_ring_, LV_OBJ_FLAG_HIDDEN);
}

// Ring geometry. The emoji GIF is 128 px, centred with the same -8 px offset.
static constexpr int kConnectingRingSize = 232;  // loading spinner: wide, so it
                                                  // doesn't hug the GIF
static constexpr int kPulseStartSize     = 150;   // pulse begins just outside the emoji
static constexpr int kPulseEndSize       = 312;   // ...and ripples out near the panel edge
static constexpr int kRingOffsetY        = -8;

void LcdDisplay::UpdateStateRing(DeviceState state, DeviceState previous) {
    if (state_ring_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    lv_anim_delete(state_ring_, nullptr);
    lv_obj_set_style_opa(state_ring_, LV_OPA_COVER, 0);

    switch (state) {
        case kDeviceStateConnecting: {
            // Loading spinner while the audio channel / websocket opens.
            // Wide radius so the ring floats clear of the emoji GIF.
            lv_obj_set_size(state_ring_, kConnectingRingSize, kConnectingRingSize);
            lv_obj_align(state_ring_, LV_ALIGN_CENTER, 0, kRingOffsetY);
            lv_arc_set_angles(state_ring_, 0, 60);
            lv_obj_set_style_arc_color(state_ring_, lv_color_hex(0x6b7280), LV_PART_INDICATOR);
            lv_obj_remove_flag(state_ring_, LV_OBJ_FLAG_HIDDEN);

            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, state_ring_);
            lv_anim_set_values(&a, 0, 360);
            lv_anim_set_duration(&a, 1000);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
                lv_arc_set_rotation(static_cast<lv_obj_t*>(obj), 270 + v);
            });
            lv_anim_start(&a);
            break;
        }
        case kDeviceStateListening:
        case kDeviceStateSpeaking: {
            // Once the call is running we keep the emoji clean — no ring.
            // Only when we *enter* a call state from a non-call state (i.e. the
            // moment the connection is established) do we play a one-shot pulse:
            // a full ring that expands outward and fades away. Toggling between
            // Listening and Speaking during the call does not re-trigger it.
            bool was_in_call = (previous == kDeviceStateListening ||
                                previous == kDeviceStateSpeaking);
            if (was_in_call) {
                lv_obj_add_flag(state_ring_, LV_OBJ_FLAG_HIDDEN);
                break;
            }

            lv_obj_set_size(state_ring_, kPulseStartSize, kPulseStartSize);
            lv_obj_align(state_ring_, LV_ALIGN_CENTER, 0, kRingOffsetY);
            lv_arc_set_angles(state_ring_, 0, 360);
            lv_obj_set_style_arc_color(state_ring_, lv_color_hex(0x22d3ee), LV_PART_INDICATOR);
            lv_obj_remove_flag(state_ring_, LV_OBJ_FLAG_HIDDEN);

            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, state_ring_);
            lv_anim_set_values(&a, 0, 100);
            lv_anim_set_duration(&a, 650);
            lv_anim_set_repeat_count(&a, 2);  // two ripples, then hide
            lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
                lv_obj_t* ring = static_cast<lv_obj_t*>(obj);
                int size = kPulseStartSize + (kPulseEndSize - kPulseStartSize) * v / 100;
                lv_obj_set_size(ring, size, size);
                lv_obj_align(ring, LV_ALIGN_CENTER, 0, kRingOffsetY);
                lv_obj_set_style_opa(ring, LV_OPA_COVER - (LV_OPA_COVER * v / 100), 0);
            });
            lv_anim_set_ready_cb(&a, [](lv_anim_t* anim) {
                lv_obj_add_flag(static_cast<lv_obj_t*>(anim->var), LV_OBJ_FLAG_HIDDEN);
            });
            lv_anim_start(&a);
            break;
        }
        default:
            lv_obj_add_flag(state_ring_, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}

// ============ Ambient ("watch face") mode ============

// Quiet hours: [start, end) wrapping midnight, e.g. 22..6
static bool IsNightTime() {
    Settings settings("night", false);
    if (!settings.GetBool("enabled", true)) {
        return false;
    }
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    if (tm_local.tm_year < 2025 - 1900) {
        return false;  // Clock not synced yet
    }
    int start = settings.GetInt("start", 22);
    int end = settings.GetInt("end", 6);
    int hour = tm_local.tm_hour;
    return start <= end ? (hour >= start && hour < end)
                        : (hour >= start || hour < end);
}

void LcdDisplay::InitializeAmbientScreen() {
    if (ambient_timeout_ms_ <= 0 || ambient_screen_ != nullptr) {
        return;
    }
    // Called from SetupUI, which already holds the display lock
    ambient_screen_ = std::make_unique<AmbientScreen>(
        lv_screen_active(), static_cast<LvglTheme*>(current_theme_));
    ambient_screen_->SetOnWake([this]() {
        HideAmbientScreen();
    });
    ambient_screen_->SetOnMinuteTick([this]() {
        // Re-evaluate night-mode dimming while the face stays up
        ApplyAmbientBrightness();
    });
    ResetAmbientTimer();
}

void LcdDisplay::ResetAmbientTimer() {
    if (ambient_timer_ == nullptr) {
        return;
    }
    esp_timer_stop(ambient_timer_);
    if (device_state_ == kDeviceStateIdle) {
        esp_timer_start_once(ambient_timer_, (uint64_t)ambient_timeout_ms_ * 1000);
    }
}

void LcdDisplay::ApplyAmbientBrightness() {
    auto backlight = Board::GetInstance().GetBacklight();
    if (backlight == nullptr) {
        return;
    }
    // Base on the user's saved brightness, not the current (possibly already
    // dimmed) level
    Settings settings("display", false);
    int user_brightness = settings.GetInt("brightness", 75);
    uint8_t dimmed = IsNightTime()
        ? 4
        : std::max<uint8_t>(user_brightness * 30 / 100, 4);
    backlight->SetBrightness(dimmed);
}

void LcdDisplay::ShowAmbientScreen() {
    if (ambient_screen_ == nullptr || device_state_ != kDeviceStateIdle) {
        return;
    }
    {
        DisplayLockGuard lock(this);
        if (ambient_screen_->IsVisible()) {
            return;
        }
        HideSliderOverlay();
        ambient_screen_->Show();
    }
    ApplyAmbientBrightness();
}

void LcdDisplay::HideAmbientScreen() {
    if (ambient_screen_ == nullptr) {
        return;
    }
    {
        DisplayLockGuard lock(this);
        if (!ambient_screen_->IsVisible()) {
            ResetAmbientTimer();
            return;
        }
        ambient_screen_->Hide();
    }
    auto backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr) {
        backlight->RestoreBrightness();
    }
    ResetAmbientTimer();
}

bool LcdDisplay::HandleTouchWake() {
    // While the ambient screen is up, presses are handled by its own tap
    // (wake) and swipe (face switch) callbacks — just keep the slider overlay
    // from opening. Otherwise a touch simply postpones ambient mode.
    if (ambient_screen_ != nullptr && ambient_screen_->IsVisible()) {
        return true;
    }
    ResetAmbientTimer();
    return false;
}

void LcdDisplay::SetWeather(const char* icon_utf8, const char* text) {
    if (ambient_screen_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    ambient_screen_->SetWeather(icon_utf8 ? icon_utf8 : "", text ? text : "");
}

void LcdDisplay::OnStateChanged(DeviceState state) {
    DeviceState previous = device_state_;
    device_state_ = state;
    if (state == kDeviceStateIdle) {
        ResetAmbientTimer();
    } else {
        HideAmbientScreen();
        if (ambient_timer_ != nullptr) {
            esp_timer_stop(ambient_timer_);
        }
    }
    UpdateStateRing(state, previous);
}
