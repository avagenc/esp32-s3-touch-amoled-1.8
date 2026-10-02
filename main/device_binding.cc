#include "device_binding.h"
#include "board.h"
#include "system_info.h"
#include "settings.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_app_desc.h>

#include <memory>

#define TAG "DeviceBinding"

// Falls back to a placeholder when the Kconfig option is disabled, so this
// translation unit always compiles regardless of the binding toggle.
#ifndef CONFIG_AVAGENC_API_BASE_URL
#define CONFIG_AVAGENC_API_BASE_URL "https://api.avagenc.com"
#endif

namespace {

constexpr char kNvsNamespace[] = "avagenc";
constexpr char kTokenKey[] = "device_token";
constexpr char kApiUrlKey[] = "api_url";  // optional NVS override of the base URL

// Build an HTTP client carrying the device identity headers, mirroring Ota.
std::unique_ptr<Http> SetupHttp() {
    auto& board = Board::GetInstance();
    auto http = board.GetNetwork()->CreateHttp(0);
    http->SetTimeout(10000);
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid().c_str());
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
    http->SetHeader("Content-Type", "application/json");
    return http;
}

}  // namespace

DeviceBinding::DeviceBinding() {
    token_ = Settings(kNvsNamespace, false).GetString(kTokenKey);
}

bool DeviceBinding::IsBound() {
    return !GetToken().empty();
}

std::string DeviceBinding::GetToken() {
    if (token_.empty()) {
        token_ = Settings(kNvsNamespace, false).GetString(kTokenKey);
    }
    return token_;
}

std::string DeviceBinding::GetDeviceId() {
    return SystemInfo::GetMacAddress();
}

void DeviceBinding::SaveToken(const std::string& token) {
    Settings settings(kNvsNamespace, true);
    settings.SetString(kTokenKey, token);
    token_ = token;
}

void DeviceBinding::ClearBinding() {
    Settings settings(kNvsNamespace, true);
    settings.EraseKey(kTokenKey);
    token_.clear();
}

std::string DeviceBinding::GetApiBaseUrl() {
    std::string url = Settings(kNvsNamespace, false).GetString(kApiUrlKey);
    if (url.empty()) {
        url = CONFIG_AVAGENC_API_BASE_URL;
    }
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

bool DeviceBinding::RequestPairing(std::string& pairing_code, std::string& pair_url, int& poll_interval_s) {
    auto http = SetupHttp();
    std::string url = GetApiBaseUrl() + "/api/v1/devices/pair/start";

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "device_id", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(body, "device_code", SystemInfo::GetDeviceCode().c_str());
    cJSON_AddStringToObject(body, "model", BOARD_NAME);
    auto app_desc = esp_app_get_description();
    cJSON_AddStringToObject(body, "fw_version", app_desc->version);
    char* body_str = cJSON_PrintUnformatted(body);
    std::string content(body_str);
    cJSON_free(body_str);
    cJSON_Delete(body);

    http->SetContent(std::move(content));
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "pair/start: failed to open %s", url.c_str());
        return false;
    }
    int status = http->GetStatusCode();
    std::string resp = http->ReadAll();
    http->Close();
    if (status != 200) {
        ESP_LOGE(TAG, "pair/start: HTTP %d, body: %s", status, resp.c_str());
        return false;
    }

    cJSON* root = cJSON_Parse(resp.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "pair/start: invalid JSON response");
        return false;
    }

    // Idempotent: the backend may report the device is already bound here.
    cJSON* bound = cJSON_GetObjectItem(root, "bound");
    cJSON* token = cJSON_GetObjectItem(root, "device_token");
    if (cJSON_IsTrue(bound) && cJSON_IsString(token)) {
        SaveToken(token->valuestring);
        cJSON_Delete(root);
        return true;
    }

    cJSON* code = cJSON_GetObjectItem(root, "pairing_code");
    cJSON* purl = cJSON_GetObjectItem(root, "pair_url");
    cJSON* interval = cJSON_GetObjectItem(root, "poll_interval");
    if (cJSON_IsString(code)) {
        pairing_code = code->valuestring;
    }
    if (cJSON_IsString(purl)) {
        pair_url = purl->valuestring;
    }
    poll_interval_s = cJSON_IsNumber(interval) ? interval->valueint : 3;
    cJSON_Delete(root);

    return !pairing_code.empty();
}

bool DeviceBinding::PollStatus(bool& bound, std::string& device_token) {
    auto http = SetupHttp();
    std::string url = GetApiBaseUrl() + "/api/v1/devices/" + SystemInfo::GetMacAddress() + "/status";

    if (!http->Open("GET", url)) {
        return false;
    }
    int status = http->GetStatusCode();
    std::string resp = http->ReadAll();
    http->Close();
    if (status != 200) {
        return false;
    }

    cJSON* root = cJSON_Parse(resp.c_str());
    if (root == nullptr) {
        return false;
    }
    cJSON* b = cJSON_GetObjectItem(root, "bound");
    cJSON* t = cJSON_GetObjectItem(root, "device_token");
    bound = cJSON_IsTrue(b);
    if (bound && cJSON_IsString(t)) {
        device_token = t->valuestring;
    }
    cJSON_Delete(root);
    return true;
}

bool DeviceBinding::EnsureBound() {
    if (IsBound()) {
        ESP_LOGI(TAG, "Device already bound");
        return true;
    }

    auto display = Board::GetInstance().GetDisplay();
    constexpr int kMaxRequestFails = 20;     // give up if backend unreachable
    constexpr int kSessionSeconds = 300;     // refresh the code roughly every 5 min
    int request_fails = 0;

    while (true) {
        std::string pairing_code, pair_url;
        int poll_interval_s = 3;
        bool got = RequestPairing(pairing_code, pair_url, poll_interval_s);

        // RequestPairing may have stored a token (already-bound case).
        if (IsBound()) {
            display->HideQrCode();
            return true;
        }

        if (!got || pairing_code.empty()) {
            if (++request_fails >= kMaxRequestFails) {
                ESP_LOGE(TAG, "Giving up pairing after %d failed attempts", request_fails);
                display->HideQrCode();
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        request_fails = 0;

        if (poll_interval_s < 2) {
            poll_interval_s = 2;
        }
        std::string qr = pair_url.empty() ? pairing_code : pair_url;
        display->ShowQrCode(qr, pairing_code, "Pindai untuk menghubungkan ke akun avagenc");

        // Poll until bound or the pairing session expires.
        int elapsed = 0;
        while (elapsed < kSessionSeconds) {
            vTaskDelay(pdMS_TO_TICKS(poll_interval_s * 1000));
            elapsed += poll_interval_s;

            bool bound = false;
            std::string token;
            if (PollStatus(bound, token) && bound && !token.empty()) {
                SaveToken(token);
                display->HideQrCode();
                ESP_LOGI(TAG, "Device bound successfully");
                return true;
            }
        }
        ESP_LOGI(TAG, "Pairing session expired, requesting a fresh code");
    }
}
