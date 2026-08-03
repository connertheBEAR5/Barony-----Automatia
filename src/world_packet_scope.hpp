/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: world_packet_scope.hpp
    Desc: Packet-scope classification shared by divergent network boundaries.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

inline bool authoritativePlayerUpdateConflicts(
    bool incomingPlayerHead,
    int incomingPlayer,
    int maximumPlayers,
    bool existingPlayerHead,
    int existingPlayer)
{
    return incomingPlayerHead
        && incomingPlayer >= 0
        && incomingPlayer < maximumPlayers
        && (!existingPlayerHead || existingPlayer != incomingPlayer);
}

inline bool authoritativePlayerCanAdoptSlotHead(
    bool incomingPlayerHead,
    int incomingPlayer,
    int maximumPlayers,
    bool slotHeadExists,
    bool slotHeadIsInActiveMap,
    bool slotHeadIsPlayer,
    int slotHeadPlayer)
{
    return incomingPlayerHead
        && incomingPlayer >= 0
        && incomingPlayer < maximumPlayers
        && slotHeadExists
        && slotHeadIsInActiveMap
        && slotHeadIsPlayer
        && slotHeadPlayer == incomingPlayer;
}

inline bool playerLimbMatchesCurrentSlotHead(
    int limbPlayer,
    int maximumPlayers,
    std::uint32_t limbParentUid,
    bool slotHeadExists,
    bool slotHeadIsPlayer,
    int slotHeadPlayer,
    std::uint32_t slotHeadUid)
{
    return limbPlayer >= 0
        && limbPlayer < maximumPlayers
        && slotHeadExists
        && slotHeadIsPlayer
        && slotHeadPlayer == limbPlayer
        && limbParentUid == slotHeadUid;
}

inline bool scopedReliablePacketStillMatches(
    const char* queuedInstanceKey,
    std::uint64_t queuedRevision,
    const char* recipientInstanceKey,
    std::uint64_t recipientRevision)
{
    return queuedInstanceKey
        && queuedInstanceKey[0] != '\0'
        && recipientInstanceKey
        && std::strcmp(queuedInstanceKey, recipientInstanceKey) == 0
        && queuedRevision == recipientRevision;
}

inline bool scopedReliablePacketCanRetry(
    const char* queuedInstanceKey,
    std::uint64_t queuedRevision,
    const char* recipientInstanceKey,
    std::uint64_t recipientRevision,
    bool recipientMayReceiveLiveSimulation)
{
    return recipientMayReceiveLiveSimulation
        && scopedReliablePacketStillMatches(
            queuedInstanceKey,
            queuedRevision,
            recipientInstanceKey,
            recipientRevision);
}

inline bool removedEntityTombstoneAppliesToInstance(
    bool tombstoneHasInstanceScope,
    const char* tombstoneInstanceKey,
    std::uint64_t tombstoneRevision,
    const char* activeInstanceKey,
    std::uint64_t activeRevision)
{
    // Retain the legacy behavior only until WorldState binds an instance.
    if (!tombstoneHasInstanceScope)
    {
        return !activeInstanceKey || activeInstanceKey[0] == '\0';
    }
    return scopedReliablePacketStillMatches(
        tombstoneInstanceKey,
        tombstoneRevision,
        activeInstanceKey,
        activeRevision);
}

inline std::size_t bodypartIdPacketLength(
    std::size_t childCount,
    bool monsterBodyparts)
{
    const std::size_t skippedChildren = monsterBodyparts ? 2 : 1;
    const std::size_t transmittedChildren = childCount > skippedChildren
        ? childCount - skippedChildren
        : 0;
    return 8 + transmittedChildren * sizeof(std::uint32_t);
}

inline bool bodypartIdPacketIsComplete(
    std::size_t packetLength,
    std::size_t childCount,
    bool monsterBodyparts)
{
    return packetLength >= bodypartIdPacketLength(
        childCount, monsterBodyparts);
}

inline bool packetUsesActiveMapScope(const std::uint8_t* data, std::size_t length)
{
    if (!data || length < 4)
    {
        return false;
    }

    // Reliable direct-connect packets wrap the original payload after the
    // SAFE header, sender index, and sequence number.
    if (length >= 13 && std::memcmp(data, "SAFE", 4) == 0)
    {
        data += 9;
        length -= 9;
    }
    if (length < 4)
    {
        return false;
    }

    static constexpr char mapPackets[][4] = {
        {'E', 'N', 'T', 'U'}, {'B', 'D', 'Y', 'I'},
        {'E', 'N', 'T', 'B'}, {'E', 'N', 'T', 'A'},
        {'E', 'N', 'T', 'S'}, {'E', 'N', 'S', 'F'},
        {'E', 'N', 'F', 'S'}, {'E', 'N', 'T', 'F'},
        {'E', 'N', 'T', 'D'}, {'E', 'N', 'T', 'E'},
		{'E', 'N', 'H', 'P'},
        {'N', 'O', 'U', 'P'}, {'S', 'P', 'P', 'E'},
        {'S', 'P', 'P', 'L'}, {'M', 'A', 'P', 'T'},
		{'E', 'X', 'P', 'L'}, {'E', 'X', 'P', 'S'},
		{'B', 'A', 'N', 'G'}, {'S', 'P', 'G', 'B'},
		{'S', 'L', 'E', 'Z'}, {'S', 'L', 'E', 'M'},
		{'M', 'A', 'G', 'E'},
		{'E', 'N', 'S', 'M'},
        {'A', 'D', 'O', 'R'}, {'A', 'L', 'I', 'T'},
        {'A', 'L', 'L', 'Y'},
        {'A', 'S', 'S', 'O'}, {'A', 'T', 'T', 'I'},
        {'A', 'P', 'I', 'T'}, {'A', 'P', 'I', 'W'},
        {'A', 'T', 'A', 'K'}, {'B', 'E', 'A', 'T'},
        {'B', 'D', 'T', 'H'}, {'B', 'E', 'L', 'I'},
        {'B', 'O', 'O', 'M'}, {'B', 'R', 'E', 'K'},
        {'C', 'A', 'L', 'L'}, {'C', 'A', 'U', 'O'},
        {'A', 'S', 'C', 'L'}, {'C', 'A', 'U', 'C'},
        {'C', 'D', 'S', 'L'},
        {'C', 'I', 'T', 'M'}, {'C', 'K', 'I', 'R'},
        {'C', 'H', 'S', 'T'}, {'C', 'K', 'O', 'R'},
        {'C', 'O', 'M', 'D'}, {'C', 'O', 'O', 'K'},
        {'D', 'A', 'E', 'D'}, {'D', 'C', 'K', 'A'},
        {'D', 'G', 'L', 'D'}, {'D', 'M', 'G', 'G'},
        {'D', 'I', 'E', 'I'}, {'D', 'R', 'O', 'P'},
        {'E', 'F', 'F', 'E'}, {'F', 'O', 'D', 'A'},
		{'I', 'T', 'E', 'M'}, {'G', 'O', 'L', 'D'},
        {'G', 'H', 'F', 'S'}, {'G', 'H', 'O', 'D'},
        {'G', 'H', 'O', 'I'}, {'G', 'H', 'O', 'S'},
        {'G', 'H', 'S', 'P'},
        {'G', 'M', 'O', 'V'},
        {'G', 'R', 'E', 'S'}, {'I', 'D', 'I', 'E'},
		{'G', 'B', 'R', 'K'},
        {'I', 'T', 'M', 'U'}, {'L', 'D', 'E', 'L'},
        {'L', 'J', 'R', 'D'},
        {'L', 'J', 'G', 'O'},
        {'L', 'E', 'A', 'D'}, {'L', 'E', 'A', 'F'},
        {'L', 'K', 'E', 'Y'},
        {'L', 'N', 'O', 'K'}, {'L', 'O', 'O', 'T'},
        {'M', 'A', 'G', 'B'}, {'M', 'B', 'X', 'C'},
        {'M', 'B', 'X', 'O'}, {'M', 'I', 'R', 'R'},
        {'N', 'P', 'C', 'I'}, {'N', 'P', 'C', 'U'},
        {'P', 'M', 'A', 'P'}, {'P', 'M', 'O', 'V'},
        {'P', 'R', 'O', 'J'}, {'R', 'A', 'T', 'F'},
        {'R', 'C', 'I', 'T'},
        {'R', 'C', 'U', 'R'}, {'R', 'E', 'P', 'A'},
        {'R', 'E', 'P', 'T'}, {'R', 'E', 'Z', 'Z'},
        {'S', 'A', 'L', 'V'},
        {'S', 'H', 'L', 'D'}, {'S', 'H', 'P', 'B'},
        {'S', 'H', 'P', 'C'}, {'S', 'H', 'P', 'S'},
		{'S', 'H', 'O', 'P'}, {'S', 'H', 'P', 'I'},
        {'S', 'I', 'G', 'N'},
        {'S', 'N', 'D', 'P'}, {'S', 'N', 'E', 'L'},
        {'S', 'N', 'E', 'K'}, {'S', 'P', 'E', 'L'},
        {'S', 'P', 'O', 'T'}, {'T', 'E', 'L', 'E'},
        {'T', 'E', 'L', 'M'}, {'T', 'N', 'S', 'P'},
		{'T', 'O', 'R', 'C'}, {'A', 'R', 'M', 'R'},
        {'U', 'D', 'I', 'E'},
        {'U', 'N', 'C', 'H'},
        {'U', 'S', 'E', 'I'}, {'V', 'A', 'M', 'P'},
		{'O', 'K', 'E', 'Y'},
        {'W', 'A', 'C', 'D'}, {'W', 'A', 'L', 'C'},
        {'W', 'A', 'L', 'D'}, {'W', 'R', 'K', 'C'},
        {'W', 'R', 'K', 'O'},
		{'S', 'U', 'M', 'M'}, {'S', 'U', 'M', 'S'},
		{'B', 'L', 'E', 'S'}, {'B', 'L', 'E', '1'},
		{'C', 'H', 'A', 'N'}, {'M', 'F', 'O', 'D'},
		{'T', 'K', 'I', 'T'}, {'M', 'I', 'D', 'G'},
		{'D', 'A', 'S', 'H'}, {'I', 'T', 'E', 'Q'},
		{'S', 'C', 'R', 'U'}, {'E', 'Q', 'U', 'I'},
		{'E', 'Q', 'U', 'S'}, {'E', 'Q', 'U', 'M'},
		{'E', 'M', 'O', 'T'}
    };

    for (const auto& packet : mapPackets)
    {
        if (std::memcmp(data, packet, sizeof(packet)) == 0)
        {
            return true;
        }
    }
    return false;
}
