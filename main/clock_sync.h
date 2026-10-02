#ifndef CLOCK_SYNC_H
#define CLOCK_SYNC_H

// Keeps the system clock in UTC and the TZ environment variable pointing at
// the user's timezone. All display code should use localtime_r().
// SNTP runs periodically once the network is up; Ota's server_time is only
// used as a fallback before the first SNTP sync (see ota.cc).
class ClockSync {
public:
    static ClockSync& GetInstance() {
        static ClockSync instance;
        return instance;
    }

    // Read tz_offset from NVS and apply it to the TZ env var. Safe to call
    // multiple times; call early so any localtime() user sees the right zone.
    void ApplyTimezone();

    // Start SNTP (idempotent). Call once the network is connected.
    void Start();

    bool IsSynced() const { return synced_; }
    void MarkSynced() { synced_ = true; }

    // Timezone offset in hours east of UTC (e.g. 7 for WIB).
    int GetTimezoneOffsetHours();
    void SetTimezoneOffsetHours(int hours);

private:
    ClockSync() = default;
    ClockSync(const ClockSync&) = delete;
    ClockSync& operator=(const ClockSync&) = delete;

    bool started_ = false;
    volatile bool synced_ = false;
};

#endif // CLOCK_SYNC_H
