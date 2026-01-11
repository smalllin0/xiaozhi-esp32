#include "ogg_demuxer.h"
#include "esp_log.h"

#define TAG "OggDemuxer"

/// @brief 重置解封器
void OggDemuxer::Reset()
{
    state_ = ParseState::FIND_PAGE;
    ctx_.packet_len = 0;
    ctx_.seg_count = 0;
    ctx_.seg_index = 0;
    ctx_.data_offset = 0;
    ctx_.bytes_needed = 0;
    ctx_.packet_continued = false;
    
    // 修复：清空缓冲区数据
    memset(ctx_.header, 0, sizeof(ctx_.header));
    memset(ctx_.seg_table, 0, sizeof(ctx_.seg_table));
    memset(ctx_.packet_buf, 0, sizeof(ctx_.packet_buf));
    
    stream_.head_seen = false;
    stream_.tags_seen = false;
    stream_.sample_rate = 48000;
}

/// @brief 处理数据块
/// @param data 输入数据
/// @param size 输入数据大小
/// @param on_packet 数据包回调
/// @return 已处理的字节数
size_t OggDemuxer::Process(const uint8_t* data, size_t size,
                          std::function<void(const uint8_t*, size_t, int)> on_packet)
{
    // size_t processed = 0;
    size_t start = 0;           // start是数据处理的位置
    size_t size_left = size;
    
    while (start < size) {
        switch (state_) {
          case ParseState::FIND_PAGE: {
            // 寻找页头"OggS"
            if (partial_header_len_) {
                // 不完整匹配
                if (size + partial_header_len_ >= 4) {
                    auto need = 4 - partial_header_len_;
                    start += need;
                    memcpy(ctx_.header + partial_header_len_, data, need);
                    if (memcmp(ctx_.header, "OggS", 4) == 0) {
                        // 转入下一阶段
                        state_ = ParseState::PARSE_HEADER;
                        ctx_.bytes_needed = 27 - 4;
                        ctx_.body_offset = 4;
                    }
                    partial_header_len_ = 0;    // 将不完整页头数据清0,进入下一轮匹配
                    continue;
                } else {
                    // 保留不完整数据
                    memcpy(ctx_.header + partial_header_len_, data, size);
                    partial_header_len_ += size;
                    return size;
                }
            } else {
                // 完整匹配
                bool found = false;
                for (; start + 4 <= size; start++) {
                    if (memcmp(data + start, "OggS", 4) == 0) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    state_ = ParseState::PARSE_HEADER;
                    ctx_.bytes_needed = 27 - 4;
                    ctx_.data_offset = 4;
                    continue;
                } else {
                    // 保留不完整的页头，有可能是
                    partial_header_len_ = std::min(size, size - start);
                    memcpy(ctx_.header, data + start, partial_header_len_);
                    return size;  // 处理完所有数据
                }
                break;
            }   
          }
                
          case ParseState::PARSE_HEADER: {
            // 修复：处理可能的部分页头
            size_t available = size - start; 
            
            if (available < ctx_.bytes_needed) {
                // 复制可用的部分
                memcpy(ctx_.header + ctx_.data_offset, data + start, available);
                ctx_.data_offset += available;
                ctx_.bytes_needed -= available;
                return size;  // 等待更多数据
            } else {
                // 有足够的数据完成页头
                memcpy(ctx_.header + ctx_.data_offset, data + start, ctx_.bytes_needed);
                
                start += ctx_.bytes_needed;
                ctx_.data_offset += ctx_.bytes_needed;
                
                // 验证页头
                if (ctx_.header[4] != 0) {  // 版本检查
                    ESP_LOGE(TAG, "无效的Ogg版本: %d", ctx_.header[4]);
                    state_ = ParseState::FIND_PAGE;
                    break;
                }
                
                ctx_.seg_count = ctx_.header[26];
                if (ctx_.seg_count > 0 && ctx_.seg_count <= 255) {
                    state_ = ParseState::PARSE_SEGMENTS;
                    ctx_.bytes_needed = ctx_.seg_count;
                    ctx_.data_offset = 0;
                } else if (ctx_.seg_count == 0) {
                    // 没有段，直接跳到下一个页面
                    state_ = ParseState::FIND_PAGE;
                } else {
                    ESP_LOGE(TAG, "无效的段数: %u", ctx_.seg_count);
                    state_ = ParseState::FIND_PAGE;
                }
            }
            break;
          }
                
          case ParseState::PARSE_SEGMENTS: {
            size_t available = size - start;
            
            // 如果需要的数据比可用的多
            if (available < ctx_.bytes_needed) {
                memcpy(ctx_.seg_table + ctx_.data_offset, data + start, available);
                ctx_.data_offset += available;
                ctx_.bytes_needed -= available;
                start += available;
                return size;  // 等待更多数据
            } else {
                memcpy(ctx_.seg_table + ctx_.data_offset, data + start, ctx_.bytes_needed);
                
                start += ctx_.bytes_needed;
                ctx_.data_offset += ctx_.bytes_needed;
                
                state_ = ParseState::PARSE_DATA;
                ctx_.seg_index = 0;
                ctx_.data_offset = 0;
                ctx_.body_size = 0;
                for (size_t i = 0; i < ctx_.seg_count; ++i) {
                    ctx_.body_size += ctx_.seg_table[i];
                }
                ctx_.body_offset = 0;
            }
            break;
          }
                
          case ParseState::PARSE_DATA: {
            while (ctx_.seg_index < ctx_.seg_count && start < size) {
                uint8_t seg_len = ctx_.seg_table[ctx_.seg_index];
                
                // 检查段数据是否已经部分读取
                if (ctx_.seg_remaining > 0) {
                    // 继续读取上次未完成的段
                    seg_len = ctx_.seg_remaining;
                } else {
                    ctx_.seg_remaining = seg_len;
                }
                
                // 检查缓冲区是否足够
                if (ctx_.packet_len + seg_len > sizeof(ctx_.packet_buf)) {
                    // 缓冲区溢出，重置
                    ESP_LOGE(TAG, "包缓冲区溢出: %zu + %u > %zu",
                            ctx_.packet_len, seg_len, sizeof(ctx_.packet_buf));
                    state_ = ParseState::FIND_PAGE;
                    ctx_.packet_len = 0;
                    ctx_.packet_continued = false;
                    ctx_.seg_remaining = 0;
                    return start;
                }
                
                // 复制数据
                size_t to_copy = std::min(size - start, (size_t)seg_len);
                memcpy(ctx_.packet_buf + ctx_.packet_len, data + start, to_copy);
                
                start += to_copy;
                ctx_.packet_len += to_copy;
                ctx_.body_offset += to_copy;
                ctx_.seg_remaining -= to_copy;
                
                // 检查段是否完整
                if (ctx_.seg_remaining > 0) {
                    // 段不完整，等待更多数据
                    return size;
                }
                
                // 段完整
                ctx_.packet_continued = (ctx_.seg_table[ctx_.seg_index] == 255);
                
                if(!ctx_.packet_continued) {
                    // 包结束
                    if (ctx_.packet_len > 0) {
                        ProcessPacket(ctx_.packet_buf, ctx_.packet_len, on_packet);
                    }
                    ctx_.packet_len = 0;
                    ctx_.packet_continued = false;
                }
                ctx_.seg_index++;
                ctx_.seg_remaining = 0;
            }
            
            if (ctx_.seg_index == ctx_.seg_count) {
                // 检查是否所有数据体都已读取
                if (ctx_.body_offset < ctx_.body_size) {
                    // 还有数据未读，但段表已处理完，这不应该发生
                    ESP_LOGW(TAG, "数据体不完整: %zu/%zu", ctx_.body_offset, ctx_.body_size);
                }
                    state_ = ParseState::FIND_PAGE;
            }
            break;
        }
      }
    }
    return size;
}

/// @brief 处理数据包
/// @param data 包数据
/// @param size 包大小
/// @param on_packet 数据包处理回调
void OggDemuxer::ProcessPacket(const uint8_t* data, size_t size, std::function<void(const uint8_t*, size_t, int)> on_packet)
{
    if (!stream_.head_seen) {
        if (size >= 8 && memcmp(data, "OpusHead", 8) == 0) {
            stream_.head_seen = true;
            if (size >= 19) {
                stream_.sample_rate = data[12] | (data[13] << 8) | (data[14] << 16) | (data[15] << 24);
                ESP_LOGI(TAG, "OpusHead: 采样率=%d", stream_.sample_rate);
            }
            return;
        }
    }
    
    if (!stream_.tags_seen) {
        if (size >= 8 && memcmp(data, "OpusTags", 8) == 0) {
            stream_.tags_seen = true;
            ESP_LOGI(TAG, "OpusTags found");
            return;
        }
    }
    
    if (stream_.head_seen && stream_.tags_seen && on_packet) {
        on_packet(data, size, stream_.sample_rate);
    }
}