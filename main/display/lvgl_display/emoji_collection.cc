#include "emoji_collection.h"

#include <esp_log.h>
#include <unordered_map>
#include <string>

#define TAG "EmojiCollection"

void EmojiCollection::AddEmoji(const std::string& name, LvglImage* image) {
    emoji_collection_[name] = image;
}

const LvglImage* EmojiCollection::GetEmojiImage(const char* name) {
    auto it = emoji_collection_.find(name);
    if (it != emoji_collection_.end()) {
        return it->second;
    }

    ESP_LOGW(TAG, "Emoji not found: %s", name);
    return nullptr;
}

EmojiCollection::~EmojiCollection() {
    for (auto it = emoji_collection_.begin(); it != emoji_collection_.end(); ++it) {
        delete it->second;
    }
    emoji_collection_.clear();
}

// These are declared in xiaozhi-fonts/src/font_emoji_32.c
extern const lv_image_dsc_t emoji_1f636_32; // neutral
extern const lv_image_dsc_t emoji_1f642_32; // happy
extern const lv_image_dsc_t emoji_1f606_32; // laughing
extern const lv_image_dsc_t emoji_1f602_32; // funny
extern const lv_image_dsc_t emoji_1f614_32; // sad
extern const lv_image_dsc_t emoji_1f620_32; // angry
extern const lv_image_dsc_t emoji_1f62d_32; // crying
extern const lv_image_dsc_t emoji_1f60d_32; // loving
extern const lv_image_dsc_t emoji_1f633_32; // embarrassed
extern const lv_image_dsc_t emoji_1f62f_32; // surprised
extern const lv_image_dsc_t emoji_1f631_32; // shocked
extern const lv_image_dsc_t emoji_1f914_32; // thinking
extern const lv_image_dsc_t emoji_1f609_32; // winking
extern const lv_image_dsc_t emoji_1f60e_32; // cool
extern const lv_image_dsc_t emoji_1f60c_32; // relaxed
extern const lv_image_dsc_t emoji_1f924_32; // delicious
extern const lv_image_dsc_t emoji_1f618_32; // kissy
extern const lv_image_dsc_t emoji_1f60f_32; // confident
extern const lv_image_dsc_t emoji_1f634_32; // sleepy
extern const lv_image_dsc_t emoji_1f61c_32; // silly
extern const lv_image_dsc_t emoji_1f644_32; // confused

Twemoji32::Twemoji32() {
    AddEmoji("neutral", new LvglSourceImage(&emoji_1f636_32));
    AddEmoji("happy", new LvglSourceImage(&emoji_1f642_32));
    AddEmoji("laughing", new LvglSourceImage(&emoji_1f606_32));
    AddEmoji("funny", new LvglSourceImage(&emoji_1f602_32));
    AddEmoji("sad", new LvglSourceImage(&emoji_1f614_32));
    AddEmoji("angry", new LvglSourceImage(&emoji_1f620_32));
    AddEmoji("crying", new LvglSourceImage(&emoji_1f62d_32));
    AddEmoji("loving", new LvglSourceImage(&emoji_1f60d_32));
    AddEmoji("embarrassed", new LvglSourceImage(&emoji_1f633_32));
    AddEmoji("surprised", new LvglSourceImage(&emoji_1f62f_32));
    AddEmoji("shocked", new LvglSourceImage(&emoji_1f631_32));
    AddEmoji("thinking", new LvglSourceImage(&emoji_1f914_32));
    AddEmoji("winking", new LvglSourceImage(&emoji_1f609_32));
    AddEmoji("cool", new LvglSourceImage(&emoji_1f60e_32));
    AddEmoji("relaxed", new LvglSourceImage(&emoji_1f60c_32));
    AddEmoji("delicious", new LvglSourceImage(&emoji_1f924_32));
    AddEmoji("kissy", new LvglSourceImage(&emoji_1f618_32));
    AddEmoji("confident", new LvglSourceImage(&emoji_1f60f_32));
    AddEmoji("sleepy", new LvglSourceImage(&emoji_1f634_32));
    AddEmoji("silly", new LvglSourceImage(&emoji_1f61c_32));
    AddEmoji("confused", new LvglSourceImage(&emoji_1f644_32));
}


// These are declared in xiaozhi-fonts/src/font_emoji_64.c (but currently unused if we switch to 128)
// We keep Twemoji64 for backward compatibility if needed, but adding Twemoji128 below.
extern const lv_image_dsc_t emoji_1f636_64; // neutral
extern const lv_image_dsc_t emoji_1f642_64; // happy
extern const lv_image_dsc_t emoji_1f606_64; // laughing
extern const lv_image_dsc_t emoji_1f602_64; // funny
extern const lv_image_dsc_t emoji_1f614_64; // sad
extern const lv_image_dsc_t emoji_1f620_64; // angry
extern const lv_image_dsc_t emoji_1f62d_64; // crying
extern const lv_image_dsc_t emoji_1f60d_64; // loving
extern const lv_image_dsc_t emoji_1f633_64; // embarrassed
extern const lv_image_dsc_t emoji_1f62f_64; // surprised
extern const lv_image_dsc_t emoji_1f631_64; // shocked
extern const lv_image_dsc_t emoji_1f914_64; // thinking
extern const lv_image_dsc_t emoji_1f609_64; // winking
extern const lv_image_dsc_t emoji_1f60e_64; // cool
extern const lv_image_dsc_t emoji_1f60c_64; // relaxed
extern const lv_image_dsc_t emoji_1f924_64; // delicious
extern const lv_image_dsc_t emoji_1f618_64; // kissy
extern const lv_image_dsc_t emoji_1f60f_64; // confident
extern const lv_image_dsc_t emoji_1f634_64; // sleepy
extern const lv_image_dsc_t emoji_1f61c_64; // silly
extern const lv_image_dsc_t emoji_1f644_64; // confused

Twemoji64::Twemoji64() {
    AddEmoji("neutral", new LvglSourceImage(&emoji_1f636_64));
    AddEmoji("happy", new LvglSourceImage(&emoji_1f642_64));
    AddEmoji("laughing", new LvglSourceImage(&emoji_1f606_64));
    AddEmoji("funny", new LvglSourceImage(&emoji_1f602_64));
    AddEmoji("sad", new LvglSourceImage(&emoji_1f614_64));
    AddEmoji("angry", new LvglSourceImage(&emoji_1f620_64));
    AddEmoji("crying", new LvglSourceImage(&emoji_1f62d_64));
    AddEmoji("loving", new LvglSourceImage(&emoji_1f60d_64));
    AddEmoji("embarrassed", new LvglSourceImage(&emoji_1f633_64));
    AddEmoji("surprised", new LvglSourceImage(&emoji_1f62f_64));
    AddEmoji("shocked", new LvglSourceImage(&emoji_1f631_64));
    AddEmoji("thinking", new LvglSourceImage(&emoji_1f914_64));
    AddEmoji("winking", new LvglSourceImage(&emoji_1f609_64));
    AddEmoji("cool", new LvglSourceImage(&emoji_1f60e_64));
    AddEmoji("relaxed", new LvglSourceImage(&emoji_1f60c_64));
    AddEmoji("delicious", new LvglSourceImage(&emoji_1f924_64));
    AddEmoji("kissy", new LvglSourceImage(&emoji_1f618_64));
    AddEmoji("confident", new LvglSourceImage(&emoji_1f60f_64));
    AddEmoji("sleepy", new LvglSourceImage(&emoji_1f634_64));
    AddEmoji("silly", new LvglSourceImage(&emoji_1f61c_64));
    AddEmoji("confused", new LvglSourceImage(&emoji_1f644_64));
}

// These are the new 128x128 emojis generated by convert_custom_emoji.py
extern const lv_image_dsc_t emoji_1f636_128; // neutral
extern const lv_image_dsc_t emoji_1f642_128; // happy
extern const lv_image_dsc_t emoji_1f606_128; // laughing
extern const lv_image_dsc_t emoji_1f602_128; // funny
extern const lv_image_dsc_t emoji_1f614_128; // sad
extern const lv_image_dsc_t emoji_1f620_128; // angry
extern const lv_image_dsc_t emoji_1f62d_128; // crying
extern const lv_image_dsc_t emoji_1f60d_128; // loving
extern const lv_image_dsc_t emoji_1f633_128; // embarrassed
extern const lv_image_dsc_t emoji_1f62f_128; // surprised
extern const lv_image_dsc_t emoji_1f631_128; // shocked
extern const lv_image_dsc_t emoji_1f914_128; // thinking
extern const lv_image_dsc_t emoji_1f609_128; // winking
extern const lv_image_dsc_t emoji_1f60e_128; // cool
extern const lv_image_dsc_t emoji_1f60c_128; // relaxed
extern const lv_image_dsc_t emoji_1f924_128; // delicious
extern const lv_image_dsc_t emoji_1f618_128; // kissy
extern const lv_image_dsc_t emoji_1f60f_128; // confident
extern const lv_image_dsc_t emoji_1f634_128; // sleepy
extern const lv_image_dsc_t emoji_1f61c_128; // silly
extern const lv_image_dsc_t emoji_1f644_128; // confused

Twemoji128::Twemoji128() {
    AddEmoji("neutral", new LvglSourceImage(&emoji_1f636_128));
    AddEmoji("happy", new LvglSourceImage(&emoji_1f642_128));
    AddEmoji("laughing", new LvglSourceImage(&emoji_1f606_128));
    AddEmoji("funny", new LvglSourceImage(&emoji_1f602_128));
    AddEmoji("sad", new LvglSourceImage(&emoji_1f614_128));
    AddEmoji("angry", new LvglSourceImage(&emoji_1f620_128));
    AddEmoji("crying", new LvglSourceImage(&emoji_1f62d_128));
    AddEmoji("loving", new LvglSourceImage(&emoji_1f60d_128));
    AddEmoji("embarrassed", new LvglSourceImage(&emoji_1f633_128));
    AddEmoji("surprised", new LvglSourceImage(&emoji_1f62f_128));
    AddEmoji("shocked", new LvglSourceImage(&emoji_1f631_128));
    AddEmoji("thinking", new LvglSourceImage(&emoji_1f914_128));
    AddEmoji("winking", new LvglSourceImage(&emoji_1f609_128));
    AddEmoji("cool", new LvglSourceImage(&emoji_1f60e_128));
    AddEmoji("relaxed", new LvglSourceImage(&emoji_1f60c_128));
    AddEmoji("delicious", new LvglSourceImage(&emoji_1f924_128));
    AddEmoji("kissy", new LvglSourceImage(&emoji_1f618_128));
    AddEmoji("confident", new LvglSourceImage(&emoji_1f60f_128));
    AddEmoji("sleepy", new LvglSourceImage(&emoji_1f634_128));
    AddEmoji("silly", new LvglSourceImage(&emoji_1f61c_128));
    AddEmoji("confused", new LvglSourceImage(&emoji_1f644_128));
}
