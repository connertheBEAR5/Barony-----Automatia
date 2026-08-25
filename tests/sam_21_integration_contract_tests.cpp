#include "sam/sam_item_limits.hpp"
#include "sam/sam_runtime_id_allocator.hpp"
#include "playable_z.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

namespace
{
bool expect(bool condition, const char* expression)
{
    if ( !condition ) { std::cerr << "FAILED: " << expression << '\n'; }
    return condition;
}

#define EXPECT(expression) do { if (!expect((expression), #expression)) return false; } while (false)

std::string readSource(const char* relative)
{
    std::ifstream input(std::filesystem::path(BARONY_SOURCE_DIR) / relative,
        std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return input ? text.str() : std::string{};
}

bool contains(const std::string& source, const std::string& token)
{
    return source.find(token) != std::string::npos;
}

bool testRuntimeIdLayout()
{
    EXPECT(SAM_ITEM_ID_BASE == 5000);
    EXPECT(SAM_ITEM_ID_LIMIT == 25000);
    EXPECT(SAM_ITEM_CAPACITY == 20000);
    EXPECT(SAM_ITEM_HUNTERS_WORKBENCH == 6000);

    std::set<int> used{5000, 5001, SAM_ITEM_HUNTERS_WORKBENCH};
    EXPECT(firstAvailableSAMRuntimeItemId(
        [&](int id) { return used.count(id) != 0; }) == 5002);

    used.clear();
    for ( int id = SAM_ITEM_ID_BASE; id < SAM_ITEM_ID_LIMIT; ++id )
    {
        used.insert(id);
    }
    EXPECT(firstAvailableSAMRuntimeItemId(
        [&](int id) { return used.count(id) != 0; }) == -1);
    return true;
}

bool testSpatialContextInheritanceContract()
{
    activeRuntimeSpatialContextStorage() = {};
    {
        const ScopedPlayableFloorRuntimeContext scoped(
            SpatialSpawnContext{5, 77, 19});
        EXPECT(activeRuntimePlayableFloor() == 5);
        EXPECT(activeRuntimeStructuralMapLayer() == 19);
        EXPECT(activeRuntimeSpatialContext().spatialRevision == 77);
    }
    EXPECT(activeRuntimePlayableFloor() == DEFAULT_PLAYABLE_FLOOR);
    EXPECT(activeRuntimeStructuralMapLayer() == 0);
    return true;
}

bool testIntegratedSourceSeams()
{
    const std::string logger = readSource("src/sam/framework/sam_logger.hpp");
    const std::string foundation = readSource("src/sam/sam_foundation.cpp");
    const std::string loader = readSource("src/sam/framework/sam_loader.cpp");
    const std::string lua = readSource("src/sam/framework/sam_lua_runtime.cpp");
    const std::string maps = readSource("src/maps.cpp");
    const std::string game = readSource("src/game.cpp");
    const std::string world = readSource("src/sam/framework/sam_world.cpp");
    const std::string itemsHeader = readSource("src/items.hpp");
    EXPECT(!logger.empty() && !foundation.empty() && !loader.empty());

    EXPECT(contains(logger, "SAM_FRAMEWORK_VERSION \"2.1.0\""));
    EXPECT(contains(foundation, "SAMItems::setRuntimeIdResolver"));
    EXPECT(contains(foundation, "SAMLoader::load"));
    EXPECT(contains(foundation, "sam:hunters_workbench"));
    EXPECT(!contains(lua, "MAXPLAYERS == 4"));
    EXPECT(contains(lua, "std::array<double, MAXPLAYERS> g_samMoveSpeed"));
    EXPECT(contains(maps, "SAMRooms::roomsFor(levelset)"));
    EXPECT(contains(game, "SAMLua::dispatchTick"));
    EXPECT(contains(game, "isRegisteredRuntimeItemId(entity->skill[10])"));
    EXPECT(contains(world, "activeRuntimePlayableFloor()"));
    EXPECT(contains(world, "map.setTileAt"));
    EXPECT(!contains(itemsHeader, "nlohmann/json.hpp"));
    EXPECT(!contains(itemsHeader, "sam/framework/"));
    return true;
}
}

int main()
{
    return testRuntimeIdLayout()
        && testSpatialContextInheritanceContract()
        && testIntegratedSourceSeams()
        ? 0 : 1;
}
