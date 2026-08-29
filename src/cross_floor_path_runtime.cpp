/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: cross_floor_path_runtime.cpp
    Desc: Adapter from the Z4B planner to Barony's existing floor-local A*.

-------------------------------------------------------------------------------*/

#include "cross_floor_path.hpp"

#include "entity.hpp"
#include "main.hpp"
#include "paths.hpp"
#include "world_state.hpp"

#include <cstdlib>

bool generateCrossFloorPath(
    const MapInstance& sourceMapInstance,
    const VerticalNavigationPoint& source,
    const MapInstance& destinationMapInstance,
    const VerticalNavigationPoint& destination,
    Entity* mover,
    Entity* target,
    const GeneratePathTypes pathingType,
    CrossFloorPathRoute& route,
    const bool lavaIsPassable)
{
    route.clear();
    const MapInstance* activeInstance = worldState.activeInstance();
    const auto pointIsInActiveMap = [](const VerticalNavigationPoint& point)
    {
        return point.tileX >= 0
            && point.tileY >= 0
            && point.tileX < static_cast<int>(map.width)
            && point.tileY < static_cast<int>(map.height);
    };
    if (!mover
        || loading
        || !activeInstance
        || activeInstance != &sourceMapInstance
        || sourceMapInstance.key() != destinationMapInstance.key()
        || sourceMapInstance.loadedMap != &map
        || destinationMapInstance.loadedMap != &map
        || mover->playableFloor != source.playableFloor
        || (target && target->playableFloor != destination.playableFloor)
        || !pointIsInActiveMap(source)
        || !pointIsInActiveMap(destination))
    {
        return false;
    }

    const FloorLocalPathQuery localPathQuery = [=](
        const VerticalNavigationPoint& localSource,
        const VerticalNavigationPoint& localDestination,
        std::vector<VerticalNavigationPoint>& localPath) -> bool
    {
        localPath.clear();
        list_t* path = generatePathOnPlayableFloor(
            localSource.tileX,
            localSource.tileY,
            localDestination.tileX,
            localDestination.tileY,
            localSource.playableFloor,
            mover,
            localDestination == destination ? target : nullptr,
            pathingType,
            lavaIsPassable);
        if (!path)
        {
            return false;
        }
        localPath.reserve(list_Size(path));
        for (node_t* node = path->first; node; node = node->next)
        {
            const pathnode_t* pathNode =
                static_cast<const pathnode_t*>(node->element);
            if (!pathNode)
            {
                list_FreeAll(path);
                std::free(path);
                localPath.clear();
                return false;
            }
            localPath.push_back({
                localSource.playableFloor,
                pathNode->x,
                pathNode->y
            });
        }
        list_FreeAll(path);
        std::free(path);
        return !localPath.empty();
    };

    return buildCrossFloorPathRoute(
        sourceMapInstance.verticalNavigation,
        sourceMapInstance.key(), source,
        destinationMapInstance.key(), destination,
        localPathQuery, route);
}
