/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: party_manager.cpp
    Desc: Map-independent, server/world-authoritative persistent party backend.

-------------------------------------------------------------------------------*/

#include "party_persistence.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

namespace AutomatiaParty
{
namespace
{
using Json = nlohmann::json;

bool unsignedValue(const Json& value, std::uint64_t& result)
{
    if (value.is_number_unsigned())
    {
        result = value.get<std::uint64_t>();
        return true;
    }
    if (value.is_number_integer())
    {
        const std::int64_t signedValue = value.get<std::int64_t>();
        if (signedValue >= 0)
        {
            result = static_cast<std::uint64_t>(signedValue);
            return true;
        }
    }
    return false;
}

Json identityToJson(const DurablePlayerIdentity& identity)
{
    return Json{
        {"kind", durableIdentityKindName(identity.kind)},
        {"value", identity.value}
    };
}

bool identityFromJson(
    const Json& document,
    DurablePlayerIdentity& identity,
    std::string& error,
    const std::string& location
)
{
    if (!document.is_object()
        || !document.contains("kind")
        || !document["kind"].is_string()
        || !document.contains("value")
        || !document["value"].is_string())
    {
        error = location + " has invalid identity fields";
        return false;
    }
    if (!durableIdentityKindFromName(
            document["kind"].get<std::string>(), identity.kind))
    {
        error = location + " has an unsupported identity kind";
        return false;
    }
    identity.value = document["value"].get<std::string>();
    if (!identity.isValid())
    {
        error = location + " has a non-canonical or unsafe identity value";
        return false;
    }
    return true;
}
}

const char* operationStatusName(const OperationStatus status)
{
    switch (status)
    {
        case OperationStatus::Success: return "success";
        case OperationStatus::InvalidIdentity: return "invalid_identity";
        case OperationStatus::InvalidParty: return "invalid_party";
        case OperationStatus::InvalidInvitation: return "invalid_invitation";
        case OperationStatus::AlreadyInParty: return "already_in_party";
        case OperationStatus::NotInParty: return "not_in_party";
        case OperationStatus::NotLeader: return "not_leader";
        case OperationStatus::TargetAlreadyInParty: return "target_already_in_party";
        case OperationStatus::TargetNotInParty: return "target_not_in_party";
        case OperationStatus::CannotTargetSelf: return "cannot_target_self";
        case OperationStatus::PartyFull: return "party_full";
        case OperationStatus::InvitationAlreadyPending: return "invitation_already_pending";
        case OperationStatus::InvitationExpired: return "invitation_expired";
        case OperationStatus::InvitationLimitReached: return "invitation_limit_reached";
        case OperationStatus::IdSpaceExhausted: return "id_space_exhausted";
        default: return "unknown";
    }
}

void PartyManager::clear()
{
    parties.clear();
    memberships.clear();
    invitations.clear();
    onlineSlotsByIdentity.clear();
    onlineIdentitiesBySlot.clear();
    nextPartyIdValue = 1;
    nextInvitationIdValue = 1;
}

const Party* PartyManager::findParty(const PartyID partyId) const
{
    const auto found = parties.find(partyId);
    return found == parties.end() ? nullptr : &found->second;
}

PartyID PartyManager::partyIdForPlayer(
    const DurablePlayerIdentity& identity
) const
{
    const auto found = memberships.find(identity);
    return found == memberships.end() ? INVALID_PARTY_ID : found->second;
}

const Party* PartyManager::findPartyForPlayer(
    const DurablePlayerIdentity& identity
) const
{
    return findParty(partyIdForPlayer(identity));
}

const std::vector<Invitation> PartyManager::invitationsFor(
    const DurablePlayerIdentity& target
) const
{
    std::vector<Invitation> result;
    for (const auto& entry : invitations)
    {
        if (entry.second.target == target)
        {
            result.push_back(entry.second);
        }
    }
    std::sort(
        result.begin(), result.end(),
        [](const Invitation& first, const Invitation& second)
        {
            return first.id < second.id;
        }
    );
    return result;
}

std::vector<PartyID> PartyManager::partyIds() const
{
    std::vector<PartyID> result;
    result.reserve(parties.size());
    for (const auto& entry : parties)
    {
        result.push_back(entry.first);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::size_t PartyManager::partyCount() const
{
    return parties.size();
}

PartyID PartyManager::nextPartyId() const
{
    return nextPartyIdValue;
}

OperationResult PartyManager::resultFor(
    const OperationStatus status,
    const PartyID partyId,
    const InvitationID invitationId,
    const std::uint64_t revision
) const
{
    return {status, partyId, invitationId, revision};
}

PartyID PartyManager::allocatePartyId()
{
    // UINT64_MAX is retained as a durable exhausted sentinel. Allocating it
    // would leave no nonzero next_id value that can be serialized safely.
    if (nextPartyIdValue == INVALID_PARTY_ID
        || nextPartyIdValue == std::numeric_limits<PartyID>::max())
    {
        return INVALID_PARTY_ID;
    }
    const PartyID allocated = nextPartyIdValue;
    ++nextPartyIdValue;
    return allocated;
}

InvitationID PartyManager::allocateInvitationId()
{
    if (invitations.size() >= MAX_PENDING_INVITATIONS)
    {
        return INVALID_INVITATION_ID;
    }
    for (std::size_t attempt = 0;
        attempt <= MAX_PENDING_INVITATIONS;
        ++attempt)
    {
        if (nextInvitationIdValue == INVALID_INVITATION_ID)
        {
            nextInvitationIdValue = 1;
        }
        const InvitationID candidate = nextInvitationIdValue++;
        if (invitations.count(candidate) == 0)
        {
            return candidate;
        }
    }
    return INVALID_INVITATION_ID;
}

OperationResult PartyManager::createParty(
    const DurablePlayerIdentity& creator
)
{
    if (!creator.isValid())
    {
        return resultFor(OperationStatus::InvalidIdentity);
    }
    if (partyIdForPlayer(creator) != INVALID_PARTY_ID)
    {
        return resultFor(OperationStatus::AlreadyInParty);
    }
    if (parties.size() >= MAX_PERSISTENT_PARTIES)
    {
        return resultFor(OperationStatus::IdSpaceExhausted);
    }
    const PartyID id = allocatePartyId();
    if (id == INVALID_PARTY_ID)
    {
        return resultFor(OperationStatus::IdSpaceExhausted);
    }
    Party party;
    party.id = id;
    party.revision = 1;
    party.leader = creator;
    party.members.push_back(creator);
    parties.emplace(id, std::move(party));
    memberships.emplace(creator, id);
    // Creating a party makes every invitation targeting this character
    // ineligible. Remove them immediately instead of leaving stale entries
    // until their timeout, and dissolve any invitation-only singleton that
    // no longer has a pending target.
    removeInvitationsForTarget(creator);
    return resultFor(OperationStatus::Success, id, 0, 1);
}

OperationResult PartyManager::invitePlayer(
    const DurablePlayerIdentity& actor,
    const DurablePlayerIdentity& target,
    const std::uint64_t currentTick,
    const std::uint64_t lifetimeTicks,
    const std::size_t maximumMembers
)
{
    if (!actor.isValid() || !target.isValid())
    {
        return resultFor(OperationStatus::InvalidIdentity);
    }
    // Callers need not wait for the periodic maintenance pass before an
    // expired invitation stops counting as pending or consuming a limit.
    (void)expireInvitations(currentTick);
    if (actor == target)
    {
        return resultFor(OperationStatus::CannotTargetSelf);
    }
    Party* party = nullptr;
    const PartyID partyId = partyIdForPlayer(actor);
    const auto foundParty = parties.find(partyId);
    if (foundParty != parties.end())
    {
        party = &foundParty->second;
    }
    if (!party)
    {
        return resultFor(OperationStatus::NotInParty);
    }
    if (party->leader != actor)
    {
        return resultFor(OperationStatus::NotLeader, partyId);
    }
    // No successful acceptance can advance a party whose revision is already
    // exhausted.  Do not create a runtime invitation that can never be used.
    if (party->revision == std::numeric_limits<std::uint64_t>::max())
    {
        return resultFor(
            OperationStatus::IdSpaceExhausted,
            partyId,
            INVALID_INVITATION_ID,
            party->revision);
    }
    if (partyIdForPlayer(target) != INVALID_PARTY_ID)
    {
        return resultFor(OperationStatus::TargetAlreadyInParty, partyId);
    }
    const std::size_t effectiveMaximum = std::min(
        maximumMembers, MAX_PERSISTENT_PARTY_MEMBERS);
    if (effectiveMaximum < 2
        || party->members.size() >= effectiveMaximum)
    {
        return resultFor(OperationStatus::PartyFull, partyId);
    }
    std::size_t targetInvitationCount = 0;
    for (const auto& entry : invitations)
    {
        const Invitation& invitation = entry.second;
        if (invitation.target == target)
        {
            ++targetInvitationCount;
            if (invitation.partyId == partyId)
            {
                return resultFor(
                    OperationStatus::InvitationAlreadyPending,
                    partyId,
                    invitation.id,
                    party->revision);
            }
        }
    }
    if (targetInvitationCount >= MAX_PENDING_INVITATIONS_PER_TARGET)
    {
        return resultFor(OperationStatus::InvitationLimitReached, partyId);
    }
    if (lifetimeTicks == 0
        || currentTick == std::numeric_limits<std::uint64_t>::max())
    {
        return resultFor(
            OperationStatus::InvitationExpired,
            partyId,
            INVALID_INVITATION_ID,
            party->revision);
    }
    const InvitationID invitationId = allocateInvitationId();
    if (invitationId == INVALID_INVITATION_ID)
    {
        return resultFor(OperationStatus::InvitationLimitReached, partyId);
    }
    const std::uint64_t maximumTick =
        std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t expiresAt = lifetimeTicks > maximumTick - currentTick
        ? maximumTick
        : currentTick + lifetimeTicks;
    invitations.emplace(invitationId, Invitation{
        invitationId, partyId, actor, target, expiresAt
    });
    return resultFor(
        OperationStatus::Success,
        partyId,
        invitationId,
        party->revision);
}

OperationResult PartyManager::acceptInvitation(
    const DurablePlayerIdentity& actor,
    const InvitationID invitationId,
    const std::uint64_t currentTick,
    const std::size_t maximumMembers
)
{
    if (!actor.isValid())
    {
        return resultFor(OperationStatus::InvalidIdentity);
    }
    if (partyIdForPlayer(actor) != INVALID_PARTY_ID)
    {
        return resultFor(OperationStatus::AlreadyInParty);
    }
    const auto foundInvitation = invitations.find(invitationId);
    if (foundInvitation == invitations.end()
        || foundInvitation->second.target != actor)
    {
        return resultFor(OperationStatus::InvalidInvitation);
    }
    const Invitation invitation = foundInvitation->second;
    if (currentTick >= invitation.expiresAtTick)
    {
        invitations.erase(foundInvitation);
        dissolveTransientSingleton(invitation.partyId);
        return resultFor(
            OperationStatus::InvitationExpired,
            invitation.partyId,
            invitationId);
    }
    const auto foundParty = parties.find(invitation.partyId);
    if (foundParty == parties.end())
    {
        invitations.erase(foundInvitation);
        return resultFor(OperationStatus::InvalidParty, invitation.partyId);
    }
    Party& party = foundParty->second;
    if (party.leader != invitation.inviter)
    {
        invitations.erase(foundInvitation);
        dissolveTransientSingleton(invitation.partyId);
        return resultFor(OperationStatus::NotLeader, invitation.partyId);
    }
    const std::size_t effectiveMaximum = std::min(
        maximumMembers, MAX_PERSISTENT_PARTY_MEMBERS);
    if (effectiveMaximum < 2
        || party.members.size() >= effectiveMaximum)
    {
        return resultFor(OperationStatus::PartyFull, invitation.partyId);
    }
    if (party.revision == std::numeric_limits<std::uint64_t>::max())
    {
        return resultFor(
            OperationStatus::IdSpaceExhausted,
            invitation.partyId,
            invitationId,
            party.revision);
    }
    party.members.push_back(actor);
    memberships.emplace(actor, party.id);
    ++party.revision;
    removeInvitationsForTarget(actor);
    if (party.revision == std::numeric_limits<std::uint64_t>::max())
    {
        // No later acceptance can advance this party. Do not expose
        // invitations which are now permanently unusable.
        removeInvitationsForParty(party.id);
    }
    return resultFor(
        OperationStatus::Success,
        party.id,
        invitationId,
        party.revision);
}

OperationResult PartyManager::declineInvitation(
    const DurablePlayerIdentity& actor,
    const InvitationID invitationId,
    const std::uint64_t currentTick
)
{
    if (!actor.isValid())
    {
        return resultFor(OperationStatus::InvalidIdentity);
    }
    const auto found = invitations.find(invitationId);
    if (found == invitations.end() || found->second.target != actor)
    {
        return resultFor(OperationStatus::InvalidInvitation);
    }
    const Invitation invitation = found->second;
    invitations.erase(found);
    dissolveTransientSingleton(invitation.partyId);
    return resultFor(
        currentTick >= invitation.expiresAtTick
            ? OperationStatus::InvitationExpired
            : OperationStatus::Success,
        invitation.partyId,
        invitationId);
}

void PartyManager::removeInvitationsForTarget(
    const DurablePlayerIdentity& target
)
{
    std::vector<PartyID> affectedParties;
    for (auto iterator = invitations.begin(); iterator != invitations.end(); )
    {
        if (iterator->second.target == target)
        {
            affectedParties.push_back(iterator->second.partyId);
            iterator = invitations.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
    std::sort(affectedParties.begin(), affectedParties.end());
    affectedParties.erase(
        std::unique(affectedParties.begin(), affectedParties.end()),
        affectedParties.end());
    for (const PartyID partyId : affectedParties)
    {
        dissolveTransientSingleton(partyId);
    }
}

void PartyManager::removeInvitationsForParty(const PartyID partyId)
{
    for (auto iterator = invitations.begin(); iterator != invitations.end(); )
    {
        if (iterator->second.partyId == partyId)
        {
            iterator = invitations.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
}

bool PartyManager::partyHasPendingInvitation(const PartyID partyId) const
{
    return std::any_of(
        invitations.begin(), invitations.end(),
        [partyId](const auto& entry)
        {
            return entry.second.partyId == partyId;
        }
    );
}

void PartyManager::disbandById(const PartyID partyId)
{
    const auto found = parties.find(partyId);
    if (found == parties.end())
    {
        return;
    }
    for (const DurablePlayerIdentity& member : found->second.members)
    {
        memberships.erase(member);
    }
    removeInvitationsForParty(partyId);
    parties.erase(found);
}

void PartyManager::dissolveTransientSingleton(const PartyID partyId)
{
    const Party* party = findParty(partyId);
    if (party && party->members.size() == 1
        && !partyHasPendingInvitation(partyId))
    {
        disbandById(partyId);
    }
}

OperationResult PartyManager::leaveParty(
    const DurablePlayerIdentity& actor
)
{
    const PartyID partyId = partyIdForPlayer(actor);
    auto found = parties.find(partyId);
    if (!actor.isValid())
    {
        return resultFor(OperationStatus::InvalidIdentity);
    }
    if (found == parties.end())
    {
        return resultFor(OperationStatus::NotInParty);
    }
    Party& party = found->second;
    const auto member = std::find(
        party.members.begin(), party.members.end(), actor);
    if (member == party.members.end())
    {
        return resultFor(OperationStatus::NotInParty);
    }
    if (party.revision == std::numeric_limits<std::uint64_t>::max())
    {
        return resultFor(
            OperationStatus::IdSpaceExhausted,
            partyId,
            INVALID_INVITATION_ID,
            party.revision);
    }
    const bool wasLeader = party.leader == actor;
    party.members.erase(member);
    memberships.erase(actor);
    removeInvitationsForTarget(actor);
    ++party.revision;
    if (party.revision == std::numeric_limits<std::uint64_t>::max())
    {
        removeInvitationsForParty(partyId);
    }
    if (party.members.size() <= 1)
    {
        disbandById(partyId);
        return resultFor(OperationStatus::Success, partyId);
    }
    if (wasLeader)
    {
        // Join order is persistent; the oldest remaining member is stable.
        party.leader = party.members.front();
        removeInvitationsForParty(partyId);
    }
    return resultFor(
        OperationStatus::Success, partyId, 0, party.revision);
}

OperationResult PartyManager::kickMember(
    const DurablePlayerIdentity& actor,
    const DurablePlayerIdentity& target
)
{
    if (!actor.isValid() || !target.isValid())
    {
        return resultFor(OperationStatus::InvalidIdentity);
    }
    if (actor == target)
    {
        return resultFor(OperationStatus::CannotTargetSelf);
    }
    const PartyID partyId = partyIdForPlayer(actor);
    auto found = parties.find(partyId);
    if (found == parties.end())
    {
        return resultFor(OperationStatus::NotInParty);
    }
    Party& party = found->second;
    if (party.leader != actor)
    {
        return resultFor(OperationStatus::NotLeader, partyId);
    }
    const auto member = std::find(
        party.members.begin(), party.members.end(), target);
    if (member == party.members.end())
    {
        return resultFor(OperationStatus::TargetNotInParty, partyId);
    }
    if (party.revision == std::numeric_limits<std::uint64_t>::max())
    {
        return resultFor(
            OperationStatus::IdSpaceExhausted,
            partyId,
            INVALID_INVITATION_ID,
            party.revision);
    }
    party.members.erase(member);
    memberships.erase(target);
    removeInvitationsForTarget(target);
    ++party.revision;
    if (party.revision == std::numeric_limits<std::uint64_t>::max())
    {
        removeInvitationsForParty(partyId);
    }
    if (party.members.size() <= 1)
    {
        disbandById(partyId);
        return resultFor(OperationStatus::Success, partyId);
    }
    return resultFor(
        OperationStatus::Success, partyId, 0, party.revision);
}

OperationResult PartyManager::promoteLeader(
    const DurablePlayerIdentity& actor,
    const DurablePlayerIdentity& target
)
{
    if (!actor.isValid() || !target.isValid())
    {
        return resultFor(OperationStatus::InvalidIdentity);
    }
    if (actor == target)
    {
        return resultFor(OperationStatus::CannotTargetSelf);
    }
    const PartyID partyId = partyIdForPlayer(actor);
    auto found = parties.find(partyId);
    if (found == parties.end())
    {
        return resultFor(OperationStatus::NotInParty);
    }
    Party& party = found->second;
    if (party.leader != actor)
    {
        return resultFor(OperationStatus::NotLeader, partyId);
    }
    if (std::find(party.members.begin(), party.members.end(), target)
        == party.members.end())
    {
        return resultFor(OperationStatus::TargetNotInParty, partyId);
    }
    if (party.revision == std::numeric_limits<std::uint64_t>::max())
    {
        return resultFor(
            OperationStatus::IdSpaceExhausted,
            partyId,
            INVALID_INVITATION_ID,
            party.revision);
    }
    party.leader = target;
    ++party.revision;
    removeInvitationsForParty(partyId);
    return resultFor(
        OperationStatus::Success, partyId, 0, party.revision);
}

OperationResult PartyManager::disbandParty(
    const DurablePlayerIdentity& actor
)
{
    if (!actor.isValid())
    {
        return resultFor(OperationStatus::InvalidIdentity);
    }
    const PartyID partyId = partyIdForPlayer(actor);
    const Party* party = findParty(partyId);
    if (!party)
    {
        return resultFor(OperationStatus::NotInParty);
    }
    if (party->leader != actor)
    {
        return resultFor(OperationStatus::NotLeader, partyId);
    }
    disbandById(partyId);
    return resultFor(OperationStatus::Success, partyId);
}

std::vector<DurablePlayerIdentity> PartyManager::expireInvitations(
    const std::uint64_t currentTick
)
{
    std::vector<DurablePlayerIdentity> affected;
    std::vector<PartyID> affectedParties;
    for (auto iterator = invitations.begin(); iterator != invitations.end(); )
    {
        if (currentTick >= iterator->second.expiresAtTick)
        {
            affected.push_back(iterator->second.target);
            affectedParties.push_back(iterator->second.partyId);
            iterator = invitations.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
    std::sort(affected.begin(), affected.end());
    affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
    std::sort(affectedParties.begin(), affectedParties.end());
    affectedParties.erase(
        std::unique(affectedParties.begin(), affectedParties.end()),
        affectedParties.end());
    for (const PartyID partyId : affectedParties)
    {
        dissolveTransientSingleton(partyId);
    }
    return affected;
}

bool PartyManager::bindOnlinePlayer(
    const DurablePlayerIdentity& identity,
    const int playerSlot,
    std::string& error
)
{
    error.clear();
    if (!identity.isValid() || playerSlot < 0
        || playerSlot >= static_cast<int>(MAX_PERSISTENT_PARTY_MEMBERS))
    {
        error = "invalid durable identity or runtime player slot";
        return false;
    }
    const auto identityBinding = onlineSlotsByIdentity.find(identity);
    if (identityBinding != onlineSlotsByIdentity.end()
        && identityBinding->second != playerSlot)
    {
        error = "durable character identity is already online";
        return false;
    }
    const auto slotBinding = onlineIdentitiesBySlot.find(playerSlot);
    if (slotBinding != onlineIdentitiesBySlot.end()
        && slotBinding->second != identity)
    {
        error = "runtime player slot is already bound to another identity";
        return false;
    }
    onlineSlotsByIdentity[identity] = playerSlot;
    onlineIdentitiesBySlot[playerSlot] = identity;
    return true;
}

void PartyManager::unbindOnlinePlayer(const int playerSlot)
{
    const auto found = onlineIdentitiesBySlot.find(playerSlot);
    if (found == onlineIdentitiesBySlot.end())
    {
        return;
    }
    onlineSlotsByIdentity.erase(found->second);
    onlineIdentitiesBySlot.erase(found);
}

void PartyManager::clearOnlineBindings()
{
    onlineSlotsByIdentity.clear();
    onlineIdentitiesBySlot.clear();
}

int PartyManager::onlineSlotFor(
    const DurablePlayerIdentity& identity
) const
{
    const auto found = onlineSlotsByIdentity.find(identity);
    return found == onlineSlotsByIdentity.end()
        ? -1
        : found->second;
}

const DurablePlayerIdentity* PartyManager::onlineIdentityFor(
    const int playerSlot
) const
{
    const auto found = onlineIdentitiesBySlot.find(playerSlot);
    return found == onlineIdentitiesBySlot.end() ? nullptr : &found->second;
}

std::vector<DurablePlayerIdentity> PartyManager::onlineIdentities() const
{
    std::vector<DurablePlayerIdentity> result;
    result.reserve(onlineIdentitiesBySlot.size());
    for (const auto& entry : onlineIdentitiesBySlot)
    {
        result.push_back(entry.second);
    }
    std::sort(result.begin(), result.end());
    return result;
}

PartyPersistence::Json PartyPersistence::toPersistentJson(
    const PartyManager& manager
)
{
    Json document{
        {"next_id", manager.nextPartyIdValue},
        {"parties", Json::array()}
    };
    for (const PartyID partyId : manager.partyIds())
    {
        const Party& party = manager.parties.at(partyId);
        // A single member is only a transient invitation staging state.
        if (party.members.size() < 2)
        {
            continue;
        }
        Json members = Json::array();
        for (const DurablePlayerIdentity& member : party.members)
        {
            members.push_back(identityToJson(member));
        }
        document["parties"].push_back(Json{
            {"id", party.id},
            {"revision", party.revision},
            {"leader", identityToJson(party.leader)},
            {"members", std::move(members)}
        });
    }
    return document;
}

bool PartyPersistence::loadPersistentJson(
    PartyManager& manager,
    const Json& document,
    std::string& error
)
{
    error.clear();
    if (!document.is_object()
        || !document.contains("next_id")
        || !document.contains("parties")
        || !document["parties"].is_array()
        || document["parties"].size() > MAX_PERSISTENT_PARTIES)
    {
        error = "party root has invalid next_id or parties fields";
        return false;
    }
    PartyManager candidate;
    std::uint64_t nextId = 0;
    if (!unsignedValue(document["next_id"], nextId) || nextId == 0)
    {
        error = "party next_id must be a nonzero unsigned integer";
        return false;
    }
    PartyID maximumId = 0;
    std::size_t partyIndex = 0;
    for (const Json& savedParty : document["parties"])
    {
        const std::string location =
            "party.parties[" + std::to_string(partyIndex) + "]";
        if (!savedParty.is_object()
            || !savedParty.contains("id")
            || !savedParty.contains("revision")
            || !savedParty.contains("leader")
            || !savedParty.contains("members")
            || !savedParty["members"].is_array()
            || savedParty["members"].size() < 2
            || savedParty["members"].size() > MAX_PERSISTENT_PARTY_MEMBERS)
        {
            error = location + " has invalid fields or member count";
            return false;
        }
        Party party;
        if (!unsignedValue(savedParty["id"], party.id)
            || party.id == INVALID_PARTY_ID
            || !unsignedValue(savedParty["revision"], party.revision)
            || party.revision == 0)
        {
            error = location + " has invalid ID or revision";
            return false;
        }
        if (candidate.parties.count(party.id) != 0)
        {
            error = location + " duplicates a party ID";
            return false;
        }
        if (!identityFromJson(
                savedParty["leader"], party.leader, error,
                location + ".leader"))
        {
            return false;
        }
        bool leaderFound = false;
        std::size_t memberIndex = 0;
        for (const Json& savedMember : savedParty["members"])
        {
            DurablePlayerIdentity member;
            if (!identityFromJson(
                    savedMember, member, error,
                    location + ".members["
                        + std::to_string(memberIndex) + "]"))
            {
                return false;
            }
            if (candidate.memberships.count(member) != 0)
            {
                error = location + " contains a duplicate world membership";
                return false;
            }
            leaderFound = leaderFound || member == party.leader;
            candidate.memberships.emplace(member, party.id);
            party.members.push_back(std::move(member));
            ++memberIndex;
        }
        if (!leaderFound)
        {
            error = location + " leader is not a party member";
            return false;
        }
        maximumId = std::max(maximumId, party.id);
        candidate.parties.emplace(party.id, std::move(party));
        ++partyIndex;
    }
    if (nextId <= maximumId)
    {
        error = "party next_id does not advance beyond persisted IDs";
        return false;
    }
    candidate.nextPartyIdValue = nextId;
    manager = std::move(candidate);
    return true;
}

bool PartyPersistence::validatePersistentJson(
    const Json& document,
    std::string& error
)
{
    PartyManager candidate;
    return loadPersistentJson(candidate, document, error);
}
}
