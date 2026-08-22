#include "../src/main.hpp"
#include "../src/game.hpp"
#include "../src/stat.hpp"
#include "../src/entity.hpp"
#include "../src/collision.hpp"
#include "../src/files.hpp"
#include "../src/magic/magic.hpp"
#include "../src/monster.hpp"
#include "../src/light.hpp"

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
#include <iterator>
#include <limits>
#include <string>
#include <utility>
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
    list_FreeAll(&light_l);
    clearAdditionalPlayableFloorLightmaps();
    for (int index = 0; index < MAXPLAYERS + 1; ++index)
    {
        lightmaps[index].clear();
        lightmapsSmoothed[index].clear();
    }
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

bool rewritePlayableZExtensionVersion(
    const std::filesystem::path& path,
    const std::uint16_t version)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.good())
    {
        return false;
    }
    const std::vector<char> bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    input.close();

    const std::array<char, 4> magic{{'P', 'Z', 'L', 'V'}};
    const auto marker = std::find_end(
        bytes.begin(), bytes.end(), magic.begin(), magic.end());
    if (marker == bytes.end()
        || static_cast<std::size_t>(bytes.end() - marker) < 6)
    {
        return false;
    }

    std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!output.good())
    {
        return false;
    }
    output.seekp((marker - bytes.begin()) + 4);
    const std::array<char, 2> encoded{{
        static_cast<char>(version & 0xffU),
        static_cast<char>((version >> 8U) & 0xffU)}};
    output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    output.close();
    return output.good();
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
    // Pre-V4.9 maps may use Entity::z as local model elevation. They must never
    // be promoted to an upper gameplay floor merely because z is negative.
    EXPECT(entity->authoredMapLayer == 0);
    EXPECT(entity->spatialRevision == 0);
    EXPECT(loaded.playableFloors.floors.size() == 1);
    EXPECT(loaded.playableFloors.hasFloor(DEFAULT_PLAYABLE_FLOOR));
    clearLoadedMap(loaded, entities, creatures);
    return true;
}

bool testLmpCompatibilityAndRoundTrip(TemporaryDataDirectory& temporary)
{
    constexpr real_t fixtureZ = -32.0;
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
    Entity* lowerTransition = newEntity(0, 1, map.entities, nullptr);
    EXPECT(saved != nullptr);
    EXPECT(lowerTransition != nullptr);
    saved->persistentID = 77;
    saved->authoredMapLayer = 3;
    saved->x = 21.75;
    saved->y = 14.5;
    saved->z = fixtureZ;
    saved->playableFloor = 2;
    saved->spatialRevision = 41;
    saved->playableFloorTransitionEnabled = true;
    saved->playableFloorTransitionDestination = DEFAULT_PLAYABLE_FLOOR;
    saved->playableFloorTransitionTargetPersistentID = 78;

    lowerTransition->persistentID = 78;
    lowerTransition->authoredMapLayer = 1;
    lowerTransition->x = 8.0;
    lowerTransition->y = 8.0;
    lowerTransition->z = fixtureZ;
    lowerTransition->playableFloorTransitionEnabled = true;
    lowerTransition->playableFloorTransitionDestination = 2;
    lowerTransition->playableFloorTransitionTargetPersistentID = 77;
    // Z3.3 layer-authored stair metadata coexists with legacy ZTRN data and
    // must survive save/load without requiring any editor-side pairing pass.
    lowerTransition->verticalLayerTransitionDelta = 1;
    lowerTransition->verticalLayerTransitionModel = 161;
    lowerTransition->verticalLayerTransitionRotation = 6;
    lowerTransition->floorDecorationHeightOffset = 12;
    lowerTransition->floorDecorationXOffset = 4;
    lowerTransition->floorDecorationYOffset = -8;
    lowerTransition->floorDecorationDestroyIfNoWall = 4;
    lowerTransition->skill[8] = static_cast<Sint32>(0x52494154); // "TAIR"
    lowerTransition->skill[9] = static_cast<Sint32>(0x00000053); // "S"
    saved->verticalLayerTransitionDelta = -1;
    saved->verticalLayerTransitionModel = 253;
    saved->verticalLayerTransitionRotation = 2;
    saved->floorDecorationHeightOffset = -4;
    saved->floorDecorationXOffset = -3;
    saved->floorDecorationYOffset = 7;
    saved->floorDecorationDestroyIfNoWall = -1;
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
        nullptr) == 2);
    EXPECT(loaded.numLayers == MAPLAYERS);
    EXPECT(list_Size(&entities) == 2);
    Entity* restored = nullptr;
    Entity* restoredLowerTransition = nullptr;
    for (node_t* node = entities.first; node; node = node->next)
    {
        Entity* candidate = static_cast<Entity*>(node->element);
        if (candidate && candidate->persistentID == 77)
        {
            restored = candidate;
        }
        else if (candidate && candidate->persistentID == 78)
        {
            restoredLowerTransition = candidate;
        }
    }
    EXPECT(restored != nullptr);
    EXPECT(restoredLowerTransition != nullptr);
    // Existing LMP x/y are integral even though runtime coordinates are real_t.
    EXPECT(restored->x == 21.0);
    EXPECT(restored->y == 14.0);
    EXPECT(restored->z == fixtureZ);
    EXPECT(restored->persistentID == 77);
    EXPECT(restored->authoredMapLayer == 3);
    EXPECT(restored->playableFloor == 2);
    // spatialRevision is runtime routing state, not authored LMP state.
    EXPECT(restored->spatialRevision == 0);
    EXPECT(restored->playableFloorTransitionEnabled);
    EXPECT(restored->playableFloorTransitionDestination == DEFAULT_PLAYABLE_FLOOR);
    EXPECT(restored->playableFloorTransitionTargetPersistentID == 78);
    EXPECT(restored->verticalLayerTransitionDelta == -1);
    EXPECT(restored->verticalLayerTransitionModel == 253);
    EXPECT(restored->verticalLayerTransitionRotation == 2);
    EXPECT(restored->floorDecorationHeightOffset == -4);
    EXPECT(restored->floorDecorationXOffset == -3);
    EXPECT(restored->floorDecorationYOffset == 7);
    EXPECT(restored->floorDecorationDestroyIfNoWall == -1);
    EXPECT(restoredLowerTransition->authoredMapLayer == 1);
    EXPECT(restoredLowerTransition->playableFloor == DEFAULT_PLAYABLE_FLOOR);
    EXPECT(restoredLowerTransition->playableFloorTransitionEnabled);
    EXPECT(restoredLowerTransition->playableFloorTransitionDestination == 2);
    EXPECT(restoredLowerTransition->playableFloorTransitionTargetPersistentID == 77);
    EXPECT(restoredLowerTransition->verticalLayerTransitionDelta == 1);
    EXPECT(restoredLowerTransition->verticalLayerTransitionModel == 161);
    EXPECT(restoredLowerTransition->verticalLayerTransitionRotation == 6);
    EXPECT(restoredLowerTransition->floorDecorationHeightOffset == 12);
    EXPECT(restoredLowerTransition->floorDecorationXOffset == 4);
    EXPECT(restoredLowerTransition->floorDecorationYOffset == -8);
    EXPECT(restoredLowerTransition->floorDecorationDestroyIfNoWall == 4);
    EXPECT(restoredLowerTransition->skill[8] == static_cast<Sint32>(0x52494154));
    EXPECT(restoredLowerTransition->skill[9] == static_cast<Sint32>(0x00000053));
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

    /*
     * PZLV v1 identifies the Z3.4A/B/C baked-Z representation explicitly.
     * ELYR layer 5 must unbake -80 from serialized Z and must override the
     * deliberately stale EFLR floor 4 assignment for this ordinary sprite.
     */
    EXPECT(resetGlobalMapHarness(2, 2, 1));
    std::snprintf(map.name, sizeof(map.name), "%s", "Phase 1 v1 migration");
    std::snprintf(map.author, sizeof(map.author), "%s", "Automatia");
    EXPECT(map.ensurePlayableFloorGeometry(4, false));
    Entity* legacyBaked = newEntity(0, 1, map.entities, nullptr);
    EXPECT(legacyBaked != nullptr);
    legacyBaked->persistentID = 901;
    legacyBaked->authoredMapLayer = 5;
    legacyBaked->playableFloor = 4;
    legacyBaked->x = 8.0;
    legacyBaked->y = 8.0;
    legacyBaked->z = 7.5 + mapLayerWorldZ(5);
    EXPECT(saveMap("phase1_legacy_baked") == 0);
    const std::filesystem::path legacyBakedPath =
        temporary.mapPath("phase1_legacy_baked.lmp");
    EXPECT(rewritePlayableZExtensionVersion(legacyBakedPath, 1));

    map_t migrated{};
    list_t migratedEntities{};
    list_t migratedCreatures{};
    migrated.entities = &migratedEntities;
    migrated.creatures = &migratedCreatures;
    EXPECT(loadMap(
        "phase1_legacy_baked.lmp",
        &migrated,
        &migratedEntities,
        &migratedCreatures,
        nullptr) == 1);
    EXPECT(list_Size(&migratedEntities) == 1);
    Entity* migratedEntity = static_cast<Entity*>(migratedEntities.first->element);
    EXPECT(migratedEntity != nullptr);
    EXPECT(migratedEntity->authoredMapLayer == 5);
    EXPECT(migratedEntity->playableFloor == 5);
    EXPECT(migratedEntity->z == 7.5);
    EXPECT(migratedEntity->worldRenderZ() == -72.5);
    clearLoadedMap(migrated, migratedEntities, migratedCreatures);

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

bool testPlayableFloorCollisionIsolation()
{
    multiplayer = SINGLE;
    EXPECT(resetGlobalMapHarness(4, 4, 1));

    PlayableFloorData upperFloor;
    upperFloor.id = 1;
    upperFloor.tiles.resize(
        static_cast<std::size_t>(map.width)
            * static_cast<std::size_t>(map.height) * MAPLAYERS,
        0);
    for (std::size_t x = 0; x < map.width; ++x)
    {
        for (std::size_t y = 0; y < map.height; ++y)
        {
            upperFloor.tiles[tileIndex(x, y, FLOORLAYER, map.height)] = 1;
        }
    }
    EXPECT(map.playableFloors.addFloor(std::move(upperFloor)));

    Entity* lower = newEntity(0, 1, map.entities, nullptr);
    Entity* upper = newEntity(0, 1, map.entities, nullptr);
    EXPECT(lower != nullptr);
    EXPECT(upper != nullptr);
    lower->x = 24;
    lower->y = 24;
    lower->sizex = 2;
    lower->sizey = 2;
    upper->x = 24;
    upper->y = 24;
    upper->sizex = 2;
    upper->sizey = 2;

    EXPECT(TileEntityList.addEntity(*lower) != nullptr);
    EXPECT(TileEntityList.addEntity(*upper) != nullptr);
    EXPECT(list_Size(TileEntityList.getTileList(1, 1)) == 2);
    EXPECT(entityInsideEntity(lower, upper));
    EXPECT(entityDist(lower, upper) == 0.0);
    EXPECT(checkObstacle(24, 24, lower, nullptr, true, false, false) == 1);

    EXPECT(upper->spatialRevision == 0);
    EXPECT(upper->setPlayableFloor(1));
    EXPECT(upper->playableFloor == 1);
    EXPECT(upper->spatialRevision == 1);
    EXPECT(!upper->setPlayableFloor(1));
    EXPECT(upper->spatialRevision == 1);

    list_t* lowerTile = TileEntityList.getTileList(1, 1, DEFAULT_PLAYABLE_FLOOR);
    list_t* upperTile = TileEntityList.getTileList(1, 1, 1);
    EXPECT(lowerTile != nullptr);
    EXPECT(upperTile != nullptr);
    EXPECT(list_Size(lowerTile) == 1);
    EXPECT(list_Size(upperTile) == 1);

    EXPECT(!entityInsideEntity(lower, upper));
    EXPECT(std::isinf(entityDist(lower, upper)));
    EXPECT(checkObstacle(24, 24, lower, nullptr, true, false, false) == 0);
    EXPECT(checkObstacle(24, 24, upper, nullptr, true, false, false) == 0);

    auto lowerLists = TileEntityList.getEntitiesWithinRadiusAroundEntity(lower, 0);
    auto upperLists = TileEntityList.getEntitiesWithinRadiusAroundEntity(upper, 0);
    EXPECT(lowerLists.size() == 1);
    EXPECT(upperLists.size() == 1);
    EXPECT(list_Size(lowerLists.front()) == 1);
    EXPECT(list_Size(upperLists.front()) == 1);
    EXPECT(lowerLists.front()->first->element == lower);
    EXPECT(upperLists.front()->first->element == upper);

    // Runtime floor changes must reindex the entity and advance the routing
    // revision so stale floor-local updates can be rejected in later Z2 work.
    EXPECT(upper->setPlayableFloor(DEFAULT_PLAYABLE_FLOOR));
    EXPECT(upper->spatialRevision == 2);
    EXPECT(list_Size(TileEntityList.getTileList(1, 1)) == 2);
    EXPECT(list_Size(TileEntityList.getTileList(1, 1, 1)) == 0);
    EXPECT(entityInsideEntity(lower, upper));
    EXPECT(entityDist(lower, upper) == 0.0);
    EXPECT(checkObstacle(24, 24, lower, nullptr, true, false, false) == 1);

    // Generic child/context propagation must use the same atomic reindexing
    // path when the child has already entered TileEntityList.
    upper->applySpatialSpawnContext(SpatialSpawnContext{1, 42, 1});
    EXPECT(upper->playableFloor == 1);
    EXPECT(upper->spatialRevision == 42);
    EXPECT(upper->authoredMapLayer == 1);
    EXPECT(list_Size(TileEntityList.getTileList(1, 1, DEFAULT_PLAYABLE_FLOOR)) == 1);
    EXPECT(list_Size(TileEntityList.getTileList(1, 1, 1)) == 1);
    upper->inheritSpatialContextFrom(lower);
    EXPECT(upper->playableFloor == DEFAULT_PLAYABLE_FLOOR);
    EXPECT(upper->authoredMapLayer == 0);
    EXPECT(list_Size(TileEntityList.getTileList(1, 1, DEFAULT_PLAYABLE_FLOOR)) == 2);
    EXPECT(list_Size(TileEntityList.getTileList(1, 1, 1)) == 0);
    return true;
}

bool testZ3TransitionPrimitive()
{
    multiplayer = SINGLE;
    EXPECT(resetGlobalMapHarness(4, 4, 1));

    PlayableFloorData upperFloor;
    upperFloor.id = 1;
    upperFloor.tiles.resize(
        static_cast<std::size_t>(map.width)
            * static_cast<std::size_t>(map.height) * MAPLAYERS,
        0);
    for (std::size_t x = 0; x < map.width; ++x)
    {
        for (std::size_t y = 0; y < map.height; ++y)
        {
            upperFloor.tiles[tileIndex(x, y, FLOORLAYER, map.height)] = 1;
        }
    }
    EXPECT(map.playableFloors.addFloor(std::move(upperFloor)));

    Entity* traveler = newEntity(0, 1, map.entities, nullptr);
    EXPECT(traveler != nullptr);
    traveler->x = 24.0;
    traveler->y = 24.0;
    traveler->z = -2.0;
    traveler->new_x = traveler->x;
    traveler->new_y = traveler->y;
    traveler->new_z = traveler->z;
    EXPECT(TileEntityList.addEntity(*traveler) != nullptr);
    EXPECT(list_Size(TileEntityList.getTileList(1, 1, DEFAULT_PLAYABLE_FLOOR)) == 1);
    EXPECT(list_Size(TileEntityList.getTileList(2, 1, 1)) == 0);

    EXPECT(traveler->transitionToPlayableFloor(1, 40.0, 24.0, -2.0));
    EXPECT(traveler->playableFloor == 1);
    EXPECT(traveler->spatialRevision == 1);
    EXPECT(traveler->x == 40.0);
    EXPECT(traveler->y == 24.0);
    EXPECT(traveler->z == -2.0);
    EXPECT(list_Size(TileEntityList.getTileList(1, 1, DEFAULT_PLAYABLE_FLOOR)) == 0);
    EXPECT(list_Size(TileEntityList.getTileList(2, 1, 1)) == 1);

    // A blocked destination must fail atomically without changing floor,
    // revision, coordinates, or the spatial index.
    EXPECT(map.setTileAt(1, 1, OBSTACLELAYER, 99, DEFAULT_PLAYABLE_FLOOR));
    EXPECT(!traveler->transitionToPlayableFloor(
        DEFAULT_PLAYABLE_FLOOR, 24.0, 24.0, -2.0));
    EXPECT(traveler->playableFloor == 1);
    EXPECT(traveler->spatialRevision == 1);
    EXPECT(traveler->x == 40.0);
    EXPECT(list_Size(TileEntityList.getTileList(2, 1, 1)) == 1);

    EXPECT(map.setTileAt(1, 1, OBSTACLELAYER, 0, DEFAULT_PLAYABLE_FLOOR));
    EXPECT(traveler->transitionToPlayableFloor(
        DEFAULT_PLAYABLE_FLOOR, 24.0, 24.0, -2.0));
    EXPECT(traveler->playableFloor == DEFAULT_PLAYABLE_FLOOR);
    EXPECT(traveler->spatialRevision == 2);
    EXPECT(list_Size(TileEntityList.getTileList(1, 1, DEFAULT_PLAYABLE_FLOOR)) == 1);
    EXPECT(list_Size(TileEntityList.getTileList(2, 1, 1)) == 0);
    return true;
}

bool testPlayableFloorGeometryIsolation()
{
    multiplayer = SINGLE;
    EXPECT(resetGlobalMapHarness(4, 4, 1));

    EXPECT(map.tilesForPlayableFloor(DEFAULT_PLAYABLE_FLOOR) == map.tiles);
    EXPECT(map.tilesForPlayableFloor(1) == nullptr);
    EXPECT(map.ensurePlayableFloorGeometry(1, false));
    EXPECT(map.tilesForPlayableFloor(1) != nullptr);
    EXPECT(map.tilesForPlayableFloor(1) != map.tilesForPlayableFloor(DEFAULT_PLAYABLE_FLOOR));

    // Z3.3B existing-layer model: playable floors are overlapping views of
    // the authored map-layer stack. Z0's obstacle layer (authored layer 1)
    // is exactly Z1's floor layer; Z1's obstacle layer is authored layer 2.
    EXPECT(map.setTileAt(1, 1, FLOORLAYER, 1, DEFAULT_PLAYABLE_FLOOR));
    EXPECT(map.setTileAt(1, 1, OBSTACLELAYER, 77, DEFAULT_PLAYABLE_FLOOR));
    EXPECT(map.setTileAt(1, 1, OBSTACLELAYER, 0, 1));
    EXPECT(map.tileAt(1, 1, FLOORLAYER, DEFAULT_PLAYABLE_FLOOR) == 1);
    EXPECT(map.tileAt(1, 1, OBSTACLELAYER, DEFAULT_PLAYABLE_FLOOR) == 77);
    EXPECT(map.tileAt(1, 1, FLOORLAYER, 1) == 77);
    EXPECT(map.tileAt(1, 1, OBSTACLELAYER, 1) == 0);

    // Writing through the Z1 floor view must update the same authored layer
    // observed as Z0's obstacle layer. This overlap is the core stair model.
    EXPECT(map.setTileAt(1, 1, FLOORLAYER, 66, 1));
    EXPECT(map.tileAt(1, 1, FLOORLAYER, 1) == 66);
    EXPECT(map.tileAt(1, 1, OBSTACLELAYER, DEFAULT_PLAYABLE_FLOOR) == 66);
    EXPECT(map.setTileAt(1, 1, OBSTACLELAYER, 77, DEFAULT_PLAYABLE_FLOOR));

    Entity* lower = newEntity(0, 1, map.entities, nullptr);
    Entity* upper = newEntity(0, 1, map.entities, nullptr);
    EXPECT(lower != nullptr);
    EXPECT(upper != nullptr);
    lower->x = upper->x = 24;
    lower->y = upper->y = 24;
    lower->sizex = upper->sizex = 2;
    lower->sizey = upper->sizey = 2;
    upper->setPlayableFloor(1);

    EXPECT(checkObstacle(24, 24, lower, nullptr, true, true, false) == 1);
    EXPECT(checkObstacle(24, 24, upper, nullptr, true, true, false) == 0);
    EXPECT(entityInsideTile(lower, 1, 1, OBSTACLELAYER));
    EXPECT(!entityInsideTile(upper, 1, 1, OBSTACLELAYER));

    // Reverse the geometry without moving either entity. Each entity must
    // immediately observe only its own playable floor's tile stack.
    EXPECT(map.setTileAt(1, 1, OBSTACLELAYER, 0, DEFAULT_PLAYABLE_FLOOR));
    EXPECT(map.setTileAt(1, 1, OBSTACLELAYER, 88, 1));
    EXPECT(checkObstacle(24, 24, lower, nullptr, true, true, false) == 0);
    EXPECT(checkObstacle(24, 24, upper, nullptr, true, true, false) == 1);
    EXPECT(!entityInsideTile(lower, 1, 1, OBSTACLELAYER));
    EXPECT(entityInsideTile(upper, 1, 1, OBSTACLELAYER));

    // Tile attributes follow the same authored-layer overlap. A Z1 floor
    // attribute is visible as a Z0 obstacle-layer attribute because both refer
    // to authored layer 1, but it must not appear on Z0's authored layer 0.
    map.setTileAttribute(1, 1, FLOORLAYER, map_t::TILE_ATTRIBUTE_SLIPPERY, true, 1);
    EXPECT(!map.tileHasAttribute(
        1, 1, FLOORLAYER, map_t::TILE_ATTRIBUTE_SLIPPERY,
        DEFAULT_PLAYABLE_FLOOR));
    EXPECT(map.tileHasAttribute(
        1, 1, OBSTACLELAYER, map_t::TILE_ATTRIBUTE_SLIPPERY,
        DEFAULT_PLAYABLE_FLOOR));
    EXPECT(map.tileHasAttribute(
        1, 1, FLOORLAYER, map_t::TILE_ATTRIBUTE_SLIPPERY, 1));

    // Diggability must also consult the requested floor's obstacle tile and
    // NODIG attribute rather than silently reading Z0.
    EXPECT(map.setTileAt(2, 2, OBSTACLELAYER, 1, DEFAULT_PLAYABLE_FLOOR));
    EXPECT(map.setTileAt(2, 2, OBSTACLELAYER, 1, 1));
    map.setTileAttribute(2, 2, OBSTACLELAYER, map_t::TILE_ATTRIBUTE_NODIG, true, 1);
    EXPECT(mapTileDiggable(2, 2, DEFAULT_PLAYABLE_FLOOR));
    EXPECT(!mapTileDiggable(2, 2, 1));
    map.setTileAttribute(2, 2, OBSTACLELAYER, map_t::TILE_ATTRIBUTE_NODIG, false, 1);
    EXPECT(mapTileDiggable(2, 2, 1));

    map.setTileAttribute(1, 1, FLOORLAYER, map_t::TILE_ATTRIBUTE_SLIPPERY, false, 1);
    EXPECT(!map.tileHasAttribute(
        1, 1, FLOORLAYER, map_t::TILE_ATTRIBUTE_SLIPPERY, 1));

    // Missing nonzero geometry is explicit: no silent fallback to floor Z0.
    EXPECT(map.tilesForPlayableFloor(2) == nullptr);
    EXPECT(map.tileAt(1, 1, OBSTACLELAYER, 2) == 0);
    return true;
}

bool testLocalElevationAndRuntimeSpawns()
{
    multiplayer = CLIENT;
    EXPECT(resetGlobalMapHarness(4, 4, 0));

    EXPECT(mapLayerWorldZ(0) == 0.0);
    EXPECT(mapLayerWorldZ(1) == -16.0);
    EXPECT(mapLayerWorldZ(2) == -32.0);
    EXPECT(mapLayerWorldZ(5) == -80.0);
    EXPECT(mapLayerWorldZ(-1) == 0.0);
    EXPECT(mapLayerWorldZ(MAPLAYERS) == mapLayerWorldZ(MAPLAYERS - 1));

    Entity* worldZExample = newEntity(0, 1, map.entities, nullptr);
    EXPECT(worldZExample != nullptr);
    worldZExample->z = 7.5;
    worldZExample->authoredMapLayer = 5;
    worldZExample->playableFloor = 2;
    EXPECT(worldZExample->structuralMapLayer() == 5);
    EXPECT(worldZExample->worldRenderZ() == -72.5);
    EXPECT(worldZExample->structuralLightmapLayer() == 5);
    worldZExample->z = 16.0;
    EXPECT(worldZExample->worldRenderZ() == -64.0);
    EXPECT(worldZExample->structuralLightmapLayer() == 4);
    worldZExample->z = 7.5;

    // Ordinary authored sprites retain one local model height. Structural
    // layer selection alone supplies their world-Z ladder.
    constexpr real_t localSpriteZ = 7.5;
    const std::array<std::pair<int, real_t>, 5> worldZByLayer{{
        {0, 7.5}, {1, -8.5}, {2, -24.5}, {5, -72.5}, {10, -152.5},
    }};
    for (const auto& [layer, expectedWorldZ] : worldZByLayer)
    {
        Entity* ordinarySprite = newEntity(0, 1, map.entities, nullptr);
        EXPECT(ordinarySprite != nullptr);
        ordinarySprite->z = localSpriteZ;
        ordinarySprite->authoredMapLayer = static_cast<Sint16>(layer);
        EXPECT(ordinarySprite->z == localSpriteZ);
        EXPECT(ordinarySprite->worldRenderZ() == expectedWorldZ);
    }
    EXPECT(worldZByLayer[0].second - worldZByLayer[1].second == 16.0);
    EXPECT(worldZByLayer[1].second - worldZByLayer[2].second == 16.0);

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
    parent->authoredMapLayer = 3;
    parent->spatialRevision = 77;
    EXPECT(parent->structuralMapLayer() == 3);
    EXPECT(parent->worldRenderZ() == -28.0);

    // A movement through the derived authored-layer stack changes structural
    // context without baking the destination layer into the actor's local z.
    map.tiles[tileIndex(1, 1, 2, map.height)] = 71;
    EXPECT(map.ensurePlayableFloorGeometry(2, false));
    Entity* floorMover = newEntity(0, 1, map.entities, nullptr);
    EXPECT(floorMover != nullptr);
    floorMover->x = 24.0;
    floorMover->y = 24.0;
    floorMover->z = 7.5;
    EXPECT(floorMover->transitionToPlayableFloor(2, 24.0, 24.0, 7.5));
    EXPECT(floorMover->playableFloor == 2);
    EXPECT(floorMover->authoredMapLayer == 2);
    EXPECT(floorMover->z == 7.5);
    EXPECT(floorMover->worldRenderZ() == -24.5);

    // An explicit legacy FLOR buffer is gameplay-separated but structurally
    // local; leaving the authored stack must not retain its old layer offset.
    EXPECT(map.ensurePlayableFloorGeometry(3, true));
    EXPECT(map.setTileAt(1, 1, FLOORLAYER, 71, 3));
    EXPECT(map.setTileAt(1, 1, OBSTACLELAYER, 0, 3));
    map.setTileAttribute(
        1, 1, FLOORLAYER, map_t::TILE_ATTRIBUTE_SLIPPERY, true, 3);
    EXPECT(!map.playableFloorUsesAuthoredLayerStack(3));
    EXPECT(floorMover->transitionToPlayableFloor(3, 24.0, 24.0, 7.5));
    EXPECT(floorMover->playableFloor == 3);
    EXPECT(floorMover->authoredMapLayer == 0);
    EXPECT(floorMover->z == 7.5);
    EXPECT(floorMover->worldRenderZ() == 7.5);

    Entity* inherited = newEntityWithSpatialContext(
        0, 1, map.entities, nullptr, parent);
    EXPECT(inherited != nullptr);
    EXPECT(inherited->playableFloor == 3);
    EXPECT(inherited->spatialRevision == 77);
    EXPECT(inherited->authoredMapLayer == 3);
    EXPECT(inherited->structuralMapLayer() == 3);
    Entity* explicitContext = newEntityWithSpatialContext(
        0, 1, map.entities, nullptr, SpatialSpawnContext{-2, 19});
    EXPECT(explicitContext != nullptr);
    EXPECT(explicitContext->playableFloor == -2);
    EXPECT(explicitContext->spatialRevision == 19);
    EXPECT(explicitContext->authoredMapLayer == 0);
    const SpatialSpawnContext gameplayOnlyContext{3, 21};
    EXPECT(gameplayOnlyContext.playableFloor == 3);
    EXPECT(gameplayOnlyContext.authoredMapLayer == 0);

    // Parent-aware runtime spawns must inherit the complete spatial context.
    // Coordinate-only/network reconstruction helpers remain explicit Z0 seams
    // until their packets/APIs gain a playable-floor field in Z2.
    Entity* clientGib = spawnGibClient(12, 34, -7, 5);
    EXPECT(clientGib != nullptr);
    EXPECT(clientGib->x == 12.0);
    EXPECT(clientGib->y == 34.0);
    EXPECT(clientGib->z == -7.0);
    EXPECT(clientGib->playableFloor == DEFAULT_PLAYABLE_FLOOR);
    EXPECT(clientGib->spatialRevision == 0);

    Entity* gib = spawnGib(parent, 5);
    EXPECT(gib != nullptr);
    EXPECT(gib->x == parent->x);
    EXPECT(gib->y == parent->y);
    EXPECT(gib->z >= 8.0);
    EXPECT(gib->z <= parent->z - 4.0);
    EXPECT(gib->playableFloor == parent->playableFloor);
    EXPECT(gib->spatialRevision == parent->spatialRevision);

    Entity* particle = spawnMagicParticleCustom(parent, 245, 1.0, 10.0);
    EXPECT(particle != nullptr);
    EXPECT(std::fabs(particle->z - parent->z) <= 0.11);
    EXPECT(std::fabs(particle->x - parent->x) <= 0.11);
    EXPECT(std::fabs(particle->y - parent->y) <= 0.11);
    EXPECT(particle->playableFloor == parent->playableFloor);
    EXPECT(particle->spatialRevision == parent->spatialRevision);

    Entity* summon = summonMonsterNoSmoke(RAT, 48, 64, true);
    EXPECT(summon != nullptr);
    EXPECT(summon->x == 48.0);
    EXPECT(summon->y == 64.0);
    EXPECT(summon->z == 6.0);
    EXPECT(summon->playableFloor == DEFAULT_PLAYABLE_FLOOR);
    EXPECT(summon->spatialRevision == 0);

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


bool testZ34CStructuralLayersAndFalling()
{
    multiplayer = SINGLE;
    EXPECT(resetGlobalMapHarness(4, 4, 1));

    // A dynamic/runtime entity follows its current playable floor for world
    // rendering even if its immutable editor-authored provenance was layer 0.
    Entity* dynamic = nullptr;
    {
        ScopedPlayableFloorRuntimeContext scope(SpatialSpawnContext{2, 55, 2});
        dynamic = newEntity(0, 1, map.entities, nullptr);
    }
    EXPECT(dynamic != nullptr);
    EXPECT(dynamic->playableFloor == 2);
    EXPECT(dynamic->authoredMapLayer == 2);
    dynamic->z = 6.0;
    EXPECT(dynamic->structuralMapLayer() == 2);
    EXPECT(dynamic->worldRenderZ() == -26.0);

    // Vertical stairs are boundary objects: a stair sourced from floor 1 is
    // authored/rendered on layer 2 rather than being pulled down to floor 1.
    Entity* stair = newEntity(0, 1, map.entities, nullptr);
    EXPECT(stair != nullptr);
    stair->playableFloor = 1;
    stair->authoredMapLayer = 2;
    stair->verticalLayerTransitionDelta = -1;
    stair->z = 7.5;
    EXPECT(stair->structuralMapLayer() == 2);
    EXPECT(stair->worldRenderZ() == -24.5);

    // Default runtime lights inherit the same structural spawn layer. This is
    // the path campfires and other behavior-created lights use upstairs.
    EXPECT(map.ensurePlayableFloorGeometry(2, false));
    auto& sharedLightmap = lightmapForPlayableFloor(0, 2, map.width, map.height);
    std::fill(sharedLightmap.begin(), sharedLightmap.end(), vec4_t{});
    light_t* structuralLight = nullptr;
    {
        ScopedPlayableFloorRuntimeContext scope(SpatialSpawnContext{2, 55, 2});
        structuralLight = lightSphere(0, 1, 1, 2, 1.f, 1.f, 1.f, 0.f, 1.f);
    }
    EXPECT(structuralLight != nullptr);
    EXPECT(structuralLight->playableFloor == 2);
    EXPECT(structuralLight->layer == 2);
    list_RemoveNode(structuralLight->node);

    // Falling searches the original authored stack downward. With layers 1 and
    // 2 empty, falling from floor 2 reaches floor 0 and crosses two blocks.
    const std::size_t x = 1;
    const std::size_t y = 1;
    map.tiles[tileIndex(x, y, 1, map.height)] = 0;
    map.tiles[tileIndex(x, y, 2, map.height)] = 0;
    PlayableFloorId landingFloor = -1;
    int floorsFallen = -1;
    EXPECT(map.findLowerPlayableFloorLanding(
        static_cast<int>(x), static_cast<int>(y), 2,
        landingFloor, floorsFallen));
    EXPECT(landingFloor == 0);
    EXPECT(floorsFallen == 2);

    // Add a valid layer-1 surface with empty layer-2 headroom: now it is the
    // nearest landing and the fall is exactly one block.
    map.tiles[tileIndex(x, y, 1, map.height)] = 71;
    map.tiles[tileIndex(x, y, 2, map.height)] = 0;
    EXPECT(map.ensurePlayableFloorGeometry(2, false));
    EXPECT(map.findLowerPlayableFloorLanding(
        static_cast<int>(x), static_cast<int>(y), 2,
        landingFloor, floorsFallen));
    EXPECT(landingFloor == 1);
    EXPECT(floorsFallen == 1);

    // If every lower authored surface is absent there is no stacked landing;
    // actPlayer must retain the original bottomless-pit behavior.
    map.tiles[tileIndex(x, y, 0, map.height)] = 0;
    map.tiles[tileIndex(x, y, 1, map.height)] = 0;
    EXPECT(!map.findLowerPlayableFloorLanding(
        static_cast<int>(x), static_cast<int>(y), 2,
        landingFloor, floorsFallen));
    EXPECT(floorsFallen == 0);
    return true;
}

bool testZ2CRuntimeFloorIsolation()
{
    multiplayer = SINGLE;
    EXPECT(resetGlobalMapHarness(4, 4, 1));

    // Z3.3B: a nonzero runtime floor can be derived from the existing authored
    // layer stack. Floor 1 sees authored layer 1 as its floor and layer 2 as
    // its obstacle layer; no second editor-owned geometry stack is required.
    EXPECT(map.setTileAt(1, 1, 1, 71, DEFAULT_PLAYABLE_FLOOR));
    EXPECT(map.setTileAt(1, 1, 2, 0, DEFAULT_PLAYABLE_FLOOR));
    EXPECT(map.ensurePlayableFloorGeometry(1, false));
    EXPECT(map.tileAt(1, 1, FLOORLAYER, 1) == 71);
    EXPECT(map.tileAt(1, 1, OBSTACLELAYER, 1) == 0);
    const PlayableFloorData* derivedFloor = map.playableFloors.find(1);
    EXPECT(derivedFloor != nullptr);
    EXPECT(derivedFloor->derivedFromMapLayers);

    // The older explicit-copy compatibility path remains available, but the
    // layer-authored stair path returns to the derived shared-world view.
    EXPECT(map.ensurePlayableFloorGeometry(1, true));
    const PlayableFloorData* explicitFloor = map.playableFloors.find(1);
    EXPECT(explicitFloor != nullptr);
    EXPECT(!explicitFloor->derivedFromMapLayers);
    EXPECT(map.ensurePlayableFloorGeometry(1, false));
    derivedFloor = map.playableFloors.find(1);
    EXPECT(derivedFloor != nullptr);
    EXPECT(derivedFloor->derivedFromMapLayers);

    // Bare runtime spawns now inherit the scoped entity simulation context.
    Entity* scopedEntity = nullptr;
    {
        ScopedPlayableFloorRuntimeContext scope(SpatialSpawnContext{1, 91, 1});
        scopedEntity = newEntity(0, 1, map.entities, nullptr);
    }
    EXPECT(scopedEntity != nullptr);
    EXPECT(scopedEntity->playableFloor == 1);
    EXPECT(scopedEntity->spatialRevision == 91);
    EXPECT(scopedEntity->authoredMapLayer == 1);

    Entity* legacyEntity = newEntity(0, 1, map.entities, nullptr);
    EXPECT(legacyEntity != nullptr);
    EXPECT(legacyEntity->playableFloor == DEFAULT_PLAYABLE_FLOOR);
    EXPECT(legacyEntity->spatialRevision == 0);
    EXPECT(playableFloorsShareRuntimeScope(1, 1));
    EXPECT(!playableFloorsShareRuntimeScope(0, 1));

    // Z3.3C: layer-authored floors are one visible/lightable structure. Their
    // collision scopes remain distinct, but their render light volume is shared.
    EXPECT(map.setTileAt(2, 1, OBSTACLELAYER, 77, DEFAULT_PLAYABLE_FLOOR));
    EXPECT(map.setTileAt(2, 1, OBSTACLELAYER, 0, 1));
    auto& lowerLightmap = lightmapForPlayableFloor(0, 0, map.width, map.height);
    auto& upperLightmap = lightmapForPlayableFloor(0, 1, map.width, map.height);
    EXPECT(&lowerLightmap == &upperLightmap);
    std::fill(lowerLightmap.begin(), lowerLightmap.end(), vec4_t{});

    // A floor-1 light writes into the same shared volume. The light's existing
    // layer coordinate is preserved; authored static lights already derive it
    // from Entity::z while runtime-local lights remain backward compatible.
    light_t* upperLight = lightSphereShadowOnPlayableFloor(
        0, 1, 1, 1, 0, 5, 1.f, 1.f, 1.f, 0.f, 1.f);
    EXPECT(upperLight != nullptr);
    EXPECT(upperLight->playableFloor == 1);
    const std::size_t litIndex =
        lightmapIndex3D(3, 1, 0, map.width, map.height);
    EXPECT(upperLightmap[litIndex].x > 0.f);
    EXPECT(lowerLightmap[litIndex].x > 0.f);

    // A lower-floor light adds to that same volume rather than disappearing
    // when the camera's collision slice moves upstairs. The sample used above
    // sits behind the intentional floor-0 wall at (2,1), so a floor-0 shadowed
    // light must NOT be expected to reach it. Sample the unobstructed source
    // tile instead while keeping the wall-shadow characterization intact.
    const std::size_t sharedLightIndex =
        lightmapIndex3D(1, 1, 0, map.width, map.height);
    const float beforeLowerLight = lowerLightmap[sharedLightIndex].x;
    light_t* lowerLight = lightSphereShadowOnPlayableFloor(
        0, 1, 1, 0, 0, 5, 1.f, 1.f, 1.f, 0.f, 1.f);
    EXPECT(lowerLight != nullptr);
    EXPECT(lowerLightmap[sharedLightIndex].x > beforeLowerLight);
    EXPECT(upperLightmap[sharedLightIndex].x == lowerLightmap[sharedLightIndex].x);

    list_RemoveNode(lowerLight->node);
    list_RemoveNode(upperLight->node);
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
        && testPlayableFloorCollisionIsolation()
        && testZ3TransitionPrimitive()
        && testPlayableFloorGeometryIsolation()
        && testLocalElevationAndRuntimeSpawns()
        && testZ34CStructuralLayersAndFalling()
        && testZ2CRuntimeFloorIsolation()
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
            << "Stage 4D/Z3.4C stacked-sprite/fall runtime passed: legacy Entity::z remains "
            << "Z0-safe, ELYR authored-layer round-trip, structural runtime rendering/light "
            << "context, floor-aware multipart spawns, lower-floor landing search, persistent "
            << "Hermit ownership contracts, and same-map stacked stairs.\n";
    }
    return passed ? 0 : 1;
}
