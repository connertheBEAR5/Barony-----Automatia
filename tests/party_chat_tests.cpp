#include "party_chat.hpp"
#include "world_packet_scope.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
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

AutomatiaParty::DurablePlayerIdentity local(const std::string& name)
{
    return {
        AutomatiaParty::DurableIdentityKind::LocalName,
        AutomatiaParty::normalizeLocalCharacterIdentity(name.c_str())
    };
}

bool bind(
    AutomatiaParty::PartyManager& manager,
    const AutomatiaParty::DurablePlayerIdentity& identity,
    const int slot)
{
    std::string error;
    return manager.bindOnlinePlayer(identity, slot, error);
}

bool addMember(
    AutomatiaParty::PartyManager& manager,
    const AutomatiaParty::DurablePlayerIdentity& leader,
    const AutomatiaParty::DurablePlayerIdentity& member,
    const std::uint64_t tick)
{
    const auto invited = manager.invitePlayer(
        leader, member, tick, 1000, 15);
    return invited
        && manager.acceptInvitation(
            member, invited.invitationId, tick + 1, 15);
}

bool contains(const std::vector<int>& values, const int value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool testWireRoundTripAndNoClientRoutingFields()
{
    using namespace AutomatiaPartyChat;
	EXPECT(channelDisplayName(Channel::Global) == std::string("GLOBAL"));
	EXPECT(channelDisplayName(Channel::Party) == std::string("PARTY"));
	EXPECT(toggleChannel(Channel::Global) == Channel::Party);
	EXPECT(toggleChannel(Channel::Party) == Channel::Global);
    Request request;
    request.senderSlot = 2;
    request.message = "Meet at the exit";
    const auto packet = encodeRequest(request);
    EXPECT(packet.size() == REQUEST_HEADER_BYTES + request.message.size());
    EXPECT(std::memcmp(packet.data(), "PCHT", 4) == 0);
    EXPECT(packet[4] == VERSION);
    EXPECT(packet[5] == static_cast<std::uint8_t>(Channel::Party));
    EXPECT(packet[6] == 2);
    // Every byte after the fixed header is message content. There is no wire
    // location in which a client can claim a PartyID or recipient list.
    EXPECT(std::equal(
        packet.begin() + REQUEST_HEADER_BYTES, packet.end(),
        request.message.begin(), request.message.end()));

    Request decoded;
    EXPECT(decodeRequest(packet.data(), packet.size(), decoded));
    EXPECT(decoded.channel == Channel::Party);
    EXPECT(decoded.senderSlot == 2);
    EXPECT(decoded.message == request.message);
    EXPECT(formatPartyMessage("Alice", decoded.message)
        == "[Party] Alice: Meet at the exit");
    return true;
}

bool testMalformedPacketsAreRejected()
{
    using namespace AutomatiaPartyChat;
    Request request;
    request.senderSlot = 1;
    request.message = "bounded";
    const auto valid = encodeRequest(request);
    Request decoded;
    EXPECT(!decodeRequest(nullptr, valid.size(), decoded));
    EXPECT(!decodeRequest(valid.data(), REQUEST_HEADER_BYTES - 1, decoded));

    auto packet = valid;
    packet[0] = 'X';
    EXPECT(!decodeRequest(packet.data(), packet.size(), decoded));
    packet = valid;
    packet[4] = VERSION + 1;
    EXPECT(!decodeRequest(packet.data(), packet.size(), decoded));
    packet = valid;
    packet[5] = static_cast<std::uint8_t>(Channel::Global);
    EXPECT(!decodeRequest(packet.data(), packet.size(), decoded));
    packet = valid;
    packet[6] = 0;
    EXPECT(!decodeRequest(packet.data(), packet.size(), decoded));
    packet = valid;
    packet[6] = 15;
    EXPECT(!decodeRequest(packet.data(), packet.size(), decoded));
    packet = valid;
    packet[7] = 0;
    packet[8] = 0;
    EXPECT(!decodeRequest(packet.data(), packet.size(), decoded));
    packet = valid;
    packet[8] = static_cast<std::uint8_t>(request.message.size() + 1);
    EXPECT(!decodeRequest(packet.data(), packet.size(), decoded));
    packet = valid;
    packet.pop_back();
    EXPECT(!decodeRequest(packet.data(), packet.size(), decoded));
    packet = valid;
    packet.push_back('x');
    EXPECT(!decodeRequest(packet.data(), packet.size(), decoded));
    packet = valid;
    packet[REQUEST_HEADER_BYTES] = '\n';
    EXPECT(!decodeRequest(packet.data(), packet.size(), decoded));

    request.message.clear();
    EXPECT(encodeRequest(request).empty());
    request.message.assign(MAX_MESSAGE_BYTES + 1, 'x');
    EXPECT(encodeRequest(request).empty());
    request.message = "global must remain on MSGS";
    request.channel = Channel::Global;
    EXPECT(encodeRequest(request).empty());
    return true;
}

bool testAuthenticatedSenderMatch()
{
    using namespace AutomatiaPartyChat;
    Request request;
    request.senderSlot = 2;
    request.message = "hello";
    EXPECT(requestClaimsAuthenticatedSender(request, 2, 4));
    EXPECT(!requestClaimsAuthenticatedSender(request, 1, 4));
    EXPECT(!requestClaimsAuthenticatedSender(request, 0, 4));
    EXPECT(!requestClaimsAuthenticatedSender(request, 2, 2));
    request.senderSlot = 14;
    EXPECT(requestClaimsAuthenticatedSender(request, 14, 15));
    EXPECT(!requestClaimsAuthenticatedSender(request, 14, 4));
    return true;
}

bool testAuthoritativeRecipientsAndPartylessRejection()
{
    using namespace AutomatiaParty;
    using namespace AutomatiaPartyChat;
    PartyManager manager;
    const auto alice = local("Alice");
    const auto bob = local("Bob");
    const auto carol = local("Carol");
    const auto dave = local("Dave");
    const auto erin = local("Erin");
    EXPECT(bind(manager, alice, 0));
    EXPECT(bind(manager, bob, 1));
    EXPECT(bind(manager, carol, 2));
    EXPECT(bind(manager, dave, 3));
    EXPECT(manager.createParty(alice));
    EXPECT(addMember(manager, alice, bob, 10));
    EXPECT(manager.createParty(carol));
    EXPECT(addMember(manager, carol, dave, 20));

    const auto aliceRecipients =
        selectAuthoritativeRecipients(manager, 0, 4);
    EXPECT(aliceRecipients.status == RecipientStatus::Ready);
    EXPECT(aliceRecipients.onlineSlots.size() == 2);
    EXPECT(contains(aliceRecipients.onlineSlots, 0));
    EXPECT(contains(aliceRecipients.onlineSlots, 1));
    EXPECT(!contains(aliceRecipients.onlineSlots, 2));
    EXPECT(!contains(aliceRecipients.onlineSlots, 3));

    EXPECT(bind(manager, erin, 3) == false); // Dave owns the runtime slot.
    manager.unbindOnlinePlayer(3);
    EXPECT(bind(manager, erin, 3));
    const auto partyless =
        selectAuthoritativeRecipients(manager, 3, 4);
    EXPECT(partyless.status == RecipientStatus::NotInParty);
    EXPECT(partyless.onlineSlots.empty());
    const auto unbound =
        selectAuthoritativeRecipients(manager, 7, 15);
    EXPECT(unbound.status == RecipientStatus::InvalidSender);
    return true;
}

bool testOfflineAndDivergentMapMembers()
{
    using namespace AutomatiaParty;
    using namespace AutomatiaPartyChat;
    PartyManager manager;
    const auto alice = local("Alice");
    const auto bob = local("Bob");
    const auto charlie = local("Charlie");
    EXPECT(bind(manager, alice, 0));
    EXPECT(bind(manager, bob, 1));
    EXPECT(bind(manager, charlie, 2));
    EXPECT(manager.createParty(alice));
    EXPECT(addMember(manager, alice, bob, 1));
    EXPECT(addMember(manager, alice, charlie, 3));

    // Routing has no map-instance input: these conceptual divergent locations
    // cannot filter any authoritative party member.
    const std::vector<std::string> mapInstances = {
        "start.lmp#world", "mines.lmp#world", "village.lmp#world"
    };
    EXPECT(mapInstances[0] != mapInstances[1]);
    auto recipients = selectAuthoritativeRecipients(manager, 0, 4);
    EXPECT(recipients.status == RecipientStatus::Ready);
    EXPECT(recipients.onlineSlots.size() == 3);
    EXPECT(contains(recipients.onlineSlots, 0));
    EXPECT(contains(recipients.onlineSlots, 1));
    EXPECT(contains(recipients.onlineSlots, 2));

    manager.unbindOnlinePlayer(2);
    recipients = selectAuthoritativeRecipients(manager, 0, 4);
    EXPECT(recipients.status == RecipientStatus::Ready);
    EXPECT(recipients.onlineSlots.size() == 2);
    EXPECT(!contains(recipients.onlineSlots, 2));
    EXPECT(manager.findPartyForPlayer(alice)->members.size() == 3);
    return true;
}

bool testFourAndFifteenPlayerScaling()
{
    using namespace AutomatiaParty;
    using namespace AutomatiaPartyChat;
    PartyManager four;
    std::vector<DurablePlayerIdentity> fourPlayers;
    for (int slot = 0; slot < 4; ++slot)
    {
        fourPlayers.push_back(local("Four" + std::to_string(slot)));
        EXPECT(bind(four, fourPlayers.back(), slot));
    }
    EXPECT(four.createParty(fourPlayers[0]));
    for (int slot = 1; slot < 4; ++slot)
    {
        EXPECT(addMember(four, fourPlayers[0], fourPlayers[slot], slot * 2));
    }
    const auto standard = selectAuthoritativeRecipients(four, 1, 4);
    EXPECT(standard.status == RecipientStatus::Ready);
    EXPECT(standard.onlineSlots.size() == 4);

    PartyManager fifteen;
    std::vector<DurablePlayerIdentity> players;
    for (int slot = 0; slot < 15; ++slot)
    {
        players.push_back(local("Super" + std::to_string(slot)));
        EXPECT(bind(fifteen, players.back(), slot));
    }
    EXPECT(fifteen.createParty(players[0]));
    for (int slot = 1; slot < 15; ++slot)
    {
        EXPECT(addMember(
            fifteen, players[0], players[slot], 100 + slot * 2));
    }
    const auto expanded =
        selectAuthoritativeRecipients(fifteen, 14, 15);
    EXPECT(expanded.status == RecipientStatus::Ready);
    EXPECT(expanded.onlineSlots.size() == 15);
    for (int slot = 0; slot < 15; ++slot)
    {
        EXPECT(contains(expanded.onlineSlots, slot));
    }
    return true;
}

bool testListenAndHeadlessSlotZeroBehavior()
{
    using namespace AutomatiaParty;
    using namespace AutomatiaPartyChat;
    const auto host = local("Host");
    const auto remote = local("Remote");

    PartyManager listen;
    EXPECT(bind(listen, host, 0));
    EXPECT(bind(listen, remote, 1));
    EXPECT(listen.createParty(host));
    EXPECT(addMember(listen, host, remote, 1));
    const auto hostRoute =
        selectAuthoritativeRecipients(listen, 0, 4);
    EXPECT(hostRoute.status == RecipientStatus::Ready);
    EXPECT(hostRoute.onlineSlots.size() == 2);
    EXPECT(contains(hostRoute.onlineSlots, 0));

    PartyManager headless;
    EXPECT(bind(headless, host, 1));
    EXPECT(bind(headless, remote, 2));
    EXPECT(headless.createParty(host));
    EXPECT(addMember(headless, host, remote, 1));
    const auto remoteRoute =
        selectAuthoritativeRecipients(headless, 1, 4);
    EXPECT(remoteRoute.status == RecipientStatus::Ready);
    EXPECT(remoteRoute.onlineSlots.size() == 2);
    EXPECT(!contains(remoteRoute.onlineSlots, 0));
    return true;
}

bool testPacketScopeIsolationIsUnchanged()
{
    const std::uint8_t partyChat[] = {'P', 'C', 'H', 'T'};
    const std::uint8_t globalChat[] = {'M', 'S', 'G', 'S'};
    const std::uint8_t entityUpdate[] = {'E', 'N', 'T', 'U'};
    EXPECT(!packetUsesActiveMapScope(partyChat, sizeof(partyChat)));
    EXPECT(!packetUsesActiveMapScope(globalChat, sizeof(globalChat)));
    EXPECT(packetUsesActiveMapScope(entityUpdate, sizeof(entityUpdate)));
    return true;
}
}

int main()
{
    const bool passed =
        testWireRoundTripAndNoClientRoutingFields()
        && testMalformedPacketsAreRejected()
        && testAuthenticatedSenderMatch()
        && testAuthoritativeRecipientsAndPartylessRejection()
        && testOfflineAndDivergentMapMembers()
        && testFourAndFifteenPlayerScaling()
        && testListenAndHeadlessSlotZeroBehavior()
        && testPacketScopeIsolationIsUnchanged();
    if (!passed)
    {
        return 1;
    }
    std::cout << "party chat tests passed\n";
    return 0;
}
