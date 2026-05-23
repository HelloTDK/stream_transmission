#include "media/GuardFrameChannel.h"

#include <algorithm>

namespace weaknet {

GuardFrameChannel::GuardFrameChannel(std::size_t mtu_bytes)
    : mtu_bytes_(std::max<std::size_t>(mtu_bytes, 256))
{
}

std::vector<GuardFramePacket> GuardFrameChannel::packetize(std::uint32_t frame_id,
                                                           const std::vector<std::uint8_t>& encoded_frame) const
{
    std::vector<GuardFramePacket> packets;
    if (encoded_frame.empty()) {
        return packets;
    }

    const auto checksum = crc32(encoded_frame);
    const auto packet_count = static_cast<std::uint16_t>((encoded_frame.size() + mtu_bytes_ - 1) / mtu_bytes_);
    packets.reserve(packet_count);

    for (std::uint16_t i = 0; i < packet_count; ++i) {
        const auto begin = static_cast<std::size_t>(i) * mtu_bytes_;
        const auto end = std::min(begin + mtu_bytes_, encoded_frame.size());

        GuardFramePacket packet;
        packet.frame_id = frame_id;
        packet.packet_index = i;
        packet.packet_count = packet_count;
        packet.crc32 = checksum;
        packet.payload.assign(encoded_frame.begin() + begin, encoded_frame.begin() + end);
        packets.push_back(std::move(packet));
    }

    return packets;
}

bool GuardFrameChannel::reassemble(const std::vector<GuardFramePacket>& packets,
                                   std::vector<std::uint8_t>& out_frame) const
{
    if (packets.empty()) {
        return false;
    }

    const auto frame_id = packets.front().frame_id;
    const auto packet_count = packets.front().packet_count;
    const auto expected_crc = packets.front().crc32;

    if (packet_count == 0 || packets.size() != packet_count) {
        return false;
    }

    std::vector<const GuardFramePacket*> ordered(packet_count, nullptr);
    for (const auto& packet : packets) {
        if (packet.frame_id != frame_id || packet.packet_count != packet_count || packet.crc32 != expected_crc) {
            return false;
        }
        if (packet.packet_index >= packet_count) {
            return false;
        }
        ordered[packet.packet_index] = &packet;
    }

    out_frame.clear();
    for (const auto* packet : ordered) {
        if (!packet) {
            return false;
        }
        out_frame.insert(out_frame.end(), packet->payload.begin(), packet->payload.end());
    }

    // 只有完整帧校验通过才交给显示层，避免弱网下出现花屏帧。
    return crc32(out_frame) == expected_crc;
}

std::uint32_t GuardFrameChannel::crc32(const std::vector<std::uint8_t>& data)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (auto byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = -(crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

} // namespace weaknet
