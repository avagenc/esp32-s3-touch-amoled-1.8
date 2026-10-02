#include "user_pairing.h"

#include "board.h"
#include "display/display.h"
#include "system_info.h"
#include "settings.h"
#include "wifi_manager.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_app_desc.h>

#define TAG "UserPairing"

// Local pairing HTTP port. Overridable via Kconfig; falls back so this TU always
// compiles regardless of the pairing toggle.
#ifndef CONFIG_AVAGENC_PAIRING_PORT
#define CONFIG_AVAGENC_PAIRING_PORT 8080
#endif

namespace {

constexpr char kNvsNamespace[] = "avagenc";
constexpr char kUserIdKey[]    = "user_id";
constexpr char kUserNameKey[]  = "user_name";
constexpr uint16_t kPairingPort = CONFIG_AVAGENC_PAIRING_PORT;

// Crockford base32 minus visually ambiguous letters (I L O U) — easy to read.
constexpr char kCodeAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

std::string GenerateCode() {
    std::string code;
    code.reserve(6);
    for (int i = 0; i < 6; ++i) {
        code += kCodeAlphabet[esp_random() % (sizeof(kCodeAlphabet) - 1)];
    }
    return code;
}

esp_err_t SendJson(httpd_req_t* req, const char* status, cJSON* obj) {
    char* s = cJSON_PrintUnformatted(obj);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    // The Android app talks to us directly (no browser), so CORS isn't required,
    // but a permissive header keeps a future web pairing page simple.
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, s ? s : "{}");
    cJSON_free(s);
    return ESP_OK;
}

esp_err_t SendError(httpd_req_t* req, const char* status, const char* err, const char* msg) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", err);
    cJSON_AddStringToObject(o, "message", msg);
    esp_err_t r = SendJson(req, status, o);
    cJSON_Delete(o);
    return r;
}

}  // namespace

// ---- HTTP handlers (friends of UserPairing, so they run in the httpd task) ----

esp_err_t UserPairingHandleInfo(httpd_req_t* req) {
    auto* self = static_cast<UserPairing*>(req->user_ctx);
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "device_id", self->GetDeviceId().c_str());
    cJSON_AddStringToObject(o, "device_code", SystemInfo::GetDeviceCode().c_str());
    cJSON_AddStringToObject(o, "model", BOARD_NAME);
    auto app_desc = esp_app_get_description();
    cJSON_AddStringToObject(o, "fw_version", app_desc->version);
    cJSON_AddBoolToObject(o, "paired", self->HasUser());
    esp_err_t r = SendJson(req, "200 OK", o);
    cJSON_Delete(o);
    return r;
}

esp_err_t UserPairingHandlePost(httpd_req_t* req) {
    auto* self = static_cast<UserPairing*>(req->user_ctx);

    int total = static_cast<int>(req->content_len);
    if (total <= 0 || total > 2048) {
        return SendError(req, "400 Bad Request", "invalid_request", "empty or oversized body");
    }
    std::string body;
    body.resize(total);
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, &body[received], total - received);
        if (r <= 0) {
            return SendError(req, "400 Bad Request", "invalid_request", "failed to read body");
        }
        received += r;
    }

    // Pairing code: prefer the X-Pair-Code header, fall back to a body field.
    std::string code;
    size_t hlen = httpd_req_get_hdr_value_len(req, "X-Pair-Code");
    if (hlen > 0 && hlen < 64) {
        char buf[64];
        if (httpd_req_get_hdr_value_str(req, "X-Pair-Code", buf, sizeof(buf)) == ESP_OK) {
            code = buf;
        }
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return SendError(req, "400 Bad Request", "invalid_request", "invalid JSON");
    }
    cJSON* uid   = cJSON_GetObjectItem(root, "user_id");
    cJSON* name  = cJSON_GetObjectItem(root, "display_name");
    cJSON* bcode = cJSON_GetObjectItem(root, "pairing_code");
    if (code.empty() && cJSON_IsString(bcode)) {
        code = bcode->valuestring;
    }
    std::string user_id      = cJSON_IsString(uid)  ? uid->valuestring  : "";
    std::string display_name = cJSON_IsString(name) ? name->valuestring : "";
    cJSON_Delete(root);

    if (user_id.empty()) {
        return SendError(req, "400 Bad Request", "invalid_request", "missing user_id");
    }
    if (!self->AcceptPairing(code, user_id, display_name)) {
        return SendError(req, "403 Forbidden", "invalid_code",
                         "pairing code does not match the one on the device screen");
    }

    ESP_LOGI(TAG, "Paired via LAN to user_id=%s", user_id.c_str());
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", true);
    cJSON_AddStringToObject(o, "device_id", self->GetDeviceId().c_str());
    cJSON_AddStringToObject(o, "device_code", SystemInfo::GetDeviceCode().c_str());
    cJSON_AddStringToObject(o, "display_name", display_name.c_str());
    esp_err_t r = SendJson(req, "200 OK", o);
    cJSON_Delete(o);

    // Wake EnsurePaired() only after the response is on the wire, so the server
    // isn't torn down before the phone gets its success reply.
    if (self->paired_sem_ != nullptr) {
        xSemaphoreGive(self->paired_sem_);
    }
    return r;
}

// ------------------------------- UserPairing --------------------------------

UserPairing::UserPairing() {
    Settings settings(kNvsNamespace, false);
    user_id_ = settings.GetString(kUserIdKey);
    display_name_ = settings.GetString(kUserNameKey);
    paired_sem_ = xSemaphoreCreateBinary();
}

UserPairing::~UserPairing() {
    StopServer();
    if (paired_sem_ != nullptr) {
        vSemaphoreDelete(paired_sem_);
    }
}

bool UserPairing::HasUser() {
    return !user_id_.empty();
}

std::string UserPairing::GetUserId() {
    if (user_id_.empty()) {
        user_id_ = Settings(kNvsNamespace, false).GetString(kUserIdKey);
    }
    return user_id_;
}

std::string UserPairing::GetDisplayName() {
    return display_name_;
}

std::string UserPairing::GetDeviceId() {
    return SystemInfo::GetMacAddress();
}

void UserPairing::Save(const std::string& user_id, const std::string& display_name) {
    Settings settings(kNvsNamespace, true);
    settings.SetString(kUserIdKey, user_id);
    settings.SetString(kUserNameKey, display_name);
    user_id_ = user_id;
    display_name_ = display_name;
}

void UserPairing::ClearUser() {
    Settings settings(kNvsNamespace, true);
    settings.EraseKey(kUserIdKey);
    settings.EraseKey(kUserNameKey);
    user_id_.clear();
    display_name_.clear();
}

bool UserPairing::AcceptPairing(const std::string& code, const std::string& user_id,
                                const std::string& display_name) {
    // Constant-time compare against the active code (proof the phone saw the QR).
    if (code_.empty() || code.size() != code_.size()) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < code_.size(); ++i) {
        diff |= static_cast<uint8_t>(code[i] ^ code_[i]);
    }
    if (diff != 0) {
        return false;
    }
    Save(user_id, display_name);
    // Note: the caller (HTTP handler) signals paired_sem_ *after* the response is
    // sent, so EnsurePaired() doesn't stop the server mid-reply.
    return true;
}

std::string UserPairing::BuildQrPayload(const std::string& ip) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "v", 1);
    cJSON_AddStringToObject(o, "type", "avagenc_pair");
    cJSON_AddStringToObject(o, "device_id", GetDeviceId().c_str());
    cJSON_AddStringToObject(o, "device_code", SystemInfo::GetDeviceCode().c_str());
    cJSON_AddStringToObject(o, "ip", ip.c_str());
    cJSON_AddNumberToObject(o, "port", kPairingPort);
    cJSON_AddStringToObject(o, "code", code_.c_str());
    std::string pair_url = "http://" + ip + ":" + std::to_string(kPairingPort) + "/api/v1/pair";
    cJSON_AddStringToObject(o, "pair_url", pair_url.c_str());
    char* s = cJSON_PrintUnformatted(o);
    std::string out(s ? s : "{}");
    cJSON_free(s);
    cJSON_Delete(o);
    return out;
}

bool UserPairing::StartServer(uint16_t port) {
    if (server_ != nullptr) {
        return true;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = port + 100;      // keep clear of the default control socket
    config.stack_size = 6144;           // headroom for cJSON + NVS write
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;

    if (httpd_start(&server_, &config) != ESP_OK) {
        server_ = nullptr;
        return false;
    }
    httpd_uri_t post_uri = {};
    post_uri.uri = "/api/v1/pair";
    post_uri.method = HTTP_POST;
    post_uri.handler = UserPairingHandlePost;
    post_uri.user_ctx = this;
    httpd_register_uri_handler(server_, &post_uri);

    httpd_uri_t info_uri = {};
    info_uri.uri = "/api/v1/pair/info";
    info_uri.method = HTTP_GET;
    info_uri.handler = UserPairingHandleInfo;
    info_uri.user_ctx = this;
    httpd_register_uri_handler(server_, &info_uri);
    return true;
}

void UserPairing::StopServer() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}

bool UserPairing::EnsurePaired() {
    if (HasUser()) {
        ESP_LOGI(TAG, "User already paired: %s", user_id_.c_str());
        return true;
    }

    auto display = Board::GetInstance().GetDisplay();
    auto& wifi = WifiManager::GetInstance();

    // Wait for a WiFi IP — pairing is LAN-based, so the phone must reach us.
    std::string ip;
    for (int i = 0; i < 100 && (ip.empty() || ip == "0.0.0.0"); ++i) {
        ip = wifi.GetIpAddress();
        if (!ip.empty() && ip != "0.0.0.0") {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (ip.empty() || ip == "0.0.0.0") {
        ESP_LOGE(TAG, "No WiFi IP available; cannot start local pairing");
        return false;
    }

    code_ = GenerateCode();
    if (!StartServer(kPairingPort)) {
        ESP_LOGE(TAG, "Failed to start pairing HTTP server on port %u", kPairingPort);
        return false;
    }
    ESP_LOGI(TAG, "Local pairing listening at http://%s:%u/api/v1/pair (code=%s)",
             ip.c_str(), kPairingPort, code_.c_str());

    const char* caption = "Pindai dengan aplikasi avagenc untuk masuk";
    display->ShowQrCode(BuildQrPayload(ip), code_, caption);

    // Block until the phone completes pairing. Wake up periodically to refresh
    // the QR if the DHCP lease hands us a new IP.
    while (!HasUser()) {
        if (xSemaphoreTake(paired_sem_, pdMS_TO_TICKS(1000)) == pdTRUE) {
            break;
        }
        std::string cur = wifi.GetIpAddress();
        if (!cur.empty() && cur != "0.0.0.0" && cur != ip) {
            ip = cur;
            ESP_LOGI(TAG, "WiFi IP changed to %s, refreshing pairing QR", ip.c_str());
            display->ShowQrCode(BuildQrPayload(ip), code_, caption);
        }
    }

    // Let the success response flush to the phone before tearing down the server.
    vTaskDelay(pdMS_TO_TICKS(300));
    display->HideQrCode();
    StopServer();
    ESP_LOGI(TAG, "Pairing complete for user_id=%s", user_id_.c_str());
    return true;
}
