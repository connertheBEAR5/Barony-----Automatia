/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: party_protocol.cpp
    Desc: Bounded, endian-stable party request and recipient snapshot protocol.

-------------------------------------------------------------------------------*/

#include "party_protocol.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace AutomatiaParty::Protocol
{
namespace
{
constexpr std::size_t requestHeaderBytes = 31;
constexpr std::size_t resultBytes = 36;
constexpr std::size_t partyStateHeaderBytes = 32;
constexpr std::size_t invitationListHeaderBytes = 16;

bool validOperation(const RequestOperation operation)
{
    return operation >= RequestOperation::Create
        && operation <= RequestOperation::Disband;
}

bool validStatus(const OperationStatus status)
{
    return status >= OperationStatus::Success
        && status <= OperationStatus::IdSpaceExhausted;
}

bool appendIdentity(
    std::vector<std::uint8_t>& destination,
    const DurablePlayerIdentity& identity
)
{
    if (!identity.isValid()
        || identity.value.size() > MAX_DURABLE_IDENTITY_BYTES
        || identity.value.size() > 0xffU)
    {
        return false;
    }
    destination.push_back(static_cast<std::uint8_t>(identity.kind));
    destination.push_back(static_cast<std::uint8_t>(identity.value.size()));
    destination.insert(
        destination.end(), identity.value.begin(), identity.value.end());
    return destination.size() <= MAX_WIRE_PACKET_BYTES;
}

bool readIdentity(
    const std::uint8_t* data,
    const std::size_t size,
    std::size_t& offset,
    DurablePlayerIdentity& identity
)
{
    if (!data || offset + 2 > size)
    {
        return false;
    }
    identity.kind = static_cast<DurableIdentityKind>(data[offset]);
    const std::size_t length = data[offset + 1];
    offset += 2;
    if (length == 0 || length > MAX_DURABLE_IDENTITY_BYTES
        || length > size - offset)
    {
        return false;
    }
    identity.value.assign(
        reinterpret_cast<const char*>(data + offset), length);
    offset += length;
    return identity.isValid();
}

bool sameTag(
    const std::uint8_t* data,
    const std::size_t size,
    const char (&tag)[5]
)
{
    return data && size >= 4 && std::memcmp(data, tag, 4) == 0;
}
}

void write32(
    std::vector<std::uint8_t>& destination,
    const std::size_t offset,
    const std::uint32_t value
)
{
    if (offset + 4 > destination.size())
    {
        return;
    }
    destination[offset] = static_cast<std::uint8_t>(value >> 24U);
    destination[offset + 1] = static_cast<std::uint8_t>(value >> 16U);
    destination[offset + 2] = static_cast<std::uint8_t>(value >> 8U);
    destination[offset + 3] = static_cast<std::uint8_t>(value);
}

void write64(
    std::vector<std::uint8_t>& destination,
    const std::size_t offset,
    const std::uint64_t value
)
{
    if (offset + 8 > destination.size())
    {
        return;
    }
    for (std::size_t byte = 0; byte < 8; ++byte)
    {
        destination[offset + byte] = static_cast<std::uint8_t>(
            value >> ((7U - byte) * 8U));
    }
}

std::uint32_t read32(
    const std::uint8_t* source,
    const std::size_t offset
)
{
    return (static_cast<std::uint32_t>(source[offset]) << 24U)
        | (static_cast<std::uint32_t>(source[offset + 1]) << 16U)
        | (static_cast<std::uint32_t>(source[offset + 2]) << 8U)
        | static_cast<std::uint32_t>(source[offset + 3]);
}

std::uint64_t read64(
    const std::uint8_t* source,
    const std::size_t offset
)
{
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < 8; ++byte)
    {
        value = (value << 8U) | source[offset + byte];
    }
    return value;
}

std::vector<std::uint8_t> encodeRequest(const Request& request)
{
    if (!validOperation(request.operation)
        || request.actorSlot >= MAX_WIRE_PARTY_MEMBERS
        || (request.targetSlot != NO_PLAYER_SLOT
            && request.targetSlot >= MAX_WIRE_PARTY_MEMBERS))
    {
        return {};
    }
    std::vector<std::uint8_t> packet(requestHeaderBytes, 0);
    std::memcpy(packet.data(), "PTYQ", 4);
    packet[4] = VERSION;
    packet[5] = static_cast<std::uint8_t>(request.operation);
    packet[6] = request.actorSlot;
    write32(packet, 8, request.requestId);
    write64(packet, 12, request.claimedPartyId);
    write64(packet, 20, request.invitationId);
    packet[28] = request.targetSlot;
    if (request.hasTargetIdentity)
    {
        if (!request.targetIdentity.isValid())
        {
            return {};
        }
        packet[29] = static_cast<std::uint8_t>(request.targetIdentity.kind);
        packet[30] = static_cast<std::uint8_t>(
            request.targetIdentity.value.size());
        packet.insert(
            packet.end(),
            request.targetIdentity.value.begin(),
            request.targetIdentity.value.end());
    }
    return packet.size() <= MAX_WIRE_PACKET_BYTES
        ? packet : std::vector<std::uint8_t>{};
}

bool decodeRequest(
    const std::uint8_t* data,
    const std::size_t size,
    Request& request
)
{
    if (!sameTag(data, size, "PTYQ")
        || size < requestHeaderBytes || size > MAX_WIRE_PACKET_BYTES
        || data[4] != VERSION || data[7] != 0)
    {
        return false;
    }
    Request decoded;
    decoded.operation = static_cast<RequestOperation>(data[5]);
    decoded.actorSlot = data[6];
    decoded.requestId = read32(data, 8);
    decoded.claimedPartyId = read64(data, 12);
    decoded.invitationId = read64(data, 20);
    decoded.targetSlot = data[28];
    if (!validOperation(decoded.operation)
        || decoded.actorSlot >= MAX_WIRE_PARTY_MEMBERS
        || (decoded.targetSlot != NO_PLAYER_SLOT
            && decoded.targetSlot >= MAX_WIRE_PARTY_MEMBERS))
    {
        return false;
    }
    const std::uint8_t identityKind = data[29];
    const std::size_t identityLength = data[30];
    if (identityKind == 0 && identityLength == 0)
    {
        if (size != requestHeaderBytes)
        {
            return false;
        }
    }
    else
    {
        if (identityLength == 0
            || identityLength > MAX_DURABLE_IDENTITY_BYTES
            || size != requestHeaderBytes + identityLength)
        {
            return false;
        }
        decoded.targetIdentity.kind =
            static_cast<DurableIdentityKind>(identityKind);
        decoded.targetIdentity.value.assign(
            reinterpret_cast<const char*>(data + requestHeaderBytes),
            identityLength);
        if (!decoded.targetIdentity.isValid())
        {
            return false;
        }
        decoded.hasTargetIdentity = true;
    }
    request = std::move(decoded);
    return true;
}

std::vector<std::uint8_t> encodeResult(const Result& result)
{
    if (!validOperation(result.operation) || !validStatus(result.status))
    {
        return {};
    }
    std::vector<std::uint8_t> packet(resultBytes, 0);
    std::memcpy(packet.data(), "PTYR", 4);
    packet[4] = VERSION;
    packet[5] = static_cast<std::uint8_t>(result.status);
    packet[6] = static_cast<std::uint8_t>(result.operation);
    write32(packet, 8, result.requestId);
    write64(packet, 12, result.partyId);
    write64(packet, 20, result.invitationId);
    write64(packet, 28, result.revision);
    return packet;
}

bool decodeResult(
    const std::uint8_t* data,
    const std::size_t size,
    Result& result
)
{
    if (!sameTag(data, size, "PTYR") || size != resultBytes
        || data[4] != VERSION || data[7] != 0)
    {
        return false;
    }
    Result decoded;
    decoded.status = static_cast<OperationStatus>(data[5]);
    decoded.operation = static_cast<RequestOperation>(data[6]);
    decoded.requestId = read32(data, 8);
    decoded.partyId = read64(data, 12);
    decoded.invitationId = read64(data, 20);
    decoded.revision = read64(data, 28);
    if (!validStatus(decoded.status) || !validOperation(decoded.operation))
    {
        return false;
    }
    result = decoded;
    return true;
}

std::vector<std::uint8_t> encodePartyState(const PartyState& state)
{
    if (state.recipientSlot >= MAX_WIRE_PARTY_MEMBERS
        || state.syncSequence == 0
        || state.members.size() > MAX_WIRE_PARTY_MEMBERS
        || (state.members.empty()
            && (state.partyId != INVALID_PARTY_ID
                || state.revision != 0
                || state.leaderIndex != NO_LEADER_INDEX))
        || (!state.members.empty()
            && (state.partyId == INVALID_PARTY_ID
                || state.revision == 0
                || state.leaderIndex >= state.members.size())))
    {
        return {};
    }
    std::vector<std::uint8_t> packet(partyStateHeaderBytes, 0);
    std::memcpy(packet.data(), "PTYS", 4);
    packet[4] = VERSION;
    packet[5] = state.recipientSlot;
    packet[6] = static_cast<std::uint8_t>(state.members.size());
    packet[7] = state.leaderIndex;
    write64(packet, 8, state.partyId);
    write64(packet, 16, state.revision);
    write64(packet, 24, state.syncSequence);
    std::unordered_set<DurablePlayerIdentity, DurablePlayerIdentityHash>
        identities;
    std::unordered_set<std::uint8_t> onlineSlots;
    for (const MemberState& member : state.members)
    {
        if (!identities.insert(member.identity).second
            || (member.onlineSlot != NO_PLAYER_SLOT
                && (member.onlineSlot >= MAX_WIRE_PARTY_MEMBERS
                    || !onlineSlots.insert(member.onlineSlot).second)))
        {
            return {};
        }
        packet.push_back(member.onlineSlot);
        if (!appendIdentity(packet, member.identity))
        {
            return {};
        }
    }
    return packet;
}

bool decodePartyState(
    const std::uint8_t* data,
    const std::size_t size,
    PartyState& state
)
{
    if (!sameTag(data, size, "PTYS")
        || size < partyStateHeaderBytes || size > MAX_WIRE_PACKET_BYTES
        || data[4] != VERSION || data[5] >= MAX_WIRE_PARTY_MEMBERS
        || data[6] > MAX_WIRE_PARTY_MEMBERS)
    {
        return false;
    }
    PartyState decoded;
    decoded.recipientSlot = data[5];
    const std::size_t memberCount = data[6];
    decoded.leaderIndex = data[7];
    decoded.partyId = read64(data, 8);
    decoded.revision = read64(data, 16);
    decoded.syncSequence = read64(data, 24);
    if (decoded.syncSequence == 0)
    {
        return false;
    }
    if ((memberCount == 0
            && (decoded.partyId != INVALID_PARTY_ID
                || decoded.revision != 0
                || decoded.leaderIndex != NO_LEADER_INDEX))
        || (memberCount != 0
            && (decoded.partyId == INVALID_PARTY_ID
                || decoded.revision == 0
                || decoded.leaderIndex >= memberCount)))
    {
        return false;
    }
    std::unordered_set<DurablePlayerIdentity, DurablePlayerIdentityHash>
        identities;
    std::unordered_set<std::uint8_t> onlineSlots;
    std::size_t offset = partyStateHeaderBytes;
    for (std::size_t member = 0; member < memberCount; ++member)
    {
        if (offset >= size)
        {
            return false;
        }
        MemberState decodedMember;
        decodedMember.onlineSlot = data[offset++];
        if (!readIdentity(
                data, size, offset, decodedMember.identity)
            || !identities.insert(decodedMember.identity).second
            || (decodedMember.onlineSlot != NO_PLAYER_SLOT
                && (decodedMember.onlineSlot >= MAX_WIRE_PARTY_MEMBERS
                    || !onlineSlots.insert(decodedMember.onlineSlot).second)))
        {
            return false;
        }
        decoded.members.push_back(std::move(decodedMember));
    }
    if (offset != size)
    {
        return false;
    }
    state = std::move(decoded);
    return true;
}

std::vector<std::uint8_t> encodeInvitationList(
    const InvitationList& invitationList
)
{
    if (invitationList.recipientSlot >= MAX_WIRE_PARTY_MEMBERS
        || invitationList.syncSequence == 0
        || invitationList.invitations.size() > MAX_WIRE_INVITATIONS)
    {
        return {};
    }
    std::vector<std::uint8_t> packet(invitationListHeaderBytes, 0);
    std::memcpy(packet.data(), "PTYI", 4);
    packet[4] = VERSION;
    packet[5] = invitationList.recipientSlot;
    packet[6] = static_cast<std::uint8_t>(
        invitationList.invitations.size());
    write64(packet, 8, invitationList.syncSequence);
    std::unordered_set<InvitationID> ids;
    for (const InvitationState& invitation : invitationList.invitations)
    {
        if (invitation.invitationId == INVALID_INVITATION_ID
            || invitation.partyId == INVALID_PARTY_ID
            || invitation.expiresAtTick == 0
            || !ids.insert(invitation.invitationId).second)
        {
            return {};
        }
        const std::size_t offset = packet.size();
        packet.resize(offset + 24, 0);
        write64(packet, offset, invitation.invitationId);
        write64(packet, offset + 8, invitation.partyId);
        write64(packet, offset + 16, invitation.expiresAtTick);
        if (!appendIdentity(packet, invitation.inviter))
        {
            return {};
        }
    }
    return packet;
}

bool decodeInvitationList(
    const std::uint8_t* data,
    const std::size_t size,
    InvitationList& invitationList
)
{
    if (!sameTag(data, size, "PTYI")
        || size < invitationListHeaderBytes || size > MAX_WIRE_PACKET_BYTES
        || data[4] != VERSION || data[5] >= MAX_WIRE_PARTY_MEMBERS
        || data[6] > MAX_WIRE_INVITATIONS || data[7] != 0)
    {
        return false;
    }
    InvitationList decoded;
    decoded.recipientSlot = data[5];
    decoded.syncSequence = read64(data, 8);
    if (decoded.syncSequence == 0)
    {
        return false;
    }
    const std::size_t count = data[6];
    std::unordered_set<InvitationID> ids;
    std::size_t offset = invitationListHeaderBytes;
    for (std::size_t index = 0; index < count; ++index)
    {
        if (offset + 24 > size)
        {
            return false;
        }
        InvitationState invitation;
        invitation.invitationId = read64(data, offset);
        invitation.partyId = read64(data, offset + 8);
        invitation.expiresAtTick = read64(data, offset + 16);
        offset += 24;
        if (invitation.invitationId == INVALID_INVITATION_ID
            || invitation.partyId == INVALID_PARTY_ID
            || invitation.expiresAtTick == 0
            || !ids.insert(invitation.invitationId).second
            || !readIdentity(data, size, offset, invitation.inviter))
        {
            return false;
        }
        decoded.invitations.push_back(std::move(invitation));
    }
    if (offset != size)
    {
        return false;
    }
    invitationList = std::move(decoded);
    return true;
}

bool isSyncSequenceNewer(
    const std::uint64_t candidate,
    const std::uint64_t current)
{
    if (candidate == 0)
    {
        return false;
    }
    if (current == 0)
    {
        return true;
    }
    const std::uint64_t distance = candidate - current;
    return distance != 0
        && distance < (std::uint64_t{1} << 63U);
}

void RecipientSnapshotState::resetPending(const std::uint64_t sequence)
{
    pendingPartyState = PartyState{};
    pendingInvitationList = InvitationList{};
    pendingSequenceValue = sequence;
    hasPendingPartyState = false;
    hasPendingInvitationList = false;
}

void RecipientSnapshotState::reset()
{
    committedPartyState = PartyState{};
    committedInvitationList = InvitationList{};
    committedSequenceValue = 0;
    resetPending();
}

SnapshotStageResult RecipientSnapshotState::preparePending(
    const std::uint64_t sequence)
{
    if (sequence == 0)
    {
        return SnapshotStageResult::Rejected;
    }
    if (!isSyncSequenceNewer(sequence, committedSequenceValue))
    {
        return SnapshotStageResult::Stale;
    }
    if (pendingSequenceValue == 0)
    {
        resetPending(sequence);
    }
    else if (sequence != pendingSequenceValue)
    {
        if (!isSyncSequenceNewer(sequence, pendingSequenceValue))
        {
            return SnapshotStageResult::Stale;
        }
        resetPending(sequence);
    }
    return SnapshotStageResult::Pending;
}

SnapshotStageResult RecipientSnapshotState::commitIfComplete()
{
    if (!hasPendingPartyState || !hasPendingInvitationList)
    {
        return SnapshotStageResult::Pending;
    }
    if (pendingPartyState.recipientSlot
            != pendingInvitationList.recipientSlot
        || pendingPartyState.syncSequence != pendingSequenceValue
        || pendingInvitationList.syncSequence != pendingSequenceValue)
    {
        resetPending();
        return SnapshotStageResult::Rejected;
    }
    committedPartyState = std::move(pendingPartyState);
    committedInvitationList = std::move(pendingInvitationList);
    committedSequenceValue = pendingSequenceValue;
    resetPending();
    return SnapshotStageResult::Committed;
}

SnapshotStageResult RecipientSnapshotState::stagePartyState(
    PartyState state)
{
    if (state.recipientSlot >= MAX_WIRE_PARTY_MEMBERS)
    {
        return SnapshotStageResult::Rejected;
    }
    const SnapshotStageResult prepared = preparePending(state.syncSequence);
    if (prepared != SnapshotStageResult::Pending)
    {
        return prepared;
    }
    pendingPartyState = std::move(state);
    hasPendingPartyState = true;
    return commitIfComplete();
}

SnapshotStageResult RecipientSnapshotState::stageInvitationList(
    InvitationList invitations)
{
    if (invitations.recipientSlot >= MAX_WIRE_PARTY_MEMBERS)
    {
        return SnapshotStageResult::Rejected;
    }
    const SnapshotStageResult prepared =
        preparePending(invitations.syncSequence);
    if (prepared != SnapshotStageResult::Pending)
    {
        return prepared;
    }
    pendingInvitationList = std::move(invitations);
    hasPendingInvitationList = true;
    return commitIfComplete();
}

const PartyState& RecipientSnapshotState::partyState() const
{
    return committedPartyState;
}

const InvitationList& RecipientSnapshotState::invitationList() const
{
    return committedInvitationList;
}

std::uint64_t RecipientSnapshotState::committedSequence() const
{
    return committedSequenceValue;
}
}
