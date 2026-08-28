/*-----------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: late_join_live_probe.cpp
    Desc: Direct-LAN late-join protocol acceptance probe.

-----------------------------------------------------------------------------*/

#include "late_join_protocol.hpp"
#include "late_join_state.hpp"
#include "lan_discovery.hpp"
#include "party_protocol.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unordered_set>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t joinPacketSize = 102;
constexpr std::size_t safeHeaderSize = 9;
constexpr int receiveTimeoutMilliseconds = 20000;

void write32(std::vector<std::uint8_t>& data, std::size_t offset,
    std::uint32_t value)
{
    LateJoinProtocol::write32(data, offset, value);
}

bool sendDatagram(int socket, const sockaddr_in& server,
    const std::vector<std::uint8_t>& data)
{
    return sendto(
        socket,
        data.data(),
        data.size(),
        0,
        reinterpret_cast<const sockaddr*>(&server),
        sizeof(server)
    ) == static_cast<ssize_t>(data.size());
}

bool sendSafe(int socket, const sockaddr_in& server, std::uint8_t player,
    std::uint32_t& packetNumber, const std::vector<std::uint8_t>& payload)
{
    std::vector<std::uint8_t> safe(safeHeaderSize + payload.size(), 0);
    std::memcpy(safe.data(), "SAFE", 4);
    safe[4] = player;
    write32(safe, 5, packetNumber++);
    std::memcpy(safe.data() + safeHeaderSize, payload.data(), payload.size());
    return sendDatagram(socket, server, safe);
}

bool acknowledgeSafe(int socket, const sockaddr_in& server,
    std::uint8_t player, std::uint32_t packetNumber)
{
    std::vector<std::uint8_t> acknowledgement(safeHeaderSize, 0);
    std::memcpy(acknowledgement.data(), "GOTP", 4);
    acknowledgement[4] = player;
    write32(acknowledgement, 5, packetNumber);
    return sendDatagram(socket, server, acknowledgement);
}

std::vector<std::uint8_t> makeJoin(const std::string& version,
    std::uint8_t requestedSlot, std::uint32_t mapSeed,
    std::uint32_t gameKey, std::uint32_t lobbyKey,
	const std::string& reconnectToken)
{
    std::vector<std::uint8_t> join(joinPacketSize, 0);
    std::memcpy(join.data(), "JOIN", 4);
    const char name[] = "AutomatiaProbe";
    std::memcpy(join.data() + 4, name, sizeof(name));
    write32(join, 36, 0);
    write32(join, 40, 0);
    write32(join, 44, 0);
    std::memcpy(
        join.data() + 48,
        version.data(),
        std::min<std::size_t>(8, version.size())
    );
    join[56] = requestedSlot;
    write32(join, 57, mapSeed);
    write32(join, 61, gameKey);
    write32(join, 65, lobbyKey);
    join[69] = 0x07;
	if (ReconnectToken::isValid(reconnectToken))
	{
		std::memcpy(join.data() + 70, reconnectToken.data(),
			ReconnectToken::length);
	}
    return join;
}

bool validRuntimeHelo(const std::vector<std::uint8_t>& packet,
    std::uint8_t& player)
{
    if (packet.size() < 10 || std::memcmp(packet.data(), "HELO", 4) != 0
        || packet[packet.size() - 2] != 1 || packet.back() > 2)
    {
        return false;
    }
    const std::uint32_t assigned = LateJoinProtocol::read32(packet.data(), 4);
    constexpr std::size_t lobbyPlayerBytes = 38;
    constexpr std::size_t savedPlayerBytes = 98;
    const std::size_t playerPayload = packet.size() - 10;
    std::size_t playerCount = 0;
    if (playerPayload % lobbyPlayerBytes == 0)
    {
        playerCount = playerPayload / lobbyPlayerBytes;
    }
    else if (playerPayload % savedPlayerBytes == 0)
    {
        playerCount = playerPayload / savedPlayerBytes;
    }
    if (assigned == 0 || playerCount == 0 || assigned >= playerCount)
    {
        return false;
    }
    player = static_cast<std::uint8_t>(assigned);
    return true;
}

bool validRuntimeStart(const std::vector<std::uint8_t>& packet,
    bool expectedReturning, std::string& mapFile,
    std::int32_t& playableFloor, std::uint64_t& spatialRevision)
{
    if (packet.size() < 28 || std::memcmp(packet.data(), "STRT", 4) != 0
        || packet[12] != (expectedReturning ? 1 : 0)
        || packet[17] < 1 || packet[17] > 5 || packet[18] == 0)
    {
        return false;
    }
    const std::uint8_t runtimeVersion = packet[17];
    const std::size_t mapLength = packet[18];
    const std::size_t metadataOffset = 19 + mapLength;
    const std::size_t positionOffset = metadataOffset + 9;
    const std::size_t positionBytes = runtimeVersion >= 5
        ? 36 : (runtimeVersion >= 2 ? 24 : 0);
    const std::size_t transformationBytes = runtimeVersion >= 3 ? 8 : 0;
    const std::size_t visiblePlayerMaskBytes = runtimeVersion >= 4 ? 4 : 0;
    if (packet.size() != positionOffset + positionBytes
            + transformationBytes + visiblePlayerMaskBytes
        || packet[metadataOffset + 8] > 1)
    {
        return false;
    }
    mapFile.assign(
        reinterpret_cast<const char*>(packet.data() + 19),
        mapLength
    );
    playableFloor = 0;
    spatialRevision = 0;
    if (runtimeVersion >= 5)
    {
        const std::uint32_t floorRaw =
            LateJoinProtocol::read32(packet.data(), positionOffset + 24);
        std::int32_t floorSigned = 0;
        static_assert(sizeof(floorRaw) == sizeof(floorSigned),
            "late-join floor field must be 32-bit");
        std::memcpy(&floorSigned, &floorRaw, sizeof(floorSigned));
        if (floorSigned < std::numeric_limits<std::int16_t>::min()
            || floorSigned > std::numeric_limits<std::int16_t>::max())
        {
            return false;
        }
        playableFloor = floorSigned;
        const std::uint64_t revisionLow =
            LateJoinProtocol::read32(packet.data(), positionOffset + 28);
        const std::uint64_t revisionHigh =
            LateJoinProtocol::read32(packet.data(), positionOffset + 32);
        spatialRevision = revisionLow | (revisionHigh << 32U);
    }
    return mapFile.size() >= 4
        && mapFile.compare(mapFile.size() - 4, 4, ".lmp") == 0;
}

bool validateDedicatedDiscovery(
	const std::vector<std::uint8_t>& packet,
	std::uint16_t expectedPort)
{
	if (packet.size() < 17 || std::memcmp(packet.data(), "SCAN", 4) != 0)
	{
		return false;
	}
	const std::uint32_t hostnameLength =
		LateJoinProtocol::read32(packet.data(), 4);
	const std::size_t legacyLength = 8 + hostnameLength + 9;
	if (hostnameLength == 0 || hostnameLength >= 256
		|| legacyLength > packet.size())
	{
		return false;
	}
	const std::size_t playerOffset = 8 + hostnameLength;
	const std::uint32_t players =
		LateJoinProtocol::read32(packet.data(), playerOffset);
	const bool locked = packet[playerOffset + 4] != 0;
	const LanDiscovery::Extension extension =
		LanDiscovery::decodeExtension(
			packet.data() + legacyLength,
			packet.size() - legacyLength);
	return players == 0 && !locked && extension.present
		&& extension.dedicated && extension.lateJoin
		&& extension.gamePort == expectedPort;
}
}

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 3 && argc != 8)
    {
        std::cerr
            << "usage: late_join_live_probe <port> [version]"
            << " [slot mapseed gamekey lobbykey reconnect_token]\n";
        return 2;
    }

    char* portEnd = nullptr;
    const long port = std::strtol(argv[1], &portEnd, 10);
    if (!portEnd || *portEnd != '\0' || port < 1 || port > 65535)
    {
        std::cerr << "invalid port\n";
        return 2;
    }
    const std::string version = argc >= 3 ? argv[2] : "v5.0.2";
    std::uint8_t requestedSlot = 0;
    std::uint32_t mapSeed = 0;
    std::uint32_t gameKey = 0;
    std::uint32_t lobbyKey = 0;
    std::string reconnectToken;
    if (argc == 8)
    {
        const auto parse32 = [](const char* value, std::uint32_t& result)
        {
            char* end = nullptr;
            errno = 0;
            const unsigned long long parsed = std::strtoull(value, &end, 10);
            if (errno != 0 || !end || *end != '\0'
                || parsed > 0xffffffffULL)
            {
                return false;
            }
            result = static_cast<std::uint32_t>(parsed);
            return true;
        };
        std::uint32_t slot = 0;
        if (!parse32(argv[3], slot) || slot == 0 || slot >= 15
            || !parse32(argv[4], mapSeed)
            || !parse32(argv[5], gameKey) || gameKey == 0
            || !parse32(argv[6], lobbyKey) || lobbyKey == 0
			|| !ReconnectToken::isValid(argv[7]))
        {
            std::cerr << "invalid returning-player parameters\n";
            return 2;
        }
        requestedSlot = static_cast<std::uint8_t>(slot);
		reconnectToken = argv[7];
    }
    const bool expectedReturning = requestedSlot != 0;

    const int socketHandle = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketHandle < 0)
    {
        std::cerr << "socket failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, "127.0.0.1", &server.sin_addr) != 1)
    {
        close(socketHandle);
        return 1;
    }

	const std::vector<std::uint8_t> scan = {'S', 'C', 'A', 'N'};
	if (!sendDatagram(socketHandle, server, scan))
	{
		std::cerr << "SCAN send failed: " << std::strerror(errno) << '\n';
		close(socketHandle);
		return 1;
	}
	pollfd scanDescriptor{socketHandle, POLLIN, 0};
	if (poll(&scanDescriptor, 1, 2000) <= 0)
	{
		std::cerr << "dedicated LAN discovery timed out\n";
		close(socketHandle);
		return 1;
	}
	std::vector<std::uint8_t> scanReply(2048, 0);
	const ssize_t scanBytes = recv(
		socketHandle, scanReply.data(), scanReply.size(), 0);
	if (scanBytes <= 0)
	{
		std::cerr << "dedicated LAN discovery receive failed\n";
		close(socketHandle);
		return 1;
	}
	scanReply.resize(static_cast<std::size_t>(scanBytes));
	if (!validateDedicatedDiscovery(
			scanReply, static_cast<std::uint16_t>(port)))
	{
		std::cerr << "invalid dedicated LAN discovery response\n";
		close(socketHandle);
		return 1;
	}

    const std::vector<std::uint8_t> join = makeJoin(
        version, requestedSlot, mapSeed, gameKey, lobbyKey, reconnectToken);
    if (!sendDatagram(socketHandle, server, join))
    {
        std::cerr << "JOIN send failed: " << std::strerror(errno) << '\n';
        close(socketHandle);
        return 1;
    }

    std::uint8_t player = 0;
    std::uint32_t outgoingSafeNumber = 1;
    std::unordered_set<std::uint32_t> receivedSafeNumbers;
    LateJoinProtocol::SnapshotAssembler assembler;
    LateJoinProtocol::Begin begin;
	LateJoinProtocol::SnapshotAssembler catchupAssembler;
	bool catchupComplete = false;
    bool requestedSnapshot = false;
    bool sentReady = false;
    bool sentGo = false;
    std::string selectedMap;
    std::int32_t selectedFloor = 0;
    std::uint64_t selectedSpatialRevision = 0;
    std::uint16_t heloTransferId = 0;
    std::uint16_t heloTotalBytes = 0;
    std::vector<std::vector<std::uint8_t>> heloChunks;
    std::vector<bool> heloReceived;

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(receiveTimeoutMilliseconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()
        ).count();
        pollfd descriptor{socketHandle, POLLIN, 0};
        const int ready = poll(
            &descriptor,
            1,
            static_cast<int>(std::max<long long>(1, remaining))
        );
        if (ready < 0 && errno == EINTR)
        {
            continue;
        }
        if (ready <= 0)
        {
            break;
        }

        std::vector<std::uint8_t> datagram(65536, 0);
        const ssize_t bytes = recv(socketHandle, datagram.data(), datagram.size(), 0);
        if (bytes <= 0)
        {
            continue;
        }
        datagram.resize(static_cast<std::size_t>(bytes));

        if (datagram.size() >= 4 && std::memcmp(datagram.data(), "GOTP", 4) == 0)
        {
            continue;
        }

        std::vector<std::uint8_t> packet = datagram;
        if (datagram.size() >= safeHeaderSize
            && std::memcmp(datagram.data(), "SAFE", 4) == 0)
        {
            const std::uint32_t safeNumber =
                LateJoinProtocol::read32(datagram.data(), 5);
            std::vector<std::uint8_t> inner(
                datagram.begin() + safeHeaderSize,
                datagram.end()
            );
            std::uint8_t acknowledgementPlayer = player;
            if (acknowledgementPlayer == 0
                && inner.size() >= 8
                && std::memcmp(inner.data(), "HELO", 4) == 0)
            {
                const std::uint32_t assigned =
                    LateJoinProtocol::read32(inner.data(), 4);
                if (assigned > 0 && assigned < 15)
                {
                    acknowledgementPlayer = static_cast<std::uint8_t>(assigned);
                }
            }
            else if (acknowledgementPlayer == 0
                && inner.size() >= 20
                && std::memcmp(inner.data(), "HLCN", 4) == 0
                && inner[6] == 0
                && std::memcmp(inner.data() + 12, "HELO", 4) == 0)
            {
                const std::uint32_t assigned =
                    LateJoinProtocol::read32(inner.data() + 12, 4);
                if (assigned > 0 && assigned < 15)
                {
                    acknowledgementPlayer =
                        static_cast<std::uint8_t>(assigned);
                }
            }
            if (!acknowledgeSafe(
                    socketHandle, server, acknowledgementPlayer, safeNumber))
            {
                std::cerr << "SAFE acknowledgement failed\n";
                close(socketHandle);
                return 1;
            }
            if (!receivedSafeNumbers.insert(safeNumber).second)
            {
                continue;
            }
            packet = std::move(inner);
        }

        if (packet.size() >= 12 && std::memcmp(packet.data(), "HLCN", 4) == 0)
        {
            const std::uint16_t transferId =
                LateJoinProtocol::read16(packet.data(), 4);
            const std::uint8_t chunkIndex = packet[6];
            const std::uint8_t chunkCount = packet[7];
            const std::uint16_t totalBytes =
                LateJoinProtocol::read16(packet.data(), 8);
            const std::uint16_t chunkBytes =
                LateJoinProtocol::read16(packet.data(), 10);
            if (transferId == 0 || chunkCount == 0 || chunkCount > 32
                || chunkIndex >= chunkCount || totalBytes == 0
                || chunkBytes == 0 || chunkBytes > 900
                || packet.size() != 12 + chunkBytes)
            {
                std::cerr << "invalid HLCN\n";
                close(socketHandle);
                return 1;
            }
            if (heloChunks.empty())
            {
                heloTransferId = transferId;
                heloTotalBytes = totalBytes;
                heloChunks.resize(chunkCount);
                heloReceived.assign(chunkCount, false);
            }
            if (transferId != heloTransferId || totalBytes != heloTotalBytes
                || chunkCount != heloChunks.size())
            {
                std::cerr << "inconsistent HLCN transaction\n";
                close(socketHandle);
                return 1;
            }
            const std::vector<std::uint8_t> payload(
                packet.begin() + 12, packet.end());
            if (heloReceived[chunkIndex]
                && heloChunks[chunkIndex] != payload)
            {
                std::cerr << "changed duplicate HLCN\n";
                close(socketHandle);
                return 1;
            }
            heloChunks[chunkIndex] = payload;
            heloReceived[chunkIndex] = true;
            if (!std::all_of(
                    heloReceived.begin(), heloReceived.end(),
                    [](bool received) { return received; }))
            {
                continue;
            }
            packet.clear();
            for (const auto& chunk : heloChunks)
            {
                packet.insert(packet.end(), chunk.begin(), chunk.end());
            }
            if (packet.size() != heloTotalBytes)
            {
                std::cerr << "HLCN total mismatch\n";
                close(socketHandle);
                return 1;
            }
        }

        if (!requestedSnapshot && validRuntimeHelo(packet, player))
        {
            std::vector<std::uint8_t> hello(5, 0);
            std::memcpy(hello.data(), "LJHI", 4);
            hello[4] = player;
            if (!sendSafe(
                    socketHandle, server, player,
                    outgoingSafeNumber, hello))
            {
                std::cerr << "LJHI send failed\n";
                close(socketHandle);
                return 1;
            }

			std::vector<std::uint8_t> character(49, 0);
			std::memcpy(character.data(), "PLYR", 4);
			character[4] = player;
			const char probeName[] = "LateJoinProbe";
			std::memcpy(character.data() + 5, probeName, sizeof(probeName));
			LateJoinProtocol::write32(character, 37, 0);
			LateJoinProtocol::write32(character, 41, 0);
			LateJoinProtocol::write32(character, 45, 0);
			if (!sendSafe(
					socketHandle, server, player,
					outgoingSafeNumber, character))
			{
				std::cerr << "PLYR send failed\n";
				close(socketHandle);
				return 1;
			}

			std::vector<std::uint8_t> ready(6, 0);
			std::memcpy(ready.data(), "REDY", 4);
			ready[4] = player;
			ready[5] = 1;
			if (!sendSafe(
					socketHandle, server, player,
					outgoingSafeNumber, ready))
			{
				std::cerr << "REDY send failed\n";
				close(socketHandle);
				return 1;
			}
            requestedSnapshot = true;
            continue;
        }

        if (packet.size() >= 4 && std::memcmp(packet.data(), "LJBG", 4) == 0)
        {
            if (!LateJoinProtocol::decodeBegin(packet.data(), packet.size(), begin)
                || !assembler.begin(begin))
            {
                std::cerr << "invalid LJBG\n";
                close(socketHandle);
                return 1;
            }
            continue;
        }
        if (packet.size() >= 4 && std::memcmp(packet.data(), "LJCH", 4) == 0)
        {
            LateJoinProtocol::Chunk chunk;
            if (!LateJoinProtocol::decodeChunk(packet.data(), packet.size(), chunk)
                || assembler.accept(chunk) == LateJoinProtocol::ReceiveResult::Rejected)
            {
                std::cerr << "invalid LJCH\n";
                close(socketHandle);
                return 1;
            }
            continue;
        }
        if (packet.size() >= 4 && std::memcmp(packet.data(), "LJDN", 4) == 0)
        {
            LateJoinProtocol::Complete complete;
            if (!LateJoinProtocol::decodeComplete(
                    packet.data(), packet.size(), complete)
                || assembler.finish(complete)
                    != LateJoinProtocol::ReceiveResult::Complete)
            {
                std::cerr << "invalid LJDN or incomplete snapshot\n";
                close(socketHandle);
                return 1;
            }
            const std::string snapshot(
                assembler.snapshot().begin(),
                assembler.snapshot().end()
            );
            if (snapshot.find("\"schema_version\"") == std::string::npos
                || snapshot.find("\"snapshot_scope\"") == std::string::npos
                || snapshot.find("\"sam_fingerprint\"") == std::string::npos
                || snapshot.find(std::to_string(begin.sessionKey))
                    == std::string::npos)
            {
                std::cerr << "snapshot metadata validation failed\n";
                close(socketHandle);
                return 1;
            }
            LateJoinProtocol::Ready readyRecord;
            readyRecord.playerIndex = player;
            readyRecord.transferId = begin.transferId;
            readyRecord.instanceRevision = begin.instanceRevision;
            readyRecord.snapshotAccepted = true;
            if (!sendSafe(
                    socketHandle, server, player, outgoingSafeNumber,
                    LateJoinProtocol::encodeReady(readyRecord)))
            {
                std::cerr << "LJRD send failed\n";
                close(socketHandle);
                return 1;
            }
            sentReady = true;
            continue;
        }
        if (packet.size() >= 4 && std::memcmp(packet.data(), "LJOK", 4) == 0)
        {
            LateJoinProtocol::Authorization authorization;
            if (!sentReady
                || !LateJoinProtocol::decodeAuthorization(
                    packet.data(), packet.size(), authorization)
                || !authorization.spawnAuthorized
                || authorization.transferId != begin.transferId
                || authorization.instanceRevision != begin.instanceRevision)
            {
                std::cerr << "invalid LJOK\n";
                close(socketHandle);
                return 1;
            }
            LateJoinProtocol::Ready goRecord;
            goRecord.playerIndex = player;
            goRecord.transferId = begin.transferId;
            goRecord.instanceRevision = begin.instanceRevision;
            goRecord.snapshotAccepted = true;
            if (!sendSafe(
                    socketHandle, server, player, outgoingSafeNumber,
                    LateJoinProtocol::encodeGo(goRecord)))
            {
                std::cerr << "LJGO send failed\n";
                close(socketHandle);
                return 1;
            }
            sentGo = true;
            continue;
        }
		if (packet.size() >= 4 && std::memcmp(packet.data(), "LJCB", 4) == 0)
		{
			LateJoinProtocol::Complete metadata;
			if (!LateJoinProtocol::decodeCatchupBegin(
					packet.data(), packet.size(), metadata)
				|| metadata.transferId != begin.transferId
				|| metadata.instanceRevision != begin.instanceRevision)
			{
				std::cerr << "invalid LJCB\n";
				close(socketHandle);
				return 1;
			}
			LateJoinProtocol::Begin catchupBegin;
			catchupBegin.transferId = metadata.transferId;
			catchupBegin.instanceRevision = metadata.instanceRevision;
			catchupBegin.chunkCount = metadata.chunkCount;
			catchupBegin.totalBytes = metadata.totalBytes;
			catchupBegin.snapshotChecksum = metadata.snapshotChecksum;
			if (!catchupAssembler.begin(catchupBegin))
			{
				std::cerr << "invalid catch-up bounds\n";
				close(socketHandle);
				return 1;
			}
			continue;
		}
		if (packet.size() >= 4 && std::memcmp(packet.data(), "LJCC", 4) == 0)
		{
			LateJoinProtocol::Chunk chunk;
			if (!LateJoinProtocol::decodeCatchupChunk(
					packet.data(), packet.size(), chunk)
				|| catchupAssembler.accept(chunk)
					== LateJoinProtocol::ReceiveResult::Rejected)
			{
				std::cerr << "invalid LJCC\n";
				close(socketHandle);
				return 1;
			}
			continue;
		}
		if (packet.size() >= 4 && std::memcmp(packet.data(), "LJCE", 4) == 0)
		{
			LateJoinProtocol::Complete complete;
			std::vector<std::vector<std::uint8_t>> packets;
			if (!LateJoinProtocol::decodeCatchupComplete(
					packet.data(), packet.size(), complete)
				|| catchupAssembler.finish(complete)
					!= LateJoinProtocol::ReceiveResult::Complete
				|| !LateJoinPacketCatchupBuffer::deserialize(
					catchupAssembler.snapshot(), packets))
			{
				std::cerr << "invalid LJCE\n";
				close(socketHandle);
				return 1;
			}
			AutomatiaParty::Protocol::RecipientSnapshotState partySnapshot;
			for (const std::vector<std::uint8_t>& record : packets)
			{
				if (record.size() >= 4
					&& std::memcmp(record.data(), "PTYS", 4) == 0)
				{
					AutomatiaParty::Protocol::PartyState state;
					if (!AutomatiaParty::Protocol::decodePartyState(
							record.data(), record.size(), state)
						|| state.recipientSlot != player
						|| partySnapshot.stagePartyState(std::move(state))
							== AutomatiaParty::Protocol::SnapshotStageResult::Rejected)
					{
						std::cerr << "invalid party state in late-join catch-up\n";
						close(socketHandle);
						return 1;
					}
				}
				else if (record.size() >= 4
					&& std::memcmp(record.data(), "PTYI", 4) == 0)
				{
					AutomatiaParty::Protocol::InvitationList invitations;
					if (!AutomatiaParty::Protocol::decodeInvitationList(
							record.data(), record.size(), invitations)
						|| invitations.recipientSlot != player
						|| partySnapshot.stageInvitationList(
								std::move(invitations))
							== AutomatiaParty::Protocol::SnapshotStageResult::Rejected)
					{
						std::cerr << "invalid invitation state in late-join catch-up\n";
						close(socketHandle);
						return 1;
					}
				}
			}
			if (partySnapshot.committedSequence() == 0
				|| partySnapshot.partyState().recipientSlot != player
				|| partySnapshot.invitationList().recipientSlot != player)
			{
				std::cerr << "late-join catch-up omitted recipient party records\n";
				close(socketHandle);
				return 1;
			}
			catchupComplete = true;
			continue;
		}
        if (sentGo && catchupComplete
                && validRuntimeStart(packet, expectedReturning, selectedMap,
                    selectedFloor, selectedSpatialRevision))
        {
            std::cout
                << "late join live probe passed: discovery=dedicated-late-join player="
                << static_cast<unsigned>(player)
                << " transfer=" << begin.transferId
                << " chunks=" << begin.chunkCount
                << " bytes=" << begin.totalBytes
                << " map=" << selectedMap
                << " floor=" << selectedFloor
                << " spatial_revision=" << selectedSpatialRevision
                << " returning=" << expectedReturning << '\n';
            close(socketHandle);
            return 0;
        }
    }

    std::cerr
        << "late join live probe timed out"
        << " player=" << static_cast<unsigned>(player)
        << " requested=" << requestedSnapshot
        << " ready=" << sentReady
        << " go=" << sentGo
        << " catchup=" << catchupComplete << '\n';
    close(socketHandle);
    return 1;
}
