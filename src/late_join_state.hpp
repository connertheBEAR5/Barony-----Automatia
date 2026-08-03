/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: late_join_state.hpp
    Desc: Bounded snapshot transaction state for late join and reconnect.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ReconnectToken
{
static constexpr std::size_t length = 32;

inline bool isValid(const std::string& token)
{
	if (token.size() != length)
	{
		return false;
	}
	for (const char character : token)
	{
		if (!((character >= '0' && character <= '9')
			|| (character >= 'a' && character <= 'f')))
		{
			return false;
		}
	}
	return true;
}

inline bool equals(const std::string& expected, const std::uint8_t* supplied)
{
	if (!isValid(expected) || !supplied)
	{
		return false;
	}
	std::uint8_t difference = 0;
	for (std::size_t index = 0; index < length; ++index)
	{
		difference |= static_cast<std::uint8_t>(expected[index]) ^ supplied[index];
	}
	return difference == 0;
}
}

class LateJoinPacketCatchupBuffer
{
public:
    static constexpr std::uint32_t maxPackets = 4096;
    static constexpr std::uint32_t maxPacketBytes = 2048;
    static constexpr std::uint32_t maxSerializedBytes = 8U * 1024U * 1024U;

    bool append(const std::uint8_t* data, std::size_t size)
    {
        if (!data || size < 4 || size > maxPacketBytes
            || packets_.size() >= maxPackets
            || serializedBytes_ > maxSerializedBytes - size - 2)
        {
            failed_ = true;
            return false;
        }
        packets_.emplace_back(data, data + size);
        serializedBytes_ += static_cast<std::uint32_t>(size + 2);
        return true;
    }

    std::vector<std::uint8_t> serialize() const
    {
        if (failed_ || serializedBytes_ > maxSerializedBytes - 4)
        {
            return {};
        }
        std::vector<std::uint8_t> bytes;
        bytes.reserve(serializedBytes_ + 4);
        const std::uint32_t count = static_cast<std::uint32_t>(packets_.size());
        bytes.push_back(static_cast<std::uint8_t>(count >> 24U));
        bytes.push_back(static_cast<std::uint8_t>(count >> 16U));
        bytes.push_back(static_cast<std::uint8_t>(count >> 8U));
        bytes.push_back(static_cast<std::uint8_t>(count));
        for (const auto& packet : packets_)
        {
            const std::uint16_t size = static_cast<std::uint16_t>(packet.size());
            bytes.push_back(static_cast<std::uint8_t>(size >> 8U));
            bytes.push_back(static_cast<std::uint8_t>(size));
            bytes.insert(bytes.end(), packet.begin(), packet.end());
        }
        return bytes;
    }

    static bool deserialize(const std::vector<std::uint8_t>& bytes,
        std::vector<std::vector<std::uint8_t>>& packets)
    {
        packets.clear();
        if (bytes.size() < 4 || bytes.size() > maxSerializedBytes)
        {
            return false;
        }
        const std::uint32_t count =
            (static_cast<std::uint32_t>(bytes[0]) << 24U)
            | (static_cast<std::uint32_t>(bytes[1]) << 16U)
            | (static_cast<std::uint32_t>(bytes[2]) << 8U)
            | bytes[3];
        if (count > maxPackets)
        {
            return false;
        }
        std::size_t offset = 4;
        packets.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            if (offset + 2 > bytes.size())
            {
                packets.clear();
                return false;
            }
            const std::size_t size =
                (static_cast<std::size_t>(bytes[offset]) << 8U)
                | bytes[offset + 1];
            offset += 2;
            if (size < 4 || size > maxPacketBytes
                || offset + size > bytes.size())
            {
                packets.clear();
                return false;
            }
            packets.emplace_back(bytes.begin() + offset,
                bytes.begin() + offset + size);
            offset += size;
        }
        if (offset != bytes.size())
        {
            packets.clear();
            return false;
        }
        return true;
    }

    void reset()
    {
        packets_.clear();
        serializedBytes_ = 0;
        failed_ = false;
    }

    bool empty() const { return packets_.empty(); }
    bool failed() const { return failed_; }
    std::size_t packetCount() const { return packets_.size(); }
    std::uint32_t serializedBytes() const { return serializedBytes_ + 4; }

private:
    std::vector<std::vector<std::uint8_t>> packets_;
    std::uint32_t serializedBytes_ = 0;
    bool failed_ = false;
};

enum class LateJoinChunkResult
{
    Rejected,
    Accepted,
    Duplicate,
    Complete
};

class LateJoinSnapshotTransaction
{
public:
    static constexpr std::uint32_t maxChunks = 20000;
    static constexpr std::uint32_t maxSnapshotBytes = 16U * 1024U * 1024U;

    enum class Phase
    {
        Idle,
        AwaitingClient,
        Receiving,
        Complete,
        Authorized,
        Failed
    };

    bool begin(
        std::uint32_t newTransferId,
        std::uint64_t newInstanceRevision,
        std::uint32_t newChunkCount,
        std::uint32_t newTotalBytes
    )
    {
        reset();
        if (newTransferId == 0
            || newChunkCount == 0
            || newChunkCount > maxChunks
            || newTotalBytes == 0
            || newTotalBytes > maxSnapshotBytes
            || newChunkCount > newTotalBytes)
        {
            phase_ = Phase::Failed;
            return false;
        }
        transferId_ = newTransferId;
        instanceRevision_ = newInstanceRevision;
        chunkCount_ = newChunkCount;
        totalBytes_ = newTotalBytes;
        received_.assign(chunkCount_, 0);
        receivedSizes_.assign(chunkCount_, 0);
        receivedChecksums_.assign(chunkCount_, 0);
        phase_ = Phase::Receiving;
        return true;
    }

    bool holdForClient()
    {
        reset();
        phase_ = Phase::AwaitingClient;
        return true;
    }

    LateJoinChunkResult acceptChunk(
        std::uint32_t packetTransferId,
        std::uint64_t packetInstanceRevision,
        std::uint32_t sequence,
        std::uint32_t payloadBytes,
        std::uint32_t payloadChecksum = 0
    )
    {
        if (phase_ != Phase::Receiving
            || packetTransferId != transferId_
            || packetInstanceRevision != instanceRevision_
            || sequence >= chunkCount_
            || payloadBytes == 0
            || payloadBytes > totalBytes_)
        {
            return LateJoinChunkResult::Rejected;
        }
        if (received_[sequence])
        {
            if (receivedSizes_[sequence] != payloadBytes
                || receivedChecksums_[sequence] != payloadChecksum)
            {
                phase_ = Phase::Failed;
                return LateJoinChunkResult::Rejected;
            }
            return LateJoinChunkResult::Duplicate;
        }
        if (receivedBytes_ > totalBytes_ - payloadBytes)
        {
            phase_ = Phase::Failed;
            return LateJoinChunkResult::Rejected;
        }
        received_[sequence] = 1;
        receivedSizes_[sequence] = payloadBytes;
        receivedChecksums_[sequence] = payloadChecksum;
        ++receivedChunks_;
        receivedBytes_ += payloadBytes;
        if (receivedChunks_ == chunkCount_)
        {
            if (receivedBytes_ != totalBytes_)
            {
                phase_ = Phase::Failed;
                return LateJoinChunkResult::Rejected;
            }
            phase_ = Phase::Complete;
            return LateJoinChunkResult::Complete;
        }
        return LateJoinChunkResult::Accepted;
    }

    bool authorize()
    {
        if (phase_ != Phase::Complete)
        {
            return false;
        }
        phase_ = Phase::Authorized;
        return true;
    }

    void fail()
    {
        phase_ = Phase::Failed;
    }

    void reset()
    {
        phase_ = Phase::Idle;
        transferId_ = 0;
        instanceRevision_ = 0;
        chunkCount_ = 0;
        totalBytes_ = 0;
        receivedChunks_ = 0;
        receivedBytes_ = 0;
        received_.clear();
        receivedSizes_.clear();
        receivedChecksums_.clear();
    }

    bool mayReceiveLiveSimulation() const
    {
        return phase_ == Phase::Idle || phase_ == Phase::Authorized;
    }

    Phase phase() const { return phase_; }
    std::uint32_t transferId() const { return transferId_; }
    std::uint64_t instanceRevision() const { return instanceRevision_; }
    std::uint32_t receivedChunks() const { return receivedChunks_; }
    std::uint32_t receivedBytes() const { return receivedBytes_; }

private:
    Phase phase_ = Phase::Idle;
    std::uint32_t transferId_ = 0;
    std::uint64_t instanceRevision_ = 0;
    std::uint32_t chunkCount_ = 0;
    std::uint32_t totalBytes_ = 0;
    std::uint32_t receivedChunks_ = 0;
    std::uint32_t receivedBytes_ = 0;
    std::vector<std::uint8_t> received_;
    std::vector<std::uint32_t> receivedSizes_;
    std::vector<std::uint32_t> receivedChecksums_;
};
