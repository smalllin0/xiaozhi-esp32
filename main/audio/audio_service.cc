#include "audio_service.h"
#include <esp_log.h>
#include <cstring>
#include "board.h"
#include "my_background.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "ogg_demuxer.h"
#include "../assets/lang_config.h"

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#if CONFIG_USE_AFE_WAKE_WORD
#include "wake_words/afe_wake_word.h"
#elif CONFIG_USE_ESP_WAKE_WORD
#include "wake_words/esp_wake_word.h"
#elif CONFIG_USE_CUSTOM_WAKE_WORD
#include "wake_words/custom_wake_word.h"
#endif

#define TAG "AudioService"

struct output_t {
    AudioService*           service;
    std::vector<int16_t>    data;
};

// 周期性的启停止107us
AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
    wake_word_timer_ = xTimerCreate(
        "wakeword",
        pdMS_TO_TICKS(10),
        pdTRUE,
        this,
        [](TimerHandle_t id){
            static bool on = true;
            auto* service = reinterpret_cast<AudioService*>(pvTimerGetTimerID(id));
            if (on) {
                xEventGroupSetBits(service->event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            }
            on = !on;
        }
    );
    voice_process_timer_ = xTimerCreate(
        "voice_process",
        pdMS_TO_TICKS(10),
        pdTRUE,
        this,
        [](TimerHandle_t id){
            static bool on = true;
            auto* service = reinterpret_cast<AudioService*>(pvTimerGetTimerID(id));
            if (on) {
                xEventGroupSetBits(service->event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
            }
            on = !on;
        }
    );
}


/// @brief 初始化音频服务
void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    /* Setup the audio codec */
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_->SetComplexity(0);

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }


    audio_processor_ = std::make_unique<NoAudioProcessor>();
    wake_word_ = std::make_unique<EspWakeWord>();


    // 创建编码任务
    audio_processor_->OnOutput([this](std::vector<int16_t>&& data){
        auto& bg = MyBackground::GetInstance();
        bg.Schedule([this,  dat = std::move(data)](void* arg) mutable {
            EncodeAudio(std::move(dat));
        },"Encode");
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

/// @brief 启动音频服务
void AudioService::Start() {
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

    /* Start the audio input task */ 
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);
}

/// @brief 停止音频服务
void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
    service_stopped_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.notify_all();
}

/// @brief 读取音频数据
/// @param data 数据目的地址
/// @param sample_rate 采样率真
/// @param samples 采样数量
bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (codec_->input_channels() == 2) {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();

    return true;
}

/// @brief 只要启用唤醒词检测、音频处理，就会一直运行下去
void AudioService::AudioInputTask() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdTRUE, pdFALSE, portMAX_DELAY);
        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }


        /* Feed the wake word */
        if (bits & AS_EVENT_WAKE_WORD_RUNNING) {
            std::vector<int16_t> data;
            int samples = wake_word_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    wake_word_->Feed(data);
                    continue;
                }
            }
        }

        /* Feed the audio processor */
        if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
            std::vector<int16_t> data;
            int samples = audio_processor_->GetFeedSize();
            if (samples > 0) {
                if (ReadAudioData(data, 16000, samples)) {
                    audio_processor_->Feed(std::move(data));
                    continue;
                }
            }
        }

        ESP_LOGE(TAG, "Should not be here, bits: %lx", bits);
        break;
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) {
        return;
    }
    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(sample_rate, 1, frame_duration);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decoder_->sample_rate(), codec->output_sample_rate());
        output_resampler_.Configure(opus_decoder_->sample_rate(), codec->output_sample_rate());
    }
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (wake_word_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

/// @brief 打开/关闭周期性语音检测
void AudioService::EnableWakeWordDetection(bool enable) {
    if (!wake_word_) {
        return;
    }

    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                return;
            }
            wake_word_initialized_ = true;
        }
        wake_word_->Start();
        xTimerStart(wake_word_timer_, 0);
    } else {
        wake_word_->Stop();
        xTimerStop(wake_word_timer_, 0);
    }
}

/// @brief 打开/关闭周期性音频处理
void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!audio_processor_initialized_) {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS);
            audio_processor_initialized_ = true;
        }

        /* We should make sure no audio is playing */
        ResetDecoder();
        audio_input_need_warmup_ = true;
        audio_processor_->Start();
        // xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
        xTimerStart(voice_process_timer_, 0);
    } else {
        audio_processor_->Stop();
        // xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
        xTimerStop(voice_process_timer_, 0);
    }
}

/// @brief 打开/关闭设备AEC功能（回声消除）
void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    if (!audio_processor_initialized_) {
        audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS);
        audio_processor_initialized_ = true;
    }

    audio_processor_->EnableDeviceAec(enable);
}

/// @brief 播放Ogg格式的音频------------可以单独放在一个任务中，避免分成多个任务解码播放
void AudioService::PlaySound(const std::string_view& ogg) {
    auto& bg = MyBackground::GetInstance();
    bg.Schedule([&ogg, this](void* arg){
        auto* buf = reinterpret_cast<const uint8_t*>(ogg.data());
        size_t size = ogg.size();

        OggDemuxer demuxer;
        demuxer.Process(buf, size, [this](const uint8_t* data, size_t size, int sample_rate){
            std::vector<uint8_t> opus_data(data, data + size);
            DecodeAudio(std::move(opus_data), sample_rate, 60);
        });
    }, "playOgg");
}

void AudioService::PlaySound() {
    auto& bg = MyBackground::GetInstance();
    bg.Schedule([this](void* arg){
        uint8_t buf[2048];
        size_t read_offset = 0;
        size_t total_size = Lang::Sounds::ogg_out_end - Lang::Sounds::ogg_out_start;
        OggDemuxer demuxer;

        while (read_offset < total_size) {
            size_t chunk = std::min(total_size - read_offset, (size_t)2048);
            memcpy(buf, Lang::Sounds::ogg_out_start + read_offset, chunk);
            demuxer.Process(buf, chunk, [this](const uint8_t* data, size_t size, int sample_rate){
                std::vector<uint8_t> opus_data(data, data + size);
                // Use the actual sample_rate provided by the demuxer
                DecodeAudio(std::move(opus_data), sample_rate, 60);
            });
            read_offset += chunk;
        }
     }, "playOgg");
}


/// @brief 音频服务是否空闲
/// @return 
bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    // return audio_encode_queue_.empty() && audio_decode_queue_.empty() && audio_playback_queue_.empty();
    return true;
}

/// @brief 重围解码器
void AudioService::ResetDecoder() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    opus_decoder_->ResetState();
    timestamp_queue_.clear();
    audio_queue_cv_.notify_all();
}

/// @brief 电源管理定时器
void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        codec_->EnableOutput(false);
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

/// @brief 音频数据编码
void AudioService::EncodeAudio(std::vector<int16_t>&& data)
{

    auto packet = std::make_unique<AudioStreamPacket>();
    packet->frame_duration = OPUS_FRAME_DURATION_MS;
    packet->sample_rate = 16000;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        if (!timestamp_queue_.empty()) {
            packet->timestamp = timestamp_queue_.front();
            timestamp_queue_.pop_front();
        }
    }
    if (!opus_encoder_->Encode(std::move(data), packet->payload)) {
        ESP_LOGE(TAG, "Failed to encode audio");
        return;
    }
    {
        // std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        // audio_send_queue_.push_back(std::move(packet));
        //应当要发送音频
        if(send_fn_) send_fn_(std::move(packet));
    }
    if (callbacks_.on_send_queue_available) {
        callbacks_.on_send_queue_available();
    }
}

/// @brief 音频数据解码
void AudioService::DecodeAudio(std::vector<uint8_t> data, int sample_rate, int frame_duration)
{
    auto task = std::make_unique<AudioTask>();
    task->type = kAudioTaskTypeDecodeToPlaybackQueue;
    task->timestamp = 0;
    SetDecodeSampleRate(sample_rate, frame_duration);
    if (opus_decoder_->Decode(std::move(data), task->pcm)) {
        if (opus_decoder_->sample_rate() != codec_->output_sample_rate()) {
            // 重采样
            auto target_size = output_resampler_.GetOutputSamples(task->pcm.size());
            std::vector<int16_t> resampled(target_size);
            output_resampler_.Process(task->pcm.data(), task->pcm.size(), resampled.data());
            task->pcm = std::move(resampled);
        }
        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }
        codec_->OutputData(task->pcm);
        last_output_time_ = std::chrono::steady_clock::now();
        #if CONFIG_USE_SERVER_AEC
            /* Record the timestamp for server AEC */
            if (task->timestamp > 0) {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                timestamp_queue_.push_back(task->timestamp);
            }
        #endif
    } else {
        ESP_LOGE(TAG, "Failed to decode audio");
    }
}