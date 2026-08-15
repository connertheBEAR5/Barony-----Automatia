/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: party_chat.cpp
    Desc: Bounded Party-chat request format and authoritative recipient model.

-------------------------------------------------------------------------------*/

#include "party_chat.hpp"

#include <algorithm>
#include <cstring>

namespace AutomatiaPartyChat
{
Channel toggleChannel(const Channel channel)
{
    return channel == Channel::Party ? Channel::Global : Channel::Party;
}

const char* channelDisplayName(const Channel channel)
{
    return channel == Channel::Party ? "PARTY" : "GLOBAL";
}

bool isValidMessage(const std::string& message)
{
    if (message.empty() || message.size() > MAX_MESSAGE_BYTES)
    {
        return false;
    }
    for (const unsigned char byte : message)
    {
        if (byte == 0 || byte < 0x20U || byte == 0x7fU)
        {
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> encodeRequest(const Request& request)
{
    if (request.channel != Channel::Party
        || request.senderSlot == 0
        || request.senderSlot >= AutomatiaParty::MAX_PERSISTENT_PARTY_MEMBERS
        || !isValidMessage(request.message))
    {
        return {};
    }
    std::vector<std::uint8_t> packet(
        REQUEST_HEADER_BYTES + request.message.size(), 0);
    std::memcpy(packet.data(), "PCHT", 4);
    packet[4] = VERSION;
    packet[5] = static_cast<std::uint8_t>(request.channel);
    packet[6] = request.senderSlot;
    packet[7] = static_cast<std::uint8_t>(request.message.size() >> 8U);
    packet[8] = static_cast<std::uint8_t>(request.message.size());
    std::memcpy(
        packet.data() + REQUEST_HEADER_BYTES,
        request.message.data(), request.message.size());
    return packet;
}

bool decodeRequest(
    const std::uint8_t* data,
    const std::size_t size,
    Request& request)
{
    if (!data || size < REQUEST_HEADER_BYTES
        || std::memcmp(data, "PCHT", 4) != 0
        || data[4] != VERSION
        || data[5] != static_cast<std::uint8_t>(Channel::Party)
        || data[6] == 0
        || data[6] >= AutomatiaParty::MAX_PERSISTENT_PARTY_MEMBERS)
    {
        return false;
    }
    const std::size_t messageLength =
        (static_cast<std::size_t>(data[7]) << 8U) | data[8];
    if (messageLength == 0 || messageLength > MAX_MESSAGE_BYTES
        || size != REQUEST_HEADER_BYTES + messageLength)
    {
        return false;
    }
    Request decoded;
    decoded.channel = Channel::Party;
    decoded.senderSlot = data[6];
    decoded.message.assign(
        reinterpret_cast<const char*>(data + REQUEST_HEADER_BYTES),
        messageLength);
    if (!isValidMessage(decoded.message))
    {
        return false;
    }
    request = std::move(decoded);
    return true;
}

bool requestClaimsAuthenticatedSender(
    const Request& request,
    const int authenticatedSlot,
    const int maximumPlayers)
{
    return authenticatedSlot > 0
        && authenticatedSlot < maximumPlayers
        && authenticatedSlot
            < static_cast<int>(
                AutomatiaParty::MAX_PERSISTENT_PARTY_MEMBERS)
        && request.senderSlot
            == static_cast<std::uint8_t>(authenticatedSlot);
}

RecipientSelection selectAuthoritativeRecipients(
    const AutomatiaParty::PartyManager& manager,
    const int senderSlot,
    const int maximumPlayers)
{
    RecipientSelection selection;
    if (senderSlot < 0 || senderSlot >= maximumPlayers
        || senderSlot >= static_cast<int>(
            AutomatiaParty::MAX_PERSISTENT_PARTY_MEMBERS))
    {
        return selection;
    }
    const AutomatiaParty::DurablePlayerIdentity* sender =
        manager.onlineIdentityFor(senderSlot);
    if (!sender)
    {
        return selection;
    }
    const AutomatiaParty::Party* party =
        manager.findPartyForPlayer(*sender);
    if (!party)
    {
        selection.status = RecipientStatus::NotInParty;
        return selection;
    }
    selection.status = RecipientStatus::Ready;
    selection.onlineSlots.reserve(party->members.size());
    for (const AutomatiaParty::DurablePlayerIdentity& member
        : party->members)
    {
        const int slot = manager.onlineSlotFor(member);
        if (slot < 0 || slot >= maximumPlayers
            || slot >= static_cast<int>(
                AutomatiaParty::MAX_PERSISTENT_PARTY_MEMBERS)
            || std::find(
                selection.onlineSlots.begin(),
                selection.onlineSlots.end(), slot)
                != selection.onlineSlots.end())
        {
            continue;
        }
        selection.onlineSlots.push_back(slot);
    }
    return selection;
}

std::string formatPartyMessage(
    const std::string& senderDisplayName,
    const std::string& message)
{
    return "[Party] " + senderDisplayName + ": " + message;
}
}
