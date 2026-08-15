#include "../src/main.hpp"
#include "../src/game.hpp"
#include "../src/stat.hpp"
#include "../src/entity.hpp"
#include "../src/collision.hpp"
#include "../src/files.hpp"
#include "../src/magic/magic.hpp"
#include "../src/monster.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{
list_t stageEntities{};
list_t stageCreatures{};
list_t stageWorldUI{};
std::array<bool, 4096> stageAnimatedTiles{};
std::array<bool, 4096> stageSwimmingTiles{};
std::array<bool, 4096> stageLavaTiles{};

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

std::size_t tileIndex(
    const std::size_t x,
    const std::size_t y,
    const std::size_t layer,
    const std::size_t height,
    const std::size_t layers = MAPLAYERS)
{
    return layer + y * layers + x * layers * height;
}

void clearGlobalMapHarness()
{
    loading = true;
    list_FreeAll(&stageEntities);
    list_FreeAll(&stageCreatures);
    list_FreeAll(&stageWorldUI);
    TileEntityList.emptyGridEntities();
    list_FreeAll(&entitiesdeleted);
    map.entities_map.clear();
    std::free(map.tiles);
    map.tiles = nullptr;
    map.width = 0;
    map.height = 0;
    map.numLayers = MAPLAYERS;
    map.playableFloors.resetToDefault();
    map.entities = &stageEntities;
    map.creatures = &stageCreatures;
    map.worldUI = &stageWorldUI;
    map.name[0] = '\0';
    map.author[0] = '\0';
    map.filename[0] = '\0';
    std::memset(map.flags, 0, sizeof(map.flags));
}

bool resetGlobalMapHarness(
    const std::size_t width,
    const std::size_t height,
    const Sint32 floorTile)
{
    clearGlobalMapHarness();
    map.width = static_cast<Uint32>(width);
    map.height = static_cast<Uint32>(height);
    map.numLayers = MAPLAYERS;
    const std::size_t count = width * height * MAPLAYERS;
    map.tiles = static_cast<Sint32*>(std::calloc(count, sizeof(Sint32)));
    EXPECT(map.tiles != nullptr);
    for (std::size_t x = 0; x < width; ++x)
    {
        for (std::size_t y = 0; y < height; ++y)
        {
            map.tiles[tileIndex(x, y, FLOORLAYER, height)] = floorTile;
        }
    }
    return true;
}

class TemporaryDataDirectory
{
public:
    TemporaryDataDirectory()
    {
        previousDatadir = datadir;
        const auto unique =
            std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path()
            / ("barony-stage4b-runtime-" + std::to_string(unique));
        valid = std::filesystem::create_directories(root / "maps");
        if (valid)
        {
            const std::string native = root.string();
            if (native.size() >= PATH_MAX)
            {
                valid = false;
            }
            else
            {
                std::snprintf(datadir, PATH_MAX, "%s", native.c_str());
            }
        }
    }

    ~TemporaryDataDirectory()
    {
        std::snprintf(datadir, PATH_MAX, "%s", previousDatadir.c_str());
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    bool ok() const
    {
        return valid;
    }

    std::filesystem::path mapPath(const std::string& filename) const
    {
        return root / "maps" / filename;
    }

private:
    std::filesystem::path root;
    std::string previousDatadir;
    bool valid = false;
};

template <typename T>
void writeNative(std::ofstream& output, const T& value)
{
    output.write(
        reinterpret_cast<const char*>(&value),
        static_cast<std::streamsize>(sizeof(value)));
}

bool writeSyntheticMap(
    const std::filesystem::path& path,
    const int editorVersion,
    const Uint32 fileLayers,
    const real_t entityZ)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    EXPECT(output.good());

    const std::string header = editorVersion == 32
        ? "BARONY LMPV3.2"
        : "BARONY LMPV4." + std::to_string(editorVersion - 40);
    EXPECT(header.size() == 14);
    output.write(header.data(), static_cast<std::streamsize>(header.size()));

    std::array<char, 32> name{};
    std::array<char, 32> author{};
    std::memcpy(name.data(), "Stage 4B fixture", 16);
    std::memcpy(author.data(), "Automatia", 8);
    output.write(name.data(), static_cast<std::streamsize>(name.size()));
    output.write(author.data(), static_cast<std::streamsize>(author.size()));

    const Uint32 width = 2;
    const Uint32 height = 2;
    const Uint32 skybox = 17;
    writeNative(output, width);
    writeNative(output, height);
    writeNative(output, skybox);
    std::array<Sint32, MAPFLAGS> flags{};
    output.write(
        reinterpret_cast<const char*>(flags.data()),
        static_cast<std::streamsize>(sizeof(flags)));

    const Uint32 storedLayers = editorVersion >= 40
        ? fileLayers
        : static_cast<Uint32>(LEGACY_MAPLAYERS);
    if (editorVersion >= 40)
    {
        writeNative(output, storedLayers);
    }
    for (Uint32 x = 0; x < width; ++x)
    {
        for (Uint32 y = 0; y < height; ++y)
        {
            for (Uint32 layer = 0; layer < storedLayers; ++layer)
            {
                const Sint32 tile = static_cast<Sint32>(
                    1 + layer + 100 * y + 1000 * x);
                writeNative(output, tile);
            }
        }
    }

    const Uint32 entityCount = 1;
    const Sint32 sprite = 0;
    const Sint32 persistentId = 314;
    const Sint32 x = 21;
    const Sint32 y = 37;
    writeNative(output, entityCount);
    writeNative(output, sprite);
    if (editorVersion >= 45)
    {
        writeNative(output, persistentId);
    }
    writeNative(output, x);
    writeNative(output, y);
    if (editorVersion >= 41)
    {
        writeNative(output, entityZ);
    }
    output.close();
    EXPECT(output.good());
    return true;
}

void clearLoadedMap(map_t& loaded, list_t& entities, list_t& creatures)
{
    list_FreeAll(&entities);
    list_FreeAll(&creatures);
    list_FreeAll(&entitiesdeleted);
    loaded.entities_map.clear();
    std::free(loaded.tiles);
    loaded.tiles = nullptr;
}

bool verifySyntheticMap(
    const std::string& filename,
    const int editorVersion,
    const Uint32 storedLayers,
    const real_t expectedSerializedZ)
{
    map_t loaded{};
    list_t entities{};
    list_t creatures{};
    loaded.entities = &entities;
    loaded.creatures = &creatures;
    const int result = loadMap(
        filename.c_str(), &loaded, &entities, &creatures, nullptr);
    EXPECT(result == 1);
    EXPECT(loaded.width == 2);
    EXPECT(loaded.height == 2);
    EXPECT(loaded.numLayers == MAPLAYERS);
    EXPECT(loaded.tiles != nullptr);

    const Uint32 expectedLayers = editorVersion >= 40
        ? storedLayers
        : static_cast<Uint32>(LEGACY_MAPLAYERS);
    for (Uint32 x = 0; x < loaded.width; ++x)
    {
        for (Uint32 y = 0; y < loaded.height; ++y)
        {
            for (Uint32 layer = 0; layer < MAPLAYERS; ++layer)
            {
                const Sint32 expected = layer < expectedLayers
                    ? static_cast<Sint32>(1 + layer + 100 * y + 1000 * x)
                    : 0;
                EXPECT(loaded.tiles[tileIndex(
                    x, y, layer, loaded.height)] == expected);
            }
        }
    }

    EXPECT(list_Size(&entities) == 1);
    Entity* entity = static_cast<Entity*>(entities.first->element);
    EXPECT(entity != nullptr);
    EXPECT(entity->x == 21.0);
    EXPECT(entity->y == 37.0);
    EXPECT(entity->z == (editorVersion >= 41 ? expectedSerializedZ : 0.0));
    EXPECT(entity->persistentID == (editorVersion >= 45 ? 314 : 0));
    EXPECT(entity->playableFloor == DEFAULT_PLAYABLE_FLOOR);
    EXPECT(entity->spatialRevision == 0);
    EXPECT(loaded.playableFloors.floors.size() == 1);
    EXPECT(loaded.playableFloors.hasFloor(DEFAULT_PLAYABLE_FLOOR));
    clearLoadedMap(loaded, entities, creatures);
    return true;
}

bool testLmpCompatibilityAndRoundTrip(TemporaryDataDirectory& temporary)
{
    constexpr real_t fixtureZ = -3.625;
    for (int editorVersion = 40; editorVersion <= 48; ++editorVersion)
    {
        const std::string filename =
            "synthetic_v4_" + std::to_string(editorVersion - 40) + ".lmp";
        EXPECT(writeSyntheticMap(
            temporary.mapPath(filename), editorVersion, MAPLAYERS, fixtureZ));
        EXPECT(verifySyntheticMap(
            filename, editorVersion, MAPLAYERS, fixtureZ));
    }

    EXPECT(writeSyntheticMap(
        temporary.mapPath("synthetic_v3_2.lmp"),
        32, LEGACY_MAPLAYERS, fixtureZ));
    EXPECT(verifySyntheticMap(
        "synthetic_v3_2.lmp", 32, LEGACY_MAPLAYERS, fixtureZ));

    // A V4 map may store fewer geometry layers; the live map expands to 32.
    EXPECT(writeSyntheticMap(
        temporary.mapPath("synthetic_v4_8_five_layers.lmp"),
        48, 5, fixtureZ));
    EXPECT(verifySyntheticMap(
        "synthetic_v4_8_five_layers.lmp", 48, 5, fixtureZ));

    EXPECT(resetGlobalMapHarness(2, 2, 0));
    std::snprintf(map.name, sizeof(map.name), "%s", "Stage 4B roundtrip");
    std::snprintf(map.author, sizeof(map.author), "%s", "Automatia");
    for (Uint32 x = 0; x < map.width; ++x)
    {
        for (Uint32 y = 0; y < map.height; ++y)
        {
            for (Uint32 layer = 0; layer < MAPLAYERS; ++layer)
            {
                map.tiles[tileIndex(x, y, layer, map.height)] =
                    static_cast<Sint32>(1 + layer + 100 * y + 1000 * x);
            }
        }
    }
    PlayableFloorData upperFloor;
    upperFloor.id = 2;
    upperFloor.tiles.resize(
        static_cast<std::size_t>(map.width)
        * static_cast<std::size_t>(map.height) * MAPLAYERS);
    for (std::size_t index = 0; index < upperFloor.tiles.size(); ++index)
    {
        upperFloor.tiles[index] = map.tiles[index] + 5000;
    }
    EXPECT(map.playableFloors.addFloor(std::move(upperFloor)));

    Entity* saved = newEntity(0, 1, map.entities, nullptr);
    EXPECT(saved != nullptr);
    saved->persistentID = 77;
    saved->x = 21.75;
    saved->y = 14.5;
    saved->z = fixtureZ;
    saved->playableFloor = 2;
    saved->spatialRevision = 41;
    EXPECT(saveMap("stage4b_roundtrip") == 0);

    std::ifstream output(
        temporary.mapPath("stage4b_roundtrip.lmp"), std::ios::binary);
    std::array<char, 14> header{};
    output.read(header.data(), static_cast<std::streamsize>(header.size()));
    EXPECT(std::string(header.data(), header.size()) == "BARONY LMPV4.9");

    map_t loaded{};
    list_t entities{};
    list_t creatures{};
    loaded.entities = &entities;
    loaded.creatures = &creatures;
    EXPECT(loadMap(
        "stage4b_roundtrip.lmp",
        &loaded,
        &entities,
        &creatures,
        nullptr) == 1);
    EXPECT(loaded.numLayers == MAPLAYERS);
    EXPECT(list_Size(&entities) == 1);
    Entity* restored = static_cast<Entity*>(entities.first->element);
    EXPECT(restored != nullptr);
    // Existing LMP x/y are integral even though runtime coordinates are real_t.
    EXPECT(restored->x == 21.0);
    EXPECT(restored->y == 14.0);
    EXPECT(restored->z == fixtureZ);
    EXPECT(restored->persistentID == 77);
    EXPECT(restored->playableFloor == 2);
    // spatialRevision is runtime routing state, not authored LMP state.
    EXPECT(restored->spatialRevision == 0);
    EXPECT(loaded.playableFloors.floors.size() == 2);
    EXPECT(loaded.playableFloors.hasFloor(DEFAULT_PLAYABLE_FLOOR));
    const PlayableFloorData* restoredUpper = loaded.playableFloors.find(2);
    EXPECT(restoredUpper != nullptr);
    EXPECT(restoredUpper->tiles.size()
        == static_cast<std::size_t>(map.width)
            * static_cast<std::size_t>(map.height) * MAPLAYERS);
    for (Uint32 index = 0; index < map.width * map.height * MAPLAYERS; ++index)
    {
        EXPECT(loaded.tiles[index] == map.tiles[index]);
        EXPECT(restoredUpper->tiles[index] == map.tiles[index] + 5000);
    }
    clearLoadedMap(loaded, entities, creatures);

    // V4.9 must reject truncated extension data instead of silently flattening.
    const std::filesystem::path validPath =
        temporary.mapPath("stage4b_roundtrip.lmp");
    const std::filesystem::path corruptPath =
        temporary.mapPath("stage4c_truncated_v4_9.lmp");
    std::filesystem::copy_file(
        validPath, corruptPath, std::filesystem::copy_options::overwrite_existing);
    const auto validSize = std::filesystem::file_size(corruptPath);
    EXPECT(validSize > 0);
    std::filesystem::resize_file(corruptPath, validSize - 1);
    map_t corrupted{};
    list_t corruptEntities{};
    list_t corruptCreatures{};
    corrupted.entities = &corruptEntities;
    corrupted.creatures = &corruptCreatures;
    EXPECT(loadMap(
        "stage4c_truncated_v4_9.lmp",
        &corrupted,
        &corruptEntities,
        &corruptCreatures,
        nullptr) == -1);
    clearLoadedMap(corrupted, corruptEntities, corruptCreatures);
    return true;
}

bool testOneFloorCollisionAndSpatialIndex()
{
    multiplayer = SINGLE;
    EXPECT(resetGlobalMapHarness(4, 4, 1));

    EXPECT(checkObstacle(24, 24, nullptr, nullptr, false, true, true) == 0);
    map.tiles[tileIndex(1, 1, OBSTACLELAYER, map.height)] = 99;
    EXPECT(checkObstacle(24, 24, nullptr, nullptr, false, true, true) == 1);
    map.tiles[tileIndex(1, 1, OBSTACLELAYER, map.height)] = 0;
    map.tiles[tileIndex(1, 1, FLOORLAYER, map.height)] = 0;
    EXPECT(checkObstacle(24, 24, nullptr, nullptr, false, true, true) == 1);
    map.tiles[tileIndex(1, 1, FLOORLAYER, map.height)] = 1;

    Entity* first = newEntity(0, 1, map.entities, nullptr);
    Entity* second = newEntity(0, 1, map.entities, nullptr);
    EXPECT(first != nullptr);
    EXPECT(second != nullptr);
    first->x = 24;
    first->y = 24;
    first->z = -4;
    first->sizex = 2;
    first->sizey = 2;
    second->x = 24;
    second->y = 24;
    second->z = 80;
    second->sizex = 2;
    second->sizey = 2;
    EXPECT(entityInsideEntity(first, second));
    EXPECT(entityDist(first, second) == 0.0);
    second->x = 27;
    second->y = 28;
    EXPECT(entityDist(first, second) == 5.0);
    second->x = 40;
    second->y = 40;
    EXPECT(!entityInsideEntity(first, second));

    map.tiles[tileIndex(1, 1, OBSTACLELAYER, map.height)] = 7;
    EXPECT(entityInsideTile(first, 1, 1, OBSTACLELAYER));
    first->z = 1000;
    EXPECT(entityInsideTile(first, 1, 1, OBSTACLELAYER));

    first->x = 24;
    first->y = 24;
    second->x = 24;
    second->y = 24;
    EXPECT(TileEntityList.addEntity(*first) != nullptr);
    EXPECT(TileEntityList.addEntity(*second) != nullptr);
    list_t* sharedTile = TileEntityList.getTileList(1, 1);
    EXPECT(sharedTile != nullptr);
    EXPECT(list_Size(sharedTile) == 2);
    second->z = -500;
    EXPECT(list_Size(sharedTile) == 2);
    second->x = 40;
    EXPECT(TileEntityList.updateEntity(*second) != nullptr);
    EXPECT(list_Size(sharedTile) == 1);
    EXPECT(list_Size(TileEntityList.getTileList(2, 1)) == 1);
    return true;
}

bool testLocalElevationAndRuntimeSpawns()
{
    multiplayer = CLIENT;
    EXPECT(resetGlobalMapHarness(4, 4, 0));

    Stat levitationStats(0);
    levitationStats.type = HUMAN;
    levitationStats.setEffectValueUnsafe(EFF_LEVITATING, 0);
    EXPECT(!isLevitating(&levitationStats));
    levitationStats.setEffectActive(EFF_LEVITATING, 1);
    EXPECT(isLevitating(&levitationStats));

    Entity* parent = newEntity(0, 1, map.entities, nullptr);
    EXPECT(parent != nullptr);
    parent->x = 32;
    parent->y = 48;
    parent->z = 20;
    parent->playableFloor = 3;
    parent->spatialRevision = 77;

    Entity* inherited = newEntityWithSpatialContext(
        0, 1, map.entities, nullptr, parent);
    EXPECT(inherited != nullptr);
    EXPECT(inherited->playableFloor == 3);
    EXPECT(inherited->spatialRevision == 77);
    Entity* explicitContext = newEntityWithSpatialContext(
        0, 1, map.entities, nullptr, SpatialSpawnContext{-2, 19});
    EXPECT(explicitContext != nullptr);
    EXPECT(explicitContext->playableFloor == -2);
    EXPECT(explicitContext->spatialRevision == 19);

    // The Stage-Z1 spawn API is floor-aware, while legacy spawn helpers are
    // characterized below and will be migrated to it before Z2 isolation.
    parent->playableFloor = DEFAULT_PLAYABLE_FLOOR;
    parent->spatialRevision = 0;

    Entity* clientGib = spawnGibClient(12, 34, -7, 5);
    EXPECT(clientGib != nullptr);
    EXPECT(clientGib->x == 12.0);
    EXPECT(clientGib->y == 34.0);
    EXPECT(clientGib->z == -7.0);

    Entity* gib = spawnGib(parent, 5);
    EXPECT(gib != nullptr);
    EXPECT(gib->x == parent->x);
    EXPECT(gib->y == parent->y);
    EXPECT(gib->z >= 8.0);
    EXPECT(gib->z <= parent->z - 4.0);

    Entity* particle = spawnMagicParticleCustom(parent, 245, 1.0, 10.0);
    EXPECT(particle != nullptr);
    EXPECT(std::fabs(particle->z - parent->z) <= 0.11);
    EXPECT(std::fabs(particle->x - parent->x) <= 0.11);
    EXPECT(std::fabs(particle->y - parent->y) <= 0.11);

    Entity* summon = summonMonsterNoSmoke(RAT, 48, 64, true);
    EXPECT(summon != nullptr);
    EXPECT(summon->x == 48.0);
    EXPECT(summon->y == 64.0);
    EXPECT(summon->z == 6.0);

    Entity* arrow = newEntity(166, 1, map.entities, nullptr);
    EXPECT(arrow != nullptr);
    arrow->x = 8;
    arrow->y = 8;
    arrow->z = 1.0;
    arrow->vel_z = 0.5;
    arrow->arrowFallSpeed = 0.25;
    arrow->arrowBoltDropOffRange = 0;
    arrow->skill[10] = 1; // already initialized
    actArrow(arrow);
    EXPECT(arrow->vel_z == 0.75);
    EXPECT(arrow->z == 1.75);
    EXPECT(arrow->pitch > 0.0);
    return true;
}

bool testPersistentMinimapAndPlacement()
{
    multiplayer = SINGLE;
    clientnum = 0;
    headless = false;
    EXPECT(resetGlobalMapHarness(2, 2, 1));
    std::snprintf(
        map.filename, sizeof(map.filename), "%s", "stage4b_minimap.lmp");
    std::memset(minimap, 0, sizeof(minimap));

    const std::vector<Sint8> original = {1, 0, 3, 4};
    EXPECT(importAutomatiaPersistentMinimapSnapshot(
        0, "stage4b_minimap.lmp#world", 2, 2, original, true));
    restoreAutomatiaPersistentMinimapForLocalPlayer();
    EXPECT(minimap[0][0] == 1);
    EXPECT(minimap[0][1] == 0);
    EXPECT(minimap[1][0] == 3);
    EXPECT(minimap[1][1] == 4);

    minimap[0][1] = 2;
    std::string mapKey;
    Sint32 width = 0;
    Sint32 height = 0;
    std::vector<Sint8> exported;
    EXPECT(exportAutomatiaPersistentMinimapSnapshot(
        0, mapKey, width, height, exported));
    EXPECT(mapKey == "stage4b_minimap.lmp#world");
    EXPECT(width == 2);
    EXPECT(height == 2);
    EXPECT(exported == std::vector<Sint8>({1, 2, 3, 4}));

    // Schema-3 minimap storage separates identical x/y discovery by floor.
    const std::vector<Sint8> upperDiscovery = {4, 3, 0, 1};
    EXPECT(importAutomatiaPersistentMinimapSnapshotForFloor(
        0, "stage4b_minimap.lmp#world", 2,
        2, 2, upperDiscovery, true));
    Sint32 upperWidth = 0;
    Sint32 upperHeight = 0;
    std::vector<Sint8> upperExported;
    EXPECT(exportAutomatiaPersistentMinimapSnapshotForFloor(
        0, "stage4b_minimap.lmp#world", 2,
        upperWidth, upperHeight, upperExported));
    EXPECT(upperWidth == 2);
    EXPECT(upperHeight == 2);
    EXPECT(upperExported == upperDiscovery);
    Sint32 floorZeroWidth = 0;
    Sint32 floorZeroHeight = 0;
    std::vector<Sint8> floorZeroExported;
    EXPECT(exportAutomatiaPersistentMinimapSnapshotForFloor(
        0, "stage4b_minimap.lmp#world", DEFAULT_PLAYABLE_FLOOR,
        floorZeroWidth, floorZeroHeight, floorZeroExported));
    EXPECT(floorZeroExported == std::vector<Sint8>({1, 2, 3, 4}));

    std::snprintf(
        map.filename, sizeof(map.filename), "%s", "stage4b_other.lmp");
    restoreAutomatiaPersistentMinimapForLocalPlayer();
    EXPECT(minimap[0][0] == 0);
    EXPECT(minimap[0][1] == 0);
    EXPECT(minimap[1][0] == 0);
    EXPECT(minimap[1][1] == 0);

    std::snprintf(
        map.filename, sizeof(map.filename), "%s", "stage4b_minimap.lmp");
    restoreAutomatiaPersistentMinimapForLocalPlayer();
    EXPECT(minimap[0][0] == 1);
    EXPECT(minimap[0][1] == 2);
    EXPECT(minimap[1][0] == 3);
    EXPECT(minimap[1][1] == 4);

    EXPECT(stageAutomatiaCharacterSavedPlacement(
        1,
        "village.lmp",
        "world",
        17,
        128.25,
        96.5,
        -1.75,
        1.25,
        -0.2,
        0.1));
    EXPECT(automatiaHasSavedPlayerPlacement(1));
    EXPECT(!stageAutomatiaCharacterSavedPlacement(
        0, "village.lmp", "world", 17,
        1, 2, 3, 4, 5, 6));
    EXPECT(!stageAutomatiaCharacterSavedPlacement(
        1, "village.lmp", "world", 17,
        1, 2, std::numeric_limits<real_t>::infinity(), 4, 5, 6));
    consumeAutomatiaSavedPlayerPlacement(1);
    EXPECT(!automatiaHasSavedPlayerPlacement(1));
    return true;
}
}

int runPlayableZRuntimeCharacterization()
{
    const int previousMultiplayer = multiplayer;
    const bool previousLoading = loading;
    bool* const previousAnimatedTiles = animatedtiles;
    bool* const previousSwimmingTiles = swimmingtiles;
    bool* const previousLavaTiles = lavatiles;
    animatedtiles = stageAnimatedTiles.data();
    swimmingtiles = stageSwimmingTiles.data();
    lavatiles = stageLavaTiles.data();
    TemporaryDataDirectory temporary;
    if (!expect(temporary.ok(), "temporary.ok()"))
    {
        animatedtiles = previousAnimatedTiles;
        swimmingtiles = previousSwimmingTiles;
        lavatiles = previousLavaTiles;
        return 1;
    }

    loading = true;
    const bool passed =
        testLmpCompatibilityAndRoundTrip(temporary)
        && testOneFloorCollisionAndSpatialIndex()
        && testLocalElevationAndRuntimeSpawns()
        && testPersistentMinimapAndPlacement();
    clearGlobalMapHarness();
    multiplayer = previousMultiplayer;
    loading = previousLoading;
    animatedtiles = previousAnimatedTiles;
    swimmingtiles = previousSwimmingTiles;
    lavatiles = previousLavaTiles;
    if (passed)
    {
        std::cout
            << "Stage 4C/Z1 playable-Z data foundation passed: legacy floor Z0, "
            << "local Entity::z, LMP V3.2/V4.0-V4.9, floor metadata, "
            << "schema-3 minimap separation, placement, and spawn context.\n";
    }
    return passed ? 0 : 1;
}
