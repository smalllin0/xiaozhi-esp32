#include "no_audio_processor.h"
#include "esp_log.h"

#define TAG "NoAudioProcessor"

/// @brief 初始化音频处理器
/// @param codec 关联的编解码器
/// @param frame_duration_ms 帧时长设置
void NoAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms)
{
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
}

/// @brief 处理音频数据（轮换为单声道），使用输出回调输出处理过后的数据
void NoAudioProcessor::Feed(std::vector<int16_t>&& data)
{
    if (!is_running_ || !output_callback_) {
        return;
    }

    if (data.size() != frame_samples_) {
        ESP_LOGE(TAG, "feed_data_size != frame_size, feed size: %u, frame size: %u", data.size(), frame_samples_);
        return;
    }

    if (codec_->input_channels() == 2) {
        auto mono_data = std::vector<int16_t>(data.size() >> 1);
        auto mono_data_size = mono_data.size();
        for (size_t i = 0, j = 0; i < mono_data_size; i++, j += 2) {
            mono_data[i] = data[j];
        }
        output_callback_(std::move(mono_data));
    } else {
        output_callback_(std::move(data));
    }
}

/// @brief 打开/关闭设备AEC功能
void NoAudioProcessor::EnableDeviceAec(bool enable)
{
    if (enable) ESP_LOGE(TAG, "MCU not support AEC.");
}


