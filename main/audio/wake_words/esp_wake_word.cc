#include "esp_wake_word.h"
#include "esp_log.h"

#define TAG "EspWakeWord"

EspWakeWord::~EspWakeWord()
{
    if (wakenet_data_) {
        wakenet_iface_->destroy(wakenet_data_);
        esp_srmodel_deinit(model_);
    }
}

/// @brief 初始化唤醒词模型
/// @param codec 编解码器
bool EspWakeWord::Initialize(AudioCodec* codec)
{
    codec_ = codec;

    model_ = esp_srmodel_init("model");     // 加载模型
    if (model_ == nullptr || model_->num == -1) {
        ESP_LOGE(TAG, "Failed to load SR model.");
        return false;
    } else if (model_->num >= 1) {
        if (model_->num > 1) {
            ESP_LOGI(TAG, "Found %d SR models, default use first one.", model_->num);
        }
    } else {
        ESP_LOGE(TAG, "No SR model found.");
        return false;
    }
    
    // 获取模型名称
    auto* model_name = model_->model_name[0];
    // 根据模型名获取唤醒词句柄
    wakenet_iface_ = (esp_wn_iface_t*)esp_wn_handle_from_name(model_name);
    // 创建唤醒词实例
    wakenet_data_ = wakenet_iface_->create(model_name, DET_MODE_95);

    // 获取唤醒词检测所需的采样率
    int frequency = wakenet_iface_->get_samp_rate(wakenet_data_);
    // 获取唤醒词检测所需的音频块大小
    int audio_chunksize = wakenet_iface_->get_samp_chunksize(wakenet_data_);
    ESP_LOGI(TAG, "Wake word: %s, freq: %d, chunksize: %d.", model_name, frequency, audio_chunksize);

    return true;
}

/// @brief 将音频数据投喂到唤醒词模型，检测到唤醒词时调用相应的回调函数
void EspWakeWord::Feed(const std::vector<int16_t>& data)
{
    if (wakenet_data_ == nullptr || !running_.load()) {
        return;
    }

    // 获取唤醒词的索引（为0时表示没有检测到）
    int res = wakenet_iface_->detect(wakenet_data_, (int16_t*)data.data());
    if (res) {
        last_detected_wake_word_ = wakenet_iface_->get_word_name(wakenet_data_, res);
        running_.store(false);

        if (wake_word_detected_callback_) {
            wake_word_detected_callback_(last_detected_wake_word_);
        }
    }
}
