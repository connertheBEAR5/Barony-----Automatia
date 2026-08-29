#include "vertical_navigation.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
bool expect(const bool condition, const char* expression)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << expression << '\n';
    }
    return condition;
}

#define EXPECT(expression) \
    do \
    { \
        if (!expect((expression), #expression)) \
        { \
            return false; \
        } \
    } while (false)

VerticalNavigationEdge stair(
    const PlayableFloorId sourceFloor,
    const int sourceX,
    const int sourceY,
    const PlayableFloorId destinationFloor,
    const int destinationX,
    const int destinationY,
    const std::int32_t persistentID)
{
    return {
        {sourceFloor, sourceX, sourceY},
        {destinationFloor, destinationX, destinationY},
        VerticalNavigationTransitionKind::LayerStair,
        persistentID,
        0
    };
}

bool testOneFloorAndLegacyZ0()
{
    VerticalNavigationGraph graph;
    EXPECT(graph.rebuild("legacy.lmp#world", {0}, {}));
    EXPECT(graph.instanceKey() == "legacy.lmp#world");
    EXPECT(graph.edgeCount() == 0);
    EXPECT(graph.rejectedCount() == 0);
    EXPECT(graph.hasFloor(0));
    EXPECT(graph.canReachFloor(
        "legacy.lmp#world", 0, "legacy.lmp#world", 0));
    EXPECT(!graph.canReachFloor(
        "legacy.lmp#world", 0, "legacy.lmp#world", 1));
    return true;
}

bool testDirectedAndBidirectionalStairs()
{
    VerticalNavigationGraph graph;
    const VerticalNavigationEdge up = stair(0, 2, 3, 1, 2, 4, 100);
    EXPECT(graph.rebuild("tower.lmp#world", {0, 1}, {up}));
    EXPECT(graph.edgeCount() == 1);
    EXPECT(graph.hasDirectedEdge(
        "tower.lmp#world", up.source, up.destination));
    EXPECT(!graph.hasDirectedEdge(
        "tower.lmp#world", up.destination, up.source));
    EXPECT(graph.canReachFloor(
        "tower.lmp#world", 0, "tower.lmp#world", 1));
    EXPECT(!graph.canReachFloor(
        "tower.lmp#world", 1, "tower.lmp#world", 0));

    const VerticalNavigationEdge down = stair(1, 2, 4, 0, 2, 3, 101);
    EXPECT(graph.rebuild("tower.lmp#world", {0, 1}, {up, down}));
    EXPECT(graph.edgeCount() == 2);
    EXPECT(graph.canReachFloor(
        "tower.lmp#world", 0, "tower.lmp#world", 1));
    EXPECT(graph.canReachFloor(
        "tower.lmp#world", 1, "tower.lmp#world", 0));
    return true;
}

bool testSeveralAndDisconnectedFloors()
{
    VerticalNavigationGraph graph;
    const std::vector<VerticalNavigationEdge> edges = {
        stair(0, 1, 1, 1, 1, 2, 200),
        stair(1, 5, 5, 2, 5, 6, 201),
        stair(2, 8, 8, 3, 8, 9, 202)
    };
    EXPECT(graph.rebuild("spire.lmp#world", {0, 1, 2, 3, 7}, edges));
    EXPECT(graph.canReachFloor(
        "spire.lmp#world", 0, "spire.lmp#world", 3));
    EXPECT(!graph.canReachFloor(
        "spire.lmp#world", 3, "spire.lmp#world", 0));
    EXPECT(!graph.canReachFloor(
        "spire.lmp#world", 0, "spire.lmp#world", 7));
    EXPECT(!graph.canReachFloor(
        "spire.lmp#world", 7, "spire.lmp#world", 0));
    return true;
}

bool testCoordinatesAndMapInstanceBoundary()
{
    VerticalNavigationGraph first;
    const VerticalNavigationEdge edge = stair(0, 4, 4, 1, 4, 4, 300);
    EXPECT(first.rebuild("same.lmp#instance_a", {0, 1}, {edge}));

    // Equal X/Y on two floors is not an edge unless authored metadata adds it.
    VerticalNavigationGraph noTransition;
    EXPECT(noTransition.rebuild("same.lmp#instance_a", {0, 1}, {}));
    EXPECT(!noTransition.canReachFloor(
        "same.lmp#instance_a", 0, "same.lmp#instance_a", 1));
    EXPECT(noTransition.edgesFrom(
        "same.lmp#instance_a", {0, 4, 4}).empty());

    // Even identical map/floor/coordinate metadata cannot cross instances.
    EXPECT(!first.hasDirectedEdge(
        "same.lmp#instance_b", edge.source, edge.destination));
    EXPECT(!first.canReachFloor(
        "same.lmp#instance_a", 0,
        "same.lmp#instance_b", 1));
    EXPECT(first.edgesFrom(
        "same.lmp#instance_b", edge.source).empty());
    return true;
}

bool testMalformedCandidatesAndUpdates()
{
    VerticalNavigationGraph graph;
    const VerticalNavigationEdge valid = stair(0, 1, 1, 1, 1, 2, 400);
    VerticalNavigationEdge sameFloor = stair(0, 1, 1, 0, 2, 2, 401);
    VerticalNavigationEdge missingFloor = stair(0, 1, 1, 9, 2, 2, 402);
    VerticalNavigationEdge negativeCoordinate = stair(0, -1, 1, 1, 2, 2, 403);
    VerticalNavigationEdge missingIdentity = stair(0, 1, 1, 1, 2, 2, 0);
    EXPECT(graph.rebuild(
        "update.lmp#world", {0, 1},
        {valid, valid, sameFloor, missingFloor,
            negativeCoordinate, missingIdentity}, 2));
    EXPECT(graph.edgeCount() == 1);
    EXPECT(graph.rejectedCount() == 7);
    const std::uint64_t firstRevision = graph.revision();

    EXPECT(graph.rebuild("update.lmp#world", {0, 1}, {}));
    EXPECT(graph.edgeCount() == 0);
    EXPECT(graph.revision() > firstRevision);
    EXPECT(!graph.canReachFloor(
        "update.lmp#world", 0, "update.lmp#world", 1));
    EXPECT(!graph.rebuild({}, {0, 1}, {valid}));
    return true;
}
}

int main()
{
    const bool passed =
        testOneFloorAndLegacyZ0()
        && testDirectedAndBidirectionalStairs()
        && testSeveralAndDisconnectedFloors()
        && testCoordinatesAndMapInstanceBoundary()
        && testMalformedCandidatesAndUpdates();
    if (passed)
    {
        std::cout << "Z4A vertical navigation graph tests passed.\n";
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

