/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: party_types.hpp
    Desc: Lightweight shared persistent-party identifiers and operation results.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace AutomatiaParty
{
using PartyID = std::uint64_t;
using InvitationID = std::uint64_t;

constexpr PartyID INVALID_PARTY_ID = 0;
constexpr InvitationID INVALID_INVITATION_ID = 0;
constexpr std::size_t MAX_PERSISTENT_PARTY_MEMBERS = 15;
constexpr std::size_t MAX_PENDING_INVITATIONS_PER_TARGET = 8;

enum class OperationStatus : std::uint8_t
{
    Success = 0,
    InvalidIdentity,
    InvalidParty,
    InvalidInvitation,
    AlreadyInParty,
    NotInParty,
    NotLeader,
    TargetAlreadyInParty,
    TargetNotInParty,
    CannotTargetSelf,
    PartyFull,
    InvitationAlreadyPending,
    InvitationExpired,
    InvitationLimitReached,
    IdSpaceExhausted
};

struct OperationResult
{
    OperationStatus status = OperationStatus::InvalidParty;
    PartyID partyId = INVALID_PARTY_ID;
    InvitationID invitationId = INVALID_INVITATION_ID;
    std::uint64_t revision = 0;

    explicit operator bool() const
    {
        return status == OperationStatus::Success;
    }
};

const char* operationStatusName(OperationStatus status);
}
