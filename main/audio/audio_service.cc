#include "audio_service.h"
#include <esp_log.h>
#include <cstring>
#include "board.h"
#include "my_background.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

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


AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
    wake_word_timer_ = xTimerCreate(
        "wakeword",
        pdMS_TO_TICKS(20),
        pdTRUE,
        this,
        [](TimerHandle_t id){
            auto* service = reinterpret_cast<AudioService*>(pvTimerGetTimerID(id));
            xEventGroupSetBits(service->event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        }
    );
    voice_process_timer_ = xTimerCreate(
        "voice_process",
        pdMS_TO_TICKS(50),
        pdTRUE,
        this,
        [](TimerHandle_t id){
            auto* service = reinterpret_cast<AudioService*>(pvTimerGetTimerID(id));
            xEventGroupSetBits(service->event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
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
        auto* dat = new output_t;
        dat->service = this;
        dat->data = std::move(data);
        bg.Schedule([](void* arg){
                auto* self = ((output_t*)arg)->service;
                self->EncodeAudio(std::move(((output_t*)arg)->data));
            },
            "Encode",
            dat,
            [](void* arg) {
                delete (output_t*)arg;
            }
        );
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
            // pdFALSE, pdFALSE, portMAX_DELAY);
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
struct play_sound_t {
    AudioService*           service;
    const std::string_view& ogg;
};
void AudioService::PlaySound(const std::string_view& ogg) {
    auto* data = new play_sound_t(this, ogg);
    auto& bg = MyBackground::GetInstance();
    bg.Schedule([](void* arg){
        auto* data = (play_sound_t*)arg;
        auto* buf = reinterpret_cast<const uint8_t*>(data->ogg.data());
        size_t size = data->ogg.size();
        bool seen_head = false;
        bool seen_tags = false;
        int sample_rate = 16000;  // Opus默认采样率
        
        // 修复：用于累积跨页数据包的缓冲区
        std::vector<uint8_t> partial_packet;
        size_t offset = 0;
        
        while (offset + 27 <= size) {
            // 1. 验证页头
            if (memcmp(buf + offset, "OggS", 4) != 0) {
                offset++;   // 尝试重新同步：寻找下一个"OggS"
                continue;
            }
            
            const uint8_t* page = buf + offset;
            
            // 2. 解析页头基本字段(仅段数)
            uint8_t segment_count = page[26];
            
            // 3. 计算数据位置
            size_t seg_table_offset = offset + 27;
            size_t body_offset = seg_table_offset + segment_count;
            
            // 4. 计算数据体总大小
            size_t body_size = 0;
            for (uint8_t i = 0; i < segment_count; ++i) {
                body_size += page[27 + i];
            }
            
            // 5. 边界检查
            if (seg_table_offset + segment_count > size || 
                body_offset + body_size > size) {
                ESP_LOGE(TAG, "Ogg文件数据不完整");
                break;  // 数据不完整
            }
            
            // 6. 处理每个段
            size_t data_pos = 0;
            for (uint8_t seg_idx = 0; seg_idx < segment_count; ++seg_idx) {
                uint8_t seg_len = page[27 + seg_idx];
                const uint8_t* seg_data = buf + body_offset + data_pos;
                
                // 修复：累积数据到partial_packet（处理跨页包）
                partial_packet.insert(partial_packet.end(), seg_data, seg_data + seg_len);
                data_pos += seg_len;
                
                // 修复：当段长度 < 255 时，表示一个完整的数据包结束
                if (seg_len < 255 && !partial_packet.empty()) {
                    const uint8_t* pkt_ptr = partial_packet.data();
                    size_t pkt_len = partial_packet.size();
                    
                    // 处理数据包
                    if (!seen_head) {
                        if (pkt_len >= 8 && memcmp(pkt_ptr, "OpusHead", 8) == 0) {
                            seen_head = true;
                            if (pkt_len >= 19) {
                                sample_rate = pkt_ptr[12] | (pkt_ptr[13] << 8) | (pkt_ptr[14] << 16) | (pkt_ptr[15] << 24);
                            }
                            // 清空缓冲区，准备下一个包
                            partial_packet.clear();
                            continue;
                        }
                    }
                    
                    if (!seen_tags) {
                        if (pkt_len >= 8 && memcmp(pkt_ptr, "OpusTags", 8) == 0) {
                            seen_tags = true;
                            partial_packet.clear();
                            continue;
                        }
                    }
                    
                    // 音频数据包
                    if (seen_head && seen_tags) {
                        auto* self = data->service;
                        if (self && !partial_packet.empty()) {
                            // 复制数据到新的vector传递给解码器
                            std::vector<uint8_t> audio_packet = std::move(partial_packet);
                            self->DecodeAudio(std::move(audio_packet), sample_rate, 60);
                        }
                    }
                    
                    // 清空partial_packet，准备下一个包
                    partial_packet.clear();
                }
                // 如果 seg_len == 255，数据包继续，不处理，等待下一个段
            }
            
            // 7. 移动到下一页
            offset = body_offset + body_size;
        }
        
        // 8. 处理最后可能剩余的部分包
        if (!partial_packet.empty()) {
            ESP_LOGW(TAG, "Ogg文件不完整");
            // 这是一个不完整的包，可能是文件截断，可以选择丢弃
        }
        
    }, "playOgg", data, [](void* arg){
        delete (play_sound_t*)arg;
    });
}

// void AudioService::PlaySound(const std::string_view& ogg) {
//     auto* data = new play_sound_t(this, ogg);
//     auto& bg = MyBackground::GetInstance();
//     bg.Schedule([](void* arg){
//         auto* data = (play_sound_t*)arg;
//         auto* buf = reinterpret_cast<const uint8_t*>(data->ogg.data());
//         auto* self = data->service;
//         size_t size = data->ogg.size();
//         size_t offset = 0;
//         size_t size_left = size;

//         // 页头缓存
//         // 不完整的包缓存
//         static std::vector<uint8_t> partial_page_header;    // 不完整的页头缓存
//         static std::vector<uint8_t> partial_packet;         // 不完整的包缓存
//         static size_t packet_len;                           // 包长度

//         static int sample_rate = 16000;                 // 当前包的采样率
//         static int frame_duration = 60;
//         while (offset <= size) {
//             auto partial_header_size = partial_page_header.size();
//             if (partial_header_size) {
//                 // 页头不完整
//             } else {
//                 // 页头是完整的
//                 if (packet_len) {
//                     // 存在不完整的包
//                 } else {
//                     // 不存在不完整的包
//                 }
//             } 
//         }

//     }, "playOgg", data, [](void* arg){
//         delete (play_sound_t*)arg;
//     });
// }
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
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_send_queue_.push_back(std::move(packet));
        //应当要发送音频
        if(send_fn_) send_fn_(packet);
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