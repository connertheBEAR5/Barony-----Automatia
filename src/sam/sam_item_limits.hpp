/*-------------------------------------------------------------------------------

    Automatia / S.A.M shared item slot contract.

    This header deliberately contains constants only. Both the engine item table and
    the S.A.M runtime registry include it, avoiding duplicate definitions and avoiding
    any dependency on the full framework or nlohmann/json.hpp.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>

inline constexpr int SAM_ITEM_ID_BASE = 5000;
inline constexpr int NUM_ITEM_SLOTS = 25000;
inline constexpr int SAM_ITEM_ID_LIMIT = NUM_ITEM_SLOTS;
inline constexpr int SAM_ITEM_CAPACITY =
    SAM_ITEM_ID_LIMIT - SAM_ITEM_ID_BASE;

// S.A.M. 2.1 currently assigns its built-in Hunter's Workbench at this fixed slot.
// The Automatia adapter must reserve/validate this slot before enabling the built-in;
// keeping the number in the shared contract makes a collision impossible to overlook.
inline constexpr int SAM_BUILTIN_ITEM_ID_BASE = 6000;
inline constexpr int SAM_ITEM_HUNTERS_WORKBENCH = 6000;

// Behaviour traits are data carried by the engine's existing lightweight item
// definition.  They live here rather than in a framework header so items.hpp does
// not acquire the S.A.M registry, JSON, Lua, or JavaScript dependency graph.
namespace SAMItemTrait
{
inline constexpr std::uint64_t RANGED = 1ULL << 0;
inline constexpr std::uint64_t QUIVER = 1ULL << 1;
inline constexpr std::uint64_t FOCI = 1ULL << 2;
inline constexpr std::uint64_t INSTRUMENT = 1ULL << 3;
inline constexpr std::uint64_t THROWN_BALL = 1ULL << 4;
inline constexpr std::uint64_t SHIELD_SLOT = 1ULL << 5;
inline constexpr std::uint64_t POTION_BAD = 1ULL << 6;
inline constexpr std::uint64_t AUTOMATON_FOOD = 1ULL << 7;
inline constexpr std::uint64_t TINKER_THROWABLE = 1ULL << 8;
inline constexpr std::uint64_t USABLE = 1ULL << 9;
inline constexpr std::uint64_t BEATITUDE_AC = 1ULL << 10;
}

static_assert(
    SAM_ITEM_CAPACITY == 20000,
    "Automatia must provide exactly 20,000 S.A.M item slots"
);
