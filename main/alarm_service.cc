#include "alarm_service.h"
#include "application.h"
#include "assets/lang_config.h"

#include <esp_log.h>

#include <cstdio>

#define TAG "AlarmService"

void AlarmService::Start() {
    if (countdown_timer_ != nullptr) {
        return;
    }
    esp_timer_create_args_t countdown_args = {
        .callback = [](void* arg) {
            auto self = static_cast<AlarmService*>(arg);
            self->timer_deadline_us_ = 0;
            self->RingNow("Timer selesai!");
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_countdown",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&countdown_args, &countdown_timer_);
}

std::string AlarmService::SetTimer(int seconds) {
    if (seconds <= 0) {
        return "Durasi timer tidak valid";
    }
    Start();
    esp_timer_stop(countdown_timer_);
    timer_deadline_us_ = esp_timer_get_time() + (int64_t)seconds * 1000000;
    esp_timer_start_once(countdown_timer_, (uint64_t)seconds * 1000000ULL);

    char buf[80];
    if (seconds >= 3600) {
        snprintf(buf, sizeof(buf), "Timer %d jam %d menit dimulai",
                 seconds / 3600, (seconds % 3600) / 60);
    } else if (seconds >= 60) {
        snprintf(buf, sizeof(buf), "Timer %d menit %d detik dimulai",
                 seconds / 60, seconds % 60);
    } else {
        snprintf(buf, sizeof(buf), "Timer %d detik dimulai", seconds);
    }
    return buf;
}

bool AlarmService::CancelTimer() {
    if (timer_deadline_us_ == 0 || countdown_timer_ == nullptr) {
        return false;
    }
    esp_timer_stop(countdown_timer_);
    timer_deadline_us_ = 0;
    return true;
}

int AlarmService::GetTimerRemainingSec() {
    if (timer_deadline_us_ == 0) {
        return -1;
    }
    int64_t remaining = timer_deadline_us_ - esp_timer_get_time();
    return remaining > 0 ? (int)(remaining / 1000000) : 0;
}

void AlarmService::RingNow(const std::string& title) {
    ESP_LOGI(TAG, "RING: %s", title.c_str());
    auto& app = Application::GetInstance();
    app.Schedule([&app, title]() {
        app.Alert("Alarm", title.c_str(), "ecstatic", Lang::Sounds::OGG_VIBRATION);
        // Ring three times so it is hard to miss
        app.PlaySound(Lang::Sounds::OGG_VIBRATION);
        app.PlaySound(Lang::Sounds::OGG_VIBRATION);
    });
}
