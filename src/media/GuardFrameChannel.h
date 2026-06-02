#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace weaknet {

struct GuardFramePacket {
    std::uint32_t frame_id = 0;
    std::uint16_t packet_index = 0;
    std::uint16_t packet_count = 0;
    std::uint32_t crc32 = 0;
    std::vector<std::uint8_t> payload;
};

// 极端弱网保底图像通道。
//
// 设计目的：
// 1. 主视频流在 80% 以上丢包时可能无法连续解码。
// 2. 保底通道以低频、低清、完整帧为目标，宁愿慢，也要让接收端周期性拿到可识别图像。
// 3. 当前类只负责分包和完整性校验，实际传输可接 WebRTC DataChannel、独立 UDP+FEC 或自研可靠链路。
class GuardFrameChannel {
public:
    explicit GuardFrameChannel(std::size_t mtu_bytes = 900);

    std::vector<GuardFramePacket> packetize(std::uint32_t frame_id, const std::vector<std::uint8_t>& encoded_frame) const;
    bool reassemble(const std::vector<GuardFramePacket>& packets, std::vector<std::uint8_t>& out_frame) const;
    static std::vector<std::uint8_t> serialize_packet(const GuardFramePacket& packet);
    static bool deserialize_packet(const std::uint8_t* data, std::size_t size, GuardFramePacket& out_packet);

private:
    static std::uint32_t crc32(const std::vector<std::uint8_t>& data);

    std::size_t mtu_bytes_ = 900;
};

} // namespace weaknet
