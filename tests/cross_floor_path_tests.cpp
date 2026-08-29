#include "cross_floor_path.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <tuple>
#include <utility>
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

struct LocalQueryHarness
{
    using QueryKey = std::tuple<
        PlayableFloorId, int, int, int, int>;
    std::set<QueryKey> blocked;
    std::vector<PlayableFloorId> queriedFloors;

    bool operator()(
        const VerticalNavigationPoint& source,
        const VerticalNavigationPoint& destination,
        std::vector<VerticalNavigationPoint>& path)
    {
        queriedFloors.push_back(source.playableFloor);
        if (source.playableFloor != destination.playableFloor
            || blocked.count({
                source.playableFloor,
                source.tileX, source.tileY,
                destination.tileX, destination.tileY}) != 0)
        {
            return false;
        }
        path.clear();
        int x = source.tileX;
        int y = source.tileY;
        while (x != destination.tileX)
        {
            x += x < destination.tileX ? 1 : -1;
            path.push_back({source.playableFloor, x, y});
        }
        while (y != destination.tileY)
        {
            y += y < destination.tileY ? 1 : -1;
            path.push_back({source.playableFloor, x, y});
        }
        return !path.empty();
    }
};

bool testSameFloorUsesOnlyOrdinaryLocalPath()
{
    VerticalNavigationGraph graph;
    EXPECT(graph.rebuild(
        "same.lmp#world", {0, 1},
        {stair(0, 8, 8, 1, 8, 9, 100)}));
    LocalQueryHarness local;
    CrossFloorPathRoute route;
    EXPECT(buildCrossFloorPathRoute(
        graph, "same.lmp#world", {0, 1, 1},
        "same.lmp#world", {0, 4, 2},
        std::ref(local), route));
    EXPECT(local.queriedFloors == std::vector<PlayableFloorId>{0});
    EXPECT(route.transitionCount == 0);
    EXPECT(route.steps.size() == 1);
    EXPECT(route.steps[0].kind == CrossFloorPathStepKind::LocalPath);
    EXPECT(route.steps[0].localPath.back()
        == (VerticalNavigationPoint{0, 4, 2}));
    return true;
}

bool testOneTransitionAndSameCoordinatesNeedAnEdge()
{
    VerticalNavigationGraph graph;
    const VerticalNavigationEdge up = stair(0, 3, 1, 1, 3, 1, 200);
    EXPECT(graph.rebuild("tower.lmp#world", {0, 1}, {up}));
    LocalQueryHarness local;
    CrossFloorPathRoute route;
    EXPECT(buildCrossFloorPathRoute(
        graph, "tower.lmp#world", {0, 1, 1},
        "tower.lmp#world", {1, 5, 1},
        std::ref(local), route));
    EXPECT(route.transitionCount == 1);
    EXPECT(route.steps.size() == 3);
    EXPECT(route.steps[1].kind
        == CrossFloorPathStepKind::VerticalTransition);
    EXPECT(route.steps[1].transition.sourcePersistentID == 200);
    EXPECT(route.steps.back().destination
        == (VerticalNavigationPoint{1, 5, 1}));

    VerticalNavigationGraph noEdge;
    EXPECT(noEdge.rebuild("tower.lmp#world", {0, 1}, {}));
    LocalQueryHarness noEdgeLocal;
    EXPECT(!buildCrossFloorPathRoute(
        noEdge, "tower.lmp#world", {0, 2, 2},
        "tower.lmp#world", {1, 2, 2},
        std::ref(noEdgeLocal), route));
    EXPECT(noEdgeLocal.queriedFloors.empty());
    return true;
}

bool testMultipleFloorsAndBlockedStair()
{
    VerticalNavigationGraph graph;
    const VerticalNavigationEdge first = stair(0, 2, 1, 1, 2, 2, 300);
    const VerticalNavigationEdge second = stair(1, 5, 2, 2, 5, 3, 301);
    EXPECT(graph.rebuild("spire.lmp#world", {0, 1, 2}, {first, second}));
    LocalQueryHarness local;
    CrossFloorPathRoute route;
    EXPECT(buildCrossFloorPathRoute(
        graph, "spire.lmp#world", {0, 1, 1},
        "spire.lmp#world", {2, 7, 3},
        std::ref(local), route));
    EXPECT(route.transitionCount == 2);
    EXPECT(route.steps.size() == 5);
    EXPECT(route.steps[1].transition.sourcePersistentID == 300);
    EXPECT(route.steps[3].transition.sourcePersistentID == 301);

    LocalQueryHarness blocked;
    blocked.blocked.insert({1, 2, 2, 5, 2});
    EXPECT(!buildCrossFloorPathRoute(
        graph, "spire.lmp#world", {0, 1, 1},
        "spire.lmp#world", {2, 7, 3},
        std::ref(blocked), route));
    return true;
}

bool testInstanceBoundaryAndLegacyMap()
{
    VerticalNavigationGraph graph;
    EXPECT(graph.rebuild(
        "shared.lmp#instance_a", {0, 1},
        {stair(0, 2, 2, 1, 2, 3, 400)}));
    LocalQueryHarness local;
    CrossFloorPathRoute route;
    EXPECT(!buildCrossFloorPathRoute(
        graph, "shared.lmp#instance_a", {0, 1, 1},
        "shared.lmp#instance_b", {1, 3, 3},
        std::ref(local), route));
    EXPECT(local.queriedFloors.empty());

    VerticalNavigationGraph legacy;
    EXPECT(legacy.rebuild("legacy.lmp#world", {0}, {}));
    EXPECT(buildCrossFloorPathRoute(
        legacy, "legacy.lmp#world", {0, 0, 0},
        "legacy.lmp#world", {0, 2, 0},
        std::ref(local), route));
    EXPECT(route.transitionCount == 0);
    EXPECT(route.steps.size() == 1);
    EXPECT(!buildCrossFloorPathRoute(
        legacy, "legacy.lmp#world", {0, 0, 0},
        "legacy.lmp#world", {1, 0, 0},
        std::ref(local), route));
    return true;
}
}

int main()
{
    const bool passed =
        testSameFloorUsesOnlyOrdinaryLocalPath()
        && testOneTransitionAndSameCoordinatesNeedAnEdge()
        && testMultipleFloorsAndBlockedStair()
        && testInstanceBoundaryAndLegacyMap();
    if (passed)
    {
        std::cout << "Z4B cross-floor path route tests passed.\n";
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
