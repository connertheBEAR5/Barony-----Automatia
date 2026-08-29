/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: vertical_navigation.cpp
    Desc: Pure MapInstance-local vertical navigation graph core.

-------------------------------------------------------------------------------*/

#include "vertical_navigation.hpp"

#include <algorithm>
#include <queue>
#include <tuple>
#include <unordered_set>

namespace
{
using EdgeKey = std::tuple<
    PlayableFloorId, int, int,
    PlayableFloorId, int, int,
    std::uint8_t, std::int32_t, std::int32_t>;

EdgeKey edgeKey(const VerticalNavigationEdge& edge)
{
    return {
        edge.source.playableFloor,
        edge.source.tileX,
        edge.source.tileY,
        edge.destination.playableFloor,
        edge.destination.tileX,
        edge.destination.tileY,
        static_cast<std::uint8_t>(edge.kind),
        edge.sourcePersistentID,
        edge.targetPersistentID
    };
}
}

bool VerticalNavigationGraph::rebuild(
    const std::string& mapInstanceKey,
    const std::vector<PlayableFloorId>& playableFloors,
    const std::vector<VerticalNavigationEdge>& candidateEdges,
    const std::size_t rejectedMetadata)
{
    clear(mapInstanceKey);
    if (mapInstanceKey.empty())
    {
        rejectedTransitions = rejectedMetadata + candidateEdges.size();
        return false;
    }

    floors = playableFloors;
    std::sort(floors.begin(), floors.end());
    floors.erase(std::unique(floors.begin(), floors.end()), floors.end());
    if (floors.empty())
    {
        floors.push_back(DEFAULT_PLAYABLE_FLOOR);
    }

    std::unordered_set<PlayableFloorId> knownFloors(
        floors.begin(), floors.end());
    std::vector<EdgeKey> acceptedKeys;
    acceptedKeys.reserve(candidateEdges.size());
    rejectedTransitions = rejectedMetadata;

    for (const VerticalNavigationEdge& edge : candidateEdges)
    {
        const bool valid =
            edge.sourcePersistentID > 0
            && edge.source.playableFloor != edge.destination.playableFloor
            && edge.source.tileX >= 0
            && edge.source.tileY >= 0
            && edge.destination.tileX >= 0
            && edge.destination.tileY >= 0
            && knownFloors.count(edge.source.playableFloor) != 0
            && knownFloors.count(edge.destination.playableFloor) != 0;
        if (!valid)
        {
            ++rejectedTransitions;
            continue;
        }

        const EdgeKey key = edgeKey(edge);
        if (std::find(acceptedKeys.begin(), acceptedKeys.end(), key)
            != acceptedKeys.end())
        {
            ++rejectedTransitions;
            continue;
        }
        acceptedKeys.push_back(key);
        transitionEdges.push_back(edge);
    }

    std::sort(
        transitionEdges.begin(), transitionEdges.end(),
        [](const VerticalNavigationEdge& first,
            const VerticalNavigationEdge& second)
        {
            return edgeKey(first) < edgeKey(second);
        });
    return true;
}

void VerticalNavigationGraph::clear(const std::string& mapInstanceKey)
{
    ownerInstanceKey = mapInstanceKey;
    floors.clear();
    transitionEdges.clear();
    rejectedTransitions = 0;
    ++graphRevision;
    if (graphRevision == 0)
    {
        ++graphRevision;
    }
}

const std::string& VerticalNavigationGraph::instanceKey() const
{
    return ownerInstanceKey;
}

const std::vector<VerticalNavigationEdge>&
VerticalNavigationGraph::edges() const
{
    return transitionEdges;
}

std::size_t VerticalNavigationGraph::edgeCount() const
{
    return transitionEdges.size();
}

std::size_t VerticalNavigationGraph::rejectedCount() const
{
    return rejectedTransitions;
}

std::uint64_t VerticalNavigationGraph::revision() const
{
    return graphRevision;
}

bool VerticalNavigationGraph::hasFloor(
    const PlayableFloorId playableFloor) const
{
    return std::binary_search(floors.begin(), floors.end(), playableFloor);
}

bool VerticalNavigationGraph::hasDirectedEdge(
    const std::string& mapInstanceKey,
    const VerticalNavigationPoint& source,
    const VerticalNavigationPoint& destination) const
{
    if (mapInstanceKey != ownerInstanceKey)
    {
        return false;
    }
    return std::any_of(
        transitionEdges.begin(), transitionEdges.end(),
        [&](const VerticalNavigationEdge& edge)
        {
            return edge.source == source && edge.destination == destination;
        });
}

std::vector<VerticalNavigationEdge> VerticalNavigationGraph::edgesFrom(
    const std::string& mapInstanceKey,
    const VerticalNavigationPoint& source) const
{
    std::vector<VerticalNavigationEdge> result;
    if (mapInstanceKey != ownerInstanceKey)
    {
        return result;
    }
    for (const VerticalNavigationEdge& edge : transitionEdges)
    {
        if (edge.source == source)
        {
            result.push_back(edge);
        }
    }
    return result;
}

bool VerticalNavigationGraph::canReachFloor(
    const std::string& sourceMapInstanceKey,
    const PlayableFloorId sourceFloor,
    const std::string& destinationMapInstanceKey,
    const PlayableFloorId destinationFloor) const
{
    if (sourceMapInstanceKey != ownerInstanceKey
        || destinationMapInstanceKey != ownerInstanceKey
        || !hasFloor(sourceFloor)
        || !hasFloor(destinationFloor))
    {
        return false;
    }
    if (sourceFloor == destinationFloor)
    {
        return true;
    }

    std::queue<PlayableFloorId> pending;
    std::unordered_set<PlayableFloorId> visited;
    pending.push(sourceFloor);
    visited.insert(sourceFloor);
    while (!pending.empty())
    {
        const PlayableFloorId floor = pending.front();
        pending.pop();
        for (const VerticalNavigationEdge& edge : transitionEdges)
        {
            if (edge.source.playableFloor != floor)
            {
                continue;
            }
            if (edge.destination.playableFloor == destinationFloor)
            {
                return true;
            }
            if (visited.insert(edge.destination.playableFloor).second)
            {
                pending.push(edge.destination.playableFloor);
            }
        }
    }
    return false;
}

