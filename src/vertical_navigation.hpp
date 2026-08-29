/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: vertical_navigation.hpp
    Desc: MapInstance-local vertical navigation transition graph.

-------------------------------------------------------------------------------*/

#pragma once

#include "playable_z.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct map_t;
class Entity;

enum class VerticalNavigationTransitionKind : std::uint8_t
{
    LayerStair,
    PairedEndpoint
};

struct VerticalNavigationPoint
{
    PlayableFloorId playableFloor = DEFAULT_PLAYABLE_FLOOR;
    int tileX = 0;
    int tileY = 0;

    bool operator==(const VerticalNavigationPoint& other) const
    {
        return playableFloor == other.playableFloor
            && tileX == other.tileX
            && tileY == other.tileY;
    }
};

struct VerticalNavigationEdge
{
    VerticalNavigationPoint source;
    VerticalNavigationPoint destination;
    VerticalNavigationTransitionKind kind =
        VerticalNavigationTransitionKind::LayerStair;
    std::int32_t sourcePersistentID = 0;
    std::int32_t targetPersistentID = 0;
};

class VerticalNavigationGraph
{
public:
    bool rebuild(
        const std::string& mapInstanceKey,
        const std::vector<PlayableFloorId>& playableFloors,
        const std::vector<VerticalNavigationEdge>& candidateEdges,
        std::size_t rejectedMetadata = 0);
    void clear(const std::string& mapInstanceKey = {});

    const std::string& instanceKey() const;
    const std::vector<VerticalNavigationEdge>& edges() const;
    std::size_t edgeCount() const;
    std::size_t rejectedCount() const;
    std::uint64_t revision() const;

    bool hasFloor(PlayableFloorId playableFloor) const;
    bool hasDirectedEdge(
        const std::string& mapInstanceKey,
        const VerticalNavigationPoint& source,
        const VerticalNavigationPoint& destination) const;
    std::vector<VerticalNavigationEdge> edgesFrom(
        const std::string& mapInstanceKey,
        const VerticalNavigationPoint& source) const;
    bool canReachFloor(
        const std::string& sourceMapInstanceKey,
        PlayableFloorId sourceFloor,
        const std::string& destinationMapInstanceKey,
        PlayableFloorId destinationFloor) const;

private:
    std::string ownerInstanceKey;
    std::vector<PlayableFloorId> floors;
    std::vector<VerticalNavigationEdge> transitionEdges;
    std::size_t rejectedTransitions = 0;
    std::uint64_t graphRevision = 0;
};

/* Rebuilds the derived graph from existing ZLDR and ZTRN entity metadata. */
bool rebuildVerticalNavigationGraphFromMap(
    VerticalNavigationGraph& graph,
    const std::string& mapInstanceKey,
    map_t& loadedMap);

/* Shared by the graph builder and the existing player stair transaction. */
bool resolveVerticalLayerStairDestination(
    map_t& loadedMap,
    const Entity& sourceStair,
    PlayableFloorId destinationFloor,
    int& destinationTileX,
    int& destinationTileY);
