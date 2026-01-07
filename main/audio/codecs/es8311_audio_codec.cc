#include "es8311_audio_codec.h"
#include "esp_codec_dev_defaults.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

#define TAG "Es8311AudioCodec" 

Es8311AudioCodec::Es8311AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate,
        int output_sample_rate, gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
        gpio_num_t pa_pin, uint8_t es8311_addr, bool use_mclk, bool pa_inverted)
    : AudioCodec(true, false, input_sample_rate, output_sample_rate, 1)
    , pa_pin_(pa_pin)
    , pa_inverted_(pa_inverted)
{
    // 1. 初始化硬件接口
    assert(input_sample_rate == output_sample_rate);
    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // 2. 创建通用接口
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != nullptr);
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = i2c_port,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if_ != nullptr);
    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != nullptr);

    // 3. 初始化ES8311专用控制逻辑（ES8311寄存器配置）
    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = use_mclk;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    es8311_cfg.pa_reverted = pa_inverted;
    codec_if_ = es8311_codec_new(&es8311_cfg);
    assert(codec_if_ != nullptr);

    ESP_LOGI(TAG, "Es8311 Audio Codec initalized.");
}


Es8311AudioCodec::~Es8311AudioCodec()
{
    esp_codec_dev_delete(dev_);

    // 删除第三层接口（es8311)
    audio_codec_delete_codec_if(codec_if_);

    // 删除第二层接口
    audio_codec_delete_ctrl_if(ctrl_if_);
    audio_codec_delete_data_if(data_if_);
    audio_codec_delete_gpio_if(gpio_if_);
}

/// @brief 创建双工的收发通道
void Es8311AudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din)
{
    // 配置I2S通道参数(硬件相关资源)
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,                            // I2S接口ID
        .role = I2S_ROLE_MASTER,                    // I2S角色（主/从）
        .dma_desc_num = AUDIO_CODEC_DMA_BUFFER_NUM, // I2S DMA buffer数量
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM, // 在一个buffer中I2S的帧数
        .auto_clear_after_cb = true,                // 发送结束是否自动清理DMA TX buffer
        .auto_clear_before_cb = false,              // 
        .intr_priority = 0                          // I2S中断优先级，为0时将自动设置为（1-3）中的1个较低优先级
    };
    // 初始化发送/接收通道
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    // I2S模式配置（通信协议相关）
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,    // 采样率
            .clk_src = I2S_CLK_SRC_DEFAULT,                     // 时钟源（默认内部PLL）
            #ifdef I2S_HW_VERSION_2         
                .ext_clk_req_hz = 0,                            // 采用外部时钟源时外部频率
            #endif
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,             // MCLK与采样率真的倍数
            // .bclk_div = 8                                       // 从模式下有效（MCLK/8=BCLK）
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, // 音频数据位宽
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,  // 每个声道（slot)占用的总位数
            .slot_mode = I2S_SLOT_MODE_STEREO,          // 声道模式(立体声stereo、单声道mono)
            .slot_mask = I2S_STD_SLOT_BOTH,             // 选择哪个声道
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,       // WS脉冲宽度（通常=slot_bit_width)
            .ws_pol = false,                            // WS信号极性（false==low)
            .bit_shift = true,                          // 在标准模式Philips格式下是否支持移位
            #ifdef I2S_HW_VERSION_1
                .msb_right = true                       // 右声道是否在MSB位置
            #else
                .left_align = true,                     // 左对齐，有效数据在slot的左侧（标准I2S为左对齐）
                .big_endian = false,                    // 字节序，ESP32默认是小端序
                .bit_order_lsb = false                  // 位序（每个字节是LSB先发还是MSB）（标准I2S为MSB）
            #endif
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Duplex channels created.");
}

/// @brief 更新音频设备状态（用于按需启停音频硬件、功放控制）
void Es8311AudioCodec::UpdateDeviceState()
{
    if ((input_enabled_ || output_enabled_) && (dev_ == nullptr)) {
        // 4. 初始化统一设备层
        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
            .codec_if = codec_if_,
            .data_if = data_if_
        };
        dev_ = esp_codec_dev_new(&dev_cfg);
        assert(dev_ != nullptr);

        esp_codec_dev_sample_info_t sample_cfg = {
            .bits_per_sample = 16,                      // 每次采样使用位数
            .channel = 1,                               // 通道数量
            .channel_mask = 0,                          // 选择的通道
            .sample_rate = (uint32_t)input_sample_rate_,// 采样速率
            .mclk_multiple = 0                          // mclk倍频
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(dev_, &sample_cfg));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(dev_, AUDIO_CODEC_DEFAULT_MIC_GAIN));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dev_, output_volume_));
    } else if (!input_enabled_ && !output_enabled_ && (dev_ != nullptr)) {
        esp_codec_dev_close(dev_);
        dev_ = nullptr;
    }
    
    if (pa_pin_ != GPIO_NUM_NC) {
        int level = output_enabled_ ? 1 : 0;
        gpio_set_level(pa_pin_, pa_inverted_ ? !level : level);
    }
}


void Es8311AudioCodec::SetOutputVolume(int volume)
{
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dev_, volume));
    AudioCodec::SetOutputVolume(volume);
}

void Es8311AudioCodec::EnableInput(bool enable)
{
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    AudioCodec::EnableInput(enable);
    UpdateDeviceState();
}

void Es8311AudioCodec::EnableOutput(bool enable)
{
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    AudioCodec::EnableOutput(enable);
    UpdateDeviceState();
}


int Es8311AudioCodec::Read(int16_t* dst, int samples)
{
    if (!input_enabled_) return 0;

    auto ret = esp_codec_dev_read(dev_, (void*)dst, samples * sizeof(int16_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write data failed: %s", esp_err_to_name(ret));
        return 0;
    }
    
    return samples;
}


int Es8311AudioCodec::Write(const int16_t* data, int samples)
{
    if (!output_enabled_) return 0;

    auto ret = esp_codec_dev_write(dev_, (void*)data, samples * sizeof(int16_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write data failed: %s", esp_err_to_name(ret));
        return 0;
    }
    
    return samples;
}