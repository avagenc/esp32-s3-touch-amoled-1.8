#ifndef EI_WAKE_WORD_H
#define EI_WAKE_WORD_H

#include <string>
#include <vector>
#include <functional>
#include <atomic>

#include "audio_codec.h"
#include "wake_word.h"

// Forward declarations for C functions in wake_word.cpp
extern "C" {
    void ei_wake_word_init();
    void ei_wake_word_feed(int16_t* audio_data, size_t length);
    bool ei_wake_word_detected();
    float ei_wake_word_get_confidence();
    void ei_wake_word_deinit();
    const char* ei_wake_word_get_label();
}

class EiWakeWord : public WakeWord {
public:
    EiWakeWord();
    ~EiWakeWord() override;

    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) override;
    void Feed(const std::vector<int16_t>& data) override;
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) override;
    void Start() override;
    void Stop() override;
    size_t GetFeedSize() override;
    void EncodeWakeWordData() override;
    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override;
    const std::string& GetLastDetectedWakeWord() const override { return last_detected_wake_word_; }

private:
    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
    AudioCodec* codec_ = nullptr;
    std::string last_detected_wake_word_;
    std::atomic<bool> running_ = false;
    int64_t last_activity_time_ = 0;
    
    static constexpr size_t FEED_SIZE = 4000;  // EI_CLASSIFIER_SLICE_SIZE = 16000/4 = 4000 samples (0.25s at 16kHz)
    static constexpr int64_t RESET_INTERVAL_US = 30 * 1000000; // 30 seconds
    
    void CheckAndResetIfIdle();
    void ResetWakeWordState();
};

#endif // EI_WAKE_WORD_H
