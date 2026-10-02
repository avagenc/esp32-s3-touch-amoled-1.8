#ifndef WEATHER_SERVICE_H
#define WEATHER_SERVICE_H

#include <string>

// Fetches the current weather from Open-Meteo (no API key) every 30 minutes
// and pushes it to the display's ambient watch face. Latitude/longitude come
// from Kconfig defaults and can be overridden via NVS ("weather"/"lat","lon").
class WeatherService {
public:
    static WeatherService& GetInstance() {
        static WeatherService instance;
        return instance;
    }

    // Idempotent; call once the network is connected.
    void Start();

    // Last formatted reading, e.g. "28° Berawan" (empty until first fetch)
    std::string GetLastReading();

    // Set the weather location by city name (city-level only — the canonical
    // location lives in the backend database; the device just caches the
    // resolved city). Geocodes via Open-Meteo, stores city + lat/lon in NVS,
    // and refreshes the weather. Returns a human-readable result.
    std::string SetLocationCity(const std::string& city);

    // The stored city name ("" if never set; falls back to Kconfig coords).
    std::string GetLocationCity();

private:
    WeatherService() = default;
    WeatherService(const WeatherService&) = delete;
    WeatherService& operator=(const WeatherService&) = delete;

    static void TaskLoop(void* arg);
    bool FetchOnce();

    bool started_ = false;
    std::string last_text_;
};

#endif // WEATHER_SERVICE_H
