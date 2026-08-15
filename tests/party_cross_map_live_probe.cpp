/*-----------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: party_cross_map_live_probe.cpp
    Desc: POSIX direct-LAN party/global-vs-map-scope live acceptance probe.

-----------------------------------------------------------------------------*/

#include "late_join_protocol.hpp"
#include "late_join_state.hpp"
#include "lan_discovery.hpp"
#include "party_chat.hpp"
#include "party_protocol.hpp"
#include "social_party_ui_model.hpp"

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
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t joinPacketSize = 102;
constexpr std::size_t safeHeaderSize = 9;
constexpr auto overallTimeout = std::chrono::seconds(75);
constexpr auto entityIsolationWindow = std::chrono::seconds(2);
constexpr char divergentMapFile[] = "partyprobe.lmp";
constexpr char globalChatProbe[] = "ordinary global chat remains global";
constexpr char globalChatDisplay[] =
    "PartyProbeA: ordinary global chat remains global";
constexpr char partylessError[] = "You are not currently in a party.";
constexpr char partylessChatProbe[] = "must not fall back to global";
constexpr char forgedPartyChatProbe[] = "forged party sender must fail";
constexpr char forgedPartyChatDisplay[] =
    "[Party] PartyProbeA: forged party sender must fail";
constexpr char crossMapPartyChatProbe[] = "cross-map party chat accepted";
constexpr char crossMapPartyChatDisplay[] =
    "[Party] PartyProbeA: cross-map party chat accepted";

bool sendDatagram(
    const int socketHandle,
    const sockaddr_in& server,
    const std::vector<std::uint8_t>& data)
{
    return sendto(
        socketHandle,
        data.data(),
        data.size(),
        0,
        reinterpret_cast<const sockaddr*>(&server),
        sizeof(server)) == static_cast<ssize_t>(data.size());
}

bool acknowledgeSafe(
    const int socketHandle,
    const sockaddr_in& server,
    const std::uint8_t player,
    const std::uint32_t packetNumber)
{
    std::vector<std::uint8_t> acknowledgement(safeHeaderSize, 0);
    std::memcpy(acknowledgement.data(), "GOTP", 4);
    acknowledgement[4] = player;
    LateJoinProtocol::write32(acknowledgement, 5, packetNumber);
    return sendDatagram(socketHandle, server, acknowledgement);
}

std::vector<std::uint8_t> makeJoin(
    const std::string& name,
    const std::string& version)
{
    std::vector<std::uint8_t> join(joinPacketSize, 0);
    std::memcpy(join.data(), "JOIN", 4);
    std::memcpy(
        join.data() + 4,
        name.data(),
        std::min<std::size_t>(31, name.size()));
    LateJoinProtocol::write32(join, 36, 0);
    LateJoinProtocol::write32(join, 40, 0);
    LateJoinProtocol::write32(join, 44, 0);
    std::memcpy(
        join.data() + 48,
        version.data(),
        std::min<std::size_t>(8, version.size()));
    join[56] = 0;
    join[69] = 0x07;
    return join;
}

bool validateDiscovery(const std::uint16_t port)
{
    const int socketHandle = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketHandle < 0)
    {
        return false;
    }
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &server.sin_addr) != 1)
    {
        close(socketHandle);
        return false;
    }
    const std::vector<std::uint8_t> request = {'S', 'C', 'A', 'N'};
    if (!sendDatagram(socketHandle, server, request))
    {
        close(socketHandle);
        return false;
    }
    pollfd descriptor{socketHandle, POLLIN, 0};
    if (poll(&descriptor, 1, 2000) <= 0)
    {
        close(socketHandle);
        return false;
    }
    std::vector<std::uint8_t> packet(2048, 0);
    const ssize_t bytes = recv(
        socketHandle, packet.data(), packet.size(), 0);
    close(socketHandle);
    if (bytes < 17)
    {
        return false;
    }
    packet.resize(static_cast<std::size_t>(bytes));
    if (std::memcmp(packet.data(), "SCAN", 4) != 0)
    {
        return false;
    }
    const std::uint32_t hostnameLength =
        LateJoinProtocol::read32(packet.data(), 4);
    const std::size_t legacyLength = 8U + hostnameLength + 9U;
    if (hostnameLength == 0 || hostnameLength >= 256
        || legacyLength > packet.size())
    {
        return false;
    }
    const LanDiscovery::Extension extension =
        LanDiscovery::decodeExtension(
            packet.data() + legacyLength,
            packet.size() - legacyLength);
    return extension.present && extension.dedicated
        && extension.lateJoin && extension.gamePort == port;
}

bool validRuntimeHelo(
    const std::vector<std::uint8_t>& packet,
    std::uint8_t& player)
{
    if (packet.size() < 10
        || std::memcmp(packet.data(), "HELO", 4) != 0
        || packet[packet.size() - 2] != 1
        || packet.back() != 1) // CharacterSaveMode::LOCAL on the wire.
    {
        return false;
    }
    const std::uint32_t assigned =
        LateJoinProtocol::read32(packet.data(), 4);
    const std::size_t rosterBytes = packet.size() - 10U;
    std::size_t rosterCount = 0;
    if (rosterBytes % 38U == 0)
    {
        rosterCount = rosterBytes / 38U;
    }
    else if (rosterBytes % 98U == 0)
    {
        rosterCount = rosterBytes / 98U;
    }
    if (assigned == 0 || rosterCount == 0 || assigned >= rosterCount)
    {
        return false;
    }
    player = static_cast<std::uint8_t>(assigned);
    return true;
}

class ProbeClient
{
public:
    ProbeClient(
        const std::uint16_t port,
        std::string characterName)
        : name(std::move(characterName))
    {
        server.sin_family = AF_INET;
        server.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);
    }

    ~ProbeClient()
    {
        if (socketHandle >= 0)
        {
            close(socketHandle);
        }
    }

    bool start()
    {
        socketHandle = socket(AF_INET, SOCK_DGRAM, 0);
        if (socketHandle < 0)
        {
            return fail("socket creation failed");
        }
        if (!sendDatagram(
                socketHandle, server, makeJoin(name, "v5.0.2")))
        {
            return fail("JOIN send failed");
        }
        return true;
    }

    int descriptor() const
    {
        return socketHandle;
    }

    const std::string& failure() const
    {
        return failureMessage;
    }

    bool isJoined() const
    {
        return joined;
    }

    std::uint8_t slot() const
    {
        return player;
    }

    const AutomatiaParty::Protocol::PartyState& partyState() const
    {
        return partySnapshot.partyState();
    }

    const AutomatiaParty::Protocol::InvitationList& invitationState() const
    {
        return partySnapshot.invitationList();
    }

    const AutomatiaParty::Protocol::Result* result(
        const std::uint32_t requestId) const
    {
        const auto found = results.find(requestId);
        return found == results.end() ? nullptr : &found->second;
    }

    bool transitioned() const
    {
        return sawLevelChange;
    }

    bool markedEntityReceived() const
    {
        return sawMarkedEntity;
    }

    bool sendPartyRequest(AutomatiaParty::Protocol::Request request)
    {
        request.actorSlot = player;
        return sendSafe(
            AutomatiaParty::Protocol::encodeRequest(request));
    }

    bool sendPartyRequestClaiming(
        AutomatiaParty::Protocol::Request request,
        const std::uint8_t claimedPlayer)
    {
        request.actorSlot = claimedPlayer;
        return sendSafeClaiming(
            AutomatiaParty::Protocol::encodeRequest(request),
            claimedPlayer);
    }

	bool sendPartyChat(const std::string& message)
	{
		AutomatiaPartyChat::Request request;
		request.senderSlot = player;
		request.message = message;
		return sendSafe(AutomatiaPartyChat::encodeRequest(request));
	}

	bool sendPartyChatClaimingSender(
		const std::string& message,
		const std::uint8_t claimedPlayer)
	{
		AutomatiaPartyChat::Request request;
		request.senderSlot = claimedPlayer;
		request.message = message;
		// The SAFE envelope retains this socket's authenticated player. Only
		// the inner PCHT sender claim is forged.
		return sendSafe(AutomatiaPartyChat::encodeRequest(request));
	}

	bool sendGlobalChat(const std::string& message)
	{
		if (message.empty()
			|| message.size() > AutomatiaPartyChat::MAX_MESSAGE_BYTES)
		{
			return fail("invalid global-chat probe message");
		}
		std::vector<std::uint8_t> packet(10 + message.size(), 0);
		std::memcpy(packet.data(), "MSGS", 4);
		packet[4] = player;
		LateJoinProtocol::write32(packet, 5, 0xffffffffU);
		std::memcpy(packet.data() + 9, message.data(), message.size());
		return sendSafe(packet);
	}

	bool receivedChat(const std::string& message) const
	{
		return std::find(
			chatMessages.begin(), chatMessages.end(), message)
			!= chatMessages.end();
	}

    bool sendKeepalive()
    {
        std::vector<std::uint8_t> keepalive = {
            'K', 'P', 'A', 'L', player
        };
        return sendSafe(keepalive);
    }

    bool receiveOne()
    {
        std::vector<std::uint8_t> datagram(65536, 0);
        const ssize_t bytes = recv(
            socketHandle, datagram.data(), datagram.size(), 0);
        if (bytes <= 0)
        {
            return errno == EAGAIN || errno == EWOULDBLOCK
                ? true : fail("UDP receive failed");
        }
        datagram.resize(static_cast<std::size_t>(bytes));
        if (datagram.size() >= 4
            && std::memcmp(datagram.data(), "GOTP", 4) == 0)
        {
            return true;
        }

        std::vector<std::uint8_t> packet = datagram;
        if (datagram.size() >= safeHeaderSize
            && std::memcmp(datagram.data(), "SAFE", 4) == 0)
        {
            const std::uint32_t safeNumber =
                LateJoinProtocol::read32(datagram.data(), 5);
            std::vector<std::uint8_t> inner(
                datagram.begin() + safeHeaderSize,
                datagram.end());
            std::uint8_t acknowledgementPlayer = player;
            if (acknowledgementPlayer == 0)
            {
                acknowledgementPlayer = assignedPlayerFromHelo(inner);
            }
            if (!acknowledgeSafe(
                    socketHandle, server,
                    acknowledgementPlayer, safeNumber))
            {
                return fail("SAFE acknowledgement failed");
            }
            if (!receivedSafeNumbers.insert(safeNumber).second)
            {
                return true;
            }
            packet = std::move(inner);
        }

        if (!consumeHeloChunks(packet))
        {
            return false;
        }
        if (packet.empty())
        {
            return true;
        }
        return handlePacket(packet);
    }

private:
    bool fail(const std::string& message)
    {
        failureMessage = name + ": " + message;
        return false;
    }

    bool sendSafe(const std::vector<std::uint8_t>& payload)
    {
        return sendSafeClaiming(payload, player);
    }

    bool sendSafeClaiming(
        const std::vector<std::uint8_t>& payload,
        const std::uint8_t claimedPlayer)
    {
        if (payload.empty() || claimedPlayer == 0)
        {
            return fail("attempted to send an invalid SAFE payload");
        }
        std::vector<std::uint8_t> safe(
            safeHeaderSize + payload.size(), 0);
        std::memcpy(safe.data(), "SAFE", 4);
        safe[4] = claimedPlayer;
        LateJoinProtocol::write32(safe, 5, outgoingSafeNumber++);
        std::memcpy(
            safe.data() + safeHeaderSize,
            payload.data(), payload.size());
        return sendDatagram(socketHandle, server, safe)
            ? true : fail("SAFE send failed");
    }

    std::uint8_t assignedPlayerFromHelo(
        const std::vector<std::uint8_t>& packet) const
    {
        if (packet.size() >= 8
            && std::memcmp(packet.data(), "HELO", 4) == 0)
        {
            const std::uint32_t assigned =
                LateJoinProtocol::read32(packet.data(), 4);
            return assigned > 0 && assigned < 15
                ? static_cast<std::uint8_t>(assigned) : 0;
        }
        if (packet.size() >= 20
            && std::memcmp(packet.data(), "HLCN", 4) == 0
            && packet[6] == 0
            && std::memcmp(packet.data() + 12, "HELO", 4) == 0)
        {
            const std::uint32_t assigned =
                LateJoinProtocol::read32(packet.data() + 12, 4);
            return assigned > 0 && assigned < 15
                ? static_cast<std::uint8_t>(assigned) : 0;
        }
        return 0;
    }

    bool consumeHeloChunks(std::vector<std::uint8_t>& packet)
    {
        if (packet.size() < 12
            || std::memcmp(packet.data(), "HLCN", 4) != 0)
        {
            return true;
        }
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
            || packet.size() != 12U + chunkBytes)
        {
            return fail("invalid HLCN record");
        }
        if (heloChunks.empty())
        {
            heloTransferId = transferId;
            heloTotalBytes = totalBytes;
            heloChunks.resize(chunkCount);
            heloReceived.assign(chunkCount, false);
        }
        if (transferId != heloTransferId
            || totalBytes != heloTotalBytes
            || chunkCount != heloChunks.size())
        {
            return fail("inconsistent HLCN transaction");
        }
        const std::vector<std::uint8_t> payload(
            packet.begin() + 12, packet.end());
        if (heloReceived[chunkIndex]
            && heloChunks[chunkIndex] != payload)
        {
            return fail("changed duplicate HLCN record");
        }
        heloChunks[chunkIndex] = payload;
        heloReceived[chunkIndex] = true;
        if (std::find(
                heloReceived.begin(), heloReceived.end(), false)
            != heloReceived.end())
        {
            packet.clear();
            return true;
        }
        packet.clear();
        for (const auto& chunk : heloChunks)
        {
            packet.insert(packet.end(), chunk.begin(), chunk.end());
        }
        return packet.size() == heloTotalBytes
            ? true : fail("HLCN total mismatch");
    }

    bool handlePacket(const std::vector<std::uint8_t>& packet)
    {
        if (!requestedSnapshot && validRuntimeHelo(packet, player))
        {
            std::vector<std::uint8_t> hello = {
                'L', 'J', 'H', 'I', player
            };
            if (!sendSafe(hello))
            {
                return false;
            }
            std::vector<std::uint8_t> character(49, 0);
            std::memcpy(character.data(), "PLYR", 4);
            character[4] = player;
            std::memcpy(
                character.data() + 5,
                name.data(),
                std::min<std::size_t>(31, name.size()));
            if (!sendSafe(character))
            {
                return false;
            }
            std::vector<std::uint8_t> ready = {
                'R', 'E', 'D', 'Y', player, 1
            };
            requestedSnapshot = sendSafe(ready);
            return requestedSnapshot;
        }

        if (packet.size() >= 4
            && std::memcmp(packet.data(), "LJBG", 4) == 0)
        {
            return LateJoinProtocol::decodeBegin(
                    packet.data(), packet.size(), begin)
                && snapshotAssembler.begin(begin)
                ? true : fail("invalid LJBG");
        }
        if (packet.size() >= 4
            && std::memcmp(packet.data(), "LJCH", 4) == 0)
        {
            LateJoinProtocol::Chunk chunk;
            return LateJoinProtocol::decodeChunk(
                    packet.data(), packet.size(), chunk)
                && snapshotAssembler.accept(chunk)
                    != LateJoinProtocol::ReceiveResult::Rejected
                ? true : fail("invalid LJCH");
        }
        if (packet.size() >= 4
            && std::memcmp(packet.data(), "LJDN", 4) == 0)
        {
            LateJoinProtocol::Complete complete;
            if (!LateJoinProtocol::decodeComplete(
                    packet.data(), packet.size(), complete)
                || snapshotAssembler.finish(complete)
                    != LateJoinProtocol::ReceiveResult::Complete)
            {
                return fail("invalid LJDN");
            }
            const std::string snapshot(
                snapshotAssembler.snapshot().begin(),
                snapshotAssembler.snapshot().end());
            if (snapshot.find("\"schema_version\":2")
                    == std::string::npos
                || snapshot.find("\"snapshot_scope\":\"map_instance\"")
                    == std::string::npos
                || snapshot.find("\"party\"") == std::string::npos)
            {
                return fail("invalid scoped schema-v2 snapshot");
            }
            LateJoinProtocol::Ready ready;
            ready.playerIndex = player;
            ready.transferId = begin.transferId;
            ready.instanceRevision = begin.instanceRevision;
            ready.snapshotAccepted = true;
            sentReady = sendSafe(LateJoinProtocol::encodeReady(ready));
            return sentReady;
        }
        if (packet.size() >= 4
            && std::memcmp(packet.data(), "LJOK", 4) == 0)
        {
            LateJoinProtocol::Authorization authorization;
            if (!sentReady
                || !LateJoinProtocol::decodeAuthorization(
                    packet.data(), packet.size(), authorization)
                || !authorization.spawnAuthorized
                || authorization.transferId != begin.transferId
                || authorization.instanceRevision != begin.instanceRevision)
            {
                return fail("invalid LJOK");
            }
            LateJoinProtocol::Ready go;
            go.playerIndex = player;
            go.transferId = begin.transferId;
            go.instanceRevision = begin.instanceRevision;
            go.snapshotAccepted = true;
            sentGo = sendSafe(LateJoinProtocol::encodeGo(go));
            return sentGo;
        }
        if (packet.size() >= 4
            && std::memcmp(packet.data(), "LJCB", 4) == 0)
        {
            LateJoinProtocol::Complete metadata;
            if (!LateJoinProtocol::decodeCatchupBegin(
                    packet.data(), packet.size(), metadata)
                || metadata.transferId != begin.transferId
                || metadata.instanceRevision != begin.instanceRevision)
            {
                return fail("invalid LJCB");
            }
            LateJoinProtocol::Begin catchupBegin;
            catchupBegin.transferId = metadata.transferId;
            catchupBegin.instanceRevision = metadata.instanceRevision;
            catchupBegin.chunkCount = metadata.chunkCount;
            catchupBegin.totalBytes = metadata.totalBytes;
            catchupBegin.snapshotChecksum = metadata.snapshotChecksum;
            return catchupAssembler.begin(catchupBegin)
                ? true : fail("invalid catch-up bounds");
        }
        if (packet.size() >= 4
            && std::memcmp(packet.data(), "LJCC", 4) == 0)
        {
            LateJoinProtocol::Chunk chunk;
            return LateJoinProtocol::decodeCatchupChunk(
                    packet.data(), packet.size(), chunk)
                && catchupAssembler.accept(chunk)
                    != LateJoinProtocol::ReceiveResult::Rejected
                ? true : fail("invalid LJCC");
        }
        if (packet.size() >= 4
            && std::memcmp(packet.data(), "LJCE", 4) == 0)
        {
            LateJoinProtocol::Complete complete;
            std::vector<std::vector<std::uint8_t>> records;
            if (!LateJoinProtocol::decodeCatchupComplete(
                    packet.data(), packet.size(), complete)
                || catchupAssembler.finish(complete)
                    != LateJoinProtocol::ReceiveResult::Complete
                || !LateJoinPacketCatchupBuffer::deserialize(
                    catchupAssembler.snapshot(), records))
            {
                return fail("invalid LJCE");
            }
            bool sawParty = false;
            bool sawInvitations = false;
            for (const auto& record : records)
            {
                if (record.size() >= 4
                    && std::memcmp(record.data(), "PTYS", 4) == 0)
                {
                    sawParty = decodePartyState(record);
                }
                else if (record.size() >= 4
                    && std::memcmp(record.data(), "PTYI", 4) == 0)
                {
                    sawInvitations = decodeInvitations(record);
                }
            }
            catchupComplete = sawParty && sawInvitations;
            return catchupComplete
                ? true : fail("catch-up omitted recipient party records");
        }
        if (packet.size() >= 4
            && std::memcmp(packet.data(), "STRT", 4) == 0)
        {
            if (!sentGo || !catchupComplete || packet.size() < 19
                || packet[17] < 1 || packet[18] == 0)
            {
                return fail("invalid STRT");
            }
            joined = true;
            return true;
        }
        if (packet.size() >= 4
            && std::memcmp(packet.data(), "PTYS", 4) == 0)
        {
            return decodePartyState(packet);
        }
        if (packet.size() >= 4
            && std::memcmp(packet.data(), "PTYI", 4) == 0)
        {
            return decodeInvitations(packet);
        }
        if (packet.size() >= 4
            && std::memcmp(packet.data(), "PTYR", 4) == 0)
        {
            AutomatiaParty::Protocol::Result decoded;
            if (!AutomatiaParty::Protocol::decodeResult(
                    packet.data(), packet.size(), decoded))
            {
                return fail("invalid PTYR");
            }
            results[decoded.requestId] = decoded;
            return true;
        }
		if (packet.size() >= 13
			&& std::memcmp(packet.data(), "MSGS", 4) == 0)
		{
			const auto end = std::find(
				packet.begin() + 12, packet.end(), 0);
			if (end == packet.end())
			{
				return fail("unterminated MSGS record");
			}
			chatMessages.emplace_back(packet.begin() + 12, end);
			return true;
		}
        if (packet.size() >= 4
            && std::memcmp(packet.data(), "LVLC", 4) == 0)
        {
            if (packet.size() >= 15)
            {
                const auto mapEnd = std::find(
                    packet.begin() + 14, packet.end(), 0);
                if (mapEnd != packet.end()
                    && std::string(packet.begin() + 14, mapEnd)
                        == divergentMapFile)
                {
                    sawLevelChange = true;
                }
            }
            return true;
        }
        if (packet.size() >= 48
            && std::memcmp(packet.data(), "ENTU", 4) == 0
            && packet[29] == 120
            && LateJoinProtocol::read16(packet.data(), 44) == 3936)
        {
            sawMarkedEntity = true;
            return true;
        }
        return true;
    }

    bool decodePartyState(const std::vector<std::uint8_t>& packet)
    {
        AutomatiaParty::Protocol::PartyState decoded;
        if (!AutomatiaParty::Protocol::decodePartyState(
                packet.data(), packet.size(), decoded)
            || decoded.recipientSlot != player)
        {
            return fail("invalid or misaddressed PTYS");
        }
        const auto staged =
            partySnapshot.stagePartyState(std::move(decoded));
        return staged != AutomatiaParty::Protocol::SnapshotStageResult::Rejected
            ? true : fail("conflicting PTYS sequence");
    }

    bool decodeInvitations(const std::vector<std::uint8_t>& packet)
    {
        AutomatiaParty::Protocol::InvitationList decoded;
        if (!AutomatiaParty::Protocol::decodeInvitationList(
                packet.data(), packet.size(), decoded)
            || decoded.recipientSlot != player)
        {
            return fail("invalid or misaddressed PTYI");
        }
        const auto staged =
            partySnapshot.stageInvitationList(std::move(decoded));
        return staged != AutomatiaParty::Protocol::SnapshotStageResult::Rejected
            ? true : fail("conflicting PTYI sequence");
    }

    int socketHandle = -1;
    sockaddr_in server{};
    std::string name;
    std::string failureMessage;
    std::uint8_t player = 0;
    std::uint32_t outgoingSafeNumber = 1;
    std::unordered_set<std::uint32_t> receivedSafeNumbers;
    LateJoinProtocol::SnapshotAssembler snapshotAssembler;
    LateJoinProtocol::SnapshotAssembler catchupAssembler;
    LateJoinProtocol::Begin begin;
    std::uint16_t heloTransferId = 0;
    std::uint16_t heloTotalBytes = 0;
    std::vector<std::vector<std::uint8_t>> heloChunks;
    std::vector<bool> heloReceived;
    bool requestedSnapshot = false;
    bool sentReady = false;
    bool sentGo = false;
    bool catchupComplete = false;
    bool joined = false;
    bool sawLevelChange = false;
    bool sawMarkedEntity = false;
    AutomatiaParty::Protocol::RecipientSnapshotState partySnapshot;
    std::unordered_map<
        std::uint32_t,
        AutomatiaParty::Protocol::Result> results;
	std::vector<std::string> chatMessages;
};

bool successful(
    const AutomatiaParty::Protocol::Result* result,
    const AutomatiaParty::Protocol::RequestOperation operation)
{
    return result
        && result->operation == operation
        && result->status == AutomatiaParty::OperationStatus::Success;
}

bool hasStatus(
    const AutomatiaParty::Protocol::Result* result,
    const AutomatiaParty::Protocol::RequestOperation operation,
    const AutomatiaParty::OperationStatus status)
{
    return result
        && result->operation == operation
        && result->status == status;
}

bool identityForOnlineSlot(
    const ProbeClient& client,
    const std::uint8_t slot,
    AutomatiaParty::DurablePlayerIdentity& identity)
{
    for (const auto& member : client.partyState().members)
    {
        if (member.onlineSlot == slot)
        {
            identity = member.identity;
            return true;
        }
    }
    return false;
}

bool sameTwoMemberParty(
    const ProbeClient& first,
    const ProbeClient& second,
    const AutomatiaParty::PartyID expected = 0)
{
    const auto& firstState = first.partyState();
    const auto& secondState = second.partyState();
    return firstState.partyId != 0
        && firstState.partyId == secondState.partyId
        && (!expected || firstState.partyId == expected)
        && firstState.revision == secondState.revision
        && firstState.members.size() == 2
        && secondState.members.size() == 2;
}

std::vector<AutomatiaSocial::ConnectedPlayer> socialRoster(
    const ProbeClient& first,
    const ProbeClient& second,
    const bool divergent)
{
    return {
        {first.slot(), "PartyProbeA", "start.lmp#world"},
        {second.slot(), "PartyProbeB",
            divergent ? "partyprobe.lmp#world" : "start.lmp#world"}
    };
}

AutomatiaSocial::ViewModel socialView(
    const ProbeClient& client,
    const std::vector<AutomatiaSocial::ConnectedPlayer>& roster)
{
    return AutomatiaSocial::buildViewModel(
        client.slot(), roster,
        client.partyState(), client.invitationState());
}
}

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 4)
    {
        std::cerr
            << "usage: party_cross_map_live_probe <port>"
            << " [--verify-restored <party-id>]\n";
        return 2;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long parsedPort = std::strtoul(argv[1], &end, 10);
    if (errno || !end || *end != '\0'
        || parsedPort == 0 || parsedPort > 65535)
    {
        std::cerr << "invalid port\n";
        return 2;
    }
    AutomatiaParty::PartyID expectedRestoredParty = 0;
    if (argc == 4)
    {
        errno = 0;
        char* partyEnd = nullptr;
        const unsigned long long parsedParty =
            std::strtoull(argv[3], &partyEnd, 10);
        if (std::strcmp(argv[2], "--verify-restored") != 0
            || errno || !partyEnd || *partyEnd != '\0'
            || parsedParty == 0)
        {
            std::cerr << "invalid restored-party arguments\n";
            return 2;
        }
        expectedRestoredParty = parsedParty;
    }
    const std::uint16_t port = static_cast<std::uint16_t>(parsedPort);
    if (!validateDiscovery(port))
    {
        std::cerr << "dedicated late-join discovery validation failed\n";
        return 1;
    }

    ProbeClient first(port, "PartyProbeA");
    ProbeClient second(port, "PartyProbeB");
    if (!first.start() || !second.start())
    {
        std::cerr << (!first.failure().empty()
            ? first.failure() : second.failure()) << '\n';
        return 1;
    }

    bool createSent = false;
	bool globalChatSent = false;
	bool globalChatVerified = false;
	bool partylessChatSent = false;
	bool partylessChatVerified = false;
    bool inviteSent = false;
    bool acceptSent = false;
    bool actorSlotSpoofSent = false;
    bool actorSlotSpoofVerified = false;
	bool partyChatSpoofSent = false;
	bool partyChatSpoofVerified = false;
    bool unauthorizedKickSent = false;
    bool fakePartySent = false;
    bool nonexistentInvitationSent = false;
    bool unauthorizedPromoteSent = false;
    bool spoofRejectionAnnounced = false;
    bool establishedAnnounced = false;
    bool promoteSent = false;
    bool crossMapAnnounced = false;
	bool crossMapChatSent = false;
	bool crossMapChatAnnounced = false;
    bool restoredKickSent = false;
    AutomatiaParty::PartyID partyId = 0;
    std::uint64_t establishedRevision = 0;
    auto actorSlotSpoofVerificationStarted =
        std::chrono::steady_clock::time_point{};
	auto partyChatSpoofVerificationStarted =
		std::chrono::steady_clock::time_point{};
    auto markedEntityAt = std::chrono::steady_clock::time_point{};
    auto lastKeepalive = std::chrono::steady_clock::now();
    const auto deadline = std::chrono::steady_clock::now() + overallTimeout;

    while (std::chrono::steady_clock::now() < deadline)
    {
        pollfd descriptors[2] = {
            {first.descriptor(), POLLIN, 0},
            {second.descriptor(), POLLIN, 0}
        };
        const int ready = poll(descriptors, 2, 100);
        if (ready < 0 && errno != EINTR)
        {
            std::cerr << "poll failed: " << std::strerror(errno) << '\n';
            return 1;
        }
        if ((descriptors[0].revents & POLLIN) && !first.receiveOne())
        {
            std::cerr << first.failure() << '\n';
            return 1;
        }
        if ((descriptors[1].revents & POLLIN) && !second.receiveOne())
        {
            std::cerr << second.failure() << '\n';
            return 1;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastKeepalive >= std::chrono::seconds(5)
            && first.slot() && second.slot())
        {
            if (!first.sendKeepalive() || !second.sendKeepalive())
            {
                std::cerr << "keepalive send failed\n";
                return 1;
            }
            lastKeepalive = now;
        }
        if (!first.isJoined() || !second.isJoined())
        {
            continue;
        }

        if (expectedRestoredParty)
        {
            if (!restoredKickSent
                && sameTwoMemberParty(
                    first, second, expectedRestoredParty))
            {
                const auto roster = socialRoster(first, second, false);
                const auto firstView = socialView(first, roster);
                const auto secondView = socialView(second, roster);
                if (!firstView.synchronized || !secondView.synchronized
                    || firstView.members.size() != 2
                    || secondView.members.size() != 2
                    || firstView.localLeader || !secondView.localLeader)
                {
                    std::cerr
                        << "restored authoritative Social projection is invalid\n";
                    return 1;
                }
                const auto target = std::find_if(
                    secondView.members.begin(), secondView.members.end(),
                    [](const AutomatiaSocial::PartyMemberRow& member)
                    {
                        return !member.localPlayer;
                    });
                if (target == secondView.members.end() || !target->canKick)
                {
                    std::cerr << "restored leader has no valid Social kick target\n";
                    return 1;
                }
                AutomatiaSocial::ActionSelection selection;
                selection.memberIdentity = target->identity;
                selection.hasMemberIdentity = true;
                AutomatiaParty::Protocol::Request request;
                if (!AutomatiaSocial::buildRequest(
                        AutomatiaSocial::Action::Kick,
                        second.slot(), 100,
                        second.partyState(), selection, request)
                    || !second.sendPartyRequest(request))
                {
                    std::cerr << "restored Social kick request failed\n";
                    return 1;
                }
                restoredKickSent = true;
                continue;
            }
            if (restoredKickSent
                && successful(
                    second.result(100),
                    AutomatiaParty::Protocol::RequestOperation::Kick)
                && first.partyState().partyId == 0
                && second.partyState().partyId == 0)
            {
                const auto roster = socialRoster(first, second, false);
                const auto firstView = socialView(first, roster);
                const auto secondView = socialView(second, roster);
                if (!firstView.synchronized || !secondView.synchronized
                    || firstView.inParty || secondView.inParty
                    || !firstView.members.empty()
                    || !secondView.members.empty()
                    || !firstView.canCreate || !secondView.canCreate)
                {
                    std::cerr
                        << "Social projection did not clear after authoritative kick\n";
                    return 1;
                }
                std::cout
                    << "SOCIAL_UI_KICK_OK party="
                    << expectedRestoredParty
                    << " leader_slot="
                    << static_cast<unsigned>(second.slot())
                    << " removed_slot="
                    << static_cast<unsigned>(first.slot())
                    << '\n';
                std::cout
                    << "restored party live probe passed: party="
                    << expectedRestoredParty
                    << " slots=" << static_cast<unsigned>(first.slot())
                    << ',' << static_cast<unsigned>(second.slot())
                    << '\n' << std::flush;
                return 0;
            }
            continue;
        }

		if (!globalChatSent)
		{
			globalChatSent = first.sendGlobalChat(globalChatProbe);
			if (!globalChatSent)
			{
				std::cerr << first.failure() << '\n';
				return 1;
			}
			continue;
		}
		if (!globalChatVerified)
		{
			if (!second.receivedChat(globalChatDisplay))
			{
				continue;
			}
			globalChatVerified = true;
			std::cout << "GLOBAL_CHAT_UNCHANGED_OK\n" << std::flush;
		}
		if (!partylessChatSent)
		{
			partylessChatSent = first.sendPartyChat(partylessChatProbe);
			if (!partylessChatSent)
			{
				std::cerr << first.failure() << '\n';
				return 1;
			}
			continue;
		}
		if (second.receivedChat(partylessError))
		{
			std::cerr << "partyless rejection leaked to another player\n";
			return 1;
		}
		if (!partylessChatVerified)
		{
			if (!first.receivedChat(partylessError))
			{
				continue;
			}
			partylessChatVerified = true;
			std::cout << "PARTYLESS_CHAT_REJECTION_OK\n" << std::flush;
		}

        if (!createSent)
        {
            const auto roster = socialRoster(first, second, false);
            const auto firstView = socialView(first, roster);
            const auto secondView = socialView(second, roster);
            if (!firstView.synchronized || !secondView.synchronized)
            {
                continue;
            }
            if (!firstView.canCreate || !secondView.canCreate
                || firstView.inParty || secondView.inParty)
            {
                std::cerr << "initial Social partyless projection is invalid\n";
                return 1;
            }
            AutomatiaParty::Protocol::Request request;
            if (!AutomatiaSocial::buildRequest(
                    AutomatiaSocial::Action::Create,
                    first.slot(), 1,
                    first.partyState(), {}, request))
            {
                std::cerr << "Social create request could not be built\n";
                return 1;
            }
            createSent = first.sendPartyRequest(request);
            if (!createSent)
            {
                std::cerr << first.failure() << '\n';
                return 1;
            }
            continue;
        }
        if (!inviteSent
            && successful(
                first.result(1),
                AutomatiaParty::Protocol::RequestOperation::Create)
            && first.partyState().partyId != 0)
        {
            partyId = first.partyState().partyId;
            const auto roster = socialRoster(first, second, false);
            const auto firstView = socialView(first, roster);
            const auto inviteTarget = std::find_if(
                firstView.players.begin(), firstView.players.end(),
                [&second](const AutomatiaSocial::PlayerRow& player)
                {
                    return player.slot == second.slot();
                });
            if (!firstView.synchronized || !firstView.localLeader
                || firstView.players.size() != 2
                || inviteTarget == firstView.players.end()
                || !inviteTarget->inviteEligible)
            {
                std::cerr << "Social invite target projection is invalid\n";
                return 1;
            }
            AutomatiaSocial::ActionSelection selection;
            selection.playerSlot = second.slot();
            AutomatiaParty::Protocol::Request request;
            if (!AutomatiaSocial::buildRequest(
                    AutomatiaSocial::Action::Invite,
                    first.slot(), 2,
                    first.partyState(), selection, request))
            {
                std::cerr << "Social invite request could not be built\n";
                return 1;
            }
            inviteSent = first.sendPartyRequest(request);
            continue;
        }
        if (!acceptSent
            && successful(
                first.result(2),
                AutomatiaParty::Protocol::RequestOperation::Invite)
            && !second.invitationState().invitations.empty())
        {
            const auto roster = socialRoster(first, second, false);
            const auto secondView = socialView(second, roster);
            if (!secondView.synchronized
                || secondView.invitations.size() != 1
                || secondView.invitations.front().inviterDisplayName
                    != "PartyProbeA")
            {
                std::cerr
                    << "authoritative invitation is missing from Social projection\n";
                return 1;
            }
            const auto& invitation = secondView.invitations.front();
            AutomatiaSocial::ActionSelection selection;
            selection.invitationId = invitation.invitationId;
            selection.invitationPartyId = invitation.partyId;
            AutomatiaParty::Protocol::Request request;
            if (!AutomatiaSocial::buildRequest(
                    AutomatiaSocial::Action::Accept,
                    second.slot(), 3,
                    second.partyState(), selection, request))
            {
                std::cerr << "Social accept request could not be built\n";
                return 1;
            }
            acceptSent = second.sendPartyRequest(request);
            continue;
        }
		if (!partyChatSpoofSent
			&& successful(
                second.result(3),
                AutomatiaParty::Protocol::RequestOperation::Accept)
            && sameTwoMemberParty(first, second, partyId))
        {
            establishedRevision = first.partyState().revision;
            const auto roster = socialRoster(first, second, false);
            const auto firstView = socialView(first, roster);
            const auto secondView = socialView(second, roster);
            if (!firstView.synchronized || !secondView.synchronized
                || firstView.members.size() != 2
                || secondView.members.size() != 2
                || !firstView.localLeader || secondView.localLeader
                || firstView.invitations.size() != 0
                || secondView.invitations.size() != 0)
            {
                std::cerr
                    << "accepted party is invalid in authoritative Social projection\n";
                return 1;
            }
            std::cout
                << "SOCIAL_UI_STATE_OK create_invite_accept party="
                << partyId << '\n' << std::flush;
			partyChatSpoofSent = second.sendPartyChatClaimingSender(
				forgedPartyChatProbe, first.slot());
			if (!partyChatSpoofSent)
			{
				std::cerr << second.failure() << '\n';
				return 1;
			}
			partyChatSpoofVerificationStarted = now;
			continue;
		}
		if (partyChatSpoofSent && !partyChatSpoofVerified)
		{
			if (first.receivedChat(forgedPartyChatDisplay)
				|| second.receivedChat(forgedPartyChatDisplay))
			{
				std::cerr << "forged PCHT sender reached a party recipient\n";
				return 1;
			}
			if (now - partyChatSpoofVerificationStarted
				< std::chrono::seconds(1))
			{
				continue;
			}
			partyChatSpoofVerified = true;
			std::cout << "PARTY_CHAT_SPOOF_REJECTION_OK\n" << std::flush;
		}
		if (partyChatSpoofVerified && !actorSlotSpoofSent
			&& sameTwoMemberParty(first, second, partyId))
		{
			AutomatiaParty::DurablePlayerIdentity member;
			if (!identityForOnlineSlot(first, second.slot(), member))
			{
				std::cerr << "could not resolve member identity for actor spoof probe\n";
				return 1;
			}
			AutomatiaParty::Protocol::Request request;
            request.operation =
                AutomatiaParty::Protocol::RequestOperation::Kick;
            request.requestId = 4;
            request.claimedPartyId = partyId;
            request.targetIdentity = member;
            request.hasTargetIdentity = true;
			actorSlotSpoofSent = second.sendPartyRequestClaiming(
				request, first.slot());
            continue;
        }
        if (actorSlotSpoofSent && !actorSlotSpoofVerified
            && (first.result(4) || second.result(4)
                || !sameTwoMemberParty(first, second, partyId)
                || first.partyState().revision != establishedRevision))
        {
            std::cerr << "SAFE actor-slot impersonation changed or answered party state\n";
            return 1;
        }
        if (actorSlotSpoofSent && !unauthorizedKickSent)
        {
            AutomatiaParty::DurablePlayerIdentity leader;
            if (!identityForOnlineSlot(first, first.slot(), leader))
            {
                std::cerr << "could not resolve leader identity for authority probe\n";
                return 1;
            }
            AutomatiaParty::Protocol::Request request;
            request.operation =
                AutomatiaParty::Protocol::RequestOperation::Kick;
            request.requestId = 5;
            request.claimedPartyId = partyId;
            request.targetIdentity = leader;
            request.hasTargetIdentity = true;
            unauthorizedKickSent = second.sendPartyRequest(request);
            continue;
        }
        if (!actorSlotSpoofVerified
            && hasStatus(
                second.result(5),
                AutomatiaParty::Protocol::RequestOperation::Kick,
                AutomatiaParty::OperationStatus::NotLeader))
        {
            if (actorSlotSpoofVerificationStarted
                == std::chrono::steady_clock::time_point{})
            {
                actorSlotSpoofVerificationStarted = now;
            }
            if (now - actorSlotSpoofVerificationStarted
                >= std::chrono::seconds(1))
            {
                actorSlotSpoofVerified = true;
            }
            else
            {
                continue;
            }
        }
        if (actorSlotSpoofVerified && !fakePartySent
            && hasStatus(
                second.result(5),
                AutomatiaParty::Protocol::RequestOperation::Kick,
                AutomatiaParty::OperationStatus::NotLeader))
        {
            AutomatiaParty::DurablePlayerIdentity member;
            if (!identityForOnlineSlot(first, second.slot(), member))
            {
                std::cerr << "could not resolve member identity for fake-party probe\n";
                return 1;
            }
            AutomatiaParty::Protocol::Request request;
            request.operation =
                AutomatiaParty::Protocol::RequestOperation::Promote;
            request.requestId = 6;
            request.claimedPartyId =
                partyId == std::numeric_limits<
                    AutomatiaParty::PartyID>::max()
                    ? partyId - 1 : partyId + 1;
            request.targetIdentity = member;
            request.hasTargetIdentity = true;
            fakePartySent = first.sendPartyRequest(request);
            continue;
        }
        if (!nonexistentInvitationSent
            && hasStatus(
                first.result(6),
                AutomatiaParty::Protocol::RequestOperation::Promote,
                AutomatiaParty::OperationStatus::InvalidParty))
        {
            AutomatiaParty::Protocol::Request request;
            request.operation =
                AutomatiaParty::Protocol::RequestOperation::Accept;
            request.requestId = 7;
            request.claimedPartyId = partyId;
            request.invitationId =
                std::numeric_limits<AutomatiaParty::InvitationID>::max();
            nonexistentInvitationSent = second.sendPartyRequest(request);
            continue;
        }
        if (!unauthorizedPromoteSent
            && hasStatus(
                second.result(7),
                AutomatiaParty::Protocol::RequestOperation::Accept,
                AutomatiaParty::OperationStatus::InvalidInvitation))
        {
            AutomatiaParty::DurablePlayerIdentity leader;
            if (!identityForOnlineSlot(first, first.slot(), leader))
            {
                std::cerr << "could not resolve leader identity for promotion probe\n";
                return 1;
            }
            AutomatiaParty::Protocol::Request request;
            request.operation =
                AutomatiaParty::Protocol::RequestOperation::Promote;
            request.requestId = 8;
            request.claimedPartyId = partyId;
            request.targetIdentity = leader;
            request.hasTargetIdentity = true;
            unauthorizedPromoteSent = second.sendPartyRequest(request);
            continue;
        }
        if (!spoofRejectionAnnounced
            && hasStatus(
                second.result(8),
                AutomatiaParty::Protocol::RequestOperation::Promote,
                AutomatiaParty::OperationStatus::NotLeader)
            && sameTwoMemberParty(first, second, partyId)
            && first.partyState().revision == establishedRevision
            && first.partyState().leaderIndex
                < first.partyState().members.size()
            && first.partyState().members[
                first.partyState().leaderIndex].onlineSlot == first.slot())
        {
            spoofRejectionAnnounced = true;
            establishedAnnounced = true;
            std::cout
                << "SPOOF_REJECTION_OK party=" << partyId
                << " revision=" << establishedRevision
                << " actor_endpoint=verified\n";
            std::cout
                << "PARTY_ESTABLISHED source="
                << static_cast<unsigned>(first.slot())
                << " transition=" << static_cast<unsigned>(second.slot())
                << " party=" << partyId << '\n' << std::flush;
            continue;
        }
        if (establishedAnnounced && second.transitioned() && !promoteSent)
        {
            const auto roster = socialRoster(first, second, true);
            const auto firstView = socialView(first, roster);
            if (!firstView.synchronized || !firstView.localLeader
                || firstView.members.size() != 2)
            {
                std::cerr
                    << "cross-map members disappeared from Social projection\n";
                return 1;
            }
            const auto target = std::find_if(
                firstView.members.begin(), firstView.members.end(),
                [](const AutomatiaSocial::PartyMemberRow& member)
                {
                    return !member.localPlayer;
                });
            if (target == firstView.members.end() || !target->canPromote)
            {
                std::cerr << "could not resolve cross-map promotion target\n";
                return 1;
            }
            AutomatiaSocial::ActionSelection selection;
            selection.memberIdentity = target->identity;
            selection.hasMemberIdentity = true;
            AutomatiaParty::Protocol::Request request;
            if (!AutomatiaSocial::buildRequest(
                    AutomatiaSocial::Action::Promote,
                    first.slot(), 9,
                    first.partyState(), selection, request))
            {
                std::cerr << "Social promote request could not be built\n";
                return 1;
            }
            promoteSent = first.sendPartyRequest(request);
            continue;
        }
        if (promoteSent && !crossMapAnnounced
            && successful(
                first.result(9),
                AutomatiaParty::Protocol::RequestOperation::Promote)
            && sameTwoMemberParty(first, second, partyId)
            && first.partyState().revision > establishedRevision
            && first.partyState().leaderIndex
                < first.partyState().members.size()
            && second.partyState().leaderIndex
                < second.partyState().members.size()
            && first.partyState().members[
                first.partyState().leaderIndex].onlineSlot == second.slot()
            && second.partyState().members[
                second.partyState().leaderIndex].onlineSlot == second.slot())
        {
            const auto roster = socialRoster(first, second, true);
            const auto firstView = socialView(first, roster);
            const auto secondView = socialView(second, roster);
            if (!firstView.synchronized || !secondView.synchronized
                || firstView.members.size() != 2
                || secondView.members.size() != 2
                || firstView.localLeader || !secondView.localLeader)
            {
                std::cerr
                    << "promoted cross-map party is invalid in Social projection\n";
                return 1;
            }
			crossMapChatSent = first.sendPartyChat(crossMapPartyChatProbe);
			if (!crossMapChatSent)
			{
				std::cerr << first.failure() << '\n';
				return 1;
			}
			crossMapAnnounced = true;
            std::cout
                << "SOCIAL_UI_CROSS_MAP_OK party=" << partyId
                << " members_visible=2 leader_slot="
                << static_cast<unsigned>(second.slot()) << '\n';
            std::cout
                << "CROSS_MAP_PARTY_OK source="
                << static_cast<unsigned>(first.slot())
                << " transitioned=" << static_cast<unsigned>(second.slot())
                << " party=" << partyId
                << " revision=" << first.partyState().revision
                << '\n' << std::flush;
			continue;
		}
		if (crossMapChatSent && !crossMapChatAnnounced
			&& first.receivedChat(crossMapPartyChatDisplay)
			&& second.receivedChat(crossMapPartyChatDisplay))
		{
			crossMapChatAnnounced = true;
			std::cout
				<< "PARTY_CHAT_CROSS_MAP_OK party=" << partyId
				<< " recipients="
				<< static_cast<unsigned>(first.slot()) << ','
				<< static_cast<unsigned>(second.slot())
				<< '\n' << std::flush;
		}
        if (second.markedEntityReceived())
        {
            std::cerr
                << "map-scoped marked ENTU leaked to transitioned party member\n";
            return 1;
        }
		if (crossMapChatAnnounced && first.markedEntityReceived()
            && markedEntityAt == std::chrono::steady_clock::time_point{})
        {
            markedEntityAt = now;
        }
        if (markedEntityAt != std::chrono::steady_clock::time_point{}
            && now - markedEntityAt >= entityIsolationWindow)
        {
            std::cout
                << "party cross-map live probe passed: party=" << partyId
                << " global_party_sync=both_maps"
                << " marked_entity=source_only"
                << " source_slot=" << static_cast<unsigned>(first.slot())
                << " transitioned_slot="
                << static_cast<unsigned>(second.slot())
                << '\n' << std::flush;
            return 0;
        }
    }

    std::cerr
        << "party cross-map live probe timed out"
        << " joined=" << first.isJoined() << ',' << second.isJoined()
        << " established=" << establishedAnnounced
        << " transitioned=" << second.transitioned()
        << " promoted=" << promoteSent
		<< " cross_map=" << crossMapAnnounced
		<< " cross_map_chat=" << crossMapChatAnnounced
        << " entity=" << first.markedEntityReceived()
        << ',' << second.markedEntityReceived() << '\n';
    return 1;
}
