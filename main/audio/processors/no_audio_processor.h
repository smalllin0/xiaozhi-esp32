#ifndef NO_AUDIO_PROCESSOR_H_
#define NO_AUDIO_PROCESSOR_H_

#include "audio_processor.h"
#include <vector>
#include <functional>

class NoAudioProcessor : public AudioProcessor {
public:
    NoAudioProcessor() = default;
    ~NoAudioProcessor() = default;

    void Initialize(AudioCodec* codec, int frame_duration_ms) override;
    void Feed(std::vector<int16_t>&& data) override;
    /// @brief 启动音频处理器
    void Start() override {
        is_running_ = true;
    }
    /// @brief 停止运行音频处理器
    void Stop() override {
        is_running_ = false;
    }
    /// @brief 判断音频处理器是否在运行
    bool IsRunning() override {
        return is_running_;
    }
    /// @brief 设置输出回调
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override {
        output_callback_ = callback;
    }
    /// @brief 设置VAD变化回调
    void OnVadStateChange(std::function<void(bool speaking)> callback) override {
        vad_state_change_callback_ = callback;
    }
    /// @brief 获取每帧采样数量
    size_t GetFeedSize() override {
        return codec_ ? frame_samples_ : 0;
    }
    /// @brief 打开/关闭AEC（回声消除）功能
    void EnableDeviceAec(bool enable) override;

private:
    bool is_running_{false};        // 是否在运行
    int frame_samples_{0};          // 每帧采样数量
    AudioCodec* codec_{nullptr};    // 关联的纺解码器
    std::function<void(std::vector<int16_t>&& data)> output_callback_;  // 音频输出回调
    std::function<void(bool speaking)> vad_state_change_callback_;      // VAD变化回调
};

#endif // !NO_AUDIO_PROCESSOR_H_