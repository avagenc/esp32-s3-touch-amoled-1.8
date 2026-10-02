#ifndef _USER_PAIRING_H_
#define _USER_PAIRING_H_

#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_http_server.h>

// Pairs this device to an avagenc user account over the *local* network — no
// backend / DB involved yet. The user_id is stored on the device (NVS).
//
// Flow (see pairing.md for the full mobile-app contract):
//   1. On boot, if no user_id is stored, the device reads its WiFi IP, mints a
//      short one-time pairing code, and starts a small HTTP server ("listening
//      mode") on the LAN.
//   2. It shows a QR on the LCD encoding { device_id, ip, port, code, ... }.
//   3. The avagenc Android app (logged-in user, same WiFi) scans the QR and
//      POSTs the user_id to the device, presenting the code as proof it saw the
//      screen (header `X-Pair-Code`).
//   4. The device validates the code, stores the user_id, stops the server,
//      hides the QR and continues booting. At runtime the stored user_id is
//      sent to the bridge (see websocket_protocol.cc, `X-User-Id`).
class UserPairing {
public:
    UserPairing();
    ~UserPairing();

    // True once a user_id is stored in NVS.
    bool HasUser();

    // Stored identity ("" if none).
    std::string GetUserId();
    std::string GetDisplayName();

    // The stable device id (WiFi MAC) used as the pairing key.
    std::string GetDeviceId();

    // Blocking pairing flow. Returns immediately if a user is already stored.
    // Otherwise it shows the QR, runs the local HTTP server and blocks until the
    // phone completes pairing. Intended to run inside a task (ActivationTask).
    // Returns true once a user is stored.
    bool EnsurePaired();

    // Forget the stored user (logout / re-pair).
    void ClearUser();

private:
    friend esp_err_t UserPairingHandlePost(httpd_req_t* req);
    friend esp_err_t UserPairingHandleInfo(httpd_req_t* req);

    std::string BuildQrPayload(const std::string& ip);
    bool StartServer(uint16_t port);
    void StopServer();
    // Called from the HTTP handler (httpd task) once a valid request arrives.
    // Validates the code, persists the identity and wakes EnsurePaired().
    bool AcceptPairing(const std::string& code, const std::string& user_id,
                        const std::string& display_name);
    void Save(const std::string& user_id, const std::string& display_name);

    std::string user_id_;
    std::string display_name_;
    std::string code_;                 // current one-time pairing code
    httpd_handle_t server_ = nullptr;
    SemaphoreHandle_t paired_sem_ = nullptr;
};

#endif // _USER_PAIRING_H_
