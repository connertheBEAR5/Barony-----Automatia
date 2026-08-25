/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: party_chat.hpp
    Desc: Bounded Party-chat request format and authoritative recipient model.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AutomatiaParty
{
class PartyManager;
}

namespace AutomatiaPartyChat
{
constexpr std::uint8_t VERSION = 1;
constexpr std::uint8_t NO_PLAYER_SLOT = 0xff;
constexpr std::size_t REQUEST_HEADER_BYTES = 9;
constexpr std::size_t MAX_MESSAGE_BYTES = 127;

enum class Channel : std::uint8_t
{
    Global = 0,
    Party = 1
};

Channel toggleChannel(Channel channel);
const char* channelDisplayName(Channel channel);

struct Request
{
    Channel channel = Channel::Party;
    std::uint8_t senderSlot = NO_PLAYER_SLOT;
    std::string message;
};

enum class RecipientStatus : std::uint8_t
{
    Ready,
    InvalidSender,
    NotInParty
};

struct RecipientSelection
{
    RecipientStatus status = RecipientStatus::InvalidSender;
    std::vector<int> onlineSlots;
};

/*
 * PCHT intentionally has no PartyID or recipient-list fields. The server
 * derives both from the authenticated runtime binding and PartyManager.
 */
std::vector<std::uint8_t> encodeRequest(const Request& request);
bool isValidMessage(const std::string& message);
bool decodeRequest(
    const std::uint8_t* data,
    std::size_t size,
    Request& request
);

bool requestClaimsAuthenticatedSender(
    const Request& request,
    int authenticatedSlot,
    int maximumPlayers
);

RecipientSelection selectAuthoritativeRecipients(
    const AutomatiaParty::PartyManager& manager,
    int senderSlot,
    int maximumPlayers
);

std::string formatPartyMessage(
    const std::string& senderDisplayName,
    const std::string& message
);
}
