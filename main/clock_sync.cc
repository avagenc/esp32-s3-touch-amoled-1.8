#include "clock_sync.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_netif_sntp.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>

#define TAG "ClockSync"

static const char* NTP_SERVER = "pool.ntp.org";

int ClockSync::GetTimezoneOffsetHours() {
    Settings settings("clock", false);
    return settings.GetInt("tz_offset", CONFIG_CLOCK_TIMEZONE_OFFSET);
}

void ClockSync::SetTimezoneOffsetHours(int hours) {
    Settings settings("clock", true);
    settings.SetInt("tz_offset", hours);
    ApplyTimezone();
}

void ClockSync::ApplyTimezone() {
    int offset = GetTimezoneOffsetHours();
    // POSIX TZ uses the opposite sign: UTC+7 is written as "UTC-7"
    char tz[16];
    snprintf(tz, sizeof(tz), "UTC%+d", -offset);
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to UTC%+d (TZ=%s)", offset, tz);
}

void ClockSync::Start() {
    ApplyTimezone();

    if (started_) {
        return;
    }
    started_ = true;

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    config.start = true;
    config.sync_cb = [](struct timeval* tv) {
        ClockSync::GetInstance().MarkSynced();
        time_t now = tv->tv_sec;
        struct tm tm_local;
        localtime_r(&now, &tm_local);
        ESP_LOGI(TAG, "SNTP synced, local time: %04d-%02d-%02d %02d:%02d:%02d",
                 tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
                 tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
    };

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start SNTP: %s", esp_err_to_name(err));
        started_ = false;
    }
}
