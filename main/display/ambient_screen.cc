#include "ambient_screen.h"
#include "lvgl_display/lvgl_theme.h"
#include "settings.h"
#include "assets.h"

#include <esp_log.h>

#include <cmath>
#include <cstdio>
#include <ctime>

#define TAG "AmbientScreen"

static const char* const kDayNames[] = {
    "Minggu", "Senin", "Selasa", "Rabu", "Kamis", "Jumat", "Sabtu"
};
static const char* const kMonthNames[] = {
    "Jan", "Feb", "Mar", "Apr", "Mei", "Jun",
    "Jul", "Agu", "Sep", "Okt", "Nov", "Des"
};

static const char* kWallpaperAsset = "wallpaper.png";

AmbientScreen::AmbientScreen(lv_obj_t* parent, LvglTheme* theme) : theme_(theme) {
    Settings settings("display", false);
    face_ = settings.GetInt("ambient_face", kFaceDigital);
    if (face_ < 0 || face_ >= kFaceCount) {
        face_ = kFaceDigital;
    }

    overlay_ = lv_obj_create(parent);
    lv_obj_set_size(overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(overlay_);
    lv_obj_set_style_radius(overlay_, 0, 0);
    lv_obj_set_style_border_width(overlay_, 0, 0);
    lv_obj_set_style_pad_all(overlay_, 0, 0);
    // True black regardless of theme: reads as "standby" and flatters the
    // round panel's bezel.
    lv_obj_set_style_bg_color(overlay_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(overlay_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);

    // Tap wakes, swipe switches face. A swipe also produces press/release, so
    // suppress the wake when a gesture happened during this press.
    lv_obj_add_event_cb(overlay_, [](lv_event_t* e) {
        auto self = static_cast<AmbientScreen*>(lv_event_get_user_data(e));
        self->gesture_active_ = false;
    }, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(overlay_, [](lv_event_t* e) {
        auto self = static_cast<AmbientScreen*>(lv_event_get_user_data(e));
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
            self->gesture_active_ = true;
            self->NextFace(dir == LV_DIR_LEFT ? 1 : -1);
        }
    }, LV_EVENT_GESTURE, this);
    lv_obj_add_event_cb(overlay_, [](lv_event_t* e) {
        auto self = static_cast<AmbientScreen*>(lv_event_get_user_data(e));
        if (self->gesture_active_) {
            self->gesture_active_ = false;
            return;
        }
        if (self->on_wake_) {
            self->on_wake_();
        }
    }, LV_EVENT_CLICKED, this);

    CreateWidgets();
    ApplyFace();

    tick_timer_ = lv_timer_create([](lv_timer_t* t) {
        static_cast<AmbientScreen*>(lv_timer_get_user_data(t))->UpdateClock();
    }, 1000, this);
    lv_timer_pause(tick_timer_);
}

AmbientScreen::~AmbientScreen() {
    DeactivateEmojiFace();
    if (tick_timer_ != nullptr) {
        lv_timer_delete(tick_timer_);
    }
    if (overlay_ != nullptr) {
        lv_obj_delete(overlay_);
    }
}

void AmbientScreen::CreateWidgets() {
    // --- Wallpaper (bottom-most, face 2 only) ---
    wallpaper_image_ = lv_image_create(overlay_);
    lv_obj_center(wallpaper_image_);
    lv_obj_add_flag(wallpaper_image_, LV_OBJ_FLAG_HIDDEN);

    // --- Emoji (face 3 only) ---
    emoji_image_ = lv_image_create(overlay_);
    lv_obj_align(emoji_image_, LV_ALIGN_CENTER, 0, -36);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    // --- Seconds progress: thin cyan arc hugging the round bezel ---
    seconds_arc_ = lv_arc_create(overlay_);
    lv_obj_set_size(seconds_arc_, LV_HOR_RES - 8, LV_VER_RES - 8);
    lv_obj_center(seconds_arc_);
    lv_arc_set_rotation(seconds_arc_, 270);
    lv_arc_set_bg_angles(seconds_arc_, 0, 360);
    lv_arc_set_range(seconds_arc_, 0, 60);
    lv_arc_set_value(seconds_arc_, 0);
    lv_obj_remove_style(seconds_arc_, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(seconds_arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(seconds_arc_, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(seconds_arc_, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(seconds_arc_, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_arc_color(seconds_arc_, lv_color_hex(0x22d3ee), LV_PART_INDICATOR);

    // --- Analog face group ---
    analog_group_ = lv_obj_create(overlay_);
    lv_obj_set_size(analog_group_, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(analog_group_);
    lv_obj_set_style_bg_opa(analog_group_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(analog_group_, 0, 0);
    lv_obj_set_style_pad_all(analog_group_, 0, 0);
    lv_obj_remove_flag(analog_group_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(analog_group_, LV_OBJ_FLAG_CLICKABLE);

    // Hour tick marks
    for (int i = 0; i < 12; i++) {
        lv_obj_t* tick = lv_obj_create(analog_group_);
        bool major = (i % 3) == 0;
        int len = major ? 18 : 10;
        lv_obj_set_size(tick, major ? 4 : 2, len);
        lv_obj_set_style_radius(tick, 2, 0);
        lv_obj_set_style_bg_color(tick, major ? lv_color_hex(0x9ca3af) : lv_color_hex(0x4b5563), 0);
        lv_obj_set_style_border_width(tick, 0, 0);
        float angle = i * 30.0f * (float)M_PI / 180.0f;
        int r = LV_HOR_RES / 2 - 16 - len / 2;
        int cx = LV_HOR_RES / 2 + (int)(r * sinf(angle));
        int cy = LV_VER_RES / 2 - (int)(r * cosf(angle));
        int tick_w = major ? 4 : 2;
        lv_obj_set_pos(tick, cx - tick_w / 2, cy - len / 2);
        lv_obj_set_style_transform_rotation(tick, i * 300, 0);  // 0.1 deg units
        lv_obj_set_style_transform_pivot_x(tick, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(tick, LV_PCT(50), 0);
    }

    auto make_hand = [this](int width, lv_color_t color) {
        lv_obj_t* line = lv_line_create(analog_group_);
        lv_obj_set_style_line_width(line, width, 0);
        lv_obj_set_style_line_color(line, color, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        return line;
    };
    hour_hand_ = make_hand(7, lv_color_hex(0xe5e7eb));
    minute_hand_ = make_hand(5, lv_color_hex(0xd1d5db));
    second_hand_ = make_hand(2, lv_color_hex(0x22d3ee));

    center_dot_ = lv_obj_create(analog_group_);
    lv_obj_set_size(center_dot_, 14, 14);
    lv_obj_set_style_radius(center_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center_dot_, lv_color_hex(0x22d3ee), 0);
    lv_obj_set_style_border_width(center_dot_, 0, 0);
    lv_obj_align(center_dot_, LV_ALIGN_CENTER, 0, 0);

    // --- Big HH:MM (digital + wallpaper + emoji faces) ---
    time_label_ = lv_label_create(overlay_);
    lv_obj_set_style_text_font(time_label_, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(time_label_, lv_color_hex(0xe5e7eb), 0);
    lv_obj_set_style_text_letter_space(time_label_, 3, 0);
    lv_label_set_text(time_label_, "--:--");
    lv_obj_align(time_label_, LV_ALIGN_CENTER, 0, -24);

    // --- "Kamis, 17 Jul" ---
    date_label_ = lv_label_create(overlay_);
    lv_obj_set_style_text_color(date_label_, lv_color_hex(0x9ca3af), 0);
    lv_label_set_text(date_label_, "");
    lv_obj_align(date_label_, LV_ALIGN_CENTER, 0, 44);

    // --- Weather line: icon glyph (icon font) + text (theme text font) ---
    weather_box_ = lv_obj_create(overlay_);
    lv_obj_set_size(weather_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(weather_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weather_box_, 0, 0);
    lv_obj_set_style_pad_all(weather_box_, 0, 0);
    lv_obj_set_style_pad_column(weather_box_, 8, 0);
    lv_obj_set_flex_flow(weather_box_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(weather_box_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(weather_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(weather_box_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(weather_box_, LV_ALIGN_CENTER, 0, 96);

    weather_icon_label_ = lv_label_create(weather_box_);
    lv_obj_set_style_text_color(weather_icon_label_, lv_color_hex(0x9ca3af), 0);
    lv_obj_set_style_text_font(weather_icon_label_, theme_->icon_font()->font(), 0);
    lv_label_set_text(weather_icon_label_, "");

    weather_text_label_ = lv_label_create(weather_box_);
    lv_obj_set_style_text_color(weather_text_label_, lv_color_hex(0x9ca3af), 0);
    lv_label_set_text(weather_text_label_, "");

    lv_obj_add_flag(weather_box_, LV_OBJ_FLAG_HIDDEN);
}

void AmbientScreen::ApplyFace() {
    bool digital = face_ == kFaceDigital;
    bool analog = face_ == kFaceAnalog;
    bool wallpaper = face_ == kFaceWallpaper;
    bool emoji = face_ == kFaceEmoji;

    auto set_hidden = [](lv_obj_t* obj, bool hidden) {
        if (hidden) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    };

    if (wallpaper && !wallpaper_loaded_) {
        LoadWallpaper();
    }
    if (wallpaper && !wallpaper_available_) {
        // No wallpaper packed in assets; fall back to the digital look
        wallpaper = false;
        digital = true;
    }

    if (emoji) {
        ActivateEmojiFace();
    } else {
        DeactivateEmojiFace();
    }

    set_hidden(wallpaper_image_, !wallpaper);
    set_hidden(emoji_image_, !emoji);
    set_hidden(analog_group_, !analog);
    set_hidden(seconds_arc_, !(digital || emoji));
    set_hidden(time_label_, analog);
    set_hidden(date_label_, false);
    set_hidden(weather_box_, weather_text_.empty());

    // Per-face layout tweaks
    if (analog) {
        lv_obj_align(date_label_, LV_ALIGN_CENTER, 0, 70);
        lv_obj_align(weather_box_, LV_ALIGN_CENTER, 0, 100);
    } else if (wallpaper) {
        // Push the clock up so the wallpaper stays visible
        lv_obj_align(time_label_, LV_ALIGN_TOP_MID, 0, 56);
        lv_obj_align(date_label_, LV_ALIGN_TOP_MID, 0, 116);
        lv_obj_align(weather_box_, LV_ALIGN_BOTTOM_MID, 0, -48);
    } else if (emoji) {
        lv_obj_align(time_label_, LV_ALIGN_CENTER, 0, 64);
        lv_obj_align(date_label_, LV_ALIGN_CENTER, 0, 112);
        lv_obj_align(weather_box_, LV_ALIGN_CENTER, 0, 140);
    } else {
        lv_obj_align(time_label_, LV_ALIGN_CENTER, 0, -24);
        lv_obj_align(date_label_, LV_ALIGN_CENTER, 0, 44);
        lv_obj_align(weather_box_, LV_ALIGN_CENTER, 0, 96);
    }

    last_minute_ = -1;  // Force refresh with the new layout
    UpdateClock();
}

void AmbientScreen::LoadWallpaper() {
    wallpaper_loaded_ = true;
    void* data = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData(kWallpaperAsset, data, size)) {
        ESP_LOGW(TAG, "No %s in assets partition, wallpaper face disabled", kWallpaperAsset);
        wallpaper_available_ = false;
        return;
    }
    wallpaper_raw_ = std::make_unique<LvglRawImage>(data, size);
    lv_image_set_src(wallpaper_image_, wallpaper_raw_->image_dsc());
    wallpaper_available_ = true;
}

void AmbientScreen::ActivateEmojiFace() {
    if (emoji_gif_ != nullptr || theme_ == nullptr) {
        return;
    }
    auto collection = theme_->emoji_collection();
    if (collection == nullptr) {
        return;
    }
    const LvglImage* image = collection->GetEmojiImage("neutral");
    if (image == nullptr || !image->IsGif()) {
        return;
    }
    emoji_gif_ = std::make_unique<LvglGif>(image->image_dsc());
    if (!emoji_gif_->IsLoaded()) {
        emoji_gif_.reset();
        return;
    }
    emoji_gif_->SetFrameCallback([this]() {
        lv_image_set_src(emoji_image_, emoji_gif_->image_dsc());
    });
    lv_image_set_src(emoji_image_, emoji_gif_->image_dsc());
    // Sleepy look: dim the emoji
    lv_obj_set_style_image_opa(emoji_image_, LV_OPA_60, 0);
    emoji_gif_->Start();
}

void AmbientScreen::DeactivateEmojiFace() {
    if (emoji_gif_ != nullptr) {
        emoji_gif_->Stop();
        emoji_gif_.reset();
    }
}

void AmbientScreen::SetFace(int face) {
    if (face < 0 || face >= kFaceCount || face == face_) {
        return;
    }
    face_ = face;
    Settings settings("display", true);
    settings.SetInt("ambient_face", face_);
    ApplyFace();
}

void AmbientScreen::NextFace(int direction) {
    int face = (face_ + direction + kFaceCount) % kFaceCount;
    SetFace(face);
}

void AmbientScreen::SetWeather(const std::string& icon_utf8, const std::string& text) {
    weather_icon_ = icon_utf8;
    weather_text_ = text;
    if (weather_box_ == nullptr) {
        return;
    }
    if (text.empty()) {
        lv_label_set_text(weather_icon_label_, "");
        lv_label_set_text(weather_text_label_, "");
        lv_obj_add_flag(weather_box_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(weather_icon_label_, icon_utf8.c_str());
    lv_label_set_text(weather_text_label_, text.c_str());
    lv_obj_remove_flag(weather_box_, LV_OBJ_FLAG_HIDDEN);
}

void AmbientScreen::UpdateAnalogHands(const struct tm& tm_local) {
    const float cx = LV_HOR_RES / 2.0f;
    const float cy = LV_VER_RES / 2.0f;

    auto set_hand = [&](lv_obj_t* hand, lv_point_precise_t* pts,
                        float angle_deg, float length, float back) {
        float a = angle_deg * (float)M_PI / 180.0f;
        float sa = sinf(a), ca = cosf(a);
        pts[0].x = (lv_value_precise_t)(cx - back * sa);
        pts[0].y = (lv_value_precise_t)(cy + back * ca);
        pts[1].x = (lv_value_precise_t)(cx + length * sa);
        pts[1].y = (lv_value_precise_t)(cy - length * ca);
        lv_line_set_points(hand, pts, 2);
    };

    float sec = (float)tm_local.tm_sec;
    float min = (float)tm_local.tm_min + sec / 60.0f;
    float hour = (float)(tm_local.tm_hour % 12) + min / 60.0f;

    set_hand(hour_hand_, hour_pts_, hour * 30.0f, 78.0f, 16.0f);
    set_hand(minute_hand_, minute_pts_, min * 6.0f, 118.0f, 16.0f);
    set_hand(second_hand_, second_pts_, sec * 6.0f, 138.0f, 26.0f);
}

void AmbientScreen::UpdateClock() {
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);

    if (tm_local.tm_year < 2025 - 1900) {
        // Clock not synced yet
        lv_label_set_text(time_label_, "--:--");
        lv_label_set_text(date_label_, "menunggu sinkronisasi jam...");
        return;
    }

    if (face_ == kFaceAnalog) {
        UpdateAnalogHands(tm_local);
    } else {
        lv_arc_set_value(seconds_arc_, tm_local.tm_sec);
    }

    if (tm_local.tm_min != last_minute_) {
        last_minute_ = tm_local.tm_min;
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", tm_local.tm_hour, tm_local.tm_min);
        lv_label_set_text(time_label_, buf);

        char date_buf[48];
        snprintf(date_buf, sizeof(date_buf), "%s, %d %s",
                 kDayNames[tm_local.tm_wday], tm_local.tm_mday,
                 kMonthNames[tm_local.tm_mon]);
        lv_label_set_text(date_label_, date_buf);

        if (on_minute_) {
            on_minute_();
        }
    }
}

void AmbientScreen::Show() {
    if (overlay_ == nullptr) {
        return;
    }
    last_minute_ = -1;  // Force immediate refresh
    if (face_ == kFaceEmoji) {
        ActivateEmojiFace();
        if (emoji_gif_ != nullptr) {
            emoji_gif_->Resume();
        }
    }
    UpdateClock();
    lv_timer_resume(tick_timer_);
    lv_obj_move_foreground(overlay_);
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
}

void AmbientScreen::Hide() {
    if (overlay_ == nullptr) {
        return;
    }
    lv_timer_pause(tick_timer_);
    if (emoji_gif_ != nullptr) {
        emoji_gif_->Pause();
    }
    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
}

bool AmbientScreen::IsVisible() const {
    return overlay_ != nullptr && !lv_obj_has_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
}
