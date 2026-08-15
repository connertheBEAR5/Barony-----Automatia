/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: automatia_identity.hpp
    Desc: Durable character identity shared by character saves and world systems.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace AutomatiaParty
{
enum class DurableIdentityKind : std::uint8_t
{
    LocalName = 1,
    SteamId = 2
};

struct DurablePlayerIdentity
{
    DurableIdentityKind kind = DurableIdentityKind::LocalName;
    std::string value;

    bool isValid() const;

    bool operator==(const DurablePlayerIdentity& other) const
    {
        return kind == other.kind && value == other.value;
    }

    bool operator!=(const DurablePlayerIdentity& other) const
    {
        return !(*this == other);
    }

    bool operator<(const DurablePlayerIdentity& other) const
    {
        return kind < other.kind
            || (kind == other.kind && value < other.value);
    }
};

struct DurablePlayerIdentityHash
{
    std::size_t operator()(const DurablePlayerIdentity& identity) const;
};

constexpr std::size_t MAX_DURABLE_IDENTITY_BYTES = 127;

/*
 * This is deliberately the same byte-wise trim/lower operation used by
 * Automatia local character saves. It does not collapse internal whitespace
 * or perform Unicode normalization.
 */
std::string normalizeLocalCharacterIdentity(const char* text);

const char* durableIdentityKindName(DurableIdentityKind kind);
bool durableIdentityKindFromName(
    const std::string& name,
    DurableIdentityKind& kind
);
}
