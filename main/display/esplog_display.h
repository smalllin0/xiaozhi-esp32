#ifndef ESPLOG_DISPLAY_H_
#define ESPLOG_DISPLAY_H_

#include "display.h"

#include <string>

typedef struct {
    const char* keyword;
    const char* emoji;
} emoji_map_t;

const emoji_map_t emoji_map[] = {
    {"neutral",     "😶"},
    {"happy",       "🙂"},
    {"laughing",    "😆"},
    {"funny",       "😂"},
    {"sad",         "😔"},
    {"angry",       "😠"},
    {"crying",      "😭"},
    {"loving",      "😍"},
    {"embarrassed", "😳"},
    {"surprised",   "😲"},
    {"shocked",     "😱"},
    {"thinking",    "🤔"},
    {"winking",     "😉"},
    {"cool",        "😎"},
    {"relaxed",     "😌"},
    {"delicious",   "🤤"},
    {"kissy",       "😘"},
    {"confident",   "😏"},
    {"sleepy",      "😴"},
    {"silly",       "😜"},
    {"confused",    "🙄"},
    // 后续根据协议扩展
};


class EspLogDisplay : public Display { 
public:
    EspLogDisplay();
    ~EspLogDisplay();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override; 
    virtual void SetIcon(const char* icon) override;
    virtual inline void SetPreviewImage(const lv_img_dsc_t* image) override {}
    virtual inline void SetTheme(const std::string& theme_name) override {}
    virtual inline void UpdateStatusBar(bool update_all = false) override {}

protected:
    virtual inline bool Lock(int timeout_ms = 0) override { return true; } 
    virtual inline void Unlock() override {}
};

#endif
