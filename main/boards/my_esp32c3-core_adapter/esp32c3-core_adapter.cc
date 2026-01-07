#include "wifi_board.h"
#include "audio/codecs/es8311_audio_codec.h"
#include "application.h"
#include "mcp_server.h"
#include "my_nvs.hpp"
#include "config.h"
#include "display/esplog_display.h"

#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>

#define TAG "Esp32c3CoreAdapterBoard"


class Esp32c3CoreAdapterBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Display* display_ = nullptr;
    bool press_to_talk_enabled_ = false;

    void InitializePowerSaveTimer() {
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeDisplay() {
        display_ = new EspLogDisplay();
    }

    void InitializeButtons() {
        // boot_button_.OnClick([this]() {
        //     auto& app = Application::GetInstance();
        //     if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
        //         ResetWifiConfiguration();
        //     }
        //     if (!press_to_talk_enabled_) {
        //         app.ToggleChatState();
        //     }
        // });
        // boot_button_.OnPressDown([this]() {
        //     if (power_save_timer_) {
        //         power_save_timer_->WakeUp();
        //     }
        //     if (press_to_talk_enabled_) {
        //         Application::GetInstance().StartListening();
        //     }
        // });
        // boot_button_.OnPressUp([this]() {
        //     if (press_to_talk_enabled_) {
        //         Application::GetInstance().StopListening();
        //     }
        // });
    }

    void InitializeTools() {
        MyNVS nvs("vendor", NVS_READONLY);
        press_to_talk_enabled_ = false;
        nvs.read("press_to_talk", press_to_talk_enabled_);

#if CONFIG_IOT_PROTOCOL_XIAOZHI
#error "XiaoZhi 协议不支持"
#elif CONFIG_IOT_PROTOCOL_MCP
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.set_press_to_talk",
            "Switch between press to talk mode (长按说话) and click to talk mode (单击说话).\n"
            "The mode can be `press_to_talk` or `click_to_talk`.",
            PropertyList({
                Property("mode", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto mode = properties["mode"].value<std::string>();
                if (mode == "press_to_talk") {
                    SetPressToTalkEnabled(true);
                    return true;
                } else if (mode == "click_to_talk") {
                    SetPressToTalkEnabled(false);
                    return true;
                }
                throw std::runtime_error("Invalid mode: " + mode);
            });
#endif
    }

public:
    Esp32c3CoreAdapterBoard()  {  
        // 把 ESP32C3 的 VDD SPI 引脚作为普通 GPIO 口使用
        esp_efuse_write_field_bit(ESP_EFUSE_VDD_SPI_AS_GPIO);

        InitializeDisplay();
        InitializeCodecI2c();
        InitializeButtons();
        InitializePowerSaveTimer();
        InitializeTools();
    }


    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    void SetPressToTalkEnabled(bool enabled) {
        press_to_talk_enabled_ = enabled;

        MyNVS nvs("vendor", NVS_READWRITE);
        nvs.write("press_to_talk", enabled);
        ESP_LOGI(TAG, "Press to talk enabled: %d", enabled);
    }

    bool IsPressToTalkEnabled() {
        return press_to_talk_enabled_;
    }

    virtual void SetPowerSaveMode(bool enabled) override {
        WifiBoard::SetPowerSaveMode(enabled);
    }
};

DECLARE_BOARD(Esp32c3CoreAdapterBoard);