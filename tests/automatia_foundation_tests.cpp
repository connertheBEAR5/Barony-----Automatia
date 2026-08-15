#include "automatia_save.hpp"
#include "world_instance.hpp"
#include "world_packet_scope.hpp"
#include "late_join_state.hpp"
#include "late_join_protocol.hpp"
#include "lan_discovery.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

static bool expect(bool condition, const char* expression)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << expression << '\n';
    }
    return condition;
}

#define EXPECT(expression) \
    do \
    { \
        if (!expect((expression), #expression)) \
        { \
            return false; \
        } \
    } while (false)

static bool testWorldInstanceIdentity()
{
    WorldInstanceIdentity identity;
    EXPECT(identity.key() == "start.lmp#world");
    EXPECT(
        WorldInstanceIdentity::canonicalMapFile("maps/VILLAGE.LMP")
        == "village.lmp"
    );
    EXPECT(identity.set("VILLAGE.LMP", "world"));
    EXPECT(identity.key() == "village.lmp#world");
    EXPECT(identity.revision == 1);
    EXPECT(identity.set("village.lmp", "world"));
    EXPECT(identity.revision == 1);
    EXPECT(identity.set("village.lmp", "private_2"));
    EXPECT(identity.key() == "village.lmp#private_2");
    EXPECT(identity.revision == 2);

    EXPECT(!identity.set("", "world"));
    EXPECT(!identity.set("maps/start.lmp", "world"));
    EXPECT(!identity.set("../start.lmp", "world"));
    EXPECT(!identity.set("start.lmp", "bad instance"));
    EXPECT(!identity.set("start.lmp", "bad#instance"));
    EXPECT(!WorldInstanceIdentity::isSafeMapFile("../start.lmp"));
    EXPECT(!WorldInstanceIdentity::isSafeMapFile("start.txt"));
    EXPECT(!WorldInstanceIdentity::isSafeMapFile("start.lmp:alternate"));
    EXPECT(!WorldInstanceIdentity::isSafeMapFile("start\n.lmp"));
    EXPECT(!WorldInstanceIdentity::isSafeMapFile(
        std::string(WorldInstanceIdentity::MAX_MAP_FILE_LENGTH + 1, 'a')));
    EXPECT(!WorldInstanceIdentity::isSafeInstanceId(
        std::string(WorldInstanceIdentity::MAX_INSTANCE_ID_LENGTH + 1, 'a')));

    const WorldInstanceIdentity unchanged = identity;
    EXPECT(!identity.set("../escape.lmp", "world"));
    EXPECT(identity.key() == unchanged.key());
    EXPECT(identity.revision == unchanged.revision);

    WorldInstanceIdentity sameMapDifferentRevision = identity;
    ++sameMapDifferentRevision.revision;
    EXPECT(identity.matches(sameMapDifferentRevision));
    WorldInstanceIdentity otherInstance = identity;
    EXPECT(otherInstance.set("village.lmp", "private_3"));
    EXPECT(!identity.matches(otherInstance));
    return true;
}

static bool testDedicatedLanDiscovery()
{
	EXPECT(LanDiscovery::isDedicatedHostSlot(true, 0));
	EXPECT(!LanDiscovery::isDedicatedHostSlot(true, 1));
	EXPECT(!LanDiscovery::isDedicatedHostSlot(false, 0));
	EXPECT(LanDiscovery::advertisedDisconnected(true, 0, false));
	EXPECT(!LanDiscovery::advertisedDisconnected(true, 1, false));
	EXPECT(LanDiscovery::advertisedLocked(true, false));
	EXPECT(!LanDiscovery::advertisedLocked(true, true));
	EXPECT(!LanDiscovery::advertisedLocked(false, false));
	EXPECT(LanDiscovery::browserShouldInclude(0, true));
	EXPECT(!LanDiscovery::browserShouldInclude(0, false));

	std::array<std::uint8_t, LanDiscovery::extensionSize> encoded{};
	EXPECT(LanDiscovery::encodeExtension(
		encoded.data(), encoded.size(), 57165, true, true)
		== encoded.size());
	const LanDiscovery::Extension decoded =
		LanDiscovery::decodeExtension(encoded.data(), encoded.size());
	EXPECT(decoded.present);
	EXPECT(decoded.dedicated);
	EXPECT(decoded.lateJoin);
	EXPECT(decoded.gamePort == 57165);
	EXPECT(!LanDiscovery::decodeExtension(encoded.data(), 6).present);
	encoded[0] = 'X';
	EXPECT(!LanDiscovery::decodeExtension(
		encoded.data(), encoded.size()).present);
	return true;
}

static bool testPacketScope()
{
    const std::array<std::uint8_t, 4> entity = {'E', 'N', 'T', 'U'};
    const std::array<std::uint8_t, 4> tile = {'M', 'A', 'P', 'T'};
    const std::array<std::uint8_t, 4> movement = {'P', 'M', 'O', 'V'};
    const std::array<std::uint8_t, 4> chat = {'M', 'S', 'G', 'S'};
    const std::array<std::uint8_t, 4> transition = {'L', 'V', 'L', 'C'};
    const std::array<std::uint8_t, 4> snapshot = {'P', 'W', 'B', 'G'};
    const std::array<std::uint8_t, 4> positionalSound = {'S', 'N', 'D', 'P'};
    const std::array<std::uint8_t, 4> entitySound = {'S', 'N', 'E', 'L'};
    const std::array<std::uint8_t, 4> notificationSound = {'S', 'N', 'D', 'N'};
    const std::array<std::uint8_t, 4> tunnelSpawn = {'T', 'N', 'S', 'P'};
    const std::array<std::uint8_t, 4> follower = {'L', 'E', 'A', 'D'};
    const std::array<std::uint8_t, 4> entityExistence = {'E', 'N', 'T', 'E'};
    const std::array<std::uint8_t, 4> assistClose = {'A', 'S', 'C', 'L'};
    const std::array<std::uint8_t, 4> suicide = {'I', 'D', 'I', 'E'};
    const std::array<std::uint8_t, 4> latencyRequest = {'P', 'N', 'G', 'U'};
    const std::array<std::uint8_t, 4> latencyResponse = {'P', 'N', 'G', 'R'};
    const std::array<std::uint8_t, 4> discovery = {'S', 'C', 'A', 'N'};
    const std::array<std::uint8_t, 4> preferences = {'G', 'P', 'P', 'R'};
    const std::array<std::uint8_t, 4> effect = {'E', 'F', 'F', 'E'};
    const std::array<std::uint8_t, 4> chest = {'C', 'H', 'S', 'T'};
    const std::array<std::uint8_t, 4> npc = {'N', 'P', 'C', 'U'};
    const std::array<std::uint8_t, 4> lateJoinReady = {'L', 'J', 'R', 'D'};
    const std::array<std::uint8_t, 4> lateJoinHello = {'L', 'J', 'H', 'I'};
    const std::array<std::uint8_t, 4> lateJoinGo = {'L', 'J', 'G', 'O'};
	const std::array<std::uint8_t, 4> explosion = {'E', 'X', 'P', 'L'};
	const std::array<std::uint8_t, 4> shop = {'S', 'H', 'O', 'P'};
	const std::array<std::uint8_t, 4> summon = {'S', 'U', 'M', 'M'};
	const std::array<std::uint8_t, 4> equip = {'E', 'Q', 'U', 'I'};
    const std::array<std::uint8_t, 4> ensemble = {'E', 'N', 'S', 'M'};
    const std::array<std::uint8_t, 4> partyRequest = {'P', 'T', 'Y', 'Q'};
    const std::array<std::uint8_t, 4> partyResult = {'P', 'T', 'Y', 'R'};
    const std::array<std::uint8_t, 4> partyState = {'P', 'T', 'Y', 'S'};
    const std::array<std::uint8_t, 4> partyInvites = {'P', 'T', 'Y', 'I'};
    std::array<std::uint8_t, 13> safeEntity{};
    std::memcpy(safeEntity.data(), "SAFE", 4);
    std::memcpy(safeEntity.data() + 9, "ENTF", 4);
    std::array<std::uint8_t, 13> safeParty{};
    std::memcpy(safeParty.data(), "SAFE", 4);
    std::memcpy(safeParty.data() + 9, "PTYS", 4);

    EXPECT(packetUsesActiveMapScope(entity.data(), entity.size()));
    EXPECT(packetUsesActiveMapScope(tile.data(), tile.size()));
    EXPECT(packetUsesActiveMapScope(movement.data(), movement.size()));
    EXPECT(packetUsesActiveMapScope(positionalSound.data(), positionalSound.size()));
    EXPECT(packetUsesActiveMapScope(entitySound.data(), entitySound.size()));
    EXPECT(packetUsesActiveMapScope(tunnelSpawn.data(), tunnelSpawn.size()));
    EXPECT(packetUsesActiveMapScope(follower.data(), follower.size()));
    EXPECT(packetUsesActiveMapScope(entityExistence.data(), entityExistence.size()));
    EXPECT(packetUsesActiveMapScope(assistClose.data(), assistClose.size()));
    EXPECT(packetUsesActiveMapScope(suicide.data(), suicide.size()));
    EXPECT(packetUsesActiveMapScope(effect.data(), effect.size()));
    EXPECT(packetUsesActiveMapScope(chest.data(), chest.size()));
    EXPECT(packetUsesActiveMapScope(npc.data(), npc.size()));
    EXPECT(packetUsesActiveMapScope(lateJoinReady.data(), lateJoinReady.size()));
    // LJHI authenticates a newly connected client before it owns a map.
    EXPECT(!packetUsesActiveMapScope(lateJoinHello.data(), lateJoinHello.size()));
    EXPECT(packetUsesActiveMapScope(lateJoinGo.data(), lateJoinGo.size()));
	EXPECT(packetUsesActiveMapScope(explosion.data(), explosion.size()));
	EXPECT(packetUsesActiveMapScope(shop.data(), shop.size()));
	EXPECT(packetUsesActiveMapScope(summon.data(), summon.size()));
	EXPECT(packetUsesActiveMapScope(equip.data(), equip.size()));
	EXPECT(packetUsesActiveMapScope(ensemble.data(), ensemble.size()));
    EXPECT(packetUsesActiveMapScope(safeEntity.data(), safeEntity.size()));
    EXPECT(!packetUsesActiveMapScope(chat.data(), chat.size()));
    EXPECT(!packetUsesActiveMapScope(transition.data(), transition.size()));
    EXPECT(!packetUsesActiveMapScope(snapshot.data(), snapshot.size()));
    EXPECT(!packetUsesActiveMapScope(notificationSound.data(), notificationSound.size()));
    EXPECT(!packetUsesActiveMapScope(latencyRequest.data(), latencyRequest.size()));
    EXPECT(!packetUsesActiveMapScope(latencyResponse.data(), latencyResponse.size()));
    EXPECT(!packetUsesActiveMapScope(discovery.data(), discovery.size()));
    EXPECT(!packetUsesActiveMapScope(preferences.data(), preferences.size()));
    EXPECT(!packetUsesActiveMapScope(partyRequest.data(), partyRequest.size()));
    EXPECT(!packetUsesActiveMapScope(partyResult.data(), partyResult.size()));
    EXPECT(!packetUsesActiveMapScope(partyState.data(), partyState.size()));
    EXPECT(!packetUsesActiveMapScope(partyInvites.data(), partyInvites.size()));
    EXPECT(!packetUsesActiveMapScope(safeParty.data(), safeParty.size()));
    EXPECT(!packetUsesActiveMapScope(nullptr, 0));
    return true;
}

static bool testLateJoinSnapshotTransaction()
{
    LateJoinSnapshotTransaction transaction;
    EXPECT(transaction.mayReceiveLiveSimulation());
    EXPECT(transaction.holdForClient());
    EXPECT(!transaction.mayReceiveLiveSimulation());
    transaction.reset();
    EXPECT(!transaction.begin(0, 7, 2, 8));
    EXPECT(transaction.phase() == LateJoinSnapshotTransaction::Phase::Failed);
    EXPECT(transaction.begin(11, 7, 3, 9));
    EXPECT(!transaction.mayReceiveLiveSimulation());
    EXPECT(transaction.acceptChunk(11, 7, 2, 3)
        == LateJoinChunkResult::Accepted);
    EXPECT(transaction.acceptChunk(11, 7, 2, 3)
        == LateJoinChunkResult::Duplicate);
    EXPECT(transaction.acceptChunk(12, 7, 0, 3)
        == LateJoinChunkResult::Rejected);
    EXPECT(transaction.acceptChunk(11, 7, 0, 3)
        == LateJoinChunkResult::Accepted);
    EXPECT(transaction.acceptChunk(11, 7, 1, 3)
        == LateJoinChunkResult::Complete);
    EXPECT(!transaction.mayReceiveLiveSimulation());
    EXPECT(transaction.authorize());
    EXPECT(transaction.mayReceiveLiveSimulation());

    EXPECT(transaction.begin(12, 8, 2, 5));
    EXPECT(transaction.acceptChunk(12, 8, 0, 2)
        == LateJoinChunkResult::Accepted);
    EXPECT(transaction.acceptChunk(12, 8, 1, 2)
        == LateJoinChunkResult::Rejected);
    EXPECT(transaction.phase() == LateJoinSnapshotTransaction::Phase::Failed);
    EXPECT(!transaction.authorize());

    EXPECT(transaction.begin(13, 9, 2, 4));
    EXPECT(transaction.acceptChunk(13, 9, 0, 2, 100)
        == LateJoinChunkResult::Accepted);
    EXPECT(transaction.acceptChunk(13, 9, 0, 2, 101)
        == LateJoinChunkResult::Rejected);
    EXPECT(transaction.phase() == LateJoinSnapshotTransaction::Phase::Failed);

    EXPECT(transaction.begin(14, 10, 1, 1));
    EXPECT(transaction.acceptChunk(14, 11, 0, 1)
        == LateJoinChunkResult::Rejected);
    EXPECT(transaction.phase() == LateJoinSnapshotTransaction::Phase::Receiving);
    EXPECT(transaction.acceptChunk(14, 10, 0, 1)
        == LateJoinChunkResult::Complete);
    EXPECT(transaction.instanceRevision() == 10);

    EXPECT(!transaction.begin(
        15,
        10,
        LateJoinSnapshotTransaction::maxChunks + 1,
        LateJoinSnapshotTransaction::maxChunks + 1));
    EXPECT(!transaction.begin(
        16,
        10,
        1,
        LateJoinSnapshotTransaction::maxSnapshotBytes + 1));
    return true;
}

static bool testAuthoritativePlayerUidCollision()
{
    EXPECT(authoritativePlayerUpdateConflicts(true, 2, 16, false, -1));
    EXPECT(authoritativePlayerUpdateConflicts(true, 2, 16, true, 1));
    EXPECT(!authoritativePlayerUpdateConflicts(true, 2, 16, true, 2));
    EXPECT(!authoritativePlayerUpdateConflicts(false, 2, 16, false, -1));
    EXPECT(!authoritativePlayerUpdateConflicts(true, -1, 16, false, -1));
    EXPECT(!authoritativePlayerUpdateConflicts(true, 16, 16, false, -1));
    EXPECT(authoritativePlayerCanAdoptSlotHead(
        true, 2, 16, true, true, true, 2));
    EXPECT(!authoritativePlayerCanAdoptSlotHead(
        true, 2, 16, true, false, true, 2));
    EXPECT(!authoritativePlayerCanAdoptSlotHead(
        true, 2, 16, true, true, true, 1));
    EXPECT(!authoritativePlayerCanAdoptSlotHead(
        false, 2, 16, true, true, true, 2));
    EXPECT(playerLimbMatchesCurrentSlotHead(
        2, 16, 55, true, true, 2, 55));
    EXPECT(!playerLimbMatchesCurrentSlotHead(
        2, 16, 54, true, true, 2, 55));
    EXPECT(!playerLimbMatchesCurrentSlotHead(
        2, 16, 55, true, true, 1, 55));
    EXPECT(scopedReliablePacketStillMatches(
        "test.lmp#world", 4, "test.lmp#world", 4));
    EXPECT(!scopedReliablePacketStillMatches(
        "test.lmp#world", 4, "start.lmp#world", 4));
    EXPECT(!scopedReliablePacketStillMatches(
        "test.lmp#world", 4, "test.lmp#world", 5));
    EXPECT(!scopedReliablePacketStillMatches(
        "", 4, "test.lmp#world", 4));
    EXPECT(scopedReliablePacketCanRetry(
        "test.lmp#world", 4, "test.lmp#world", 4, true));
    EXPECT(!scopedReliablePacketCanRetry(
        "test.lmp#world", 4, "test.lmp#world", 4, false));
    EXPECT(!removedEntityTombstoneAppliesToInstance(
        false, "", 0, "test.lmp#world", 4));
    EXPECT(removedEntityTombstoneAppliesToInstance(
        false, "", 0, "", 0));
    EXPECT(removedEntityTombstoneAppliesToInstance(
        true, "test.lmp#world", 4, "test.lmp#world", 4));
    EXPECT(!removedEntityTombstoneAppliesToInstance(
        true, "start.lmp#world", 4, "test.lmp#world", 4));
    EXPECT(!removedEntityTombstoneAppliesToInstance(
        true, "test.lmp#world", 3, "test.lmp#world", 4));
    EXPECT(bodypartIdPacketLength(29, false) == 120);
    EXPECT(bodypartIdPacketLength(29, true) == 116);
    EXPECT(bodypartIdPacketLength(0, false) == 8);
    EXPECT(bodypartIdPacketIsComplete(120, 29, false));
    EXPECT(!bodypartIdPacketIsComplete(119, 29, false));
    return true;
}

static bool testLateJoinWireProtocol()
{
    using namespace LateJoinProtocol;

    Begin begin;
    begin.transferId = 0x10203040U;
    begin.instanceRevision = 0x0102030405060708ULL;
    begin.chunkCount = 2;
    begin.totalBytes = 5;
    begin.snapshotChecksum = 0xfeedbeefU;
    begin.flags = 3;
    begin.sessionKey = 0x55667788U;
    const auto beginPacket = encodeBegin(begin);
    Begin decodedBegin;
    EXPECT(decodeBegin(beginPacket.data(), beginPacket.size(), decodedBegin));
    EXPECT(decodedBegin.transferId == begin.transferId);
    EXPECT(decodedBegin.instanceRevision == begin.instanceRevision);
    EXPECT(decodedBegin.chunkCount == begin.chunkCount);
    EXPECT(decodedBegin.totalBytes == begin.totalBytes);
    EXPECT(decodedBegin.snapshotChecksum == begin.snapshotChecksum);
    EXPECT(decodedBegin.flags == begin.flags);
    EXPECT(decodedBegin.sessionKey == begin.sessionKey);
    auto badBegin = beginPacket;
    badBegin[4] = 0xff;
    EXPECT(!decodeBegin(badBegin.data(), badBegin.size(), decodedBegin));
    EXPECT(!decodeBegin(beginPacket.data(), beginPacket.size() - 1, decodedBegin));
    badBegin = beginPacket;
    write16(badBegin, 6, static_cast<std::uint16_t>(beginPacketSize - 1));
    EXPECT(!decodeBegin(badBegin.data(), badBegin.size(), decodedBegin));

    Chunk chunk;
    chunk.transferId = begin.transferId;
    chunk.instanceRevision = begin.instanceRevision;
    chunk.sequence = 1;
    chunk.payload = {1, 2, 3, 4, 5};
    const auto chunkPacket = encodeChunk(chunk);
    Chunk decodedChunk;
    EXPECT(decodeChunk(chunkPacket.data(), chunkPacket.size(), decodedChunk));
    EXPECT(decodedChunk.transferId == chunk.transferId);
    EXPECT(decodedChunk.instanceRevision == chunk.instanceRevision);
    EXPECT(decodedChunk.sequence == chunk.sequence);
    EXPECT(decodedChunk.payload == chunk.payload);
    auto corruptChunk = chunkPacket;
    corruptChunk.back() ^= 0xff;
    EXPECT(!decodeChunk(corruptChunk.data(), corruptChunk.size(), decodedChunk));
    EXPECT(!decodeChunk(chunkPacket.data(), chunkPacket.size() - 1, decodedChunk));
    chunk.payload.assign(maxChunkPayload + 1, 1);
    EXPECT(encodeChunk(chunk).empty());

    Complete complete;
    complete.transferId = begin.transferId;
    complete.instanceRevision = begin.instanceRevision;
    complete.chunkCount = begin.chunkCount;
    complete.totalBytes = begin.totalBytes;
    complete.snapshotChecksum = begin.snapshotChecksum;
    const auto completePacket = encodeComplete(complete);
    Complete decodedComplete;
    EXPECT(decodeComplete(completePacket.data(), completePacket.size(), decodedComplete));
    EXPECT(decodedComplete.snapshotChecksum == complete.snapshotChecksum);
    EXPECT(!decodeComplete(
        completePacket.data(), completePacket.size() - 1, decodedComplete));

    Authorization authorization;
    authorization.transferId = begin.transferId;
    authorization.instanceRevision = begin.instanceRevision;
    authorization.spawnAuthorized = true;
    const auto authorizationPacket = encodeAuthorization(authorization);
    Authorization decodedAuthorization;
    EXPECT(decodeAuthorization(authorizationPacket.data(), authorizationPacket.size(),
        decodedAuthorization));
    EXPECT(decodedAuthorization.transferId == authorization.transferId);
    EXPECT(decodedAuthorization.instanceRevision == authorization.instanceRevision);
    EXPECT(decodedAuthorization.spawnAuthorized);
    auto badAuthorization = authorizationPacket;
    badAuthorization[16] = 2;
    EXPECT(!decodeAuthorization(badAuthorization.data(), badAuthorization.size(),
        decodedAuthorization));
    EXPECT(!decodeAuthorization(
        authorizationPacket.data(), authorizationPacket.size() - 1,
        decodedAuthorization));

    Ready ready;
    ready.playerIndex = 3;
    ready.transferId = begin.transferId;
    ready.instanceRevision = begin.instanceRevision;
    ready.snapshotAccepted = true;
    const auto readyPacket = encodeReady(ready);
    Ready decodedReady;
    EXPECT(decodeReady(readyPacket.data(), readyPacket.size(), decodedReady));
    EXPECT(decodedReady.playerIndex == ready.playerIndex);
    EXPECT(decodedReady.transferId == ready.transferId);
    EXPECT(decodedReady.instanceRevision == ready.instanceRevision);
    EXPECT(decodedReady.snapshotAccepted);
    const auto goPacket = encodeGo(ready);
    Ready decodedGo;
    EXPECT(decodeGo(goPacket.data(), goPacket.size(), decodedGo));
    EXPECT(decodedGo.playerIndex == ready.playerIndex);
    EXPECT(decodedGo.transferId == ready.transferId);
    EXPECT(!decodeReady(readyPacket.data(), readyPacket.size() - 1, decodedReady));
    auto malformedReady = readyPacket;
    malformedReady[17] = 2;
    EXPECT(!decodeReady(malformedReady.data(), malformedReady.size(), decodedReady));
    auto malformedGo = goPacket;
    malformedGo[9] ^= 1;
    EXPECT(decodeGo(malformedGo.data(), malformedGo.size(), decodedGo));
    EXPECT(decodedGo.instanceRevision != ready.instanceRevision);
    ready.playerIndex = 16;
    EXPECT(encodeReady(ready).empty());

    Abort abort;
    abort.playerIndex = 3;
    abort.transferId = begin.transferId;
    abort.instanceRevision = begin.instanceRevision;
    abort.reason = 2;
    const auto abortPacket = encodeAbort(abort);
    Abort decodedAbort;
    EXPECT(decodeAbort(abortPacket.data(), abortPacket.size(), decodedAbort));
    EXPECT(decodedAbort.playerIndex == abort.playerIndex);
    EXPECT(decodedAbort.transferId == abort.transferId);
    EXPECT(decodedAbort.instanceRevision == abort.instanceRevision);
    EXPECT(decodedAbort.reason == abort.reason);
    auto malformedAbort = abortPacket;
    malformedAbort[17] = 0;
    EXPECT(!decodeAbort(malformedAbort.data(), malformedAbort.size(), decodedAbort));
    EXPECT(!decodeAbort(abortPacket.data(), abortPacket.size() - 1, decodedAbort));
    abort.transferId = 0;
    const auto preTransferAbort = encodeAbort(abort);
    EXPECT(decodeAbort(preTransferAbort.data(), preTransferAbort.size(), decodedAbort));
    EXPECT(decodedAbort.transferId == 0);

    Chunk catchupChunk = chunk;
    catchupChunk.payload = {9, 8, 7};
    const auto catchupBeginPacket = encodeCatchupBegin(complete);
    const auto catchupChunkPacket = encodeCatchupChunk(catchupChunk);
    const auto catchupCompletePacket = encodeCatchupComplete(complete);
    Complete decodedCatchupBegin;
    Chunk decodedCatchupChunk;
    Complete decodedCatchupComplete;
    EXPECT(decodeCatchupBegin(catchupBeginPacket.data(),
        catchupBeginPacket.size(), decodedCatchupBegin));
    EXPECT(decodeCatchupChunk(catchupChunkPacket.data(),
        catchupChunkPacket.size(), decodedCatchupChunk));
    EXPECT(decodeCatchupComplete(catchupCompletePacket.data(),
        catchupCompletePacket.size(), decodedCatchupComplete));
    EXPECT(decodedCatchupChunk.payload == catchupChunk.payload);
    auto corruptCatchupChunk = catchupChunkPacket;
    corruptCatchupChunk.back() ^= 1;
    EXPECT(!decodeCatchupChunk(corruptCatchupChunk.data(),
        corruptCatchupChunk.size(), decodedCatchupChunk));

    const std::array<std::uint8_t, 9> crcSample =
        {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT(crc32(crcSample.data(), crcSample.size()) == 0xcbf43926U);

    const std::vector<std::uint8_t> snapshot = {10, 20, 30, 40, 50};
    Begin assemblyBegin;
    assemblyBegin.transferId = 77;
    assemblyBegin.instanceRevision = 12;
    assemblyBegin.chunkCount = 2;
    assemblyBegin.totalBytes = static_cast<std::uint32_t>(snapshot.size());
    assemblyBegin.snapshotChecksum = crc32(snapshot.data(), snapshot.size());
    SnapshotAssembler assembler;
    EXPECT(assembler.begin(assemblyBegin));
    Chunk second;
    second.transferId = 77;
    second.instanceRevision = 12;
    second.sequence = 1;
    second.payload = {40, 50};
    Chunk first = second;
    first.sequence = 0;
    first.payload = {10, 20, 30};
    EXPECT(assembler.accept(second) == ReceiveResult::Accepted);
    EXPECT(assembler.accept(second) == ReceiveResult::Duplicate);
    EXPECT(assembler.accept(first) == ReceiveResult::Accepted);
    Complete assemblyComplete;
    assemblyComplete.transferId = assemblyBegin.transferId;
    assemblyComplete.instanceRevision = assemblyBegin.instanceRevision;
    assemblyComplete.chunkCount = assemblyBegin.chunkCount;
    assemblyComplete.totalBytes = assemblyBegin.totalBytes;
    assemblyComplete.snapshotChecksum = assemblyBegin.snapshotChecksum;
    EXPECT(assembler.finish(assemblyComplete) == ReceiveResult::Complete);
    EXPECT(assembler.complete());
    EXPECT(assembler.snapshot() == snapshot);

    EXPECT(assembler.begin(assemblyBegin));
    Chunk wrongRevision = first;
    ++wrongRevision.instanceRevision;
    EXPECT(assembler.accept(wrongRevision) == ReceiveResult::Rejected);
    EXPECT(assembler.receiving());
    EXPECT(assembler.accept(first) == ReceiveResult::Accepted);
    EXPECT(assembler.accept(second) == ReceiveResult::Accepted);
    Complete wrongCompleteRevision = assemblyComplete;
    ++wrongCompleteRevision.instanceRevision;
    EXPECT(assembler.finish(wrongCompleteRevision) == ReceiveResult::Rejected);
    EXPECT(assembler.failed());

    EXPECT(assembler.begin(assemblyBegin));
    EXPECT(assembler.accept(first) == ReceiveResult::Accepted);
    EXPECT(assembler.accept(second) == ReceiveResult::Accepted);
    Complete badWholeCrc = assemblyComplete;
    badWholeCrc.snapshotChecksum ^= 1;
    EXPECT(assembler.finish(badWholeCrc) == ReceiveResult::Rejected);
    EXPECT(assembler.failed());

    Begin incorrectSnapshotCrc = assemblyBegin;
    incorrectSnapshotCrc.snapshotChecksum ^= 1;
    EXPECT(assembler.begin(incorrectSnapshotCrc));
    EXPECT(assembler.accept(first) == ReceiveResult::Accepted);
    EXPECT(assembler.accept(second) == ReceiveResult::Accepted);
    Complete matchingIncorrectCrc = assemblyComplete;
    matchingIncorrectCrc.snapshotChecksum =
        incorrectSnapshotCrc.snapshotChecksum;
    EXPECT(assembler.finish(matchingIncorrectCrc) == ReceiveResult::Rejected);
    EXPECT(assembler.failed());

    EXPECT(assembler.begin(assemblyBegin));
    EXPECT(assembler.accept(second) == ReceiveResult::Accepted);
    second.payload = {41, 50};
    EXPECT(assembler.accept(second) == ReceiveResult::Rejected);
    EXPECT(assembler.failed());

    Begin oversized = assemblyBegin;
    oversized.totalBytes = maxSnapshotBytes + 1;
    EXPECT(!assembler.begin(oversized));
    Begin tooManyChunks = assemblyBegin;
    tooManyChunks.chunkCount = maxChunkCount + 1;
    EXPECT(!assembler.begin(tooManyChunks));
    Begin impossibleCapacity = assemblyBegin;
    impossibleCapacity.chunkCount = 1;
    impossibleCapacity.totalBytes = maxChunkPayload + 1;
    EXPECT(!assembler.begin(impossibleCapacity));
    return true;
}

static bool testLateJoinPacketCatchupBuffer()
{
    LateJoinPacketCatchupBuffer buffer;
    const std::array<std::uint8_t, 6> first = {'E', 'N', 'T', 'U', 1, 2};
    const std::array<std::uint8_t, 5> second = {'M', 'A', 'P', 'T', 3};
    EXPECT(buffer.append(first.data(), first.size()));
    EXPECT(buffer.append(second.data(), second.size()));
    EXPECT(buffer.packetCount() == 2);
    const auto serialized = buffer.serialize();
    std::vector<std::vector<std::uint8_t>> packets;
    EXPECT(LateJoinPacketCatchupBuffer::deserialize(serialized, packets));
    EXPECT(packets.size() == 2);
    EXPECT(std::equal(packets[0].begin(), packets[0].end(), first.begin()));
    EXPECT(std::equal(packets[1].begin(), packets[1].end(), second.begin()));

    auto truncated = serialized;
    truncated.pop_back();
    EXPECT(!LateJoinPacketCatchupBuffer::deserialize(truncated, packets));
    auto trailing = serialized;
    trailing.push_back(0);
    EXPECT(!LateJoinPacketCatchupBuffer::deserialize(trailing, packets));
    auto invalidCount = serialized;
    invalidCount[0] = 0xff;
    EXPECT(!LateJoinPacketCatchupBuffer::deserialize(invalidCount, packets));
    const std::array<std::uint8_t, 3> tooShort = {'B', 'A', 'D'};
    EXPECT(!buffer.append(tooShort.data(), tooShort.size()));
    EXPECT(buffer.failed());
    EXPECT(buffer.serialize().empty());
    buffer.reset();
    EXPECT(buffer.empty());
    EXPECT(!buffer.failed());
    EXPECT(LateJoinPacketCatchupBuffer::deserialize(buffer.serialize(), packets));
    EXPECT(packets.empty());
    return true;
}

static bool testReconnectTokenValidation()
{
	const std::string valid = "0123456789abcdef0123456789abcdef";
	EXPECT(ReconnectToken::isValid(valid));
	EXPECT(!ReconnectToken::isValid(valid.substr(1)));
	EXPECT(!ReconnectToken::isValid(
		"0123456789ABCDEF0123456789ABCDEF"));
	EXPECT(ReconnectToken::equals(
		valid, reinterpret_cast<const std::uint8_t*>(valid.data())));
	std::string different = valid;
	different.back() = '0';
	EXPECT(!ReconnectToken::equals(
		valid, reinterpret_cast<const std::uint8_t*>(different.data())));
	EXPECT(!ReconnectToken::equals(valid, nullptr));
	return true;
}

static bool testAtomicWorldSave()
{
    using AutomatiaSave::Json;

    Json document = AutomatiaSave::makeEmptyWorldSave("foundation-session");
    document["future_field"] = Json{{"preserve_me", true}};
    document["unknown_custom_items"].push_back(Json{
        {"stable_id", "missing_mod:mystery_item"},
        {"source", "missing_mod"},
        {"opaque_payload", Json{{"quality", 42}}}
    });
    document["map_instances"].push_back(Json{
        {"map_file", "start.lmp"},
        {"instance_id", "world"},
        {"revision", 3},
        {"loaded", true},
        {"playable_floors", Json::array({0})},
		{"players_present", Json::array({0})},
        {"persistent_state", Json{
			{"followers", Json::array({Json{{"owner", 0}, {"uid", 41}}})},
			{"dropped_items", Json::array({Json{{"stable_id", "core:rock"}}})},
			{"chests", Json::array({Json{{"persistent_id", 7}}})},
			{"shops", Json::array({Json{{"persistent_id", 8}}})},
			{"mechanisms", Json::array({Json{{"persistent_id", 9}}})}
		}}
    });
	document["map_instances"].push_back(Json{
		{"map_file", "mines01.lmp"},
		{"instance_id", "world"},
		{"revision", 4},
		{"loaded", false},
        {"playable_floors", Json::array({0})},
		{"players_present", Json::array({1})},
		{"persistent_state", Json{{"dropped_items", Json::array()}}}
	});
    document["active_instance"] = "start.lmp#world";
    document["players"].push_back(Json{
        {"player_id", "test-player"},
        {"map_file", "start.lmp"},
        {"instance_id", "world"},
        {"revision", 3},
        {"playable_floor", 0},
        {"inventory", Json::array()}
    });
	document["players"].push_back(Json{
		{"player_id", "second-player"},
		{"map_file", "mines01.lmp"},
		{"instance_id", "world"},
		{"revision", 4},
        {"playable_floor", 0},
		{"inventory", Json::array()}
	});
	document["quests"] = Json{{"foundation_quest", Json{{"stage", 2}}}};
	document["dialogue"] = Json{{"npc:7", Json{{"current_node", 3}}}};

    EXPECT(AutomatiaSave::validate(document).ok);
    EXPECT(document["schema_version"] == 3);
    EXPECT(document["party"]["next_id"] == 1);

    Json versionTwo = document;
    versionTwo["schema_version"] = 2;
    for (Json& instance : versionTwo["map_instances"])
    {
        instance.erase("playable_floors");
    }
    for (Json& player : versionTwo["players"])
    {
        player.erase("playable_floor");
    }
    EXPECT(AutomatiaSave::validate(versionTwo).ok);

    Json versionOne = versionTwo;
    versionOne["schema_version"] = 1;
    versionOne.erase("party");
    EXPECT(AutomatiaSave::validate(versionOne).ok);
    Json missingParty = document;
    missingParty.erase("party");
    EXPECT(!AutomatiaSave::validate(missingParty));
    Json invalidFloors = document;
    invalidFloors["map_instances"][0]["playable_floors"] = Json::array({2});
    EXPECT(!AutomatiaSave::validate(invalidFloors));
    invalidFloors = document;
    invalidFloors["map_instances"][0]["playable_floors"] = Json::array({0, 0});
    EXPECT(!AutomatiaSave::validate(invalidFloors));
    invalidFloors = document;
    invalidFloors["players"][0]["playable_floor"] = 2;
    EXPECT(!AutomatiaSave::validate(invalidFloors));

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("barony-automatia-test-" + std::to_string(unique));
    const std::filesystem::path savePath = directory / "world.json";

    EXPECT(AutomatiaSave::writeAtomic(savePath, document).ok);
    EXPECT(std::filesystem::exists(savePath));
    EXPECT(!std::filesystem::exists(savePath.string() + ".tmp"));

    Json loaded;
    EXPECT(AutomatiaSave::load(savePath, loaded).ok);
    EXPECT(loaded["future_field"] == document["future_field"]);
    EXPECT(loaded["unknown_custom_items"] == document["unknown_custom_items"]);
    loaded["unknown_custom_items"][0]["opaque_payload"]["nested"] =
        Json::array({"future", 7, Json{{"enabled", true}}});
    EXPECT(AutomatiaSave::writeAtomic(savePath, loaded).ok);
    Json roundTripped;
    EXPECT(AutomatiaSave::load(savePath, roundTripped).ok);
    EXPECT(roundTripped == loaded);
	EXPECT(roundTripped["map_instances"][0]["persistent_state"]["followers"].size()
		== 1);
	EXPECT(roundTripped["map_instances"][0]["persistent_state"]["chests"].size()
		== 1);
	EXPECT(roundTripped["map_instances"][0]["persistent_state"]["shops"].size()
		== 1);
	EXPECT(roundTripped["map_instances"][0]["persistent_state"]["mechanisms"].size()
		== 1);
	EXPECT(roundTripped["quests"]["foundation_quest"]["stage"] == 2);
	EXPECT(roundTripped["dialogue"]["npc:7"]["current_node"] == 3);
    EXPECT(roundTripped["unknown_custom_items"][0]["opaque_payload"]["nested"]
        == loaded["unknown_custom_items"][0]["opaque_payload"]["nested"]);

    Json invalid = document;
    invalid["map_instances"][0]["map_file"] = "../escape.lmp";
    EXPECT(!AutomatiaSave::validate(invalid));
    invalid = document;
    invalid["schema_version"] = AutomatiaSave::CURRENT_SCHEMA_VERSION + 1;
    EXPECT(!AutomatiaSave::validate(invalid));
    invalid = document;
    invalid["active_instance"] = "missing.lmp#world";
    EXPECT(!AutomatiaSave::validate(invalid));
    invalid = document;
    invalid["map_instances"].push_back(invalid["map_instances"][0]);
    EXPECT(!AutomatiaSave::validate(invalid));
    invalid = document;
    invalid["map_instances"][0]["map_seed"] = -1;
    EXPECT(!AutomatiaSave::validate(invalid));
    invalid = document;
    invalid["map_instances"][0]["dirty"] = "yes";
    EXPECT(!AutomatiaSave::validate(invalid));
    invalid = document;
    invalid["map_instances"][0]["players_present"] = Json::array({1, 1});
    EXPECT(!AutomatiaSave::validate(invalid));
    invalid = document;
    invalid["players"].push_back(invalid["players"][0]);
    EXPECT(!AutomatiaSave::validate(invalid));
    invalid = document;
    invalid["players"][0]["instance_id"] = "missing";
    EXPECT(!AutomatiaSave::validate(invalid));
    invalid = document;
    invalid["map_instances"][0]["players_present"] = Json::array({256});
    EXPECT(!AutomatiaSave::validate(invalid));
    invalid = document;
    invalid["players"][0]["position"] = Json::array({
        1.0,
        std::numeric_limits<double>::infinity(),
        2.0
    });
    EXPECT(!AutomatiaSave::validate(invalid));
    invalid = document;
    invalid["unknown_custom_items"][0]["stable_id"] = "";
    EXPECT(!AutomatiaSave::validate(invalid));

    invalid = document;
    invalid["schema_version"] = AutomatiaSave::CURRENT_SCHEMA_VERSION + 1;
    EXPECT(!AutomatiaSave::writeAtomic(savePath, invalid));
    EXPECT(AutomatiaSave::load(savePath, loaded).ok);
    EXPECT(loaded["future_field"] == document["future_field"]);

    {
        std::ofstream corrupt(savePath, std::ios::binary | std::ios::trunc);
        corrupt << "{not-json";
    }
    EXPECT(!AutomatiaSave::load(savePath, loaded));

    const std::filesystem::path oversizedPath = directory / "oversized.json";
    {
        std::ofstream oversizedFile(
            oversizedPath, std::ios::binary | std::ios::trunc);
        oversizedFile.seekp(
            static_cast<std::streamoff>(AutomatiaSave::MAX_SAVE_BYTES));
        oversizedFile.put('x');
    }
    EXPECT(!AutomatiaSave::load(oversizedPath, loaded));

    std::error_code cleanupError;
    std::filesystem::remove(savePath, cleanupError);
    std::filesystem::remove(oversizedPath, cleanupError);
    std::filesystem::remove(directory, cleanupError);
    return true;
}

int main()
{
    return testWorldInstanceIdentity()
		&& testDedicatedLanDiscovery()
        && testPacketScope()
        && testAuthoritativePlayerUidCollision()
        && testLateJoinSnapshotTransaction()
        && testLateJoinWireProtocol()
        && testLateJoinPacketCatchupBuffer()
        && testReconnectTokenValidation()
        && testAtomicWorldSave()
        ? 0
        : 1;
}
