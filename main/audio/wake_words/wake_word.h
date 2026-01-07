#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#include <string>
#include <vector>
#include <functional>

#include "audio_codec.h"

class WakeWord {
public:
    virtual ~WakeWord() = default;
    /// @brief 初始化
    virtual bool Initialize(AudioCodec* codec) = 0;
    /// @brief 将音频数据投喂到唤醒词模型，检测到唤醒词时调用相应的回调函数
    virtual void Feed(const std::vector<int16_t>& data) = 0;
    /// @brief 设置唤醒词回调
    virtual void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) = 0;
    /// @brief 启动唤醒词检测
    virtual void Start() = 0;
    /// @brief 关闭唤醒词检测
    virtual void Stop() = 0;
    /// @brief 获取期望的采样点数
    virtual size_t GetFeedSize() = 0;
    /// @brief 编码唤醒词
    virtual void EncodeWakeWordData() = 0;
    /// @brief 获取Opus编码的唤醒音频
    virtual bool GetWakeWordOpus(std::vector<uint8_t>& opus) = 0;
    /// @brief 获取最后一次检测到的唤醒词
    virtual const std::string& GetLastDetectedWakeWord() const = 0;
};

#endif
