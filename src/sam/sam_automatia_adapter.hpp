/*-------------------------------------------------------------------------------

    Narrow engine adapter used by the S.A.M. 2.1 scripting runtimes.

    Framework headers must not include Barony's full game graph.  The implementation
    owns the engine-specific details, including Playable-Z spawn inheritance.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>

class Entity;
class Stat;

Entity* samSpawnGroundItem(
    int runtimeItemType,
    int status,
    int beatitude,
    int count,
    int tileX,
    int tileY);

bool samMonsterHasTrait(const Stat* stats, std::uint64_t traitBit);
