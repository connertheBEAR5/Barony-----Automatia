/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: cross_floor_path.cpp
    Desc: Pure Z4B cross-floor route planner.

-------------------------------------------------------------------------------*/

#include "cross_floor_path.hpp"

#include <limits>
#include <map>
#include <queue>
#include <tuple>
#include <utility>

namespace
{
using PointKey = std::tuple<PlayableFloorId, int, int>;

PointKey pointKey(const VerticalNavigationPoint& point)
{
    return {point.playableFloor, point.tileX, point.tileY};
}

struct RouteCandidate
{
    VerticalNavigationPoint location;
    CrossFloorPathRoute route;
    std::size_t cost = 0;
    std::uint64_t order = 0;
};

struct RouteCandidateLater
{
    bool operator()(const RouteCandidate& first, const RouteCandidate& second) const
    {
        return first.cost != second.cost
            ? first.cost > second.cost
            : first.order > second.order;
    }
};

void appendLocalStep(
    CrossFloorPathRoute& route,
    const VerticalNavigationPoint& source,
    const VerticalNavigationPoint& destination,
    std::vector<VerticalNavigationPoint> path)
{
    if (source == destination)
    {
        return;
    }
    route.localNodeCount += path.size();
    route.steps.push_back({
        CrossFloorPathStepKind::LocalPath,
        source,
        destination,
        std::move(path),
        {}
    });
}

void appendTransitionStep(
    CrossFloorPathRoute& route,
    const VerticalNavigationEdge& edge)
{
    ++route.transitionCount;
    route.steps.push_back({
        CrossFloorPathStepKind::VerticalTransition,
        edge.source,
        edge.destination,
        {},
        edge
    });
}

bool queryLocalPath(
    const FloorLocalPathQuery& query,
    const VerticalNavigationPoint& source,
    const VerticalNavigationPoint& destination,
    std::vector<VerticalNavigationPoint>& path)
{
    path.clear();
    if (source.playableFloor != destination.playableFloor)
    {
        return false;
    }
    if (source == destination)
    {
        return true;
    }
    if (!query || !query(source, destination, path))
    {
        path.clear();
        return false;
    }
    for (const VerticalNavigationPoint& point : path)
    {
        if (point.playableFloor != source.playableFloor)
        {
            path.clear();
            return false;
        }
    }
    return !path.empty() && path.back() == destination;
}
}

void CrossFloorPathRoute::clear()
{
    mapInstanceKey.clear();
    source = {};
    destination = {};
    steps.clear();
    localNodeCount = 0;
    transitionCount = 0;
}

bool buildCrossFloorPathRoute(
    const VerticalNavigationGraph& graph,
    const std::string& sourceMapInstanceKey,
    const VerticalNavigationPoint& source,
    const std::string& destinationMapInstanceKey,
    const VerticalNavigationPoint& destination,
    const FloorLocalPathQuery& localPathQuery,
    CrossFloorPathRoute& route)
{
    route.clear();
    if (sourceMapInstanceKey.empty()
        || sourceMapInstanceKey != destinationMapInstanceKey
        || sourceMapInstanceKey != graph.instanceKey()
        || !graph.hasFloor(source.playableFloor)
        || !graph.hasFloor(destination.playableFloor)
        || source.tileX < 0 || source.tileY < 0
        || destination.tileX < 0 || destination.tileY < 0
        || !localPathQuery)
    {
        return false;
    }

    CrossFloorPathRoute initial;
    initial.mapInstanceKey = sourceMapInstanceKey;
    initial.source = source;
    initial.destination = destination;

    // Ordinary local navigation remains the direct/default path. Do not scan
    // any transition or other floor for a same-floor request.
    if (source.playableFloor == destination.playableFloor)
    {
        std::vector<VerticalNavigationPoint> localPath;
        if (!queryLocalPath(
                localPathQuery, source, destination, localPath))
        {
            return false;
        }
        appendLocalStep(initial, source, destination, std::move(localPath));
        route = std::move(initial);
        return true;
    }

    std::priority_queue<
        RouteCandidate,
        std::vector<RouteCandidate>,
        RouteCandidateLater> pending;
    std::map<PointKey, std::size_t> bestCostAtPoint;
    std::uint64_t nextOrder = 1;
    pending.push({source, initial, 0, 0});
    bestCostAtPoint.emplace(pointKey(source), 0);

    bool foundRoute = false;
    std::size_t bestRouteCost = std::numeric_limits<std::size_t>::max();
    while (!pending.empty())
    {
        RouteCandidate candidate = pending.top();
        pending.pop();
        const auto known = bestCostAtPoint.find(pointKey(candidate.location));
        if (known == bestCostAtPoint.end() || candidate.cost != known->second)
        {
            continue;
        }
        if (candidate.cost >= bestRouteCost)
        {
            continue;
        }

        if (candidate.location.playableFloor == destination.playableFloor)
        {
            std::vector<VerticalNavigationPoint> finalPath;
            if (queryLocalPath(
                    localPathQuery, candidate.location, destination,
                    finalPath))
            {
                const std::size_t finalCost = candidate.cost + finalPath.size();
                if (finalCost < bestRouteCost)
                {
                    CrossFloorPathRoute completed = candidate.route;
                    appendLocalStep(
                        completed, candidate.location, destination,
                        std::move(finalPath));
                    route = std::move(completed);
                    bestRouteCost = finalCost;
                    foundRoute = true;
                }
            }
        }

        for (const VerticalNavigationEdge& edge : graph.edges())
        {
            if (edge.source.playableFloor
                != candidate.location.playableFloor)
            {
                continue;
            }
            std::vector<VerticalNavigationPoint> localPath;
            if (!queryLocalPath(
                    localPathQuery, candidate.location, edge.source,
                    localPath))
            {
                continue;
            }
            const std::size_t nextCost =
                candidate.cost + localPath.size() + 1;
            if (nextCost >= bestRouteCost)
            {
                continue;
            }
            const PointKey destinationKey = pointKey(edge.destination);
            const auto previous = bestCostAtPoint.find(destinationKey);
            if (previous != bestCostAtPoint.end()
                && previous->second <= nextCost)
            {
                continue;
            }

            CrossFloorPathRoute nextRoute = candidate.route;
            appendLocalStep(
                nextRoute, candidate.location, edge.source,
                std::move(localPath));
            appendTransitionStep(nextRoute, edge);
            bestCostAtPoint[destinationKey] = nextCost;
            pending.push({
                edge.destination,
                std::move(nextRoute),
                nextCost,
                nextOrder++
            });
        }
    }
    if (!foundRoute)
    {
        route.clear();
    }
    return foundRoute;
}
