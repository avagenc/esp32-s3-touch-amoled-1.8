#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"
#include "gif/lvgl_gif.h"
#include "ambient_screen.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <atomic>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000


class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* qr_overlay_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles

    // Ring around the emoji: pulses while listening, spins while speaking
    lv_obj_t* state_ring_ = nullptr;

    // Ambient ("watch face") mode shown after the device idles
    std::unique_ptr<AmbientScreen> ambient_screen_ = nullptr;
    esp_timer_handle_t ambient_timer_ = nullptr;
    int ambient_timeout_ms_ = 0;  // 0 = disabled
    DeviceState device_state_ = kDeviceStateUnknown;

    // Slider overlay for brightness and volume
    lv_obj_t* slider_overlay_ = nullptr;
    lv_obj_t* brightness_arc_ = nullptr;
    lv_obj_t* volume_arc_ = nullptr;
    lv_obj_t* brightness_icon_ = nullptr;
    lv_obj_t* volume_icon_ = nullptr;
    lv_obj_t* brightness_label_ = nullptr;
    lv_obj_t* volume_label_ = nullptr;
    esp_timer_handle_t slider_hide_timer_ = nullptr;

    // Quick-action buttons on the slider overlay
    lv_obj_t* wifi_btn_ = nullptr;
    lv_obj_t* logout_btn_ = nullptr;
    lv_obj_t* logout_btn_label_ = nullptr;
    int64_t logout_armed_at_us_ = 0;  // two-tap confirm for logout

    void InitializeLcdThemes();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;
    void CreateSliderOverlay();
    void CreateQuickActionButtons();
    void ShowSliderOverlay();
    void HideSliderOverlay();
    void CreateStateRing(lv_obj_t* screen);
    void UpdateStateRing(DeviceState state, DeviceState previous);
    void InitializeAmbientScreen();
    void ResetAmbientTimer();
    void ApplyAmbientBrightness();
    void ShowAmbientScreen();
    void HideAmbientScreen();
    // Touch handler shared by all SetupUI variants; returns true if the touch
    // belongs to the ambient screen (so the slider overlay must not open).
    bool HandleTouchWake();

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
    virtual void ShowQrCode(const std::string& qr_data, const std::string& code, const std::string& caption) override;
    virtual void HideQrCode() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;
    
    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);

    virtual void OnStateChanged(DeviceState state) override;
    virtual void SetWeather(const char* icon_utf8, const char* text) override;
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // LCD_DISPLAY_H
