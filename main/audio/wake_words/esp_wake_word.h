#ifndef ESP_WAKE_WORD_H_
#define ESP_WAKE_WORD_H_

#include "wake_word.h"
#include <atomic>
#include "audio_codec.h"
#include <vector>
#include <functional>
#include <string>
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"


class EspWakeWord : public WakeWord {
public:
    EspWakeWord(){}
    ~EspWakeWord();

    bool Initialize(AudioCodec* codec);
    void Feed(const std::vector<int16_t>& data);
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
        wake_word_detected_callback_ = callback;
    }
    void Start() {
        running_.store(true);
    }
    void Stop() {
        running_.store(false);
    }
    size_t GetFeedSize() {
        return wakenet_data_ ? wakenet_iface_->get_samp_chunksize(wakenet_data_) : 0;
    }
    void EncodeWakeWordData(){}
    bool GetWakeWordOpus(std::vector<uint8_t>& opus) {
        return false;
    }
    const std::string& GetLastDetectedWakeWord() const { return last_detected_wake_word_; }
private:
    std::atomic<bool>   running_{false};
    esp_wn_iface_t*     wakenet_iface_{nullptr};    // 唤醒句柄
    model_iface_data_t* wakenet_data_{nullptr};     // 创建句柄词检测实例
    srmodel_list_t*     model_{nullptr};            // 加载的模型列表
    AudioCodec*         codec_{nullptr};            // 音频编解码实例

    std::string         last_detected_wake_word_;
    std::function<void(const std::string& wake_work)> wake_word_detected_callback_;
};

#endif // !ESP_WAKE_WORD_H_