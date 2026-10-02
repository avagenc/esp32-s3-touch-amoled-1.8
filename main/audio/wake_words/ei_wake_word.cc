#include "ei_wake_word.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <cstring>

#define TAG "EiWakeWord"

EiWakeWord::EiWakeWord() {
    last_activity_time_ = esp_timer_get_time();
    ESP_LOGI(TAG, "EiWakeWord created");
}

EiWakeWord::~EiWakeWord() {
    Stop();
}

bool EiWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    if (!codec) {
        ESP_LOGE(TAG, "Codec is nullptr");
        return false;
    }

    codec_ = codec;
    
    // Check if codec sample rate matches EI model (16kHz)
    if (codec->input_sample_rate() != 16000) {
        ESP_LOGW(TAG, "Codec sample rate %d Hz != 16000 Hz, may need resampling",
                 codec->input_sample_rate());
    }

    ESP_LOGI(TAG, "Initialized with codec sample rate: %d Hz, feed size: %zu samples",
             codec->input_sample_rate(), FEED_SIZE);
    
    return true;
}

void EiWakeWord::Start() {
    if (running_) {
        return;
    }

    // Initialize EI wake word detection
    ei_wake_word_init();
    
    running_ = true;
    last_detected_wake_word_ = "";
    last_activity_time_ = esp_timer_get_time();
    
    ESP_LOGI(TAG, "Started - listening for '%s'", ei_wake_word_get_label());
}

void EiWakeWord::Stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    ei_wake_word_deinit();
    
    ESP_LOGI(TAG, "Stopped");
}

void EiWakeWord::Feed(const std::vector<int16_t>& data) {
    if (!running_ || data.size() != FEED_SIZE) {
        if (data.size() != FEED_SIZE) {
            ESP_LOGW(TAG, "Feed size mismatch: got %zu, expected %zu", data.size(), FEED_SIZE);
        }
        return;
    }

    // Check if idle for too long and reset if needed
    CheckAndResetIfIdle();

    // Feed audio directly to EI classifier (4000 samples = 1 slice)
    ei_wake_word_feed(const_cast<int16_t*>(data.data()), data.size());

    // Check if wake word was detected
    if (ei_wake_word_detected()) {
        float confidence = ei_wake_word_get_confidence();
        last_detected_wake_word_ = ei_wake_word_get_label();
        last_activity_time_ = esp_timer_get_time();

        ESP_LOGI(TAG, "*** WAKE WORD DETECTED: %s (confidence: %.2f) ***",
                 last_detected_wake_word_.c_str(), confidence);

        if (wake_word_detected_callback_) {
            wake_word_detected_callback_(last_detected_wake_word_);
        }
    }
}

void EiWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

size_t EiWakeWord::GetFeedSize() {
    return FEED_SIZE;  // 4000 samples = EI_CLASSIFIER_SLICE_SIZE
}

void EiWakeWord::EncodeWakeWordData() {
    // Edge Impulse wake word doesn't need to encode wake word data
}

bool EiWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    // Edge Impulse wake word doesn't provide opus encoded wake word data
    return false;
}

void EiWakeWord::CheckAndResetIfIdle() {
    int64_t current_time = esp_timer_get_time();
    int64_t idle_time = current_time - last_activity_time_;
    
    if (idle_time >= RESET_INTERVAL_US) {
        ESP_LOGI(TAG, "Idle for %lld seconds, resetting wake word state",
                 idle_time / 1000000);
        ResetWakeWordState();
        last_activity_time_ = current_time;
    }
}

void EiWakeWord::ResetWakeWordState() {
    ei_wake_word_deinit();
    ei_wake_word_init();
    ESP_LOGI(TAG, "Wake word state reset");
}
