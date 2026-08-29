/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: cross_floor_path.hpp
    Desc: Z4B route plans combining floor-local paths and Z4A transitions.

-------------------------------------------------------------------------------*/

#pragma once

#include "vertical_navigation.hpp"

#include <functional>
#include <string>
#include <vector>

class Entity;
struct MapInstance;
enum GeneratePathTypes : int;

enum class CrossFloorPathStepKind : std::uint8_t
{
    LocalPath,
    VerticalTransition
};

struct CrossFloorPathStep
{
    CrossFloorPathStepKind kind = CrossFloorPathStepKind::LocalPath;
    VerticalNavigationPoint source;
    VerticalNavigationPoint destination;
    // Matches generatePath(): the starting tile is omitted and the destination
    // tile is included. Empty means the route is already at this point.
    std::vector<VerticalNavigationPoint> localPath;
    VerticalNavigationEdge transition;
};

struct CrossFloorPathRoute
{
    std::string mapInstanceKey;
    VerticalNavigationPoint source;
    VerticalNavigationPoint destination;
    std::vector<CrossFloorPathStep> steps;
    std::size_t localNodeCount = 0;
    std::size_t transitionCount = 0;

    void clear();
};

using FloorLocalPathQuery = std::function<bool(
    const VerticalNavigationPoint& source,
    const VerticalNavigationPoint& destination,
    std::vector<VerticalNavigationPoint>& path)>;

/*
 * Pure Z4B planner. It only asks for ordinary paths on the current candidate
 * floor and traverses explicit Z4A edges; it never treats equal X/Y on two
 * floors as adjacency.
 */
bool buildCrossFloorPathRoute(
    const VerticalNavigationGraph& graph,
    const std::string& sourceMapInstanceKey,
    const VerticalNavigationPoint& source,
    const std::string& destinationMapInstanceKey,
    const VerticalNavigationPoint& destination,
    const FloorLocalPathQuery& localPathQuery,
    CrossFloorPathRoute& route);

/*
 * Runtime adapter over Barony's existing generatePath() implementation.
 * Both MapInstance arguments must identify the currently active loaded map.
 * This is a query only: it does not assign an Entity::path or change AI state.
 */
bool generateCrossFloorPath(
    const MapInstance& sourceMapInstance,
    const VerticalNavigationPoint& source,
    const MapInstance& destinationMapInstance,
    const VerticalNavigationPoint& destination,
    Entity* mover,
    Entity* target,
    GeneratePathTypes pathingType,
    CrossFloorPathRoute& route,
    bool lavaIsPassable = false);

