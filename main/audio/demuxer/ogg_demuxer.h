#ifndef OGG_DEMUXER_H_
#define OGG_DEMUXER_H_

#include <functional>
#include <cstdint>
#include <cstring>
#include <vector>

class OggDemuxer {
private:
    enum ParseState : int8_t {
        FIND_PAGE,
        PARSE_HEADER,
        PARSE_SEGMENTS,
        PARSE_DATA
    };

    // 流信息
    struct {
        int sample_rate = 48000;
        bool head_seen = false;
        bool tags_seen = false;
    } stream_;

    // 使用固定大小的缓冲区避免动态分配
    struct {
        uint8_t header[27];             // Ogg页头
        uint8_t seg_table[255];         // 当前存储的段表
        uint8_t packet_buf[8192];       // 8KB包缓冲区
        size_t packet_len = 0;          // 缓冲区中累计的数据长度
        size_t seg_count = 0;           // 当前页段数
        size_t seg_index = 0;           // 当前处理的段索引
        size_t data_offset = 0;         // 解析当前阶段已读取的字节数
        size_t bytes_needed = 0;        // 解析当前字段还需要读取的字节数
        size_t seg_remaining = 0;       // 当前段剩余需要读取的字节数
        size_t body_size = 0;           // 数据体总大小
        size_t body_offset = 0;         // 数据体已读取的字节数
        
        bool packet_continued = false;  // 当前包是否跨多个段
    } ctx_;
    
    ParseState state_ = ParseState::FIND_PAGE;
public:
    OggDemuxer() {
        Reset();
    }
    
    void Reset();
    
    // 处理数据块，返回已处理的字节数
    size_t Process(const uint8_t* data, size_t size,
                  std::function<void(const uint8_t*, size_t, int)> on_packet);
    
private:
    void ProcessPacket(const uint8_t* data, size_t size,
                      std::function<void(const uint8_t*, size_t, int)> on_packet);
};

#endif