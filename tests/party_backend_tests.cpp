#include "automatia_identity.hpp"
#include "automatia_save.hpp"
#include "party_manager.hpp"
#include "party_protocol.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{
bool expect(const bool condition, const char* expression)
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
        if (!expect(static_cast<bool>(expression), #expression)) \
        { \
            return false; \
        } \
    } while (false)

AutomatiaParty::DurablePlayerIdentity local(const char* name)
{
    return {
        AutomatiaParty::DurableIdentityKind::LocalName,
        AutomatiaParty::normalizeLocalCharacterIdentity(name)
    };
}

bool testDurableIdentity()
{
    using namespace AutomatiaParty;
    EXPECT(normalizeLocalCharacterIdentity("  ALIce \t") == "alice");
    EXPECT(normalizeLocalCharacterIdentity("A  B") == "a  b");
    EXPECT(local("Alice").isValid());
    const DurablePlayerIdentity leadingSpace{
        DurableIdentityKind::LocalName, " Alice"};
    const DurablePlayerIdentity steam{
        DurableIdentityKind::SteamId, "76561198000000001"};
    const DurablePlayerIdentity numericLocal{
        DurableIdentityKind::LocalName, "76561198000000001"};
    const DurablePlayerIdentity leadingZeroSteam{
        DurableIdentityKind::SteamId, "076561198000000001"};
    const DurablePlayerIdentity nonNumericSteam{
        DurableIdentityKind::SteamId, "steam-user"};
    const DurablePlayerIdentity oversized{
        DurableIdentityKind::LocalName, std::string(128, 'a')};
    EXPECT(!leadingSpace.isValid());
    EXPECT(steam.isValid());
    EXPECT(numericLocal.isValid());
    EXPECT(numericLocal != steam);
    EXPECT(!leadingZeroSteam.isValid());
    EXPECT(!nonNumericSteam.isValid());
    EXPECT(!oversized.isValid());
    const std::string utf8Name = "\xc3\x84LICE";
    EXPECT(normalizeLocalCharacterIdentity(utf8Name.c_str())
        == "\xc3\x84lice");
    return true;
}

bool testPartyLifecycleAndAuthority()
{
    using namespace AutomatiaParty;
    PartyManager manager;
    const auto alice = local("Alice");
    const auto bob = local("Bob");
    const auto carol = local("Carol");
    const auto dave = local("Dave");

    const OperationResult created = manager.createParty(alice);
    EXPECT(created);
    EXPECT(created.partyId == 1);
    EXPECT(manager.findPartyForPlayer(alice)->leader == alice);
    EXPECT(!manager.createParty(alice));

    const OperationResult invitation = manager.invitePlayer(
        alice, bob, 100, 50, 15);
    EXPECT(invitation);
    EXPECT(invitation.invitationId != 0);
    EXPECT(!manager.invitePlayer(alice, bob, 101, 50, 15));
    EXPECT(!manager.acceptInvitation(carol, invitation.invitationId, 102, 15));
    EXPECT(manager.acceptInvitation(bob, invitation.invitationId, 102, 15));
    EXPECT(manager.partyIdForPlayer(alice) == manager.partyIdForPlayer(bob));
    EXPECT(manager.findPartyForPlayer(bob)->members.size() == 2);

    const OperationResult carolInvitation = manager.invitePlayer(
        alice, carol, 110, 50, 15);
    EXPECT(carolInvitation);
    EXPECT(manager.declineInvitation(
        carol, carolInvitation.invitationId, 111));
    EXPECT(manager.partyIdForPlayer(carol) == INVALID_PARTY_ID);

    const OperationResult daveInvitation = manager.invitePlayer(
        alice, dave, 120, 10, 15);
    EXPECT(daveInvitation);
    EXPECT(manager.acceptInvitation(
        dave, daveInvitation.invitationId, 129, 15));
    EXPECT(manager.findPartyForPlayer(alice)->members.size() == 3);

    EXPECT(!manager.kickMember(bob, dave));
    EXPECT(!manager.promoteLeader(bob, dave));
    EXPECT(manager.promoteLeader(alice, bob));
    EXPECT(manager.findPartyForPlayer(alice)->leader == bob);
    EXPECT(!manager.disbandParty(alice));
    EXPECT(manager.kickMember(bob, dave));
    EXPECT(manager.partyIdForPlayer(dave) == INVALID_PARTY_ID);
    EXPECT(manager.findPartyForPlayer(alice)->members.size() == 2);

    // Two-member remnants dissolve instead of persisting a singleton.
    EXPECT(manager.leaveParty(alice));
    EXPECT(manager.partyCount() == 0);
    EXPECT(manager.partyIdForPlayer(bob) == INVALID_PARTY_ID);

    EXPECT(manager.createParty(alice));
    const OperationResult disbandInvitation = manager.invitePlayer(
        alice, bob, 200, 50, 15);
    EXPECT(disbandInvitation);
    EXPECT(manager.acceptInvitation(
        bob, disbandInvitation.invitationId, 201, 15));
    EXPECT(manager.disbandParty(alice));
    EXPECT(manager.partyCount() == 0);
    EXPECT(manager.partyIdForPlayer(alice) == INVALID_PARTY_ID);
    EXPECT(manager.partyIdForPlayer(bob) == INVALID_PARTY_ID);
    return true;
}

bool testLeadershipDisconnectAndDeterminism()
{
    using namespace AutomatiaParty;
    PartyManager manager;
    const auto alice = local("Alice");
    const auto bob = local("Bob");
    const auto carol = local("Carol");
    std::string error;

    EXPECT(manager.createParty(alice));
    auto invitation = manager.invitePlayer(alice, bob, 0, 100, 15);
    EXPECT(invitation);
    EXPECT(manager.acceptInvitation(bob, invitation.invitationId, 1, 15));
    invitation = manager.invitePlayer(alice, carol, 2, 100, 15);
    EXPECT(invitation);
    EXPECT(manager.acceptInvitation(carol, invitation.invitationId, 3, 15));

    EXPECT(manager.bindOnlinePlayer(alice, 1, error));
    EXPECT(!manager.bindOnlinePlayer(bob, 1, error));
    EXPECT(manager.bindOnlinePlayer(bob, 2, error));
    EXPECT(!manager.bindOnlinePlayer(alice, 3, error));
    EXPECT(!manager.bindOnlinePlayer(
        carol, static_cast<int>(MAX_PERSISTENT_PARTY_MEMBERS), error));
    manager.unbindOnlinePlayer(1);
    EXPECT(manager.onlineSlotFor(alice) == -1);
    EXPECT(manager.findPartyForPlayer(alice)->leader == alice);
    EXPECT(manager.findPartyForPlayer(alice)->members.size() == 3);

    // Runtime slots are ephemeral: reconnecting elsewhere restores by the
    // same durable identity and never rewrites membership or leadership.
    EXPECT(manager.bindOnlinePlayer(alice, 3, error));
    EXPECT(manager.onlineSlotFor(alice) == 3);
    EXPECT(manager.findPartyForPlayer(alice)->leader == alice);
    EXPECT(manager.findPartyForPlayer(alice)->members.size() == 3);
    manager.unbindOnlinePlayer(3);

    // Explicit leader leave promotes the oldest remaining join-order member.
    EXPECT(manager.leaveParty(alice));
    EXPECT(manager.findPartyForPlayer(bob)->leader == bob);
    EXPECT(manager.findPartyForPlayer(bob)->members[0] == bob);
    EXPECT(manager.findPartyForPlayer(carol)->id
        == manager.findPartyForPlayer(bob)->id);

    PartyManager kickManager;
    EXPECT(kickManager.createParty(alice));
    const auto kickInvitation = kickManager.invitePlayer(
        alice, bob, 0, 100, 15);
    EXPECT(kickInvitation);
    EXPECT(kickManager.acceptInvitation(
        bob, kickInvitation.invitationId, 1, 15));
    EXPECT(kickManager.kickMember(alice, bob));
    EXPECT(kickManager.partyCount() == 0);
    EXPECT(kickManager.partyIdForPlayer(alice) == INVALID_PARTY_ID);
    EXPECT(kickManager.partyIdForPlayer(bob) == INVALID_PARTY_ID);
    return true;
}

bool testInvitationExpiryAndTransientSingleton()
{
    using namespace AutomatiaParty;
    PartyManager manager;
    const auto alice = local("Alice");
    const auto bob = local("Bob");

    EXPECT(manager.createParty(alice));
    const OperationResult invitation = manager.invitePlayer(
        alice, bob, 10, 5, 15);
    EXPECT(invitation);
    EXPECT(!manager.expireInvitations(14).size());
    const auto affected = manager.expireInvitations(15);
    EXPECT(affected.size() == 1 && affected[0] == bob);
    EXPECT(manager.partyCount() == 0);
    EXPECT(!manager.acceptInvitation(bob, invitation.invitationId, 15, 15));

    const auto carol = local("Carol");
    EXPECT(manager.createParty(alice));
    const auto aliceInvite = manager.invitePlayer(alice, carol, 20, 50, 15);
    EXPECT(aliceInvite);
    EXPECT(manager.createParty(bob));
    const auto bobInvite = manager.invitePlayer(bob, carol, 20, 50, 15);
    EXPECT(bobInvite);
    EXPECT(manager.acceptInvitation(
        carol, aliceInvite.invitationId, 21, 15));
    EXPECT(manager.partyCount() == 1);
    EXPECT(manager.partyIdForPlayer(bob) == INVALID_PARTY_ID);

    // Expired entries stop blocking a replacement invite immediately, even
    // between periodic maintenance passes.
    PartyManager staleManager;
    EXPECT(staleManager.createParty(alice));
    auto staleInvite = staleManager.invitePlayer(alice, bob, 30, 50, 15);
    EXPECT(staleInvite);
    EXPECT(staleManager.acceptInvitation(
        bob, staleInvite.invitationId, 31, 15));
    staleInvite = staleManager.invitePlayer(alice, carol, 40, 5, 15);
    EXPECT(staleInvite);
    const auto replacement = staleManager.invitePlayer(
        alice, carol, 45, 5, 15);
    EXPECT(replacement);
    EXPECT(replacement.invitationId != staleInvite.invitationId);
    EXPECT(staleManager.invitationsFor(carol).size() == 1);
    EXPECT(staleManager.invitePlayer(
        alice, local("ZeroLifetime"), 45, 0, 15).status
        == OperationStatus::InvitationExpired);

    // Choosing to create a party invalidates every incoming invitation and
    // cleans up invitation-only singleton parties immediately.
    PartyManager createManager;
    EXPECT(createManager.createParty(alice));
    EXPECT(createManager.invitePlayer(alice, carol, 1, 100, 15));
    EXPECT(createManager.createParty(bob));
    EXPECT(createManager.invitePlayer(bob, carol, 1, 100, 15));
    EXPECT(createManager.createParty(carol));
    EXPECT(createManager.invitationsFor(carol).empty());
    EXPECT(createManager.partyIdForPlayer(alice) == INVALID_PARTY_ID);
    EXPECT(createManager.partyIdForPlayer(bob) == INVALID_PARTY_ID);
    EXPECT(createManager.partyIdForPlayer(carol) != INVALID_PARTY_ID);
    EXPECT(createManager.partyCount() == 1);
    return true;
}

bool testPartySizeUsesServerLimit()
{
    using namespace AutomatiaParty;
    PartyManager manager;
    const auto leader = local("Leader");
    EXPECT(manager.createParty(leader));
    for (std::size_t index = 1;
        index < MAX_PERSISTENT_PARTY_MEMBERS; ++index)
    {
        const DurablePlayerIdentity member{
            DurableIdentityKind::LocalName,
            "member" + std::to_string(index)
        };
        const OperationResult invitation = manager.invitePlayer(
            leader, member, index, 100, MAX_PERSISTENT_PARTY_MEMBERS);
        EXPECT(invitation);
        EXPECT(manager.acceptInvitation(
            member, invitation.invitationId, index,
            MAX_PERSISTENT_PARTY_MEMBERS));
    }
    EXPECT(manager.findPartyForPlayer(leader)->members.size()
        == MAX_PERSISTENT_PARTY_MEMBERS);
    EXPECT(manager.invitePlayer(
        leader, local("Overflow"), 20, 100,
        MAX_PERSISTENT_PARTY_MEMBERS).status
        == OperationStatus::PartyFull);

    // A lower runtime/server limit remains authoritative when configured.
    PartyManager fourPlayerManager;
    EXPECT(fourPlayerManager.createParty(leader));
    for (std::size_t index = 1; index < 4; ++index)
    {
        const DurablePlayerIdentity member{
            DurableIdentityKind::LocalName,
            "limited" + std::to_string(index)
        };
        const OperationResult invitation = fourPlayerManager.invitePlayer(
            leader, member, index, 100, 4);
        EXPECT(invitation);
        EXPECT(fourPlayerManager.acceptInvitation(
            member, invitation.invitationId, index, 4));
    }
    EXPECT(fourPlayerManager.invitePlayer(
        leader, local("Fifth"), 20, 100, 4).status
        == OperationStatus::PartyFull);
    return true;
}

bool testPersistenceAndMigration()
{
    using namespace AutomatiaParty;
    PartyManager manager;
    const auto alice = local("Alice");
    const auto bob = local("Bob");
    const auto carol = local("Carol");

    EXPECT(manager.createParty(alice));
    auto invitation = manager.invitePlayer(alice, bob, 1, 100, 15);
    EXPECT(invitation);
    EXPECT(manager.acceptInvitation(bob, invitation.invitationId, 2, 15));
    EXPECT(manager.createParty(carol));
    const PartyID durableId = manager.partyIdForPlayer(alice);
    const PartyID nextId = manager.nextPartyId();

    const auto saved = manager.toPersistentJson();
    EXPECT(saved["parties"].size() == 1);
    EXPECT(saved["parties"][0]["id"].get<std::uint64_t>() == durableId);
    EXPECT(saved["next_id"].get<std::uint64_t>() == nextId);
    EXPECT(saved.dump().find("slot") == std::string::npos);

    PartyManager restored;
    std::string error;
    EXPECT(restored.loadPersistentJson(saved, error));
    EXPECT(restored.partyIdForPlayer(alice) == durableId);
    EXPECT(restored.partyIdForPlayer(bob) == durableId);
    EXPECT(restored.partyIdForPlayer(carol) == INVALID_PARTY_ID);
    EXPECT(restored.nextPartyId() == nextId);
    EXPECT(restored.findParty(durableId)->leader == alice);

    auto invalid = saved;
    invalid["parties"][0]["members"].push_back(
        invalid["parties"][0]["members"][0]);
    EXPECT(!PartyManager::validatePersistentJson(invalid, error));
    EXPECT(!restored.loadPersistentJson(invalid, error));
    EXPECT(restored.partyIdForPlayer(alice) == durableId);
    EXPECT(restored.partyIdForPlayer(bob) == durableId);
    invalid = saved;
    invalid["parties"][0]["leader"] = {
        {"kind", "local"}, {"value", "not-a-member"}};
    EXPECT(!PartyManager::validatePersistentJson(invalid, error));
    invalid = saved;
    invalid["next_id"] = durableId;
    EXPECT(!PartyManager::validatePersistentJson(invalid, error));
    invalid = saved;
    auto secondParty = invalid["parties"][0];
    secondParty["id"] = durableId + 1;
    secondParty["leader"] = {
        {"kind", "local"}, {"value", "carol"}};
    secondParty["members"][0] = secondParty["leader"];
    invalid["parties"].push_back(secondParty);
    invalid["next_id"] = durableId + 2;
    EXPECT(!PartyManager::validatePersistentJson(invalid, error));

    auto exhausted = saved;
    exhausted["next_id"] = std::numeric_limits<std::uint64_t>::max();
    PartyManager exhaustedManager;
    EXPECT(exhaustedManager.loadPersistentJson(exhausted, error));
    EXPECT(exhaustedManager.createParty(local("Dana")).status
        == OperationStatus::IdSpaceExhausted);
    EXPECT(exhaustedManager.nextPartyId()
        == std::numeric_limits<std::uint64_t>::max());

    auto maximumRevision = saved;
    maximumRevision["parties"][0]["revision"] =
        std::numeric_limits<std::uint64_t>::max();
    PartyManager revisionManager;
    EXPECT(revisionManager.loadPersistentJson(maximumRevision, error));
    EXPECT(revisionManager.promoteLeader(alice, bob).status
        == OperationStatus::IdSpaceExhausted);
    EXPECT(revisionManager.leaveParty(alice).status
        == OperationStatus::IdSpaceExhausted);
    const auto revisionInvite = revisionManager.invitePlayer(
        alice, carol, 10, 100, 15);
    EXPECT(revisionInvite.status == OperationStatus::IdSpaceExhausted);
    EXPECT(revisionInvite.invitationId == INVALID_INVITATION_ID);
    EXPECT(revisionManager.invitationsFor(carol).empty());
    EXPECT(revisionManager.findPartyForPlayer(alice)->revision
        == std::numeric_limits<std::uint64_t>::max());
    EXPECT(revisionManager.findPartyForPlayer(alice)->members.size() == 2);
    EXPECT(revisionManager.partyIdForPlayer(carol) == INVALID_PARTY_ID);

    auto nearlyMaximumRevision = saved;
    nearlyMaximumRevision["parties"][0]["revision"] =
        std::numeric_limits<std::uint64_t>::max() - 1;
    PartyManager terminalRevisionManager;
    EXPECT(terminalRevisionManager.loadPersistentJson(
        nearlyMaximumRevision, error));
    const auto terminalCarolInvite = terminalRevisionManager.invitePlayer(
        alice, carol, 10, 100, 15);
    const auto terminalDaveInvite = terminalRevisionManager.invitePlayer(
        alice, local("Dave"), 10, 100, 15);
    EXPECT(terminalCarolInvite);
    EXPECT(terminalDaveInvite);
    EXPECT(terminalRevisionManager.acceptInvitation(
        carol, terminalCarolInvite.invitationId, 11, 15));
    EXPECT(terminalRevisionManager.findPartyForPlayer(alice)->revision
        == std::numeric_limits<std::uint64_t>::max());
    EXPECT(terminalRevisionManager.invitationsFor(local("Dave")).empty());
    EXPECT(terminalRevisionManager.acceptInvitation(
        local("Dave"), terminalDaveInvite.invitationId, 12, 15).status
        == OperationStatus::InvalidInvitation);

    auto v2 = AutomatiaSave::makeEmptyWorldSave("party-test");
    v2["party"] = saved;
    EXPECT(AutomatiaSave::validate(v2));
    v2["save_transaction_id"] = "0123456789abcdef";
    EXPECT(AutomatiaSave::validate(v2));
    auto invalidTransaction = v2;
    invalidTransaction["save_transaction_id"] = "";
    EXPECT(!AutomatiaSave::validate(invalidTransaction));
    auto missingParty = v2;
    missingParty.erase("party");
    EXPECT(!AutomatiaSave::validate(missingParty));
    auto v1 = missingParty;
    v1["schema_version"] = 1;
    EXPECT(AutomatiaSave::validate(v1));
    auto corruptV2 = v2;
    corruptV2["party"]["parties"][0]["members"][0]["value"] =
        " Alice";
    EXPECT(!AutomatiaSave::validate(corruptV2));
    corruptV2 = v2;
    corruptV2["party"]["parties"][0]["id"] = 0;
    EXPECT(!AutomatiaSave::validate(corruptV2));
    corruptV2 = v2;
    corruptV2["party"]["next_id"] = -1;
    EXPECT(!AutomatiaSave::validate(corruptV2));
    return true;
}

bool testWireProtocol()
{
    using namespace AutomatiaParty;
    using namespace AutomatiaParty::Protocol;
    const auto alice = local("Alice");
    const auto bob = local("Bob");

    Request request;
    request.operation = RequestOperation::Kick;
    request.actorSlot = 2;
    request.requestId = 0x10203040U;
    request.claimedPartyId = 0x0102030405060708ULL;
    request.targetIdentity = bob;
    request.hasTargetIdentity = true;
    const auto requestPacket = encodeRequest(request);
    Request decodedRequest;
    EXPECT(decodeRequest(
        requestPacket.data(), requestPacket.size(), decodedRequest));
    EXPECT(decodedRequest.operation == request.operation);
    EXPECT(decodedRequest.actorSlot == request.actorSlot);
    EXPECT(decodedRequest.requestId == request.requestId);
    EXPECT(decodedRequest.claimedPartyId == request.claimedPartyId);
    EXPECT(decodedRequest.hasTargetIdentity);
    EXPECT(decodedRequest.targetIdentity == bob);
    EXPECT(!decodeRequest(
        requestPacket.data(), requestPacket.size() - 1, decodedRequest));
    for (std::size_t size = 0; size < requestPacket.size(); ++size)
    {
        EXPECT(!decodeRequest(requestPacket.data(), size, decodedRequest));
    }
    auto malformed = requestPacket;
    malformed[4] = 0xff;
    EXPECT(!decodeRequest(malformed.data(), malformed.size(), decodedRequest));
    malformed = requestPacket;
    malformed[6] = static_cast<std::uint8_t>(MAX_WIRE_PARTY_MEMBERS);
    EXPECT(!decodeRequest(malformed.data(), malformed.size(), decodedRequest));
    request.actorSlot = static_cast<std::uint8_t>(MAX_WIRE_PARTY_MEMBERS);
    EXPECT(encodeRequest(request).empty());
    request.actorSlot = 2;

    Result result;
    result.operation = RequestOperation::Kick;
    result.status = OperationStatus::NotLeader;
    result.requestId = request.requestId;
    result.partyId = request.claimedPartyId;
    result.invitationId = 9;
    result.revision = 11;
    const auto resultPacket = encodeResult(result);
    Result decodedResult;
    EXPECT(decodeResult(resultPacket.data(), resultPacket.size(), decodedResult));
    EXPECT(decodedResult.status == result.status);
    EXPECT(decodedResult.revision == result.revision);
    for (std::size_t size = 0; size < resultPacket.size(); ++size)
    {
        EXPECT(!decodeResult(resultPacket.data(), size, decodedResult));
    }

    PartyState state;
    state.recipientSlot = 2;
    state.partyId = 42;
    state.revision = 7;
    state.syncSequence = 1;
    state.leaderIndex = 0;
    state.members = {{alice, 1}, {bob, NO_PLAYER_SLOT}};
    const auto statePacket = encodePartyState(state);
    PartyState decodedState;
    EXPECT(decodePartyState(statePacket.data(), statePacket.size(), decodedState));
    EXPECT(decodedState.partyId == 42);
    EXPECT(decodedState.syncSequence == 1);
    EXPECT(decodedState.members.size() == 2);
    EXPECT(decodedState.members[1].identity == bob);
    EXPECT(decodedState.members[1].onlineSlot == NO_PLAYER_SLOT);
    malformed = statePacket;
    malformed.push_back(0);
    EXPECT(!decodePartyState(malformed.data(), malformed.size(), decodedState));
    for (std::size_t size = 0; size < statePacket.size(); ++size)
    {
        EXPECT(!decodePartyState(statePacket.data(), size, decodedState));
    }
    malformed = statePacket;
    std::fill(malformed.begin() + 24, malformed.begin() + 32, 0);
    EXPECT(!decodePartyState(malformed.data(), malformed.size(), decodedState));
    malformed = statePacket;
    malformed[32] = static_cast<std::uint8_t>(MAX_WIRE_PARTY_MEMBERS);
    EXPECT(!decodePartyState(malformed.data(), malformed.size(), decodedState));
    auto invalidOnlineState = state;
    invalidOnlineState.members[0].onlineSlot =
        static_cast<std::uint8_t>(MAX_WIRE_PARTY_MEMBERS);
    EXPECT(encodePartyState(invalidOnlineState).empty());

    PartyState partyless;
    partyless.recipientSlot = 3;
    partyless.syncSequence = 2;
    const auto partylessPacket = encodePartyState(partyless);
    EXPECT(decodePartyState(
        partylessPacket.data(), partylessPacket.size(), decodedState));
    EXPECT(decodedState.members.empty());
    EXPECT(decodedState.partyId == INVALID_PARTY_ID);

    InvitationList list;
    list.recipientSlot = 2;
    list.syncSequence = 1;
    list.invitations.push_back({9, 42, 999, alice});
    const auto listPacket = encodeInvitationList(list);
    InvitationList decodedList;
    EXPECT(decodeInvitationList(listPacket.data(), listPacket.size(), decodedList));
    EXPECT(decodedList.invitations.size() == 1);
    EXPECT(decodedList.syncSequence == 1);
    EXPECT(decodedList.invitations[0].inviter == alice);
    malformed = listPacket;
    malformed[6] = 9;
    EXPECT(!decodeInvitationList(malformed.data(), malformed.size(), decodedList));
    for (std::size_t size = 0; size < listPacket.size(); ++size)
    {
        EXPECT(!decodeInvitationList(listPacket.data(), size, decodedList));
    }
    malformed = listPacket;
    std::fill(malformed.begin() + 8, malformed.begin() + 16, 0);
    EXPECT(!decodeInvitationList(
        malformed.data(), malformed.size(), decodedList));
    malformed = listPacket;
    std::fill(malformed.begin() + 32, malformed.begin() + 40, 0);
    EXPECT(!decodeInvitationList(
        malformed.data(), malformed.size(), decodedList));
    auto zeroExpiryList = list;
    zeroExpiryList.invitations[0].expiresAtTick = 0;
    EXPECT(encodeInvitationList(zeroExpiryList).empty());
    list.recipientSlot = static_cast<std::uint8_t>(MAX_WIRE_PARTY_MEMBERS);
    EXPECT(encodeInvitationList(list).empty());
    return true;
}

bool testRecipientSnapshotOrdering()
{
    using namespace AutomatiaParty;
    using namespace AutomatiaParty::Protocol;
    const auto alice = local("Alice");
    const auto bob = local("Bob");

    PartyState firstParty;
    firstParty.recipientSlot = 2;
    firstParty.partyId = 10;
    firstParty.revision = 1;
    firstParty.syncSequence = 1;
    firstParty.leaderIndex = 0;
    firstParty.members = {{alice, 1}, {bob, 2}};
    InvitationList firstInvitations;
    firstInvitations.recipientSlot = 2;
    firstInvitations.syncSequence = 1;

    RecipientSnapshotState snapshot;
    EXPECT(snapshot.stageInvitationList(firstInvitations)
        == SnapshotStageResult::Pending);
    EXPECT(snapshot.committedSequence() == 0);
    EXPECT(snapshot.stagePartyState(firstParty)
        == SnapshotStageResult::Committed);
    EXPECT(snapshot.committedSequence() == 1);
    EXPECT(snapshot.partyState().partyId == 10);

    PartyState secondParty = firstParty;
    secondParty.partyId = 20;
    secondParty.revision = 2;
    secondParty.syncSequence = 2;
    InvitationList secondInvitations = firstInvitations;
    secondInvitations.syncSequence = 2;
    secondInvitations.invitations.push_back({1, 20, 100, alice});

    PartyState thirdParty = secondParty;
    thirdParty.partyId = 30;
    thirdParty.revision = 3;
    thirdParty.syncSequence = 3;
    InvitationList thirdInvitations = secondInvitations;
    thirdInvitations.syncSequence = 3;
    thirdInvitations.invitations.clear();

    EXPECT(snapshot.stagePartyState(secondParty)
        == SnapshotStageResult::Pending);
    EXPECT(snapshot.stagePartyState(thirdParty)
        == SnapshotStageResult::Pending);
    EXPECT(snapshot.stageInvitationList(secondInvitations)
        == SnapshotStageResult::Stale);
    EXPECT(snapshot.committedSequence() == 1);
    EXPECT(snapshot.stageInvitationList(thirdInvitations)
        == SnapshotStageResult::Committed);
    EXPECT(snapshot.committedSequence() == 3);
    EXPECT(snapshot.partyState().partyId == 30);
    EXPECT(snapshot.invitationList().invitations.empty());
    EXPECT(snapshot.stagePartyState(secondParty)
        == SnapshotStageResult::Stale);
    EXPECT(snapshot.stageInvitationList(secondInvitations)
        == SnapshotStageResult::Stale);
    EXPECT(snapshot.partyState().partyId == 30);

    PartyState conflicting = thirdParty;
    conflicting.syncSequence = 4;
    InvitationList conflictingInvitations = thirdInvitations;
    conflictingInvitations.syncSequence = 4;
    conflictingInvitations.recipientSlot = 3;
    EXPECT(snapshot.stagePartyState(conflicting)
        == SnapshotStageResult::Pending);
    EXPECT(snapshot.stageInvitationList(conflictingInvitations)
        == SnapshotStageResult::Rejected);
    EXPECT(snapshot.committedSequence() == 3);

    EXPECT(isSyncSequenceNewer(1, 0));
    EXPECT(!isSyncSequenceNewer(1, 1));
    EXPECT(isSyncSequenceNewer(
        1, std::numeric_limits<std::uint64_t>::max()));
    EXPECT(!isSyncSequenceNewer(
        std::numeric_limits<std::uint64_t>::max(), 1));
    return true;
}
}

int main()
{
    return testDurableIdentity()
        && testPartyLifecycleAndAuthority()
        && testLeadershipDisconnectAndDeterminism()
        && testInvitationExpiryAndTransientSingleton()
        && testPartySizeUsesServerLimit()
        && testPersistenceAndMigration()
        && testWireProtocol()
        && testRecipientSnapshotOrdering()
        ? 0
        : 1;
}
