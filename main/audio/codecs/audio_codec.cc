#include "audio_codec.h"
#include "my_nvs.hpp"
#include "esp_log.h"

#define TAG "AudioCodec"

#define MIN_VOLUME  0
#define MAX_VOLUME  100

/// @brief 通过Write接口播放音频数据
void AudioCodec::OutputData(std::vector<int16_t>& data)
{
    Write(data.data(), data.size());
}

/// @brief 通过Read接口读取音频数据，成功返回true
bool AudioCodec::InputData(std::vector<int16_t>& data)
{
    int samples = Read(data.data(), data.size());
    return samples > 0;
}

/// @brief 启动音频编解码器
void AudioCodec::Start()
{
    MyNVS nvs("audio", NVS_READONLY);

    // 从nvs中读取音量设置
    nvs.read("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume invalid(%d), setting to default(70)", output_volume_);
        output_volume_ = 70;
    }

    if (tx_handle_) {
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    }

    if (rx_handle_) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    }

    EnableInput(true);
    EnableOutput(true);
    ESP_LOGI(TAG, "Audio codec started.");
}

/// @brief 保存并设置输出音量
void AudioCodec::SetOutputVolume(int volume)
{
    if (volume < MIN_VOLUME || volume > MAX_VOLUME) {
        ESP_LOGW(TAG, "volume=%d invalid, valid volume: [%d, %d]", volume, MIN_VOLUME, MAX_VOLUME);
        return;
    }
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);

    MyNVS nvs("audio", NVS_READWRITE);
    nvs.write("output_volume", output_volume_);
}

/// @brief 打开/关闭音频输入
void AudioCodec::EnableInput(bool enable)
{
    if (input_enabled_ == enable) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "%s audio codec input.", enable ? "Enable" : "Disable");
}

/// @brief 启用/禁用音频输出
void AudioCodec::EnableOutput(bool enable)
{
    if (output_enabled_ == enable) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "%s audio codec output.", enable ? "Enable" : "Disable");
}