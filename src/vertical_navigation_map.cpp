/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: vertical_navigation_map.cpp
    Desc: Derives Z4A graph edges from existing ZLDR/ZTRN map entities.

-------------------------------------------------------------------------------*/

#include "vertical_navigation.hpp"

#include "entity.hpp"
#include "main.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace
{
bool pointIsInMap(const map_t& loadedMap, const real_t x, const real_t y)
{
    return std::isfinite(static_cast<double>(x))
        && std::isfinite(static_cast<double>(y))
        && x >= 0.0
        && y >= 0.0
        && x < static_cast<real_t>(loadedMap.width) * 16.0
        && y < static_cast<real_t>(loadedMap.height) * 16.0;
}

bool tileIsPassable(
    const map_t& loadedMap,
    const PlayableFloorId playableFloor,
    const int tileX,
    const int tileY)
{
    return tileX >= 0
        && tileY >= 0
        && tileX < static_cast<int>(loadedMap.width)
        && tileY < static_cast<int>(loadedMap.height)
        && loadedMap.tileAt(tileX, tileY, FLOORLAYER, playableFloor) != 0
        && loadedMap.tileAt(tileX, tileY, OBSTACLELAYER, playableFloor) == 0;
}
}

bool resolveVerticalLayerStairDestination(
    map_t& loadedMap,
    const Entity& sourceStair,
    const PlayableFloorId destinationFloor,
    int& destinationTileX,
    int& destinationTileY)
{
    if ((sourceStair.verticalLayerTransitionDelta != -1
            && sourceStair.verticalLayerTransitionDelta != 1)
        || destinationFloor != sourceStair.playableFloor
            + sourceStair.verticalLayerTransitionDelta
        || !pointIsInMap(loadedMap, sourceStair.x, sourceStair.y)
        || !loadedMap.ensurePlayableFloorGeometry(destinationFloor, false))
    {
        return false;
    }

    static constexpr int stairExitDX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
    static constexpr int stairExitDY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    const int stairRotation = std::clamp(
        static_cast<int>(sourceStair.verticalLayerTransitionRotation), 0, 7);
    const int candidateRotations[8] = {
        stairRotation,
        (stairRotation + 4) & 7,
        (stairRotation + 2) & 7,
        (stairRotation + 6) & 7,
        (stairRotation + 1) & 7,
        (stairRotation + 7) & 7,
        (stairRotation + 3) & 7,
        (stairRotation + 5) & 7
    };
    const int sourceTileX = static_cast<int>(sourceStair.x / 16.0);
    const int sourceTileY = static_cast<int>(sourceStair.y / 16.0);
    for (const int candidateRotation : candidateRotations)
    {
        const int tileX = sourceTileX + stairExitDX[candidateRotation];
        const int tileY = sourceTileY + stairExitDY[candidateRotation];
        if (tileIsPassable(loadedMap, destinationFloor, tileX, tileY))
        {
            destinationTileX = tileX;
            destinationTileY = tileY;
            return true;
        }
    }

    if (tileIsPassable(
            loadedMap, destinationFloor, sourceTileX, sourceTileY))
    {
        destinationTileX = sourceTileX;
        destinationTileY = sourceTileY;
        return true;
    }
    return false;
}

bool rebuildVerticalNavigationGraphFromMap(
    VerticalNavigationGraph& graph,
    const std::string& mapInstanceKey,
    map_t& loadedMap)
{
    std::vector<VerticalNavigationEdge> candidates;
    std::size_t rejected = 0;
    std::unordered_map<std::int32_t, Entity*> entityByPersistentID;
    std::unordered_set<std::int32_t> duplicatePersistentIDs;

    for (node_t* node = loadedMap.entities ? loadedMap.entities->first : nullptr;
        node; node = node->next)
    {
        Entity* entity = static_cast<Entity*>(node->element);
        if (!entity || entity->persistentID <= 0)
        {
            continue;
        }
        const auto inserted = entityByPersistentID.emplace(
            entity->persistentID, entity);
        if (!inserted.second)
        {
            duplicatePersistentIDs.insert(entity->persistentID);
        }
    }

    for (node_t* node = loadedMap.entities ? loadedMap.entities->first : nullptr;
        node; node = node->next)
    {
        Entity* source = static_cast<Entity*>(node->element);
        if (!source)
        {
            continue;
        }

        /* Runtime stair traversal gives ZLDR priority over legacy ZTRN. */
        if (source->verticalLayerTransitionDelta != 0)
        {
            const int destinationFloorRaw =
                static_cast<int>(source->playableFloor)
                + static_cast<int>(source->verticalLayerTransitionDelta);
            int destinationTileX = 0;
            int destinationTileY = 0;
            if (source->persistentID <= 0
                || duplicatePersistentIDs.count(source->persistentID) != 0
                || destinationFloorRaw < 0
                || destinationFloorRaw > MAPLAYERS - CEILINGLAYER - 1
                || !loadedMap.playableFloors.hasFloor(source->playableFloor)
                || !resolveVerticalLayerStairDestination(
                    loadedMap, *source,
                    static_cast<PlayableFloorId>(destinationFloorRaw),
                    destinationTileX, destinationTileY))
            {
                ++rejected;
                continue;
            }
            candidates.push_back({
                {
                    source->playableFloor,
                    static_cast<int>(source->x / 16.0),
                    static_cast<int>(source->y / 16.0)
                },
                {
                    static_cast<PlayableFloorId>(destinationFloorRaw),
                    destinationTileX,
                    destinationTileY
                },
                VerticalNavigationTransitionKind::LayerStair,
                source->persistentID,
                0
            });
            continue;
        }

        if (!source->playableFloorTransitionEnabled)
        {
            continue;
        }
        const auto targetFound = entityByPersistentID.find(
            source->playableFloorTransitionTargetPersistentID);
        Entity* target = targetFound == entityByPersistentID.end()
            ? nullptr : targetFound->second;
        if (source->persistentID <= 0
            || duplicatePersistentIDs.count(source->persistentID) != 0
            || source->playableFloorTransitionTargetPersistentID <= 0
            || duplicatePersistentIDs.count(
                source->playableFloorTransitionTargetPersistentID) != 0
            || source->playableFloorTransitionDestination
                == source->playableFloor
            || !pointIsInMap(loadedMap, source->x, source->y)
            || !target
            || target->playableFloor
                != source->playableFloorTransitionDestination
            || !pointIsInMap(loadedMap, target->x, target->y)
            || !loadedMap.playableFloors.hasFloor(source->playableFloor)
            || !loadedMap.playableFloors.hasFloor(
                source->playableFloorTransitionDestination)
            || !tileIsPassable(
                loadedMap,
                source->playableFloorTransitionDestination,
                static_cast<int>(target->x / 16.0),
                static_cast<int>(target->y / 16.0)))
        {
            ++rejected;
            continue;
        }
        candidates.push_back({
            {
                source->playableFloor,
                static_cast<int>(source->x / 16.0),
                static_cast<int>(source->y / 16.0)
            },
            {
                target->playableFloor,
                static_cast<int>(target->x / 16.0),
                static_cast<int>(target->y / 16.0)
            },
            VerticalNavigationTransitionKind::PairedEndpoint,
            source->persistentID,
            target->persistentID
        });
    }

    std::vector<PlayableFloorId> floors;
    floors.reserve(loadedMap.playableFloors.floors.size());
    for (const PlayableFloorData& floor : loadedMap.playableFloors.floors)
    {
        floors.push_back(floor.id);
    }
    return graph.rebuild(mapInstanceKey, floors, candidates, rejected);
}
