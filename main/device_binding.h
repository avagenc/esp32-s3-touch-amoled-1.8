#ifndef _DEVICE_BINDING_H_
#define _DEVICE_BINDING_H_

#include <string>

// Pairs this device to an avagenc user account.
//
// The device identity key is the WiFi MAC (sent as Device-Id everywhere).
// When the device has no stored binding token it asks the avagenc backend for a
// short pairing code, shows a QR code + the code on the LCD, and polls until a
// user (logged in to the avagenc app/web) binds it. The backend then issues an
// opaque device_token which is stored in NVS and sent as a Bearer token on the
// audio websocket; the backend resolves the user_id from that token.
class DeviceBinding {
public:
    DeviceBinding();

    // True if a device_token is already stored in NVS.
    bool IsBound();

    // The stored device token ("" if none).
    std::string GetToken();

    // The stable device id (MAC) used as the pairing key.
    std::string GetDeviceId();

    // Blocking pairing flow. Returns immediately if already bound. Otherwise it
    // requests a pairing session, shows the QR + short code on the display, and
    // polls the backend until bound. Refreshes the code when a session expires.
    // Returns true once bound; returns false only if the backend is unreachable
    // for too long (so the caller can still continue booting).
    // Intended to run inside a task (e.g. Application::ActivationTask).
    bool EnsureBound();

    // Forget the stored token (re-pair / testing).
    void ClearBinding();

private:
    std::string GetApiBaseUrl();
    void SaveToken(const std::string& token);
    bool RequestPairing(std::string& pairing_code, std::string& pair_url, int& poll_interval_s);
    bool PollStatus(bool& bound, std::string& device_token);

    std::string token_;
};

#endif // _DEVICE_BINDING_H_
