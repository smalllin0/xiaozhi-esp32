#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"
#include "gif/lvgl_gif.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <atomic>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000


class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;       // 顶部状态栏，放置系统状态图标
    lv_obj_t* status_bar_ = nullptr;    // 中心状态文本层
    lv_obj_t* content_ = nullptr;       // 主内容区域
    lv_obj_t* container_ = nullptr;     // 背景容器，覆盖整个屏幕
    lv_obj_t* side_bar_ = nullptr;      // 侧边栏（暂未使用）
    lv_obj_t* bottom_bar_ = nullptr;    // 底部字幕/消息栏
    lv_obj_t* preview_image_ = nullptr; // 预览图像显示对象
    lv_obj_t* emoji_label_ = nullptr;   // 静态表情图像
    lv_obj_t* emoji_image_ = nullptr;   // 动态表情图像
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;         // GIF动画控制器
    lv_obj_t* emoji_box_ = nullptr;                             // 居中表情容器
    lv_obj_t* chat_message_label_ = nullptr;                    // 聊天消息文本标签
    esp_timer_handle_t preview_timer_ = nullptr;                // 预览图自动隐藏定时器
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr; // 预览图缓存
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles

    void InitializeLcdThemes();
    void SetupUI();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;
    
    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // LCD_DISPLAY_H
