#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <memory>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include <opus_encoder.h>
#include <opus_decoder.h>
#include <opus_resampler.h>

#include "audio_codec.h"
#include "audio_processor.h"
#include "wake_word.h"
#include "protocol.h"
#include "ogg_demuxer.h"


/*
 * There are two types of audio data flow:
 * 1. (MIC) -> [Processors] -> {Encode Queue} -> [Opus Encoder] -> {Send Queue} -> (Server)
 * 2. (Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue} -> (Speaker)
 *
 * We use one task for MIC / Speaker / Processors, and one task for Opus Encoder / Opus Decoder.
 * 
 * Decode Queue and Send Queue are the main queues, because Opus packets are quite smaller than PCM packets.
 * 
 */

#define OPUS_FRAME_DURATION_MS 60
#define MAX_ENCODE_TASKS_IN_QUEUE 2
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
#define MAX_DECODE_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define AUDIO_TESTING_MAX_DURATION_MS 10000
#define MAX_TIMESTAMPS_IN_QUEUE 3

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000


#define AS_EVENT_WAKE_WORD_RUNNING          BIT1
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING    BIT2

struct AudioServiceCallbacks {
    std::function<void(void)> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
};


enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
};

struct AudioTask {
    AudioTaskType type;
    std::vector<int16_t> pcm;
    uint32_t timestamp;
};

/***************** */
using SendFun_t = std::function<bool(std::unique_ptr<AudioStreamPacket> packet)>;
struct Opus_t {
    bool head_seen{false};
    bool tags_seen{false};
    int sample_rate = 48000;
};
/************/

class AudioService {
public:
    AudioService();
    ~AudioService() {
        if (event_group_ != nullptr) {
            vEventGroupDelete(event_group_);
        }
    }
    void DecodeAudio(std::vector<uint8_t> data, int sample_rate, int frame_duration);   // 后续要放到private中
    void ResetOpusParser() {
        opus_info_ = {
            .head_seen = false,
            .tags_seen = false,
            .sample_rate = 48000
        };
    }



    
    /// @brief 设置服务所需的回调函数
    void SetCallbacks(AudioServiceCallbacks& callbacks) {
        callbacks_ = callbacks;
    }
    /// @brief 获取最后一次的唤醒词
    const std::string& GetLastWakeWord() const {
        return wake_word_->GetLastDetectedWakeWord();
    }
    /// @brief 将唤醒词音频进行编码
    void EncodeWakeWord() {
        if (wake_word_) wake_word_->EncodeWakeWordData();
    }

    void SetSendFun(SendFun_t fun) {
        send_fn_ = fun;
    }





    void Initialize(AudioCodec* codec);
    void Start();
    void Stop();
    std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
    bool IsVoiceDetected() const { return voice_detected_; }
    bool IsIdle();
    bool IsWakeWordRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING; }
    bool IsAudioProcessorRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING; }

    void EnableWakeWordDetection(bool enable);
    void EnableVoiceProcessing(bool enable);
    void EnableDeviceAec(bool enable);


    // bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
    void PlaySound(const std::string_view& sound);
    void PlaySound();
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    void ResetDecoder();

private:
    void EncodeAudio(std::vector<int16_t>&& data);

    TimerHandle_t               wake_word_timer_{nullptr};      // 唤醒词定时器
    TimerHandle_t               voice_process_timer_{nullptr};  // 音频处理定时器
    SendFun_t                   send_fn_{nullptr};              // 发送回调
    std::unique_ptr<WakeWord>   wake_word_;                     // 唤醒词句柄
    OggDemuxer                  demuxer_;
    Opus_t                      opus_info_;





    /* .............................................. */

    AudioCodec* codec_ = nullptr;
    AudioServiceCallbacks callbacks_;                   // 音频服务回调
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<OpusEncoderWrapper> opus_encoder_;
    std::unique_ptr<OpusDecoderWrapper> opus_decoder_;
    OpusResampler input_resampler_;
    OpusResampler reference_resampler_;
    OpusResampler output_resampler_;

    EventGroupHandle_t event_group_;

    // Audio encode / decode
    TaskHandle_t audio_input_task_handle_ = nullptr;
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_;
    // For server AEC
    std::deque<uint32_t> timestamp_queue_;

    bool wake_word_initialized_ = false;
    bool audio_processor_initialized_ = false;
    bool voice_detected_ = false;
    bool service_stopped_ = true;
    bool audio_input_need_warmup_ = false;

    esp_timer_handle_t audio_power_timer_ = nullptr;            // 电源管理定时器
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    void AudioInputTask();
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckAndUpdateAudioPowerState();
};

#endif