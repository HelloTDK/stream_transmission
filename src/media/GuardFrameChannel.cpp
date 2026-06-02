#include "media/GuardFrameChannel.h"

#include <algorithm>
#include <cstring>

namespace weaknet {
namespace {

constexpr std::uint32_t kGuardPacketMagic = 0x57474631u; // "WGF1"

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

bool read_u16(const std::uint8_t*& data, std::size_t& size, std::uint16_t& out)
{
    if (size < 2) {
        return false;
    }
    out = static_cast<std::uint16_t>(data[0] << 8) |
        static_cast<std::uint16_t>(data[1]);
    data += 2;
    size -= 2;
    return true;
}

bool read_u32(const std::uint8_t*& data, std::size_t& size, std::uint32_t& out)
{
    if (size < 4) {
        return false;
    }
    out = (static_cast<std::uint32_t>(data[0]) << 24) |
        (static_cast<std::uint32_t>(data[1]) << 16) |
        (static_cast<std::uint32_t>(data[2]) << 8) |
        static_cast<std::uint32_t>(data[3]);
    data += 4;
    size -= 4;
    return true;
}

} // namespace

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

std::vector<std::uint8_t> GuardFrameChannel::serialize_packet(const GuardFramePacket& packet)
{
    std::vector<std::uint8_t> out;
    out.reserve(20 + packet.payload.size());
    append_u32(out, kGuardPacketMagic);
    append_u32(out, packet.frame_id);
    append_u16(out, packet.packet_index);
    append_u16(out, packet.packet_count);
    append_u32(out, packet.crc32);
    append_u32(out, static_cast<std::uint32_t>(packet.payload.size()));
    out.insert(out.end(), packet.payload.begin(), packet.payload.end());
    return out;
}

bool GuardFrameChannel::deserialize_packet(const std::uint8_t* data, std::size_t size, GuardFramePacket& out_packet)
{
    if (!data) {
        return false;
    }

    std::uint32_t magic = 0;
    std::uint32_t frame_id = 0;
    std::uint16_t packet_index = 0;
    std::uint16_t packet_count = 0;
    std::uint32_t crc32_value = 0;
    std::uint32_t payload_size = 0;

    if (!read_u32(data, size, magic) ||
        !read_u32(data, size, frame_id) ||
        !read_u16(data, size, packet_index) ||
        !read_u16(data, size, packet_count) ||
        !read_u32(data, size, crc32_value) ||
        !read_u32(data, size, payload_size)) {
        return false;
    }

    if (magic != kGuardPacketMagic || payload_size != size || packet_count == 0) {
        return false;
    }

    out_packet.frame_id = frame_id;
    out_packet.packet_index = packet_index;
    out_packet.packet_count = packet_count;
    out_packet.crc32 = crc32_value;
    out_packet.payload.assign(data, data + payload_size);
    return true;
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
