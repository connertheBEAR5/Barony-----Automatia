/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: social_party_ui_model.hpp
    Desc: UI-only projection of authoritative party recipient state.

-------------------------------------------------------------------------------*/

#pragma once

#include "party_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AutomatiaSocial
{
enum class PlayerRelationship : std::uint8_t
{
    You,
    InYourParty,
    Connected
};

struct ConnectedPlayer
{
    std::uint8_t slot = AutomatiaParty::Protocol::NO_PLAYER_SLOT;
    std::string displayName;

    // Deliberately not used to filter party membership. It exists so tests
    // can prove that divergent-map roster metadata has no effect on Social.
    std::string mapInstance;
};

struct PlayerRow
{
    std::uint8_t slot = AutomatiaParty::Protocol::NO_PLAYER_SLOT;
    std::string displayName;
    PlayerRelationship relationship = PlayerRelationship::Connected;
    bool inviteEligible = false;
};

struct PartyMemberRow
{
    AutomatiaParty::DurablePlayerIdentity identity;
    std::string displayName;
    std::uint8_t onlineSlot = AutomatiaParty::Protocol::NO_PLAYER_SLOT;
    bool online = false;
    bool leader = false;
    bool localPlayer = false;
    bool canKick = false;
    bool canPromote = false;
};

struct InvitationRow
{
    AutomatiaParty::InvitationID invitationId =
        AutomatiaParty::INVALID_INVITATION_ID;
    AutomatiaParty::PartyID partyId = AutomatiaParty::INVALID_PARTY_ID;
    std::uint64_t expiresAtTick = 0;
    AutomatiaParty::DurablePlayerIdentity inviter;
    std::string inviterDisplayName;
};

struct ViewModel
{
    bool synchronized = false;
    bool inParty = false;
    bool localLeader = false;
    bool partyFull = false;
    bool canCreate = false;
    bool canLeave = false;
    bool canDisband = false;
    AutomatiaParty::PartyID partyId = AutomatiaParty::INVALID_PARTY_ID;
    std::uint64_t revision = 0;
    std::uint64_t syncSequence = 0;
    std::vector<PlayerRow> players;
    std::vector<PartyMemberRow> members;
    std::vector<InvitationRow> invitations;
};

enum class Action : std::uint8_t
{
    Create,
    Invite,
    Accept,
    Decline,
    Leave,
    Kick,
    Promote,
    Disband
};

struct ActionSelection
{
    std::uint8_t playerSlot = AutomatiaParty::Protocol::NO_PLAYER_SLOT;
    AutomatiaParty::InvitationID invitationId =
        AutomatiaParty::INVALID_INVITATION_ID;
    AutomatiaParty::PartyID invitationPartyId =
        AutomatiaParty::INVALID_PARTY_ID;
    AutomatiaParty::DurablePlayerIdentity memberIdentity;
    bool hasMemberIdentity = false;
};

ViewModel buildViewModel(
    int localPlayerSlot,
    const std::vector<ConnectedPlayer>& connectedPlayers,
    const AutomatiaParty::Protocol::PartyState& partyState,
    const AutomatiaParty::Protocol::InvitationList& invitationList,
    std::size_t maximumPartyMembers =
        AutomatiaParty::Protocol::MAX_WIRE_PARTY_MEMBERS
);

bool buildRequest(
    Action action,
    int localPlayerSlot,
    std::uint32_t requestId,
    const AutomatiaParty::Protocol::PartyState& partyState,
    const ActionSelection& selection,
    AutomatiaParty::Protocol::Request& request
);

std::string durableDisplayName(
    const AutomatiaParty::DurablePlayerIdentity& identity
);
std::string durableSelectionKey(
    const AutomatiaParty::DurablePlayerIdentity& identity
);
const char* relationshipLabel(PlayerRelationship relationship);
const char* pendingMessage(Action action);
std::string resultMessage(
    const AutomatiaParty::Protocol::Result& result
);

/*
 * Returns the first visible row needed to keep selection in a bounded view.
 * The production Frame widget performs the actual scrolling; this helper
 * makes the 15-player edge behavior deterministic and independently testable.
 */
std::size_t scrollOffsetForSelection(
    std::size_t selectedIndex,
    std::size_t rowCount,
    std::size_t visibleRows,
    std::size_t currentOffset
);
}
