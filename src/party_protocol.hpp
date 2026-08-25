/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: party_protocol.hpp
    Desc: Bounded, endian-stable party request and recipient snapshot protocol.

-------------------------------------------------------------------------------*/

#pragma once

#include "automatia_identity.hpp"
#include "party_types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace AutomatiaParty::Protocol
{
constexpr std::uint8_t VERSION = 2;
constexpr std::uint8_t NO_PLAYER_SLOT = 0xff;
constexpr std::uint8_t NO_LEADER_INDEX = 0xff;
constexpr std::size_t MAX_WIRE_PARTY_MEMBERS = 15;
constexpr std::size_t MAX_WIRE_INVITATIONS =
    MAX_PENDING_INVITATIONS_PER_TARGET;
constexpr std::size_t MAX_WIRE_PACKET_BYTES = 2039;

enum class RequestOperation : std::uint8_t
{
    Create = 1,
    Invite = 2,
    Accept = 3,
    Decline = 4,
    Leave = 5,
    Kick = 6,
    Promote = 7,
    Disband = 8
};

struct Request
{
    RequestOperation operation = RequestOperation::Create;
    std::uint8_t actorSlot = NO_PLAYER_SLOT;
    std::uint32_t requestId = 0;
    PartyID claimedPartyId = INVALID_PARTY_ID;
    InvitationID invitationId = INVALID_INVITATION_ID;
    std::uint8_t targetSlot = NO_PLAYER_SLOT;
    DurablePlayerIdentity targetIdentity;
    bool hasTargetIdentity = false;
};

struct Result
{
    RequestOperation operation = RequestOperation::Create;
    OperationStatus status = OperationStatus::InvalidParty;
    std::uint32_t requestId = 0;
    PartyID partyId = INVALID_PARTY_ID;
    InvitationID invitationId = INVALID_INVITATION_ID;
    std::uint64_t revision = 0;
};

struct MemberState
{
    DurablePlayerIdentity identity;
    std::uint8_t onlineSlot = NO_PLAYER_SLOT;
};

struct PartyState
{
    std::uint8_t recipientSlot = NO_PLAYER_SLOT;
    PartyID partyId = INVALID_PARTY_ID;
    std::uint64_t revision = 0;
    std::uint64_t syncSequence = 0;
    std::uint8_t leaderIndex = NO_LEADER_INDEX;
    std::vector<MemberState> members;
};

struct InvitationState
{
    InvitationID invitationId = INVALID_INVITATION_ID;
    PartyID partyId = INVALID_PARTY_ID;
    std::uint64_t expiresAtTick = 0;
    DurablePlayerIdentity inviter;
};

struct InvitationList
{
    std::uint8_t recipientSlot = NO_PLAYER_SLOT;
    std::uint64_t syncSequence = 0;
    std::vector<InvitationState> invitations;
};

enum class SnapshotStageResult : std::uint8_t
{
    Rejected,
    Stale,
    Pending,
    Committed
};

/*
 * PTYS and PTYI form one recipient snapshot, but reliable UDP packets can be
 * delivered out of order. This bounded assembler commits only a matching pair
 * and ignores an older pair after a newer sequence has been observed.
 */
class RecipientSnapshotState
{
public:
    void reset();
    SnapshotStageResult stagePartyState(PartyState state);
    SnapshotStageResult stageInvitationList(InvitationList invitations);

    const PartyState& partyState() const;
    const InvitationList& invitationList() const;
    std::uint64_t committedSequence() const;

private:
    SnapshotStageResult preparePending(std::uint64_t sequence);
    SnapshotStageResult commitIfComplete();
    void resetPending(std::uint64_t sequence = 0);

    PartyState committedPartyState;
    InvitationList committedInvitationList;
    PartyState pendingPartyState;
    InvitationList pendingInvitationList;
    std::uint64_t committedSequenceValue = 0;
    std::uint64_t pendingSequenceValue = 0;
    bool hasPendingPartyState = false;
    bool hasPendingInvitationList = false;
};

bool isSyncSequenceNewer(
    std::uint64_t candidate,
    std::uint64_t current
);

std::vector<std::uint8_t> encodeRequest(const Request& request);
bool decodeRequest(
    const std::uint8_t* data,
    std::size_t size,
    Request& request
);

std::vector<std::uint8_t> encodeResult(const Result& result);
bool decodeResult(
    const std::uint8_t* data,
    std::size_t size,
    Result& result
);

std::vector<std::uint8_t> encodePartyState(const PartyState& state);
bool decodePartyState(
    const std::uint8_t* data,
    std::size_t size,
    PartyState& state
);

std::vector<std::uint8_t> encodeInvitationList(
    const InvitationList& invitationList
);
bool decodeInvitationList(
    const std::uint8_t* data,
    std::size_t size,
    InvitationList& invitationList
);

void write32(
    std::vector<std::uint8_t>& destination,
    std::size_t offset,
    std::uint32_t value
);
void write64(
    std::vector<std::uint8_t>& destination,
    std::size_t offset,
    std::uint64_t value
);
std::uint32_t read32(const std::uint8_t* source, std::size_t offset);
std::uint64_t read64(const std::uint8_t* source, std::size_t offset);
}
