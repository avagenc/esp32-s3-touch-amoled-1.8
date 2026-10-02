#include "weather_service.h"
#include "board.h"
#include "display.h"
#include "settings.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <cJSON.h>
#include <font_awesome.h>

#include <cctype>
#include <cstdio>

#define TAG "WeatherService"

static const int kFetchIntervalMin = 30;
static const int kRetryIntervalMin = 5;

// Map an Open-Meteo WMO weather code to a Font Awesome icon name and an
// Indonesian description.
static void DescribeWeatherCode(int code, bool is_day,
                                const char*& icon, const char*& text) {
    switch (code) {
        case 0:
            icon = is_day ? "sun" : "moon";
            text = "Cerah";
            break;
        case 1:
        case 2:
            icon = is_day ? "cloud_sun" : "cloud_moon";
            text = "Cerah Berawan";
            break;
        case 3:
            icon = "clouds";
            text = "Mendung";
            break;
        case 45:
        case 48:
            icon = "cloud_fog";
            text = "Berkabut";
            break;
        case 51: case 53: case 55: case 56: case 57:
            icon = "cloud_drizzle";
            text = "Gerimis";
            break;
        case 61: case 63: case 65: case 66: case 67:
            icon = "cloud_rain";
            text = "Hujan";
            break;
        case 71: case 73: case 75: case 77: case 85: case 86:
            icon = "cloud_sleet";
            text = "Salju";
            break;
        case 80: case 81: case 82:
            icon = "cloud_showers";
            text = "Hujan Deras";
            break;
        case 95: case 96: case 99:
            icon = "cloud_bolt";
            text = "Badai Petir";
            break;
        default:
            icon = "cloud";
            text = "Berawan";
            break;
    }
}

void WeatherService::Start() {
#if CONFIG_WEATHER_ENABLE
    if (started_) {
        return;
    }
    started_ = true;
    xTaskCreate(TaskLoop, "weather", 4096, this, 2, nullptr);
#endif
}

std::string WeatherService::GetLastReading() {
    return last_text_;
}

std::string WeatherService::GetLocationCity() {
    Settings settings("weather", false);
    return settings.GetString("city", "");
}

// URL-encode just enough for a city name (spaces and non-ASCII)
static std::string UrlEncode(const std::string& in) {
    std::string out;
    char buf[8];
    for (unsigned char c : in) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            out += (char)c;
        } else {
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string WeatherService::SetLocationCity(const std::string& city) {
    if (city.empty()) {
        return "Nama kota kosong";
    }

    char url[320];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=id",
             UrlEncode(city).c_str());

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return "Jaringan belum siap";
    }
    auto http = network->CreateHttp(0);
    http->SetTimeout(15000);
    if (!http->Open("GET", url) || http->GetStatusCode() != 200) {
        http->Close();
        return "Gagal menghubungi layanan geocoding";
    }
    std::string body = http->ReadAll();
    http->Close();

    std::string result = "Kota tidak ditemukan";
    cJSON* root = cJSON_Parse(body.c_str());
    if (root != nullptr) {
        cJSON* results = cJSON_GetObjectItem(root, "results");
        cJSON* first = cJSON_IsArray(results) ? cJSON_GetArrayItem(results, 0) : nullptr;
        if (cJSON_IsObject(first)) {
            cJSON* lat = cJSON_GetObjectItem(first, "latitude");
            cJSON* lon = cJSON_GetObjectItem(first, "longitude");
            cJSON* name = cJSON_GetObjectItem(first, "name");
            if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
                const char* resolved = cJSON_IsString(name) ? name->valuestring : city.c_str();
                char lat_s[16], lon_s[16];
                snprintf(lat_s, sizeof(lat_s), "%.4f", lat->valuedouble);
                snprintf(lon_s, sizeof(lon_s), "%.4f", lon->valuedouble);
                {
                    Settings settings("weather", true);
                    settings.SetString("city", resolved);
                    settings.SetString("lat", lat_s);
                    settings.SetString("lon", lon_s);
                }
                FetchOnce();  // Refresh right away with the new location
                char msg[96];
                snprintf(msg, sizeof(msg), "Lokasi cuaca diset ke %s", resolved);
                result = msg;
            }
        }
        cJSON_Delete(root);
    }
    return result;
}

void WeatherService::TaskLoop(void* arg) {
    auto self = static_cast<WeatherService*>(arg);
    // Give the network stack a moment before the first fetch
    vTaskDelay(pdMS_TO_TICKS(5000));
    while (true) {
        bool ok = self->FetchOnce();
        int delay_min = ok ? kFetchIntervalMin : kRetryIntervalMin;
        vTaskDelay(pdMS_TO_TICKS((uint64_t)delay_min * 60 * 1000));
    }
}

bool WeatherService::FetchOnce() {
#if CONFIG_WEATHER_ENABLE
    Settings settings("weather", false);
    std::string lat = settings.GetString("lat", CONFIG_WEATHER_LATITUDE);
    std::string lon = settings.GetString("lon", CONFIG_WEATHER_LONGITUDE);

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,weather_code,is_day",
             lat.c_str(), lon.c_str());

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    auto http = network->CreateHttp(0);
    http->SetTimeout(15000);
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "Failed to open %s", url);
        return false;
    }
    if (http->GetStatusCode() != 200) {
        ESP_LOGW(TAG, "Weather fetch HTTP %d", http->GetStatusCode());
        http->Close();
        return false;
    }
    std::string body = http->ReadAll();
    http->Close();

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Invalid weather JSON");
        return false;
    }
    bool ok = false;
    cJSON* current = cJSON_GetObjectItem(root, "current");
    if (cJSON_IsObject(current)) {
        cJSON* temp = cJSON_GetObjectItem(current, "temperature_2m");
        cJSON* code = cJSON_GetObjectItem(current, "weather_code");
        cJSON* is_day = cJSON_GetObjectItem(current, "is_day");
        if (cJSON_IsNumber(temp) && cJSON_IsNumber(code)) {
            const char* icon_name = nullptr;
            const char* desc = nullptr;
            DescribeWeatherCode(code->valueint,
                                !cJSON_IsNumber(is_day) || is_day->valueint != 0,
                                icon_name, desc);
            char text[48];
            snprintf(text, sizeof(text), "%d° %s",
                     (int)(temp->valuedouble + 0.5), desc);
            last_text_ = text;

            const char* icon_utf8 = font_awesome_get_utf8(icon_name);
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->SetWeather(icon_utf8 != nullptr ? icon_utf8 : "", text);
            }
            ESP_LOGI(TAG, "Weather: %s (code %d)", text, code->valueint);
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
#else
    return false;
#endif
}
