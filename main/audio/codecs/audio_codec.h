#ifndef AUDIO_CODEC_H_
#define AUDIO_CODEC_H_

#include "driver/i2s_common.h"
#include <vector>

#define AUDIO_CODEC_DMA_BUFFER_NUM      6       // I2S DMA缓冲区数量
#define AUDIO_CODEC_DMA_FRAME_NUM       240     // 一个DMA缓冲区中的帧数（为对齐，24位数据帧数必须为3的倍数）
#define AUDIO_CODEC_DEFAULT_MIC_GAIN    30.0    // 


/// @brief 编解码器类
class AudioCodec {
public:
    AudioCodec(bool duplex, bool input_reference, int input_sample_rate, int output_sample_rate, int input_channels)
        : duplex_(duplex), input_reference_(input_reference), input_sample_rate_(input_sample_rate), output_sample_rate_(output_sample_rate), input_channels_(input_channels)
    {}
    AudioCodec(){};
    virtual ~AudioCodec(){}

    // 上层调用公共接口
    
    /// @brief 设置输出音量
    virtual void SetOutputVolume(int volume);
    /// @brief 打开/关闭输入
    virtual void EnableInput(bool enable);
    /// @brief 打开/关闭输出p
    virtual void EnableOutput(bool enable);
    /// @brief 播放音频数据
    virtual void OutputData(std::vector<int16_t>& data);
    /// @brief 接收音频数据
    virtual bool InputData(std::vector<int16_t>& data);
    virtual void Start();

    /// @brief 是否支持全双工
    bool duplex() const { return duplex_; }
    bool input_reference() const { return input_reference_; }
    /// @brief 输入采样率
    int input_sample_rate() const { return input_sample_rate_; }
    /// @brief 输出采样率
    int output_sample_rate() const { return output_sample_rate_; }
    /// @brief 输入通道数量
    int input_channels() const { return input_channels_; }
    /// @brief 输出通道数量
    int output_channels() const { return output_channels_; }
    /// @brief 输出音量
    int output_volume() const { return output_volume_; }
    /// @brief 输入是否打开
    bool input_enabled() const { return input_enabled_; }
    /// @brief 输出是否打开
    bool output_enabled() const { return output_enabled_; }

protected:
    /// @brief 读取音频数据
    virtual int Read(int16_t* dst, int samples) = 0;

    /// @brief 写入音频数据
    virtual int Write(const int16_t* data, int samples) = 0;

    i2s_chan_handle_t   tx_handle_{nullptr};    // I2S发送句柄
    i2s_chan_handle_t   rx_handle_{nullptr};    // I2S接收句柄

    bool    duplex_{false};             // 是否支持全双工（同时录音+播放）
    bool    input_reference_{false};    // 
    bool    input_enabled_{false};      // 是否启用输入
    bool    output_enabled_{false};     // 是否启用输出
    int     input_sample_rate_{0};      // 输入采样率
    int     output_sample_rate_{0};     // 输出采样率真
    int     input_channels_{1};         // 输入声道数
    int     output_channels_{1};        // 输出声道数
    int     output_volume_{70};         // 输出音量

};

#endif // !AUDIO_CODEC_H_