/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: playable_z.hpp
    Desc: Typed data foundation for discrete playable floors.

    Stage Z2B adds floor-owned geometry access and floor-selected rendering on top
    of the Z2A spatial/collision isolation. Pathfinding and transitions remain
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
// Persistent schemas cannot include main.hpp's legacy MAPLAYERS macro.
// This is the serialized authored structural-layer range: indices 0..31.
constexpr std::size_t AUTHORED_MAP_LAYER_COUNT = 32;

struct SpatialSpawnContext
{
    PlayableFloorId playableFloor = DEFAULT_PLAYABLE_FLOOR;
    std::uint64_t spatialRevision = 0;
    // Structural authored index; deliberately not inferred from playableFloor.
    std::int16_t authoredMapLayer = 0;

    SpatialSpawnContext() = default;

    SpatialSpawnContext(
        const PlayableFloorId floor,
        const std::uint64_t revision,
        const std::int16_t authoredLayer = 0)
        : playableFloor(floor)
        , spatialRevision(revision)
        , authoredMapLayer(authoredLayer)
    {
    }
};


/*
 * Stage Z2C runtime context. Entity::z remains local elevation; this context
 * carries the discrete playable-floor identity, spatial revision, and current
 * structural authored layer while an entity behavior is executing. Runtime-
 * created entities/effects and legacy light-spawn calls can inherit both the
 * executing entity's gameplay floor and render/light layer without
 * changing hundreds of existing call signatures. Outside an entity behavior the
 * context is the legacy Z0/revision-0 default, so map loading/editor placement
 * remains backward compatible.
 */
inline SpatialSpawnContext& activeRuntimeSpatialContextStorage()
{
    static thread_local SpatialSpawnContext context{};
    return context;
}

inline SpatialSpawnContext activeRuntimeSpatialContext()
{
    return activeRuntimeSpatialContextStorage();
}

inline PlayableFloorId activeRuntimePlayableFloor()
{
    return activeRuntimeSpatialContextStorage().playableFloor;
}

inline std::int16_t activeRuntimeStructuralMapLayer()
{
    return activeRuntimeSpatialContextStorage().authoredMapLayer;
}

class ScopedPlayableFloorRuntimeContext
{
public:
    explicit ScopedPlayableFloorRuntimeContext(const SpatialSpawnContext& context)
        : previous(activeRuntimeSpatialContextStorage())
    {
        activeRuntimeSpatialContextStorage() = context;
    }

    ScopedPlayableFloorRuntimeContext(
        PlayableFloorId playableFloor,
        std::uint64_t spatialRevision = 0)
        : ScopedPlayableFloorRuntimeContext(
            SpatialSpawnContext{playableFloor, spatialRevision})
    {
    }

    ~ScopedPlayableFloorRuntimeContext()
    {
        activeRuntimeSpatialContextStorage() = previous;
    }

    ScopedPlayableFloorRuntimeContext(
        const ScopedPlayableFloorRuntimeContext&) = delete;
    ScopedPlayableFloorRuntimeContext& operator=(
        const ScopedPlayableFloorRuntimeContext&) = delete;

private:
    SpatialSpawnContext previous{};
};

inline bool playableFloorsShareRuntimeScope(
    PlayableFloorId first,
    PlayableFloorId second)
{
    return first == second;
}

struct PlayableFloorData
{
    PlayableFloorId id = DEFAULT_PLAYABLE_FLOOR;

    /*
     * Floor Z0 remains the legacy compatibility view through map_t::tiles.
     * For Z0 this vector is intentionally empty. Nonzero floors own a full
     * width * height * MAPLAYERS tile stack here; map_t floor-aware accessors
     * select it explicitly and never silently fall back to Z0.
     */
    std::vector<std::int32_t> tiles;

    /*
     * Stage Z3.3B: nonzero runtime floors may be derived directly from the
     * existing authored 32-layer map stack instead of owning separately
     * authored geometry. Floor N then interprets authored layer N as its
     * floor, N+1 as walls, N+2 as ceiling, and so on. Explicit legacy FLOR
     * chunks remain supported with this flag false.
     */
    bool derivedFromMapLayers = false;

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
