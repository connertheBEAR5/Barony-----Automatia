/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: social_party_ui_model.cpp
    Desc: UI-only projection of authoritative party recipient state.

-------------------------------------------------------------------------------*/

#include "social_party_ui_model.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace AutomatiaSocial
{
namespace
{
constexpr std::uint8_t noSlot =
    AutomatiaParty::Protocol::NO_PLAYER_SLOT;

const ConnectedPlayer* connectedAtSlot(
    const std::unordered_map<int, const ConnectedPlayer*>& bySlot,
    const std::uint8_t slot)
{
    const auto found = bySlot.find(static_cast<int>(slot));
    return found == bySlot.end() ? nullptr : found->second;
}

std::string connectedIdentityDisplayName(
    const AutomatiaParty::DurablePlayerIdentity& identity,
    const std::vector<ConnectedPlayer>& connectedPlayers)
{
    if (identity.kind == AutomatiaParty::DurableIdentityKind::LocalName)
    {
        for (const ConnectedPlayer& player : connectedPlayers)
        {
            if (AutomatiaParty::normalizeLocalCharacterIdentity(
                    player.displayName.c_str()) == identity.value)
            {
                return player.displayName;
            }
        }
    }
    return durableDisplayName(identity);
}
}

std::string durableDisplayName(
    const AutomatiaParty::DurablePlayerIdentity& identity)
{
    if (identity.kind == AutomatiaParty::DurableIdentityKind::LocalName)
    {
        return identity.value.empty() ? "Unknown player" : identity.value;
    }
    if (identity.kind == AutomatiaParty::DurableIdentityKind::SteamId)
    {
        // The authoritative schema intentionally persists the canonical Steam
        // identity, not a mutable persona name. Avoid exposing the raw key;
        // retain a short suffix so multiple offline members remain distinct.
        const std::size_t suffixLength = std::min<std::size_t>(4,
            identity.value.size());
        return suffixLength == 0
            ? "Steam player"
            : "Steam player (..."
                + identity.value.substr(identity.value.size() - suffixLength)
                + ")";
    }
    return "Unknown player";
}

std::string durableSelectionKey(
    const AutomatiaParty::DurablePlayerIdentity& identity)
{
    return std::to_string(static_cast<unsigned>(identity.kind))
        + ":" + identity.value;
}

const char* relationshipLabel(const PlayerRelationship relationship)
{
    switch (relationship)
    {
        case PlayerRelationship::You: return "You";
        case PlayerRelationship::InYourParty: return "In Your Party";
        case PlayerRelationship::Connected: return "Connected";
        default: return "Connected";
    }
}

ViewModel buildViewModel(
    const int localPlayerSlot,
    const std::vector<ConnectedPlayer>& connectedPlayers,
    const AutomatiaParty::Protocol::PartyState& partyState,
    const AutomatiaParty::Protocol::InvitationList& invitationList,
    const std::size_t maximumPartyMembers)
{
    using namespace AutomatiaParty;
    using namespace AutomatiaParty::Protocol;

    ViewModel model;
    if (localPlayerSlot < 0
        || localPlayerSlot >= static_cast<int>(MAX_WIRE_PARTY_MEMBERS))
    {
        return model;
    }
    const std::uint8_t localSlot =
        static_cast<std::uint8_t>(localPlayerSlot);
    model.synchronized = partyState.recipientSlot == localSlot
        && invitationList.recipientSlot == localSlot
        && partyState.syncSequence != 0
        && partyState.syncSequence == invitationList.syncSequence;
    if (!model.synchronized)
    {
        // The multiplayer roster is session-global state in its own right.
        // Keep it visible while the paired PTYS/PTYI snapshot is in flight,
        // but expose no party mutations until that authoritative pair commits.
        std::vector<ConnectedPlayer> sortedConnected = connectedPlayers;
        std::sort(sortedConnected.begin(), sortedConnected.end(),
            [](const ConnectedPlayer& left, const ConnectedPlayer& right)
            {
                return left.slot < right.slot;
            });
        std::unordered_set<int> emittedSlots;
        for (const ConnectedPlayer& connected : sortedConnected)
        {
            if (connected.slot >= MAX_WIRE_PARTY_MEMBERS
                || !emittedSlots.insert(connected.slot).second)
            {
                continue;
            }
            PlayerRow row;
            row.slot = connected.slot;
            row.displayName = connected.displayName.empty()
                ? "Unknown player" : connected.displayName;
            row.relationship = connected.slot == localSlot
                ? PlayerRelationship::You
                : PlayerRelationship::Connected;
            row.inviteEligible = false;
            model.players.push_back(std::move(row));
        }
        return model;
    }

    model.partyId = partyState.partyId;
    model.revision = partyState.revision;
    model.syncSequence = partyState.syncSequence;
    model.inParty = partyState.partyId != INVALID_PARTY_ID;
    const std::size_t capacity = std::max<std::size_t>(1,
        std::min<std::size_t>(
            maximumPartyMembers, MAX_WIRE_PARTY_MEMBERS));
    model.partyFull = partyState.members.size() >= capacity;

    std::unordered_map<int, const ConnectedPlayer*> connectedBySlot;
    for (const ConnectedPlayer& player : connectedPlayers)
    {
        if (player.slot >= MAX_WIRE_PARTY_MEMBERS
            || connectedBySlot.count(player.slot) != 0)
        {
            continue;
        }
        connectedBySlot.emplace(player.slot, &player);
    }

    std::unordered_set<int> partySlots;
    model.members.reserve(partyState.members.size());
    for (std::size_t index = 0; index < partyState.members.size(); ++index)
    {
        const MemberState& member = partyState.members[index];
        PartyMemberRow row;
        row.identity = member.identity;
        row.onlineSlot = member.onlineSlot;
        row.online = member.onlineSlot != noSlot;
        row.leader = index == partyState.leaderIndex;
        row.localPlayer = member.onlineSlot == localSlot;
        if (row.online)
        {
            partySlots.insert(member.onlineSlot);
        }
        if (const ConnectedPlayer* connected =
            connectedAtSlot(connectedBySlot, member.onlineSlot))
        {
            row.displayName = connected->displayName;
        }
        if (row.displayName.empty())
        {
            row.displayName = durableDisplayName(member.identity);
        }
        model.members.push_back(std::move(row));
    }

    model.localLeader = partyState.leaderIndex < model.members.size()
        && model.members[partyState.leaderIndex].localPlayer;
    for (PartyMemberRow& member : model.members)
    {
        member.canKick = model.localLeader && !member.localPlayer;
        member.canPromote = model.localLeader && !member.localPlayer;
    }

    std::vector<ConnectedPlayer> sortedConnected = connectedPlayers;
    std::sort(sortedConnected.begin(), sortedConnected.end(),
        [](const ConnectedPlayer& left, const ConnectedPlayer& right)
        {
            return left.slot < right.slot;
        });
    std::unordered_set<int> emittedSlots;
    for (const ConnectedPlayer& connected : sortedConnected)
    {
        if (connected.slot >= MAX_WIRE_PARTY_MEMBERS
            || !emittedSlots.insert(connected.slot).second)
        {
            continue;
        }
        PlayerRow row;
        row.slot = connected.slot;
        row.displayName = connected.displayName.empty()
            ? "Unknown player" : connected.displayName;
        if (connected.slot == localSlot)
        {
            row.relationship = PlayerRelationship::You;
        }
        else if (partySlots.count(connected.slot) != 0)
        {
            row.relationship = PlayerRelationship::InYourParty;
        }
        else
        {
            row.relationship = PlayerRelationship::Connected;
        }
        row.inviteEligible = model.localLeader
            && !model.partyFull
            && row.relationship == PlayerRelationship::Connected;
        model.players.push_back(std::move(row));
    }

    model.invitations.reserve(invitationList.invitations.size());
    for (const InvitationState& invitation : invitationList.invitations)
    {
        InvitationRow row;
        row.invitationId = invitation.invitationId;
        row.partyId = invitation.partyId;
        row.expiresAtTick = invitation.expiresAtTick;
        row.inviter = invitation.inviter;
        row.inviterDisplayName = connectedIdentityDisplayName(
            invitation.inviter, connectedPlayers);
        model.invitations.push_back(std::move(row));
    }

    model.canCreate = !model.inParty;
    model.canLeave = model.inParty;
    model.canDisband = model.localLeader;
    return model;
}

bool buildRequest(
    const Action action,
    const int localPlayerSlot,
    const std::uint32_t requestId,
    const AutomatiaParty::Protocol::PartyState& partyState,
    const ActionSelection& selection,
    AutomatiaParty::Protocol::Request& request)
{
    using namespace AutomatiaParty;
    using namespace AutomatiaParty::Protocol;

    request = {};
    if (localPlayerSlot < 0
        || localPlayerSlot >= static_cast<int>(MAX_WIRE_PARTY_MEMBERS)
        || requestId == 0)
    {
        return false;
    }
    request.actorSlot = static_cast<std::uint8_t>(localPlayerSlot);
    request.requestId = requestId;

    switch (action)
    {
        case Action::Create:
            if (partyState.partyId != INVALID_PARTY_ID)
                return false;
            request.operation = RequestOperation::Create;
            return true;
        case Action::Invite:
            if (partyState.partyId == INVALID_PARTY_ID
                || selection.playerSlot == NO_PLAYER_SLOT
                || selection.playerSlot >= MAX_WIRE_PARTY_MEMBERS
                || selection.playerSlot == request.actorSlot)
            {
                return false;
            }
            request.operation = RequestOperation::Invite;
            request.claimedPartyId = partyState.partyId;
            request.targetSlot = selection.playerSlot;
            return true;
        case Action::Accept:
        case Action::Decline:
            if (selection.invitationId == INVALID_INVITATION_ID
                || selection.invitationPartyId == INVALID_PARTY_ID)
            {
                return false;
            }
            request.operation = action == Action::Accept
                ? RequestOperation::Accept : RequestOperation::Decline;
            request.claimedPartyId = selection.invitationPartyId;
            request.invitationId = selection.invitationId;
            return true;
        case Action::Leave:
        case Action::Disband:
            if (partyState.partyId == INVALID_PARTY_ID)
                return false;
            request.operation = action == Action::Leave
                ? RequestOperation::Leave : RequestOperation::Disband;
            request.claimedPartyId = partyState.partyId;
            return true;
        case Action::Kick:
        case Action::Promote:
            if (partyState.partyId == INVALID_PARTY_ID
                || !selection.hasMemberIdentity
                || !selection.memberIdentity.isValid())
            {
                return false;
            }
            request.operation = action == Action::Kick
                ? RequestOperation::Kick : RequestOperation::Promote;
            request.claimedPartyId = partyState.partyId;
            request.targetIdentity = selection.memberIdentity;
            request.hasTargetIdentity = true;
            return true;
        default:
            return false;
    }
}

const char* pendingMessage(const Action action)
{
    switch (action)
    {
        case Action::Create: return "Creating party...";
        case Action::Invite: return "Sending invitation...";
        case Action::Accept: return "Accepting invitation...";
        case Action::Decline: return "Declining invitation...";
        case Action::Leave: return "Leaving party...";
        case Action::Kick: return "Removing party member...";
        case Action::Promote: return "Transferring leadership...";
        case Action::Disband: return "Disbanding party...";
        default: return "Waiting for server...";
    }
}

std::string resultMessage(
    const AutomatiaParty::Protocol::Result& result)
{
    using AutomatiaParty::OperationStatus;
    if (result.status == OperationStatus::Success)
    {
        using AutomatiaParty::Protocol::RequestOperation;
        switch (result.operation)
        {
            case RequestOperation::Create: return "Party created.";
            case RequestOperation::Invite: return "Invitation sent.";
            case RequestOperation::Accept: return "You joined the party.";
            case RequestOperation::Decline: return "Invitation declined.";
            case RequestOperation::Leave: return "You left the party.";
            case RequestOperation::Kick: return "Party member removed.";
            case RequestOperation::Promote: return "Leadership transferred.";
            case RequestOperation::Disband: return "Party disbanded.";
            default: return "Party request completed.";
        }
    }

    switch (result.status)
    {
        case OperationStatus::InvalidIdentity:
            return "That player is no longer available.";
        case OperationStatus::InvalidParty:
            return "The party no longer exists.";
        case OperationStatus::InvalidInvitation:
            return "That invitation is no longer valid.";
        case OperationStatus::AlreadyInParty:
            return "You are already in a party.";
        case OperationStatus::NotInParty:
            return "You are not currently in a party.";
        case OperationStatus::NotLeader:
            return "You are not the party leader.";
        case OperationStatus::TargetAlreadyInParty:
            return "That player is already in a party.";
        case OperationStatus::TargetNotInParty:
            return "That player is no longer in your party.";
        case OperationStatus::CannotTargetSelf:
            return "You cannot use that action on yourself.";
        case OperationStatus::PartyFull:
            return "The party is full.";
        case OperationStatus::InvitationAlreadyPending:
            return "That player already has your invitation.";
        case OperationStatus::InvitationExpired:
            return "That invitation has expired.";
        case OperationStatus::InvitationLimitReached:
            return "That player has too many pending invitations.";
        case OperationStatus::IdSpaceExhausted:
            return "The party request could not be completed.";
        default:
            return "The party request could not be completed.";
    }
}

std::size_t scrollOffsetForSelection(
    const std::size_t selectedIndex,
    const std::size_t rowCount,
    const std::size_t visibleRows,
    const std::size_t currentOffset)
{
    if (rowCount == 0 || visibleRows == 0)
    {
        return 0;
    }
    const std::size_t clampedSelection =
        std::min(selectedIndex, rowCount - 1);
    const std::size_t maximumOffset = rowCount > visibleRows
        ? rowCount - visibleRows : 0;
    std::size_t offset = std::min(currentOffset, maximumOffset);
    if (clampedSelection < offset)
    {
        offset = clampedSelection;
    }
    else if (clampedSelection >= offset + visibleRows)
    {
        offset = clampedSelection - visibleRows + 1;
    }
    return std::min(offset, maximumOffset);
}
}
