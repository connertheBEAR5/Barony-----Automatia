/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: party_manager.hpp
    Desc: Map-independent, server/world-authoritative persistent party backend.

-------------------------------------------------------------------------------*/

#pragma once

#include "automatia_identity.hpp"
#include "party_types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace AutomatiaParty
{
constexpr std::size_t MAX_PERSISTENT_PARTIES = 4096;
constexpr std::size_t MAX_PENDING_INVITATIONS = 256;

struct Party
{
    PartyID id = INVALID_PARTY_ID;
    std::uint64_t revision = 0;
    DurablePlayerIdentity leader;
    std::vector<DurablePlayerIdentity> members;
};

struct Invitation
{
    InvitationID id = INVALID_INVITATION_ID;
    PartyID partyId = INVALID_PARTY_ID;
    DurablePlayerIdentity inviter;
    DurablePlayerIdentity target;
    std::uint64_t expiresAtTick = 0;
};

class PartyManager
{
public:
    void clear();

    const Party* findParty(PartyID partyId) const;
    const Party* findPartyForPlayer(
        const DurablePlayerIdentity& identity
    ) const;
    PartyID partyIdForPlayer(const DurablePlayerIdentity& identity) const;
    const std::vector<Invitation> invitationsFor(
        const DurablePlayerIdentity& target
    ) const;
    std::vector<PartyID> partyIds() const;
    std::size_t partyCount() const;
    PartyID nextPartyId() const;

    OperationResult createParty(const DurablePlayerIdentity& creator);
    OperationResult invitePlayer(
        const DurablePlayerIdentity& actor,
        const DurablePlayerIdentity& target,
        std::uint64_t currentTick,
        std::uint64_t lifetimeTicks,
        std::size_t maximumMembers
    );
    OperationResult acceptInvitation(
        const DurablePlayerIdentity& actor,
        InvitationID invitationId,
        std::uint64_t currentTick,
        std::size_t maximumMembers
    );
    OperationResult declineInvitation(
        const DurablePlayerIdentity& actor,
        InvitationID invitationId,
        std::uint64_t currentTick
    );
    OperationResult leaveParty(const DurablePlayerIdentity& actor);
    OperationResult kickMember(
        const DurablePlayerIdentity& actor,
        const DurablePlayerIdentity& target
    );
    OperationResult promoteLeader(
        const DurablePlayerIdentity& actor,
        const DurablePlayerIdentity& target
    );
    OperationResult disbandParty(const DurablePlayerIdentity& actor);

    std::vector<DurablePlayerIdentity> expireInvitations(
        std::uint64_t currentTick
    );

    bool bindOnlinePlayer(
        const DurablePlayerIdentity& identity,
        int playerSlot,
        std::string& error
    );
    void unbindOnlinePlayer(int playerSlot);
    void clearOnlineBindings();
    int onlineSlotFor(const DurablePlayerIdentity& identity) const;
    const DurablePlayerIdentity* onlineIdentityFor(int playerSlot) const;
    std::vector<DurablePlayerIdentity> onlineIdentities() const;

private:
    friend class PartyPersistence;

    using MembershipMap = std::unordered_map<
        DurablePlayerIdentity,
        PartyID,
        DurablePlayerIdentityHash>;
    using OnlineSlotMap = std::unordered_map<
        DurablePlayerIdentity,
        int,
        DurablePlayerIdentityHash>;

    OperationResult resultFor(
        OperationStatus status,
        PartyID partyId = INVALID_PARTY_ID,
        InvitationID invitationId = INVALID_INVITATION_ID,
        std::uint64_t revision = 0
    ) const;
    PartyID allocatePartyId();
    InvitationID allocateInvitationId();
    void removeInvitationsForTarget(
        const DurablePlayerIdentity& target
    );
    void removeInvitationsForParty(PartyID partyId);
    bool partyHasPendingInvitation(PartyID partyId) const;
    void disbandById(PartyID partyId);
    void dissolveTransientSingleton(PartyID partyId);

    std::unordered_map<PartyID, Party> parties;
    MembershipMap memberships;
    std::unordered_map<InvitationID, Invitation> invitations;
    OnlineSlotMap onlineSlotsByIdentity;
    std::unordered_map<int, DurablePlayerIdentity> onlineIdentitiesBySlot;
    PartyID nextPartyIdValue = 1;
    InvitationID nextInvitationIdValue = 1;
};
}
