#ifndef AMBIENT_SCREEN_H
#define AMBIENT_SCREEN_H

#include "lvgl_display/gif/lvgl_gif.h"

#include <lvgl.h>

#include <functional>
#include <memory>
#include <string>

class LvglTheme;

// Full-screen ambient ("watch face") overlay shown when the device sits idle.
// Owned by LcdDisplay; every public method must be called with the display
// lock held. The internal lv_timer and touch callbacks run inside the LVGL
// task, so they need no extra locking.
//
// Interaction while visible: tap wakes the device (on_wake callback), swipe
// left/right switches between faces (persisted to NVS).
class AmbientScreen {
public:
    enum Face {
        kFaceDigital = 0,
        kFaceAnalog,
        kFaceWallpaper,
        kFaceEmoji,
        kFaceCount
    };

    AmbientScreen(lv_obj_t* parent, LvglTheme* theme);
    ~AmbientScreen();

    void Show();
    void Hide();
    bool IsVisible() const;

    // Tap-to-wake handler; runs in the LVGL task.
    void SetOnWake(std::function<void()> cb) { on_wake_ = std::move(cb); }
    // Fires on every minute change while visible (used to re-evaluate the
    // night-mode dim level); runs in the LVGL task.
    void SetOnMinuteTick(std::function<void()> cb) { on_minute_ = std::move(cb); }

    // icon_utf8: Font Awesome glyph (font_awesome_get_utf8), text: "26° Cerah"
    void SetWeather(const std::string& icon_utf8, const std::string& text);

    void SetFace(int face);
    void NextFace(int direction);

private:
    void CreateWidgets();
    void ApplyFace();
    void UpdateClock();
    void UpdateAnalogHands(const struct tm& tm_local);
    void ActivateEmojiFace();
    void DeactivateEmojiFace();
    void LoadWallpaper();

    LvglTheme* theme_ = nullptr;
    lv_obj_t* overlay_ = nullptr;

    // Shared
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* weather_box_ = nullptr;        // row: icon glyph + text
    lv_obj_t* weather_icon_label_ = nullptr; // icon font
    lv_obj_t* weather_text_label_ = nullptr; // text font
    lv_obj_t* seconds_arc_ = nullptr;

    // Analog face
    lv_obj_t* analog_group_ = nullptr;
    lv_obj_t* hour_hand_ = nullptr;
    lv_obj_t* minute_hand_ = nullptr;
    lv_obj_t* second_hand_ = nullptr;
    lv_obj_t* center_dot_ = nullptr;
    lv_point_precise_t hour_pts_[2];
    lv_point_precise_t minute_pts_[2];
    lv_point_precise_t second_pts_[2];

    // Wallpaper face
    lv_obj_t* wallpaper_image_ = nullptr;
    std::unique_ptr<LvglImage> wallpaper_raw_ = nullptr;
    bool wallpaper_loaded_ = false;
    bool wallpaper_available_ = true;

    // Emoji face
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> emoji_gif_ = nullptr;

    lv_timer_t* tick_timer_ = nullptr;
    std::function<void()> on_wake_;
    std::function<void()> on_minute_;
    int face_ = kFaceDigital;
    int last_minute_ = -1;
    bool gesture_active_ = false;
    std::string weather_icon_;
    std::string weather_text_;
};

#endif // AMBIENT_SCREEN_H
