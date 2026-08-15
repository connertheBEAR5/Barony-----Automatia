/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: automatia_identity.cpp
    Desc: Durable character identity shared by character saves and world systems.

-------------------------------------------------------------------------------*/

#include "automatia_identity.hpp"

#include <algorithm>
#include <charconv>
#include <functional>
#include <limits>

namespace AutomatiaParty
{
namespace
{
bool safeIdentityText(const std::string& value)
{
    if (value.empty() || value.size() > MAX_DURABLE_IDENTITY_BYTES)
    {
        return false;
    }
    return std::none_of(
        value.begin(),
        value.end(),
        [](const unsigned char character)
        {
            return character < 0x20 || character == 0x7f;
        }
    );
}
}

std::string normalizeLocalCharacterIdentity(const char* text)
{
    std::string result = text ? text : "";
    const auto isSpace = [](const unsigned char character)
    {
        // Durable identity normalization must not depend on the process
        // locale. These are exactly the ASCII whitespace bytes accepted by
        // the previous C-locale implementation.
        return character == ' ' || character == '\t'
            || character == '\n' || character == '\r'
            || character == '\f' || character == '\v';
    };
    while (!result.empty()
        && isSpace(static_cast<unsigned char>(result.front())))
    {
        result.erase(result.begin());
    }
    while (!result.empty()
        && isSpace(static_cast<unsigned char>(result.back())))
    {
        result.pop_back();
    }
    for (char& character : result)
    {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte >= 'A' && byte <= 'Z')
        {
            character = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return result;
}

bool DurablePlayerIdentity::isValid() const
{
    if (!safeIdentityText(value))
    {
        return false;
    }
    switch (kind)
    {
        case DurableIdentityKind::LocalName:
            return value == normalizeLocalCharacterIdentity(value.c_str());
        case DurableIdentityKind::SteamId:
        {
            if (value.front() == '0')
            {
                return false;
            }
            std::uint64_t parsed = 0;
            const char* begin = value.data();
            const char* end = begin + value.size();
            const std::from_chars_result result =
                std::from_chars(begin, end, parsed);
            return result.ec == std::errc{}
                && result.ptr == end
                && parsed != 0
                && std::to_string(parsed) == value;
        }
        default:
            return false;
    }
}

std::size_t DurablePlayerIdentityHash::operator()(
    const DurablePlayerIdentity& identity
) const
{
    const std::size_t valueHash = std::hash<std::string>{}(identity.value);
    const std::size_t kindHash = static_cast<std::size_t>(identity.kind);
    return valueHash ^ (kindHash + 0x9e3779b9U
        + (valueHash << 6U) + (valueHash >> 2U));
}

const char* durableIdentityKindName(const DurableIdentityKind kind)
{
    switch (kind)
    {
        case DurableIdentityKind::LocalName:
            return "local";
        case DurableIdentityKind::SteamId:
            return "steam";
        default:
            return "invalid";
    }
}

bool durableIdentityKindFromName(
    const std::string& name,
    DurableIdentityKind& kind
)
{
    if (name == "local")
    {
        kind = DurableIdentityKind::LocalName;
        return true;
    }
    if (name == "steam")
    {
        kind = DurableIdentityKind::SteamId;
        return true;
    }
    return false;
}
}
