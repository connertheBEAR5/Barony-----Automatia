/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: late_join_protocol.hpp
    Desc: Bounded, versioned wire records for late-join snapshots.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace LateJoinProtocol
{
constexpr std::uint16_t version = 1;
constexpr std::size_t beginPacketSize = 40;
constexpr std::size_t chunkHeaderSize = 26;
constexpr std::size_t completePacketSize = 28;
constexpr std::size_t authorizePacketSize = 17;
constexpr std::size_t readyPacketSize = 18;
constexpr std::size_t abortPacketSize = 18;
constexpr std::size_t maxChunkPayload = 900;
constexpr std::uint32_t maxChunkCount = 20000;
constexpr std::uint32_t maxSnapshotBytes = 16U * 1024U * 1024U;

enum class ReceiveResult
{
    Rejected,
    Accepted,
    Duplicate,
    Complete
};

struct Begin
{
    std::uint32_t transferId = 0;
    std::uint64_t instanceRevision = 0;
    std::uint32_t chunkCount = 0;
    std::uint32_t totalBytes = 0;
    std::uint32_t snapshotChecksum = 0;
    std::uint32_t flags = 0;
    std::uint32_t sessionKey = 0;
};

struct Chunk
{
    std::uint32_t transferId = 0;
    std::uint64_t instanceRevision = 0;
    std::uint32_t sequence = 0;
    std::vector<std::uint8_t> payload;
};

struct Complete
{
    std::uint32_t transferId = 0;
    std::uint64_t instanceRevision = 0;
    std::uint32_t chunkCount = 0;
    std::uint32_t totalBytes = 0;
    std::uint32_t snapshotChecksum = 0;
};

struct Authorization
{
    std::uint32_t transferId = 0;
    std::uint64_t instanceRevision = 0;
    bool spawnAuthorized = false;
};

struct Ready
{
    std::uint8_t playerIndex = 0;
    std::uint32_t transferId = 0;
    std::uint64_t instanceRevision = 0;
    bool snapshotAccepted = false;
};

struct Abort
{
    std::uint8_t playerIndex = 0;
    std::uint32_t transferId = 0;
    std::uint64_t instanceRevision = 0;
    std::uint8_t reason = 0;
};

inline std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
{
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
        {
            const std::uint32_t mask =
                static_cast<std::uint32_t>(
                    -static_cast<std::int32_t>(crc & 1U)
                );
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

inline void write16(std::vector<std::uint8_t>& packet, std::size_t offset,
    std::uint16_t value)
{
    packet[offset] = static_cast<std::uint8_t>(value >> 8U);
    packet[offset + 1] = static_cast<std::uint8_t>(value);
}

inline void write32(std::vector<std::uint8_t>& packet, std::size_t offset,
    std::uint32_t value)
{
    for (int byte = 3; byte >= 0; --byte)
    {
        packet[offset + (3 - byte)] =
            static_cast<std::uint8_t>(value >> (byte * 8));
    }
}

inline void write64(std::vector<std::uint8_t>& packet, std::size_t offset,
    std::uint64_t value)
{
    for (int byte = 7; byte >= 0; --byte)
    {
        packet[offset + (7 - byte)] =
            static_cast<std::uint8_t>(value >> (byte * 8));
    }
}

inline std::uint16_t read16(const std::uint8_t* data, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[offset]) << 8U)
        | data[offset + 1]
    );
}

inline std::uint32_t read32(const std::uint8_t* data, std::size_t offset)
{
    std::uint32_t value = 0;
    for (int byte = 0; byte < 4; ++byte)
    {
        value = (value << 8U) | data[offset + byte];
    }
    return value;
}

inline std::uint64_t read64(const std::uint8_t* data, std::size_t offset)
{
    std::uint64_t value = 0;
    for (int byte = 0; byte < 8; ++byte)
    {
        value = (value << 8U) | data[offset + byte];
    }
    return value;
}

inline bool hasTag(const std::uint8_t* data, std::size_t size, const char tag[5])
{
    return data && size >= 4 && std::memcmp(data, tag, 4) == 0;
}

inline std::vector<std::uint8_t> encodeBegin(const Begin& begin)
{
    std::vector<std::uint8_t> packet(beginPacketSize, 0);
    std::memcpy(packet.data(), "LJBG", 4);
    write16(packet, 4, version);
    write16(packet, 6, static_cast<std::uint16_t>(beginPacketSize));
    write32(packet, 8, begin.transferId);
    write64(packet, 12, begin.instanceRevision);
    write32(packet, 20, begin.chunkCount);
    write32(packet, 24, begin.totalBytes);
    write32(packet, 28, begin.snapshotChecksum);
    write32(packet, 32, begin.flags);
    write32(packet, 36, begin.sessionKey);
    return packet;
}

inline bool decodeBegin(const std::uint8_t* data, std::size_t size, Begin& begin)
{
    if (!hasTag(data, size, "LJBG") || size != beginPacketSize
        || read16(data, 4) != version || read16(data, 6) != beginPacketSize)
    {
        return false;
    }
    begin.transferId = read32(data, 8);
    begin.instanceRevision = read64(data, 12);
    begin.chunkCount = read32(data, 20);
    begin.totalBytes = read32(data, 24);
    begin.snapshotChecksum = read32(data, 28);
    begin.flags = read32(data, 32);
    begin.sessionKey = read32(data, 36);
    return begin.transferId != 0 && begin.chunkCount != 0
        && begin.totalBytes != 0;
}

inline std::vector<std::uint8_t> encodeChunk(const Chunk& chunk)
{
    if (chunk.payload.empty() || chunk.payload.size() > maxChunkPayload)
    {
        return {};
    }
    std::vector<std::uint8_t> packet(chunkHeaderSize + chunk.payload.size(), 0);
    std::memcpy(packet.data(), "LJCH", 4);
    write32(packet, 4, chunk.transferId);
    write64(packet, 8, chunk.instanceRevision);
    write32(packet, 16, chunk.sequence);
    write16(packet, 20, static_cast<std::uint16_t>(chunk.payload.size()));
    write32(packet, 22, crc32(chunk.payload.data(), chunk.payload.size()));
    std::memcpy(packet.data() + chunkHeaderSize, chunk.payload.data(),
        chunk.payload.size());
    return packet;
}

inline bool decodeChunk(const std::uint8_t* data, std::size_t size, Chunk& chunk)
{
    if (!hasTag(data, size, "LJCH") || size < chunkHeaderSize)
    {
        return false;
    }
    const std::size_t payloadSize = read16(data, 20);
    if (payloadSize == 0 || payloadSize > maxChunkPayload
        || size != chunkHeaderSize + payloadSize)
    {
        return false;
    }
    const std::uint8_t* payload = data + chunkHeaderSize;
    if (crc32(payload, payloadSize) != read32(data, 22))
    {
        return false;
    }
    chunk.transferId = read32(data, 4);
    chunk.instanceRevision = read64(data, 8);
    chunk.sequence = read32(data, 16);
    chunk.payload.assign(payload, payload + payloadSize);
    return chunk.transferId != 0;
}

inline std::vector<std::uint8_t> encodeComplete(const Complete& complete)
{
    std::vector<std::uint8_t> packet(completePacketSize, 0);
    std::memcpy(packet.data(), "LJDN", 4);
    write32(packet, 4, complete.transferId);
    write64(packet, 8, complete.instanceRevision);
    write32(packet, 16, complete.chunkCount);
    write32(packet, 20, complete.totalBytes);
    write32(packet, 24, complete.snapshotChecksum);
    return packet;
}

inline bool decodeComplete(const std::uint8_t* data, std::size_t size,
    Complete& complete)
{
    if (!hasTag(data, size, "LJDN") || size != completePacketSize)
    {
        return false;
    }
    complete.transferId = read32(data, 4);
    complete.instanceRevision = read64(data, 8);
    complete.chunkCount = read32(data, 16);
    complete.totalBytes = read32(data, 20);
    complete.snapshotChecksum = read32(data, 24);
    return complete.transferId != 0 && complete.chunkCount != 0
        && complete.totalBytes != 0;
}

inline std::vector<std::uint8_t> encodeAuthorization(
    const Authorization& authorization)
{
    std::vector<std::uint8_t> packet(authorizePacketSize, 0);
    std::memcpy(packet.data(), "LJOK", 4);
    write32(packet, 4, authorization.transferId);
    write64(packet, 8, authorization.instanceRevision);
    packet[16] = authorization.spawnAuthorized ? 1 : 0;
    return packet;
}

inline bool decodeAuthorization(const std::uint8_t* data, std::size_t size,
    Authorization& authorization)
{
    if (!hasTag(data, size, "LJOK") || size != authorizePacketSize
        || data[16] > 1)
    {
        return false;
    }
    authorization.transferId = read32(data, 4);
    authorization.instanceRevision = read64(data, 8);
    authorization.spawnAuthorized = data[16] == 1;
    return authorization.transferId != 0;
}

inline std::vector<std::uint8_t> encodeReady(const Ready& ready)
{
    if (ready.playerIndex >= 16)
    {
        return {};
    }
    std::vector<std::uint8_t> packet(readyPacketSize, 0);
    std::memcpy(packet.data(), "LJRD", 4);
    packet[4] = ready.playerIndex;
    write32(packet, 5, ready.transferId);
    write64(packet, 9, ready.instanceRevision);
    packet[17] = ready.snapshotAccepted ? 1 : 0;
    return packet;
}

inline bool decodeReady(const std::uint8_t* data, std::size_t size, Ready& ready)
{
    if (!hasTag(data, size, "LJRD") || size != readyPacketSize
        || data[4] >= 16 || data[17] > 1)
    {
        return false;
    }
    ready.playerIndex = data[4];
    ready.transferId = read32(data, 5);
    ready.instanceRevision = read64(data, 9);
    ready.snapshotAccepted = data[17] == 1;
    return ready.transferId != 0;
}

inline std::vector<std::uint8_t> encodeGo(const Ready& ready)
{
    std::vector<std::uint8_t> packet = encodeReady(ready);
    if (!packet.empty())
    {
        std::memcpy(packet.data(), "LJGO", 4);
    }
    return packet;
}

inline bool decodeGo(const std::uint8_t* data, std::size_t size, Ready& ready)
{
    if (!hasTag(data, size, "LJGO"))
    {
        return false;
    }
    std::vector<std::uint8_t> readyPacket(data, data + size);
    std::memcpy(readyPacket.data(), "LJRD", 4);
    return decodeReady(readyPacket.data(), readyPacket.size(), ready);
}

inline std::vector<std::uint8_t> encodeAbort(const Abort& abort)
{
    if (abort.playerIndex >= 16 || abort.reason == 0)
    {
        return {};
    }
    std::vector<std::uint8_t> packet(abortPacketSize, 0);
    std::memcpy(packet.data(), "LJAB", 4);
    packet[4] = abort.playerIndex;
    write32(packet, 5, abort.transferId);
    write64(packet, 9, abort.instanceRevision);
    packet[17] = abort.reason;
    return packet;
}

inline bool decodeAbort(const std::uint8_t* data, std::size_t size, Abort& abort)
{
    if (!hasTag(data, size, "LJAB") || size != abortPacketSize
        || data[4] >= 16 || data[17] == 0)
    {
        return false;
    }
    abort.playerIndex = data[4];
    abort.transferId = read32(data, 5);
    abort.instanceRevision = read64(data, 9);
    abort.reason = data[17];
    return true;
}

inline std::vector<std::uint8_t> encodeCatchupBegin(const Complete& begin)
{
    std::vector<std::uint8_t> packet = encodeComplete(begin);
    std::memcpy(packet.data(), "LJCB", 4);
    return packet;
}

inline bool decodeCatchupBegin(const std::uint8_t* data, std::size_t size,
    Complete& begin)
{
    if (!hasTag(data, size, "LJCB"))
    {
        return false;
    }
    std::vector<std::uint8_t> ordinary(data, data + size);
    std::memcpy(ordinary.data(), "LJDN", 4);
    return decodeComplete(ordinary.data(), ordinary.size(), begin);
}

inline std::vector<std::uint8_t> encodeCatchupChunk(const Chunk& chunk)
{
    std::vector<std::uint8_t> packet = encodeChunk(chunk);
    if (!packet.empty())
    {
        std::memcpy(packet.data(), "LJCC", 4);
    }
    return packet;
}

inline bool decodeCatchupChunk(const std::uint8_t* data, std::size_t size,
    Chunk& chunk)
{
    if (!hasTag(data, size, "LJCC"))
    {
        return false;
    }
    std::vector<std::uint8_t> ordinary(data, data + size);
    std::memcpy(ordinary.data(), "LJCH", 4);
    return decodeChunk(ordinary.data(), ordinary.size(), chunk);
}

inline std::vector<std::uint8_t> encodeCatchupComplete(const Complete& complete)
{
    std::vector<std::uint8_t> packet = encodeComplete(complete);
    std::memcpy(packet.data(), "LJCE", 4);
    return packet;
}

inline bool decodeCatchupComplete(const std::uint8_t* data, std::size_t size,
    Complete& complete)
{
    if (!hasTag(data, size, "LJCE"))
    {
        return false;
    }
    std::vector<std::uint8_t> ordinary(data, data + size);
    std::memcpy(ordinary.data(), "LJDN", 4);
    return decodeComplete(ordinary.data(), ordinary.size(), complete);
}

class SnapshotAssembler
{
public:
    bool begin(const Begin& beginRecord)
    {
        reset();
        if (beginRecord.transferId == 0 || beginRecord.chunkCount == 0
            || beginRecord.chunkCount > maxChunkCount
            || beginRecord.totalBytes == 0
            || beginRecord.totalBytes > maxSnapshotBytes
            || beginRecord.chunkCount > beginRecord.totalBytes
            || static_cast<std::uint64_t>(beginRecord.chunkCount)
                * maxChunkPayload < beginRecord.totalBytes)
        {
            failed_ = true;
            return false;
        }
        begin_ = beginRecord;
        chunks_.resize(begin_.chunkCount);
        received_.assign(begin_.chunkCount, 0);
        receiving_ = true;
        return true;
    }

    ReceiveResult accept(const Chunk& chunk)
    {
        if (!receiving_ || failed_ || chunk.transferId != begin_.transferId
            || chunk.instanceRevision != begin_.instanceRevision
            || chunk.sequence >= begin_.chunkCount || chunk.payload.empty()
            || chunk.payload.size() > maxChunkPayload)
        {
            return ReceiveResult::Rejected;
        }
        if (received_[chunk.sequence])
        {
            if (chunks_[chunk.sequence] != chunk.payload)
            {
                fail();
                return ReceiveResult::Rejected;
            }
            return ReceiveResult::Duplicate;
        }
        if (chunk.payload.size() > begin_.totalBytes
            || receivedBytes_ > begin_.totalBytes
                - static_cast<std::uint32_t>(chunk.payload.size()))
        {
            fail();
            return ReceiveResult::Rejected;
        }
        chunks_[chunk.sequence] = chunk.payload;
        received_[chunk.sequence] = 1;
        ++receivedChunks_;
        receivedBytes_ += static_cast<std::uint32_t>(chunk.payload.size());
        return ReceiveResult::Accepted;
    }

    ReceiveResult finish(const Complete& complete)
    {
        if (!receiving_ || failed_ || complete.transferId != begin_.transferId
            || complete.instanceRevision != begin_.instanceRevision
            || complete.chunkCount != begin_.chunkCount
            || complete.totalBytes != begin_.totalBytes
            || complete.snapshotChecksum != begin_.snapshotChecksum
            || receivedChunks_ != begin_.chunkCount
            || receivedBytes_ != begin_.totalBytes)
        {
            fail();
            return ReceiveResult::Rejected;
        }
        snapshot_.clear();
        snapshot_.reserve(begin_.totalBytes);
        for (const auto& chunk : chunks_)
        {
            snapshot_.insert(snapshot_.end(), chunk.begin(), chunk.end());
        }
        if (snapshot_.size() != begin_.totalBytes
            || crc32(snapshot_.data(), snapshot_.size())
                != begin_.snapshotChecksum)
        {
            fail();
            return ReceiveResult::Rejected;
        }
        receiving_ = false;
        complete_ = true;
        chunks_.clear();
        received_.clear();
        return ReceiveResult::Complete;
    }

    void reset()
    {
        begin_ = {};
        chunks_.clear();
        received_.clear();
        snapshot_.clear();
        receivedChunks_ = 0;
        receivedBytes_ = 0;
        receiving_ = false;
        complete_ = false;
        failed_ = false;
    }

    void fail()
    {
        receiving_ = false;
        complete_ = false;
        failed_ = true;
        chunks_.clear();
        received_.clear();
        snapshot_.clear();
    }

    bool receiving() const { return receiving_; }
    bool complete() const { return complete_; }
    bool failed() const { return failed_; }
    const std::vector<std::uint8_t>& snapshot() const { return snapshot_; }

private:
    Begin begin_;
    std::vector<std::vector<std::uint8_t>> chunks_;
    std::vector<std::uint8_t> received_;
    std::vector<std::uint8_t> snapshot_;
    std::uint32_t receivedChunks_ = 0;
    std::uint32_t receivedBytes_ = 0;
    bool receiving_ = false;
    bool complete_ = false;
    bool failed_ = false;
};
}
