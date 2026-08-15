/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: playable_z.hpp
    Desc: Typed data foundation for discrete playable floors.

    Stage Z2A adds discrete-floor spatial-index and entity-collision isolation.
    Rendering, floor-specific tile access, pathfinding, and transitions remain
    later Z2/Z3 work. Legacy content continues to occupy floor Z0.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>

using PlayableFloorId = std::int16_t;

constexpr PlayableFloorId DEFAULT_PLAYABLE_FLOOR = 0;
constexpr std::size_t MAX_PLAYABLE_FLOORS_PER_MAP = 64;

struct SpatialSpawnContext
{
    PlayableFloorId playableFloor = DEFAULT_PLAYABLE_FLOOR;
    std::uint64_t spatialRevision = 0;
};

struct PlayableFloorData
{
    PlayableFloorId id = DEFAULT_PLAYABLE_FLOOR;

    /*
     * Floor Z0 remains the legacy compatibility view through map_t::tiles.
     * For Z0 this vector is intentionally empty. Nonzero floors own a full
     * width * height * MAPLAYERS tile stack here until the Z2 access layer
     * makes floor selection explicit throughout gameplay.
     */
    std::vector<std::int32_t> tiles;

    /* Reserved now so tile attributes do not need another spatial redesign. */
    std::unordered_map<std::int32_t, std::uint32_t> tileAttributes;
};

struct PlayableFloorTable
{
    std::vector<PlayableFloorData> floors{PlayableFloorData{}};

    void resetToDefault()
    {
        floors.clear();
        floors.push_back(PlayableFloorData{});
    }

    bool hasFloor(const PlayableFloorId id) const
    {
        for (const PlayableFloorData& floor : floors)
        {
            if (floor.id == id)
            {
                return true;
            }
        }
        return false;
    }

    PlayableFloorData* find(const PlayableFloorId id)
    {
        for (PlayableFloorData& floor : floors)
        {
            if (floor.id == id)
            {
                return &floor;
            }
        }
        return nullptr;
    }

    const PlayableFloorData* find(const PlayableFloorId id) const
    {
        for (const PlayableFloorData& floor : floors)
        {
            if (floor.id == id)
            {
                return &floor;
            }
        }
        return nullptr;
    }

    bool addFloor(PlayableFloorData floor)
    {
        if (floors.size() >= MAX_PLAYABLE_FLOORS_PER_MAP || hasFloor(floor.id))
        {
            return false;
        }
        floors.push_back(std::move(floor));
        return true;
    }
};
