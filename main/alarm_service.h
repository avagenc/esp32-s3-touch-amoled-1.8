#ifndef ALARM_SERVICE_H
#define ALARM_SERVICE_H

#include <esp_timer.h>

#include <string>

// Countdown timer plus a backend-triggered ring function, both exposed as MCP
// tools. Alarm SCHEDULES are not stored on the device — they live in the
// avagenc backend database (which has its own triggers); when an alarm is due
// the backend calls `self.alarm.ring` and the device just rings.
class AlarmService {
public:
    static AlarmService& GetInstance() {
        static AlarmService instance;
        return instance;
    }

    // Create the countdown timer. Idempotent.
    void Start();

    // Countdown timer (one at a time; a new one replaces the old)
    std::string SetTimer(int seconds);
    bool CancelTimer();
    int GetTimerRemainingSec();  // -1 if no timer running

    // Ring immediately (alert + sound). Called by the backend via MCP when a
    // scheduled alarm fires.
    void RingNow(const std::string& title);

private:
    AlarmService() = default;
    AlarmService(const AlarmService&) = delete;
    AlarmService& operator=(const AlarmService&) = delete;

    esp_timer_handle_t countdown_timer_ = nullptr;
    int64_t timer_deadline_us_ = 0;  // 0 = no timer
};

#endif // ALARM_SERVICE_H
