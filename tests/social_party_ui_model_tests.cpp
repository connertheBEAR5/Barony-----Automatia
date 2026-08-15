#include "social_party_ui_model.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
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

AutomatiaParty::Protocol::PartyState partylessState(
    const std::uint8_t recipient,
    const std::uint64_t sequence)
{
    AutomatiaParty::Protocol::PartyState state;
    state.recipientSlot = recipient;
    state.syncSequence = sequence;
    return state;
}

AutomatiaParty::Protocol::InvitationList invitationState(
    const std::uint8_t recipient,
    const std::uint64_t sequence)
{
    AutomatiaParty::Protocol::InvitationList invitations;
    invitations.recipientSlot = recipient;
    invitations.syncSequence = sequence;
    return invitations;
}

bool authoritativeSnapshot(
    const AutomatiaParty::PartyManager& manager,
    const AutomatiaParty::DurablePlayerIdentity& recipient,
    const std::uint8_t recipientSlot,
    const std::uint64_t sequence,
    AutomatiaParty::Protocol::PartyState& partyState,
    AutomatiaParty::Protocol::InvitationList& invitationList)
{
    using namespace AutomatiaParty;
    using namespace AutomatiaParty::Protocol;

    PartyState outgoingParty;
    outgoingParty.recipientSlot = recipientSlot;
    outgoingParty.syncSequence = sequence;
    if (const Party* party = manager.findPartyForPlayer(recipient))
    {
        outgoingParty.partyId = party->id;
        outgoingParty.revision = party->revision;
        for (std::size_t index = 0; index < party->members.size(); ++index)
        {
            const DurablePlayerIdentity& member = party->members[index];
            if (member == party->leader)
            {
                outgoingParty.leaderIndex =
                    static_cast<std::uint8_t>(index);
            }
            const int onlineSlot = manager.onlineSlotFor(member);
            outgoingParty.members.push_back({member,
                onlineSlot >= 0
                    ? static_cast<std::uint8_t>(onlineSlot)
                    : NO_PLAYER_SLOT});
        }
    }

    InvitationList outgoingInvitations;
    outgoingInvitations.recipientSlot = recipientSlot;
    outgoingInvitations.syncSequence = sequence;
    for (const Invitation& invitation : manager.invitationsFor(recipient))
    {
        outgoingInvitations.invitations.push_back({
            invitation.id, invitation.partyId,
            invitation.expiresAtTick, invitation.inviter});
    }

    PartyState decodedParty;
    InvitationList decodedInvitations;
    const std::vector<std::uint8_t> encodedParty =
        encodePartyState(outgoingParty);
    const std::vector<std::uint8_t> encodedInvitations =
        encodeInvitationList(outgoingInvitations);
    if (!decodePartyState(
            encodedParty.data(), encodedParty.size(), decodedParty)
        || !decodeInvitationList(
            encodedInvitations.data(), encodedInvitations.size(),
            decodedInvitations))
    {
        return false;
    }
    RecipientSnapshotState paired;
    if (paired.stageInvitationList(std::move(decodedInvitations))
            != SnapshotStageResult::Pending
        || paired.stagePartyState(std::move(decodedParty))
            != SnapshotStageResult::Committed)
    {
        return false;
    }
    partyState = paired.partyState();
    invitationList = paired.invitationList();
    return true;
}

bool testPartylessCreateAndAuthoritativeRefresh()
{
    using namespace AutomatiaParty;
    using namespace AutomatiaParty::Protocol;
    using namespace AutomatiaSocial;

    const std::vector<ConnectedPlayer> connected = {
        {1, "Alice", "start.lmp#world"},
        {2, "Bob", "mines.lmp#world"}
    };
    const PartyState empty = partylessState(1, 10);
    const InvitationList noInvites = invitationState(1, 10);
    const ViewModel before = buildViewModel(1, connected, empty, noInvites);
    EXPECT(before.synchronized);
    EXPECT(!before.inParty);
    EXPECT(before.canCreate);
    EXPECT(!before.localLeader);
    EXPECT(before.players.size() == 2);
    EXPECT(!before.players[1].inviteEligible);

    Request request;
    EXPECT(buildRequest(Action::Create, 1, 1, empty, {}, request));
    EXPECT(request.operation == RequestOperation::Create);
    EXPECT(request.actorSlot == 1);
    EXPECT(request.requestId == 1);
    // Building/sending a request does not alter the authoritative projection.
    EXPECT(!before.inParty);

    Result result;
    result.operation = RequestOperation::Create;
    result.status = OperationStatus::Success;
    EXPECT(resultMessage(result) == "Party created.");

    PartyState created = empty;
    created.partyId = 42;
    created.revision = 1;
    created.syncSequence = 11;
    created.leaderIndex = 0;
    created.members = {{local("Alice"), 1}};
    InvitationList createdInvites = invitationState(1, 11);
    const ViewModel after = buildViewModel(
        1, connected, created, createdInvites);
    EXPECT(after.inParty);
    EXPECT(after.localLeader);
    EXPECT(!after.canCreate);
    EXPECT(after.players[1].inviteEligible);
    return true;
}

bool testRosterRemainsVisibleBeforeAuthoritativePair()
{
    using namespace AutomatiaParty::Protocol;
    using namespace AutomatiaSocial;

    const std::vector<ConnectedPlayer> connected = {
        {1, "Alice", "start.lmp#world"},
        {2, "Bob", "mines.lmp#world"}
    };
    PartyState partyState;
    InvitationList invitationList;
    const ViewModel waiting = buildViewModel(
        1, connected, partyState, invitationList);
    EXPECT(!waiting.synchronized);
    EXPECT(waiting.players.size() == 2);
    EXPECT(waiting.players[0].relationship == PlayerRelationship::You);
    EXPECT(waiting.players[1].relationship
        == PlayerRelationship::Connected);
    EXPECT(!waiting.players[0].inviteEligible);
    EXPECT(!waiting.players[1].inviteEligible);
    EXPECT(waiting.members.empty());
    EXPECT(waiting.invitations.empty());
    EXPECT(!waiting.canCreate);
    EXPECT(!waiting.canLeave);
    EXPECT(!waiting.canDisband);
    return true;
}

bool testInvitationsAcceptDeclineAndLiveRefresh()
{
    using namespace AutomatiaParty;
    using namespace AutomatiaParty::Protocol;
    using namespace AutomatiaSocial;

    const std::vector<ConnectedPlayer> connected = {
        {1, "Alice", "start.lmp#world"},
        {2, "Bob", "village.lmp#world"}
    };
    PartyState state = partylessState(2, 20);
    InvitationList invitations = invitationState(2, 20);
    invitations.invitations.push_back({7, 99, 1000, local("Alice")});
    const ViewModel offered = buildViewModel(
        2, connected, state, invitations);
    EXPECT(offered.invitations.size() == 1);
    EXPECT(offered.invitations[0].inviterDisplayName == "Alice");

    ActionSelection selected;
    selected.invitationId = 7;
    selected.invitationPartyId = 99;
    Request accept;
    EXPECT(buildRequest(Action::Accept, 2, 2, state, selected, accept));
    EXPECT(accept.operation == RequestOperation::Accept);
    EXPECT(accept.invitationId == 7);
    EXPECT(accept.claimedPartyId == 99);
    Request decline;
    EXPECT(buildRequest(Action::Decline, 2, 3, state, selected, decline));
    EXPECT(decline.operation == RequestOperation::Decline);

    // An invitation remains until a newer authoritative PTYI removes it.
    EXPECT(offered.invitations.size() == 1);
    InvitationList cleared = invitationState(2, 21);
    PartyState stillPartyless = partylessState(2, 21);
    EXPECT(buildViewModel(2, connected, stillPartyless, cleared)
        .invitations.empty());

    PartyState accepted = partylessState(2, 22);
    accepted.partyId = 99;
    accepted.revision = 2;
    accepted.leaderIndex = 0;
    accepted.members = {{local("Alice"), 1}, {local("Bob"), 2}};
    InvitationList acceptedInvitations = invitationState(2, 22);
    const ViewModel joined = buildViewModel(
        2, connected, accepted, acceptedInvitations);
    EXPECT(joined.inParty);
    EXPECT(joined.members.size() == 2);
    EXPECT(joined.members[1].localPlayer);
    EXPECT(!joined.localLeader);
    return true;
}

bool testPartyRowsLeaderControlsAndRequests()
{
    using namespace AutomatiaParty;
    using namespace AutomatiaParty::Protocol;
    using namespace AutomatiaSocial;

    const DurablePlayerIdentity alice = local("Alice");
    const DurablePlayerIdentity bob = local("Bob");
    const DurablePlayerIdentity charlie = local("Charlie");
    const std::vector<ConnectedPlayer> connected = {
        {1, "Alice", "start.lmp#world"},
        {2, "Bob", "mines.lmp#world"}
    };
    PartyState state;
    state.recipientSlot = 1;
    state.partyId = 123;
    state.revision = 8;
    state.syncSequence = 30;
    state.leaderIndex = 0;
    state.members = {
        {alice, 1}, {bob, 2}, {charlie, NO_PLAYER_SLOT}
    };
    InvitationList invitations = invitationState(1, 30);
    const ViewModel leader = buildViewModel(
        1, connected, state, invitations);
    EXPECT(leader.members.size() == 3);
    EXPECT(leader.members[0].leader);
    EXPECT(leader.members[0].localPlayer);
    EXPECT(leader.members[1].online);
    EXPECT(!leader.members[2].online);
    EXPECT(leader.members[2].displayName == "charlie");
    EXPECT(leader.members[1].canKick);
    EXPECT(leader.members[2].canKick);
    EXPECT(leader.members[1].canPromote);
    EXPECT(leader.canLeave);
    EXPECT(leader.canDisband);

    ActionSelection member;
    member.memberIdentity = charlie;
    member.hasMemberIdentity = true;
    Request request;
    EXPECT(buildRequest(Action::Kick, 1, 4, state, member, request));
    EXPECT(request.operation == RequestOperation::Kick);
    EXPECT(request.hasTargetIdentity);
    EXPECT(request.targetIdentity == charlie);
    EXPECT(buildRequest(Action::Promote, 1, 5, state, member, request));
    EXPECT(request.operation == RequestOperation::Promote);
    EXPECT(buildRequest(Action::Leave, 1, 6, state, {}, request));
    EXPECT(request.operation == RequestOperation::Leave);
    EXPECT(buildRequest(Action::Disband, 1, 7, state, {}, request));
    EXPECT(request.operation == RequestOperation::Disband);

    state.recipientSlot = 2;
    state.syncSequence = 31;
    InvitationList bobInvites = invitationState(2, 31);
    const ViewModel nonLeader = buildViewModel(
        2, connected, state, bobInvites);
    EXPECT(!nonLeader.localLeader);
    EXPECT(!nonLeader.canDisband);
    EXPECT(!nonLeader.members[0].canKick);
    EXPECT(!nonLeader.members[2].canPromote);
    return true;
}

bool testInviteAndDivergentMapsRemainVisible()
{
    using namespace AutomatiaParty;
    using namespace AutomatiaParty::Protocol;
    using namespace AutomatiaSocial;

    std::vector<ConnectedPlayer> connected;
    connected.push_back({0, "Alice", "start.lmp#world"});
    connected.push_back({1, "Bob", "mines.lmp#world"});
    connected.push_back({2, "Charlie", "village.lmp#world"});
    PartyState state;
    state.recipientSlot = 0;
    state.partyId = 55;
    state.revision = 3;
    state.syncSequence = 40;
    state.leaderIndex = 0;
    state.members = {
        {local("Alice"), 0},
        {local("Bob"), 1},
        {local("Charlie"), 2}
    };
    InvitationList invitations = invitationState(0, 40);
    ViewModel model = buildViewModel(0, connected, state, invitations);
    EXPECT(model.members.size() == 3);
    EXPECT(model.members[0].online);
    EXPECT(model.members[1].online);
    EXPECT(model.members[2].online);
    EXPECT(model.players[1].relationship
        == PlayerRelationship::InYourParty);
    EXPECT(model.players[2].relationship
        == PlayerRelationship::InYourParty);

    connected.push_back({3, "Dora", "underworld.lmp#world"});
    model = buildViewModel(0, connected, state, invitations);
    EXPECT(model.players.back().inviteEligible);
    ActionSelection selected;
    selected.playerSlot = 3;
    Request request;
    EXPECT(buildRequest(Action::Invite, 0, 8, state, selected, request));
    EXPECT(request.targetSlot == 3);
    EXPECT(request.claimedPartyId == 55);
    return true;
}

bool testFifteenPlayerPopulationAndScrolling()
{
    using namespace AutomatiaParty::Protocol;
    using namespace AutomatiaSocial;

    std::vector<ConnectedPlayer> connected;
    for (std::uint8_t slot = 0; slot < MAX_WIRE_PARTY_MEMBERS; ++slot)
    {
        connected.push_back({slot,
            "Player " + std::to_string(static_cast<int>(slot) + 1),
            "map-" + std::to_string(slot)});
    }
    PartyState state = partylessState(14, 50);
    InvitationList invitations = invitationState(14, 50);
    const ViewModel model = buildViewModel(
        14, connected, state, invitations);
    EXPECT(model.players.size() == MAX_WIRE_PARTY_MEMBERS);
    EXPECT(model.players.back().relationship == PlayerRelationship::You);
    EXPECT(scrollOffsetForSelection(0, 15, 6, 0) == 0);
    EXPECT(scrollOffsetForSelection(14, 15, 6, 0) == 9);
    EXPECT(scrollOffsetForSelection(8, 15, 6, 9) == 8);
    EXPECT(scrollOffsetForSelection(99, 15, 6, 0) == 9);
    return true;
}

bool testConfiguredPartyCapacityControlsInviteEligibility()
{
    using namespace AutomatiaParty::Protocol;
    using namespace AutomatiaSocial;

    const std::vector<ConnectedPlayer> connected = {
        {0, "Alice", "start.lmp#world"},
        {1, "Eve", "start.lmp#world"}
    };
    PartyState state;
    state.recipientSlot = 0;
    state.partyId = 88;
    state.revision = 2;
    state.syncSequence = 60;
    state.leaderIndex = 0;
    state.members = {
        {local("Alice"), 0},
        {local("Bob"), NO_PLAYER_SLOT},
        {local("Charlie"), NO_PLAYER_SLOT},
        {local("Dora"), NO_PLAYER_SLOT}
    };
    const InvitationList invitations = invitationState(0, 60);
    const ViewModel normalBuild = buildViewModel(
        0, connected, state, invitations, 4);
    EXPECT(normalBuild.partyFull);
    EXPECT(!normalBuild.players[1].inviteEligible);

    const ViewModel expandedBuild = buildViewModel(
        0, connected, state, invitations, 15);
    EXPECT(!expandedBuild.partyFull);
    EXPECT(expandedBuild.players[1].inviteEligible);
    return true;
}

bool testReadableFailuresAndOfflineSteamFallback()
{
    using namespace AutomatiaParty;
    using namespace AutomatiaParty::Protocol;
    using namespace AutomatiaSocial;

    const DurablePlayerIdentity steam{
        DurableIdentityKind::SteamId, "76561198000001234"};
    EXPECT(durableDisplayName(steam) == "Steam player (...1234)");
    EXPECT(durableDisplayName(steam).find("76561198000001234")
        == std::string::npos);

    Result result;
    result.operation = RequestOperation::Kick;
    result.status = OperationStatus::NotLeader;
    EXPECT(resultMessage(result) == "You are not the party leader.");
    result.status = OperationStatus::InvalidInvitation;
    EXPECT(resultMessage(result) == "That invitation is no longer valid.");
    result.status = OperationStatus::TargetAlreadyInParty;
    EXPECT(resultMessage(result) == "That player is already in a party.");
    result.status = OperationStatus::IdSpaceExhausted;
    EXPECT(!resultMessage(result).empty());
    return true;
}

bool testAuthoritativeManagerProtocolAndSocialFlow()
{
    using namespace AutomatiaParty;
    using namespace AutomatiaParty::Protocol;
    using namespace AutomatiaSocial;

    PartyManager manager;
    const DurablePlayerIdentity alice = local("Alice");
    const DurablePlayerIdentity bob = local("Bob");
    std::string error;
    EXPECT(manager.bindOnlinePlayer(alice, 1, error));
    EXPECT(manager.bindOnlinePlayer(bob, 2, error));
    const std::vector<ConnectedPlayer> sameMap = {
        {1, "Alice", "start.lmp#world"},
        {2, "Bob", "start.lmp#world"}
    };

    PartyState aliceState;
    InvitationList aliceInvitations;
    EXPECT(authoritativeSnapshot(
        manager, alice, 1, 100, aliceState, aliceInvitations));
    const ViewModel initial = buildViewModel(
        1, sameMap, aliceState, aliceInvitations, 15);
    EXPECT(initial.canCreate);
    Request request;
    EXPECT(buildRequest(Action::Create, 1, 20,
        aliceState, {}, request));
    EXPECT(!initial.inParty);
    EXPECT(manager.createParty(alice));

    EXPECT(authoritativeSnapshot(
        manager, alice, 1, 101, aliceState, aliceInvitations));
    ViewModel aliceView = buildViewModel(
        1, sameMap, aliceState, aliceInvitations, 15);
    EXPECT(aliceView.inParty);
    EXPECT(aliceView.localLeader);
    ActionSelection inviteSelection;
    inviteSelection.playerSlot = 2;
    EXPECT(buildRequest(Action::Invite, 1, 21,
        aliceState, inviteSelection, request));
    const OperationResult invite = manager.invitePlayer(
        alice, bob, 200, 600, 15);
    EXPECT(invite);

    PartyState bobState;
    InvitationList bobInvitations;
    EXPECT(authoritativeSnapshot(
        manager, bob, 2, 102, bobState, bobInvitations));
    ViewModel bobView = buildViewModel(
        2, sameMap, bobState, bobInvitations, 15);
    EXPECT(bobView.invitations.size() == 1);
    ActionSelection invitationSelection;
    invitationSelection.invitationId =
        bobView.invitations.front().invitationId;
    invitationSelection.invitationPartyId =
        bobView.invitations.front().partyId;
    EXPECT(buildRequest(Action::Accept, 2, 22,
        bobState, invitationSelection, request));
    EXPECT(manager.acceptInvitation(
        bob, invitationSelection.invitationId, 201, 15));

    const std::vector<ConnectedPlayer> divergentMaps = {
        {1, "Alice", "start.lmp#world"},
        {2, "Bob", "mines.lmp#world"}
    };
    EXPECT(authoritativeSnapshot(
        manager, alice, 1, 103, aliceState, aliceInvitations));
    EXPECT(authoritativeSnapshot(
        manager, bob, 2, 103, bobState, bobInvitations));
    aliceView = buildViewModel(
        1, divergentMaps, aliceState, aliceInvitations, 15);
    bobView = buildViewModel(
        2, divergentMaps, bobState, bobInvitations, 15);
    EXPECT(aliceView.members.size() == 2);
    EXPECT(bobView.members.size() == 2);
    EXPECT(aliceView.localLeader);
    EXPECT(!bobView.localLeader);

    ActionSelection memberSelection;
    memberSelection.memberIdentity = bob;
    memberSelection.hasMemberIdentity = true;
    EXPECT(buildRequest(Action::Promote, 1, 23,
        aliceState, memberSelection, request));
    EXPECT(manager.promoteLeader(alice, bob));
    EXPECT(authoritativeSnapshot(
        manager, alice, 1, 104, aliceState, aliceInvitations));
    EXPECT(authoritativeSnapshot(
        manager, bob, 2, 104, bobState, bobInvitations));
    aliceView = buildViewModel(
        1, divergentMaps, aliceState, aliceInvitations, 15);
    bobView = buildViewModel(
        2, divergentMaps, bobState, bobInvitations, 15);
    EXPECT(!aliceView.localLeader);
    EXPECT(bobView.localLeader);

    memberSelection.memberIdentity = alice;
    EXPECT(buildRequest(Action::Kick, 2, 24,
        bobState, memberSelection, request));
    EXPECT(manager.kickMember(bob, alice));
    EXPECT(authoritativeSnapshot(
        manager, alice, 1, 105, aliceState, aliceInvitations));
    EXPECT(authoritativeSnapshot(
        manager, bob, 2, 105, bobState, bobInvitations));
    aliceView = buildViewModel(
        1, divergentMaps, aliceState, aliceInvitations, 15);
    bobView = buildViewModel(
        2, divergentMaps, bobState, bobInvitations, 15);
    EXPECT(!aliceView.inParty && !bobView.inParty);
    EXPECT(aliceView.members.empty() && bobView.members.empty());
    return true;
}
}

int main()
{
    const bool success = testPartylessCreateAndAuthoritativeRefresh()
        && testRosterRemainsVisibleBeforeAuthoritativePair()
        && testInvitationsAcceptDeclineAndLiveRefresh()
        && testPartyRowsLeaderControlsAndRequests()
        && testInviteAndDivergentMapsRemainVisible()
        && testFifteenPlayerPopulationAndScrolling()
        && testConfiguredPartyCapacityControlsInviteEligibility()
        && testReadableFailuresAndOfflineSteamFallback()
        && testAuthoritativeManagerProtocolAndSocialFlow();
    if (!success)
    {
        return 1;
    }
    std::cout << "Social party UI model tests passed.\n";
    return 0;
}
