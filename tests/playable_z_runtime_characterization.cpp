#include "../src/main.hpp"
#include "../src/game.hpp"
#include "../src/stat.hpp"
#include "../src/entity.hpp"
#include "../src/engine/audio/sound.hpp"
#include "../src/collision.hpp"
#include "../src/files.hpp"
#include "../src/magic/magic.hpp"
#include "../src/monster.hpp"
#include "../src/light.hpp"
#include "../src/player.hpp"
#include "../src/paths.hpp"
#include "../src/items.hpp"
#include "../src/automatia_save.hpp"
#include "../src/world_state.hpp"
#include "../src/vertical_navigation.hpp"
#include "../src/cross_floor_path.hpp"
#include "../src/follower_vertical_navigation.hpp"
#include "../src/hostile_vertical_navigation.hpp"
#ifdef SAM_FRAMEWORK_ENABLED
#include "../src/sam/sam_item_registry_foundation.hpp"
#endif

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
	authoredRoomGroupsReset(map.roomGroups);
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
    // Every pre-V4.10 fixture has no optional ambience metadata.
    EXPECT(!loaded.ambience.enabled);
    EXPECT(loaded.ambience.resource[0] == '\0');
    EXPECT(!loaded.ambientLight.enabled);
	EXPECT(loaded.roomGroups.count == 0);
	EXPECT(loaded.roomGroups.nextID == 1);
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
	for (const PlayableFloorId floorID : {5, 7})
	{
		PlayableFloorData authoredFloor;
		authoredFloor.id = floorID;
		authoredFloor.tiles.resize(
			static_cast<std::size_t>(map.width)
			* static_cast<std::size_t>(map.height) * MAPLAYERS);
		for (std::size_t index = 0; index < authoredFloor.tiles.size(); ++index)
		{
			authoredFloor.tiles[index] = map.tiles[index] + 5000 * floorID;
		}
		EXPECT(map.playableFloors.addFloor(std::move(authoredFloor)));
	}

    // Include a model-backed ceiling editor sprite in the current-format
    // round-trip. Its fixed -24 runtime height is local; ELYR owns the
    // structural layer independently from the stair fixtures.
    constexpr real_t ceilingLocalZ = -24.0;
    Entity* saved = newEntity(0, 1, map.entities, nullptr);
    Entity* lowerTransition = newEntity(0, 1, map.entities, nullptr);
    Entity* savedCeiling = newEntity(119, 1, map.entities, nullptr);
	Entity* savedCustomItem = newEntity(8, 1, map.entities, nullptr);
	Entity* savedCustomInventoryMonster = newEntity(10, 1, map.entities, nullptr);
    EXPECT(saved != nullptr);
    EXPECT(lowerTransition != nullptr);
    EXPECT(savedCeiling != nullptr);
	EXPECT(savedCustomItem != nullptr);
	EXPECT(savedCustomInventoryMonster != nullptr);
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
    lowerTransition->verticalLayerTransitionModel = 0;
    lowerTransition->verticalLayerTransitionRotation = 6;
    lowerTransition->floorDecorationHeightOffset = 12;
    lowerTransition->floorDecorationXOffset = 4;
    lowerTransition->floorDecorationYOffset = -8;
    lowerTransition->floorDecorationDestroyIfNoWall = 4;
    lowerTransition->skill[8] = static_cast<Sint32>(0x52494154); // "TAIR"
    lowerTransition->skill[9] = static_cast<Sint32>(0x00000053); // "S"
    saved->verticalLayerTransitionDelta = -1;
    // Zero is the legacy/default sentinel. Save must resolve it to the
    // directional model rather than letting the editor preview show null.vox.
    saved->verticalLayerTransitionModel = 0;
    saved->verticalLayerTransitionRotation = 2;
    saved->floorDecorationHeightOffset = -4;
    saved->floorDecorationXOffset = -3;
    saved->floorDecorationYOffset = 7;
    saved->floorDecorationDestroyIfNoWall = -1;

    savedCeiling->persistentID = 79;
    savedCeiling->authoredMapLayer = 2;
    savedCeiling->playableFloor = 2;
    savedCeiling->x = 12.0;
    savedCeiling->y = 27.0;
    savedCeiling->z = ceilingLocalZ;
    savedCeiling->ceilingTileModel = 1219;
    savedCeiling->ceilingTileDir = 3;
    savedCeiling->ceilingTileAllowTrap = 1;
    savedCeiling->ceilingTileBreakable = 1;

	// Generic authored stable-item metadata shares the ordinary Entity/Stat
	// copy paths. LMP item fields retain the legacy runtime-ID-plus-two encoding;
	// stable identity must remap that value rather than treating it as raw.
	savedCustomItem->persistentID = 80;
	savedCustomItem->authoredMapLayer = 5;
	savedCustomItem->playableFloor = 5;
	savedCustomItem->x = 16.0;
	savedCustomItem->y = 16.0;
	savedCustomItem->z = 2.25;
	savedCustomItem->skill[10] = 6123 + EDITOR_ITEM_ID_OFFSET;
	savedCustomItem->authoredItemStableID = "fixture:custom_item";
	setSpriteAttributes(savedCustomInventoryMonster, nullptr, nullptr);
	savedCustomInventoryMonster->behavior = &actMonster;
	Stat* savedMonsterStats = savedCustomInventoryMonster->getStats();
	EXPECT(savedMonsterStats != nullptr);
	savedCustomInventoryMonster->persistentID = 81;
	savedCustomInventoryMonster->authoredMapLayer = 7;
	savedCustomInventoryMonster->playableFloor = 7;
	savedCustomInventoryMonster->x = 24.0;
	savedCustomInventoryMonster->y = 24.0;
	savedCustomInventoryMonster->z = -1.5;
	savedMonsterStats->EDITOR_ITEMS[0] = 6124 + EDITOR_ITEM_ID_OFFSET;
	savedMonsterStats->EDITOR_ITEM_STABLE_IDS[0] = "fixture:monster_weapon";

	EXPECT(authoredRoomGroupAdd(map.roomGroups, "Whole tower",
		0, 0, 1, 1, 0, 31, AUTHORED_ROOM_GROUP_BOTH) == 0);
	EXPECT(authoredRoomGroupAdd(map.roomGroups, "Upper cache",
		1, 1, 1, 1, 5, 7, AUTHORED_ROOM_GROUP_SPRITES) == 1);
    // This remains independent from the existing packed custom-fog fields.
    map.flags[MAP_FLAG_GENBYTES5] = (static_cast<Uint32>(0xA5) << 24)
        | (static_cast<Uint32>(24) << 16) | (static_cast<Uint32>(180) << 8);
    map.flags[MAP_FLAG_GENBYTES6] = (static_cast<Uint32>(32) << 24)
        | (static_cast<Uint32>(64) << 16) | (static_cast<Uint32>(96) << 8);
    map.ambience.enabled = true;
    map.ambience.loop = true;
    map.ambience.volume = 67;
    map.ambience.fadeInMilliseconds = 350;
    map.ambience.fadeOutMilliseconds = 900;
    std::snprintf(map.ambience.resource, sizeof(map.ambience.resource), "%s",
        "sound/ambience/fixture_cave_wind.ogg");
    // This is the map-wide RGB light base used by Hell maps, not fog or audio.
    map.ambientLight.enabled = true;
    map.ambientLight.red = 32;
    map.ambientLight.green = 12;
    map.ambientLight.blue = 6;
	// Keep both saved ZLDR stairs traversable after reload so the Z4A graph can
	// be reconstructed solely from the existing map metadata.
	map.tiles[tileIndex(0, 1, 1, map.height)] = 71;
	map.tiles[tileIndex(0, 1, 2, map.height)] = 0;
	map.tiles[tileIndex(1, 1, 1, map.height)] = 71;
	map.tiles[tileIndex(1, 1, 2, map.height)] = 0;
	PlayableFloorData* savedUpperGeometry = map.playableFloors.find(2);
	EXPECT(savedUpperGeometry != nullptr);
	for (const std::size_t changedIndex : {
		tileIndex(0, 1, 1, map.height),
		tileIndex(0, 1, 2, map.height),
		tileIndex(1, 1, 1, map.height),
		tileIndex(1, 1, 2, map.height)})
	{
		savedUpperGeometry->tiles[changedIndex] =
			map.tiles[changedIndex] + 5000;
	}
    EXPECT(saveMap("stage4b_roundtrip") == 0);

    std::ifstream output(
        temporary.mapPath("stage4b_roundtrip.lmp"), std::ios::binary);
    std::array<char, 15> header{};
    output.read(header.data(), static_cast<std::streamsize>(header.size()));
    EXPECT(std::string(header.data(), header.size()) == "BARONY LMPV4.10");

#ifdef SAM_FRAMEWORK_ENABLED
	SAMItemRegistryFoundation::clear();
	EXPECT(SAMItemRegistryFoundation::registerFrameworkBuiltin(
		"fixture:custom_item", 7123, "Custom map fixture", "TOOL"));
	EXPECT(SAMItemRegistryFoundation::registerFrameworkBuiltin(
		"fixture:monster_weapon", 7124, "Monster map fixture", "WEAPON"));
#endif

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
		nullptr) == 5);
	EXPECT(loaded.numLayers == MAPLAYERS);
	EXPECT(list_Size(&entities) == 5);
    Entity* restored = nullptr;
    Entity* restoredLowerTransition = nullptr;
    Entity* restoredCeiling = nullptr;
	Entity* restoredCustomItem = nullptr;
	Entity* restoredCustomInventoryMonster = nullptr;
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
        else if (candidate && candidate->persistentID == 79)
        {
            restoredCeiling = candidate;
        }
		else if (candidate && candidate->persistentID == 80)
		{
			restoredCustomItem = candidate;
		}
		else if (candidate && candidate->persistentID == 81)
		{
			restoredCustomInventoryMonster = candidate;
		}
    }
    EXPECT(restored != nullptr);
    EXPECT(restoredLowerTransition != nullptr);
    EXPECT(restoredCeiling != nullptr);
	EXPECT(restoredCustomItem != nullptr);
	EXPECT(restoredCustomInventoryMonster != nullptr);
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
    EXPECT(restoredCeiling->sprite == 119);
    EXPECT(restoredCeiling->z == ceilingLocalZ);
    EXPECT(restoredCeiling->authoredMapLayer == 2);
    EXPECT(restoredCeiling->playableFloor == 2);
    EXPECT(restoredCeiling->worldRenderZ()
        == ceilingLocalZ + mapLayerWorldZ(2));
    EXPECT(restoredCeiling->ceilingTileModel == 1219);
    EXPECT(restoredCeiling->ceilingTileDir == 3);
    EXPECT(restoredCeiling->ceilingTileAllowTrap == 1);
    EXPECT(restoredCeiling->ceilingTileBreakable == 1);
	EXPECT(restoredCustomItem->authoredItemStableID == "fixture:custom_item");
	EXPECT(restoredCustomItem->skill[10]
		== 7123 + EDITOR_ITEM_ID_OFFSET);
	EXPECT(restoredCustomItem->authoredMapLayer == 5);
	EXPECT(restoredCustomItem->playableFloor == 5);
	EXPECT(restoredCustomItem->z == 2.25);
	restoredCustomInventoryMonster->behavior = &actMonster;
	Stat* restoredMonsterStats = restoredCustomInventoryMonster->getStats();
	EXPECT(restoredMonsterStats != nullptr);
	EXPECT(restoredMonsterStats->EDITOR_ITEMS[0]
		== 7124 + EDITOR_ITEM_ID_OFFSET);
	EXPECT(restoredMonsterStats->EDITOR_ITEM_STABLE_IDS[0]
		== "fixture:monster_weapon");
	EXPECT(restoredCustomInventoryMonster->authoredMapLayer == 7);
	EXPECT(restoredCustomInventoryMonster->playableFloor == 7);
	EXPECT(restoredCustomInventoryMonster->z == -1.5);

	Entity* copiedCustomItem = newEntity(8, 1, &entities, nullptr);
	EXPECT(copiedCustomItem != nullptr);
	setSpriteAttributes(copiedCustomItem, restoredCustomItem, restoredCustomItem);
	EXPECT(copiedCustomItem->authoredItemStableID == "fixture:custom_item");
	list_RemoveNode(copiedCustomItem->mynode);
	Entity* copiedCustomMonster = newEntity(10, 1, &entities, nullptr);
	EXPECT(copiedCustomMonster != nullptr);
	setSpriteAttributes(copiedCustomMonster, restoredCustomInventoryMonster,
		restoredCustomInventoryMonster);
	copiedCustomMonster->behavior = &actMonster;
	EXPECT(copiedCustomMonster->getStats() != nullptr);
	EXPECT(copiedCustomMonster->getStats()->EDITOR_ITEM_STABLE_IDS[0]
		== "fixture:monster_weapon");
	list_RemoveNode(copiedCustomMonster->mynode);
#ifdef SAM_FRAMEWORK_ENABLED
	SAMItemRegistryFoundation::clear();
#endif

	EXPECT(loaded.roomGroups.count == 2);
	EXPECT(std::string(loaded.roomGroups.entries[0].name) == "Whole tower");
	EXPECT(loaded.roomGroups.entries[0].bottomLayer == 0);
	EXPECT(loaded.roomGroups.entries[0].topLayer == 31);
	EXPECT(loaded.roomGroups.entries[0].contentMask == AUTHORED_ROOM_GROUP_BOTH);
	EXPECT(std::string(loaded.roomGroups.entries[1].name) == "Upper cache");
	EXPECT(loaded.roomGroups.entries[1].bottomLayer == 5);
	EXPECT(loaded.roomGroups.entries[1].topLayer == 7);
	EXPECT(loaded.roomGroups.entries[1].contentMask
		== AUTHORED_ROOM_GROUP_SPRITES);
    EXPECT(loaded.flags[MAP_FLAG_GENBYTES5] == map.flags[MAP_FLAG_GENBYTES5]);
    EXPECT(loaded.flags[MAP_FLAG_GENBYTES6] == map.flags[MAP_FLAG_GENBYTES6]);
    EXPECT(loaded.ambience.enabled);
    EXPECT(loaded.ambience.loop);
    EXPECT(loaded.ambience.volume == 67);
    EXPECT(loaded.ambience.fadeInMilliseconds == 350);
    EXPECT(loaded.ambience.fadeOutMilliseconds == 900);
    EXPECT(std::string(loaded.ambience.resource)
        == "sound/ambience/fixture_cave_wind.ogg");
    EXPECT(loaded.ambientLight.enabled);
    EXPECT(loaded.ambientLight.red == 32);
    EXPECT(loaded.ambientLight.green == 12);
    EXPECT(loaded.ambientLight.blue == 6);
	VerticalNavigationGraph restoredVerticalGraph;
	EXPECT(rebuildVerticalNavigationGraphFromMap(
		restoredVerticalGraph, "stage4b_roundtrip.lmp#world", loaded));
	EXPECT(restoredVerticalGraph.instanceKey()
		== "stage4b_roundtrip.lmp#world");
	EXPECT(restoredVerticalGraph.edgeCount() == 2);
	EXPECT(restoredVerticalGraph.canReachFloor(
		"stage4b_roundtrip.lmp#world", 0,
		"stage4b_roundtrip.lmp#world", 1));
	EXPECT(restoredVerticalGraph.canReachFloor(
		"stage4b_roundtrip.lmp#world", 2,
		"stage4b_roundtrip.lmp#world", 1));
	EXPECT(!restoredVerticalGraph.canReachFloor(
		"stage4b_roundtrip.lmp#world", 1,
		"stage4b_roundtrip.lmp#world", 2));
    EXPECT(loaded.playableFloors.floors.size() == 5);
    EXPECT(loaded.playableFloors.hasFloor(DEFAULT_PLAYABLE_FLOOR));
	EXPECT(loaded.playableFloors.hasFloor(1));
	EXPECT(loaded.playableFloors.hasFloor(5));
	EXPECT(loaded.playableFloors.hasFloor(7));
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

#ifdef SAM_FRAMEWORK_ENABLED
	// Loading the same authored map without its S.A.M definitions preserves the
	// stable metadata but must not reinterpret either old numeric ID as another
	// mod's item. Ground-item activation will omit the unavailable stable item;
	// a zero monster slot generates no equipment.
	SAMItemRegistryFoundation::clear();
	map_t missingContentMap{};
	list_t missingContentEntities{};
	list_t missingContentCreatures{};
	missingContentMap.entities = &missingContentEntities;
	missingContentMap.creatures = &missingContentCreatures;
	EXPECT(loadMap("stage4b_roundtrip.lmp", &missingContentMap,
		&missingContentEntities, &missingContentCreatures, nullptr) == 5);
	Entity* missingGroundItem = nullptr;
	Entity* missingMonster = nullptr;
	for (node_t* node = missingContentEntities.first; node; node = node->next)
	{
		Entity* candidate = static_cast<Entity*>(node->element);
		if (candidate && candidate->persistentID == 80)
		{
			missingGroundItem = candidate;
		}
		else if (candidate && candidate->persistentID == 81)
		{
			missingMonster = candidate;
		}
	}
	EXPECT(missingGroundItem != nullptr);
	EXPECT(missingGroundItem->authoredItemStableID == "fixture:custom_item");
	EXPECT(missingGroundItem->skill[10] == 0);
	EXPECT(missingMonster != nullptr);
	missingMonster->behavior = &actMonster;
	EXPECT(missingMonster->getStats() != nullptr);
	EXPECT(missingMonster->getStats()->EDITOR_ITEM_STABLE_IDS[0]
		== "fixture:monster_weapon");
	EXPECT(missingMonster->getStats()->EDITOR_ITEMS[0] == 0);
	clearLoadedMap(missingContentMap, missingContentEntities,
		missingContentCreatures);
#endif

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

    // V4.10 must reject truncated map metadata/PZLV data instead of flattening.
    const std::filesystem::path validPath =
        temporary.mapPath("stage4b_roundtrip.lmp");
    const std::filesystem::path corruptPath =
        temporary.mapPath("stage4c_truncated_v4_10.lmp");
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
        "stage4c_truncated_v4_10.lmp",
        &corrupted,
        &corruptEntities,
        &corruptCreatures,
        nullptr) == -1);
    clearLoadedMap(corrupted, corruptEntities, corruptCreatures);
    return true;
}

bool testMapAmbienceLifecycleCharacterization()
{
    // A floor transition within one MapInstance must retain the existing loop;
    // a divergent-map activation gets its own loop and fades the previous one.
    EXPECT(!mapAmbienceRequiresRestart("caves.lmp#world", "caves.lmp#world"));
    EXPECT(mapAmbienceRequiresRestart("caves.lmp#world", "ruins.lmp#world"));
    EXPECT(mapAmbienceRequiresRestart("caves.lmp#world", "caves.lmp#private_2"));

    // This is deliberately a control-flow characterization, not an assertion
    // that an audio device emitted samples.
    EXPECT(!mapAmbienceCanUseAudio(true, false));
    EXPECT(!mapAmbienceCanUseAudio(false, true));
    EXPECT(mapAmbienceCanUseAudio(false, false));
    const bool previousHeadless = headless;
    const bool previousNoSound = no_sound;
    headless = true;
    no_sound = false;
    map_t ambienceMap{};
    ambienceMap.ambience.enabled = true;
    std::snprintf(ambienceMap.ambience.resource,
        sizeof(ambienceMap.ambience.resource), "%s", "sound/ambience/test.ogg");
    syncMapAmbience(ambienceMap, "caves.lmp#world");
    stopMapAmbience();
    headless = previousHeadless;
    no_sound = previousNoSound;
    return true;
}

#ifdef SAM_FRAMEWORK_ENABLED
list_t* attachEmptyChestInventory(Entity& chest)
{
	node_t* inventoryNode = list_AddNodeFirst(&chest.children);
	if (!inventoryNode)
	{
		return nullptr;
	}
	list_t* inventory = static_cast<list_t*>(std::calloc(1, sizeof(list_t)));
	if (!inventory)
	{
		list_RemoveNode(inventoryNode);
		return nullptr;
	}
	inventoryNode->element = inventory;
	inventoryNode->deconstructor = &listDeconstructor;
	inventoryNode->size = sizeof(list_t);
	return inventory;
}

const AutomatiaSave::Json* persistentMechanismById(
	const AutomatiaSave::Json& document,
	const std::string& mapKey,
	const Sint32 persistentID)
{
	if (!document.contains("map_instances")
		|| !document["map_instances"].is_array())
	{
		return nullptr;
	}
	for (const AutomatiaSave::Json& savedMap : document["map_instances"])
	{
		if (!savedMap.is_object()
			|| savedMap.value("map_file", std::string{}) + "#"
				+ savedMap.value("instance_id", std::string{}) != mapKey
			|| !savedMap.contains("persistent_state")
			|| !savedMap["persistent_state"].is_object())
		{
			continue;
		}
		const AutomatiaSave::Json& persistent = savedMap["persistent_state"];
		if (!persistent.contains("mechanisms")
			|| !persistent["mechanisms"].is_array())
		{
			return nullptr;
		}
		for (const AutomatiaSave::Json& mechanism : persistent["mechanisms"])
		{
			if (mechanism.is_object()
				&& mechanism.value("persistent_id", Sint32{0}) == persistentID)
			{
				return &mechanism;
			}
		}
	}
	return nullptr;
}

bool testSAMPersistentContainersAndMonsterEquipment(
	TemporaryDataDirectory& temporary)
{
	constexpr Sint32 capturedRuntimeID = 6123;
	constexpr Sint32 restoredRuntimeID = 7123;
	constexpr Sint32 chestPersistentID = 7101;
	constexpr Sint32 mimicPersistentID = 7201;
	constexpr PlayableFloorId upperFloor = 5;
	const std::string stableID = "fixture:persistent_item";
	const std::string activeKey = "cross_feature.lmp#instance_a";
	const std::string otherKey = "cross_feature.lmp#instance_b";
	const std::string sessionID = "cross-feature-session";
	const std::string transactionID = "cross-feature-transaction";

	multiplayer = SINGLE;
	resetPersistentWorldSession();
	SAMItemRegistryFoundation::clear();
	EXPECT(SAMItemRegistryFoundation::registerFrameworkBuiltin(
		stableID, capturedRuntimeID, "Persistent fixture", "TOOL"));
	EXPECT(resetGlobalMapHarness(4, 4, 1));
	EXPECT(map.ensurePlayableFloorGeometry(upperFloor, false));
	std::snprintf(
		map.filename, sizeof(map.filename), "%s", "cross_feature.lmp");
	EXPECT(worldState.bindMap(map, map.filename, "instance_a"));

	MapInstanceSummary otherInstance;
	EXPECT(otherInstance.identity.set("cross_feature.lmp", "instance_b"));
	otherInstance.identity.revision = 1;
	otherInstance.playableFloors = {DEFAULT_PLAYABLE_FLOOR, upperFloor};
	EXPECT(worldState.registerUnloadedInstance(otherInstance));

	Entity* chest = newEntity(21, 1, map.entities, nullptr);
	EXPECT(chest != nullptr);
	chest->behavior = &actChest;
	chest->persistentID = chestPersistentID;
	chest->authoredMapLayer = upperFloor;
	chest->playableFloor = upperFloor;
	chest->x = 24.0;
	chest->y = 24.0;
	chest->z = 1.25;
	chest->chestHealth = 31;
	chest->chestMaxHealth = 40;
	chest->chestLocked = 1;
	chest->chestVoidState = 0;
	list_t* chestInventory = attachEmptyChestInventory(*chest);
	EXPECT(chestInventory != nullptr);
	Item* chestItem = newItem(
		static_cast<ItemType>(capturedRuntimeID),
		EXCELLENT, 2, 3, 0x12345678U, true, chestInventory);
	EXPECT(chestItem != nullptr);
	EXPECT(static_cast<Sint32>(chestItem->type) == capturedRuntimeID);
	chestItem->x = 2;
	chestItem->y = 1;

	Entity* miniMimic = newEntity(10, 1, map.entities, nullptr);
	EXPECT(miniMimic != nullptr);
	setSpriteAttributes(miniMimic, nullptr, nullptr);
	miniMimic->behavior = &actMonster;
	miniMimic->persistentID = mimicPersistentID;
	miniMimic->authoredMapLayer = upperFloor;
	miniMimic->playableFloor = upperFloor;
	miniMimic->x = 40.0;
	miniMimic->y = 48.0;
	miniMimic->z = -1.5;
	miniMimic->yaw = 0.75;
	miniMimic->skill[3] = 2;
	Stat* mimicStats = miniMimic->getStats();
	EXPECT(mimicStats != nullptr);
	mimicStats->type = MINIMIMIC;
	mimicStats->HP = 37;
	mimicStats->MAXHP = 50;
	mimicStats->MP = 4;
	mimicStats->MAXMP = 9;
	Item* mimicWeapon = newItem(
		static_cast<ItemType>(capturedRuntimeID),
		SERVICABLE, -1, 1, 0x87654321U, true, &mimicStats->inventory);
	EXPECT(mimicWeapon != nullptr);
	EXPECT(static_cast<Sint32>(mimicWeapon->type) == capturedRuntimeID);
	mimicStats->weapon = mimicWeapon;

	std::string snapshot;
	std::string error;
	EXPECT(serializeAutomatiaPersistentWorldSnapshot(
		sessionID, 0, snapshot, error));
	const AutomatiaSave::Json scoped = AutomatiaSave::Json::parse(snapshot);
	EXPECT(scoped.value("snapshot_scope", std::string{}) == "map_instance");
	EXPECT(scoped["map_instances"].size() == 1);
	EXPECT(scoped["map_instances"][0]["instance_id"] == "instance_a");
	const AutomatiaSave::Json* savedChest = persistentMechanismById(
		scoped, activeKey, chestPersistentID);
	const AutomatiaSave::Json* savedMimic = persistentMechanismById(
		scoped, activeKey, mimicPersistentID);
	EXPECT(savedChest != nullptr);
	EXPECT(savedMimic != nullptr);
	EXPECT((*savedChest)["playable_floor"] == upperFloor);
	EXPECT((*savedChest)["authored_map_layer"] == upperFloor);
	EXPECT((*savedChest)["chest_inventory"].size() == 1);
	EXPECT((*savedChest)["chest_inventory"][0]["stable_id"] == stableID);
	EXPECT((*savedMimic)["playable_floor"] == upperFloor);
	EXPECT((*savedMimic)["authored_map_layer"] == upperFloor);
	EXPECT((*savedMimic)["monster_items"].size() == 1);
	EXPECT((*savedMimic)["monster_items"][0]["stable_id"] == stableID);
	EXPECT((*savedMimic)["monster_items"][0]["slot"] == 1);

	const std::filesystem::path savePath =
		temporary.mapPath("cross_feature_world.json");
	const std::string savePathText = savePath.string();
	EXPECT(writeAutomatiaPersistentWorldSave(
		savePathText.c_str(), sessionID, transactionID, error));
	AutomatiaSave::Json diskDocument;
	EXPECT(AutomatiaSave::load(savePath, diskDocument).ok);
	EXPECT(diskDocument["map_instances"].size() == 2);
	EXPECT(persistentMechanismById(
		diskDocument, activeKey, chestPersistentID) != nullptr);
	EXPECT(persistentMechanismById(
		diskDocument, otherKey, chestPersistentID) == nullptr);
	EXPECT(loadAutomatiaPersistentWorldSave(
		savePathText.c_str(), sessionID, transactionID, error));

	clearGlobalMapHarness();
	resetPersistentWorldSession();
	SAMItemRegistryFoundation::clear();
	EXPECT(SAMItemRegistryFoundation::registerFrameworkBuiltin(
		stableID, restoredRuntimeID, "Persistent fixture", "TOOL"));
	EXPECT(SAMItemRegistryFoundation::runtimeIdForStableId(stableID)
		== restoredRuntimeID);
	EXPECT(!SAMItemRegistryFoundation::isRegisteredRuntimeItemId(
		capturedRuntimeID));

	EXPECT(resetGlobalMapHarness(4, 4, 1));
	EXPECT(map.ensurePlayableFloorGeometry(upperFloor, false));
	std::snprintf(
		map.filename, sizeof(map.filename), "%s", "cross_feature.lmp");
	EXPECT(worldState.bindMap(map, map.filename, "instance_a"));

	Entity* restoredChest = newEntity(21, 1, map.entities, nullptr);
	EXPECT(restoredChest != nullptr);
	restoredChest->behavior = &actChest;
	restoredChest->persistentID = chestPersistentID;
	restoredChest->authoredMapLayer = upperFloor;
	restoredChest->playableFloor = DEFAULT_PLAYABLE_FLOOR;
	restoredChest->chestVoidState = 0;
	list_t* restoredChestInventory = attachEmptyChestInventory(*restoredChest);
	EXPECT(restoredChestInventory != nullptr);
	EXPECT(newItem(GEM_ROCK, WORN, 0, 1, 0, false,
		restoredChestInventory) != nullptr);

	Entity* restoredMimic = newEntity(10, 1, map.entities, nullptr);
	EXPECT(restoredMimic != nullptr);
	setSpriteAttributes(restoredMimic, nullptr, nullptr);
	restoredMimic->behavior = &actMonster;
	restoredMimic->persistentID = mimicPersistentID;
	restoredMimic->authoredMapLayer = upperFloor;
	restoredMimic->playableFloor = DEFAULT_PLAYABLE_FLOOR;
	restoredMimic->skill[3] = 2;
	Stat* restoredMimicStats = restoredMimic->getStats();
	EXPECT(restoredMimicStats != nullptr);
	restoredMimicStats->type = MINIMIMIC;
	restoredMimicStats->HP = 1;
	restoredMimicStats->MAXHP = 1;
	Item* generatedWeapon = newItem(
		GEM_ROCK, WORN, 0, 1, 0, false, &restoredMimicStats->inventory);
	EXPECT(generatedWeapon != nullptr);
	restoredMimicStats->weapon = generatedWeapon;

	applyPersistentMechanismStates();
	EXPECT(applyPersistentMonsterLivingState(restoredMimic));
	EXPECT(restoredChest->playableFloor == upperFloor);
	EXPECT(restoredChest->authoredMapLayer == upperFloor);
	EXPECT(list_Size(restoredChestInventory) == 1);
	Item* restoredChestItem = restoredChestInventory->first
		? static_cast<Item*>(restoredChestInventory->first->element)
		: nullptr;
	EXPECT(restoredChestItem != nullptr);
	EXPECT(static_cast<Sint32>(restoredChestItem->type) == restoredRuntimeID);
	EXPECT(restoredChestItem->count == 3);
	EXPECT(restoredMimic->playableFloor == upperFloor);
	EXPECT(restoredMimic->authoredMapLayer == upperFloor);
	EXPECT(restoredMimic->z == -1.5);
	EXPECT(list_Size(&restoredMimicStats->inventory) == 1);
	EXPECT(restoredMimicStats->weapon != nullptr);
	EXPECT(static_cast<Sint32>(restoredMimicStats->weapon->type)
		== restoredRuntimeID);

	// The same persistent IDs in the sibling MapInstance have no saved state.
	clearGlobalMapHarness();
	EXPECT(resetGlobalMapHarness(4, 4, 1));
	EXPECT(map.ensurePlayableFloorGeometry(upperFloor, false));
	std::snprintf(
		map.filename, sizeof(map.filename), "%s", "cross_feature.lmp");
	EXPECT(worldState.bindMap(map, map.filename, "instance_b"));
	Entity* siblingChest = newEntity(21, 1, map.entities, nullptr);
	EXPECT(siblingChest != nullptr);
	siblingChest->behavior = &actChest;
	siblingChest->persistentID = chestPersistentID;
	siblingChest->authoredMapLayer = upperFloor;
	siblingChest->playableFloor = upperFloor;
	siblingChest->x = 24.0;
	siblingChest->y = 24.0;
	siblingChest->z = 1.25;
	siblingChest->chestVoidState = 0;
	list_t* siblingInventory = attachEmptyChestInventory(*siblingChest);
	EXPECT(siblingInventory != nullptr);
	EXPECT(newItem(GEM_ROCK, WORN, 0, 1, 0, false, siblingInventory)
		!= nullptr);
	applyPersistentMechanismStates();
	EXPECT(list_Size(siblingInventory) == 1);
	EXPECT(static_cast<Item*>(siblingInventory->first->element)->type == GEM_ROCK);
	EXPECT(siblingChest->playableFloor == upperFloor);
	EXPECT(siblingChest->authoredMapLayer == upperFloor);

	SAMItemRegistryFoundation::clear();
	resetPersistentWorldSession();
	clearGlobalMapHarness();
	return true;
}
#endif

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

bool testWallBusterUsesOwningPlayableFloor()
{
    multiplayer = SINGLE;
    EXPECT(resetGlobalMapHarness(4, 4, 1));

    const int x = 1;
    const int y = 1;
    // Floor 1 is an authored view: its floor/wall/ceiling are map layers
    // 1/2/3. The base floor's wall (layer 1) must survive a floor-1 buster.
    EXPECT(map.ensurePlayableFloorGeometry(1, false));
    EXPECT(map.setTileAt(x, y, FLOORLAYER, 9, 1));
    EXPECT(map.setTileAt(x, y, OBSTACLELAYER, 88, 1));
    EXPECT(map.setTileAt(x, y, CEILINGLAYER, 77, 1));
    EXPECT(map.tileAt(x, y, OBSTACLELAYER, DEFAULT_PLAYABLE_FLOOR) == 9);

    Entity* buster = newEntity(66, 1, map.entities, nullptr);
    EXPECT(buster != nullptr);
    buster->x = x * 16.0 + 8.0;
    buster->y = y * 16.0 + 8.0;
    buster->z = 7.5;
    buster->playableFloor = 1;
    buster->authoredMapLayer = 1;
    buster->skill[28] = 2;
    actWallBuster(buster);

    EXPECT(map.tileAt(x, y, OBSTACLELAYER, 1) == 0);
    EXPECT(map.tileAt(x, y, CEILINGLAYER, 1) == 0);
    EXPECT(map.tileAt(x, y, OBSTACLELAYER, DEFAULT_PLAYABLE_FLOOR) == 9);
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
	// Ordinary local model offsets on opposite sides of z=8 remain in their
	// authored light slice. This is the lever base (7.5) / handle (8.5) case.
	worldZExample->z = 8.5;
	EXPECT(worldZExample->structuralLightmapLayer() == 5);
	worldZExample->z = -8.5;
	EXPECT(worldZExample->structuralLightmapLayer() == 5);
    worldZExample->z = 16.0;
    EXPECT(worldZExample->worldRenderZ() == -64.0);
    EXPECT(worldZExample->structuralLightmapLayer() == 4);
	worldZExample->z = -16.0;
	EXPECT(worldZExample->structuralLightmapLayer() == 6);
    worldZExample->z = 7.5;

	// Sprite 119 is the old fixed-height ceiling model. Its source/CPU structural
	// lookup may resolve z=-24 to slice 2, but legacy entity shaders historically
	// sampled slice 0. Merely containing this sprite cannot classify the map as
	// a modern authored stack.
	Entity* legacyLightHeight = newEntity(119, 1, map.entities, nullptr);
	EXPECT(legacyLightHeight != nullptr);
	legacyLightHeight->z = -24.0;
	EXPECT(legacyLightHeight->authoredMapLayer == 0);
	EXPECT(!map.hasAuthoredPlayableFloorStack());
	EXPECT(legacyLightHeight->worldRenderZ() == -24.0);
	EXPECT(legacyLightHeight->structuralLightmapLayer()
		== entityZToLightmapLayer(-24.0));
	EXPECT(legacyLightHeight->visualLightmapLayer() == 0);

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

	// The same ceiling model in a genuine authored stack keeps -24 local and
	// receives the layer-2 structural height exactly once. Its visual shader now
	// uses the structural light lookup instead of the legacy slice-0 rule.
	Entity* modernCeiling = newEntity(119, 1, map.entities, nullptr);
	EXPECT(modernCeiling != nullptr);
	modernCeiling->z = -24.0;
	modernCeiling->playableFloor = 2;
	modernCeiling->authoredMapLayer = 2;
	EXPECT(map.hasAuthoredPlayableFloorStack());
	EXPECT(modernCeiling->z == -24.0);
	EXPECT(modernCeiling->worldRenderZ() == -56.0);
	EXPECT(modernCeiling->visualLightmapLayer()
		== modernCeiling->structuralLightmapLayer());

	// A death camera keeps its own local orbit height while inheriting the
	// player's structural slice. Camera Z is expressed in doubled entity-Z
	// units, matching the normal player and Project Spirit camera paths.
	Entity* deathCamera = newEntityWithSpatialContext(
		-1, 1, map.entities, nullptr, floorMover);
	EXPECT(deathCamera != nullptr);
	deathCamera->z = -2.0;
	EXPECT(deathCamera->playableFloor == 2);
	EXPECT(deathCamera->authoredMapLayer == 2);
	const real_t deathCameraStructuralOffset =
		2.0 * mapLayerWorldZ(deathCamera->structuralMapLayer());
	EXPECT(deathCameraStructuralOffset == -64.0);
	EXPECT(deathCamera->z * 2.0 + deathCameraStructuralOffset == -68.0);

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
	EXPECT(floorMover->visualLightmapLayer() == 0);

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

    // Summons are ordinary runtime spawns: when their caller is being
    // simulated on an authored floor, they inherit that floor rather than
    // silently returning to Z0.
    Entity* upperSummon = nullptr;
    {
        ScopedPlayableFloorRuntimeContext scope(SpatialSpawnContext{2, 56, 2});
        upperSummon = summonMonsterNoSmoke(RAT, 64, 64, true);
    }
    EXPECT(upperSummon != nullptr);
    EXPECT(upperSummon->playableFloor == 2);
    EXPECT(upperSummon->authoredMapLayer == 2);
    EXPECT(upperSummon->spatialRevision == 56);

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

    // The immediate upper-to-lower case is the manual regression: a missing
    // floor-1 surface above a valid floor-0 surface lands one level down.
    const std::size_t x = 1;
    const std::size_t y = 1;
    map.tiles[tileIndex(x, y, 1, map.height)] = 0;
    map.tiles[tileIndex(x, y, 2, map.height)] = 0;
	// The destination tile is clear, but its eastern neighbor is a lower-floor
	// wall. A ledge-edge position must be moved inward far enough for the
	// player's complete collision footprint, not merely its center point.
	map.tiles[tileIndex(x + 1, y, 1, map.height)] = 71;
    EXPECT(map.ensurePlayableFloorGeometry(1, false));
    PlayableFloorId landingFloor = -1;
    int floorsFallen = -1;
    EXPECT(map.findLowerPlayableFloorLanding(
        static_cast<int>(x), static_cast<int>(y), 1,
        landingFloor, floorsFallen));
    EXPECT(landingFloor == 0);
    EXPECT(floorsFallen == 1);

    // Exercise the authoritative same-map transaction, not only the lookup.
    // It must retain local z, move gameplay/structural context to floor 0, and
    // report the one structural step used by actPlayer's damage calculation.
    EXPECT(players[0] == nullptr);
    const int previousClientnum = clientnum;
    const bool previousIntro = intro;
	Stat* const previousPlayerStats = stats[0];
    {
        Player fallingPlayer(0, false);
        Entity* fallingEntity = newEntity(0, 1, map.entities, nullptr);
        EXPECT(fallingEntity != nullptr);
        fallingEntity->x = 31.5;
        fallingEntity->y = 24.0;
        fallingEntity->z = 7.5;
		fallingEntity->sizex = 4;
		fallingEntity->sizey = 4;
        fallingEntity->playableFloor = 1;
        fallingEntity->authoredMapLayer = 1;
        fallingPlayer.entity = fallingEntity;
        players[0] = &fallingPlayer;
        clientnum = 1;
        intro = false;

		// The left casting hand is HUD-owned rather than parent/bodypart-owned.
		// It must still follow the player's structural camera slice through PZTR.
		Entity* leftCastingHand = newEntityWithSpatialContext(
			-1, 1, map.entities, nullptr, fallingEntity);
		EXPECT(leftCastingHand != nullptr);
		leftCastingHand->behavior = &actLeftHandMagic;
		leftCastingHand->flags[NOUPDATE] = true;
		leftCastingHand->skill[2] = 0;
		fallingPlayer.hud.magicLeftHand = leftCastingHand;

		// A world-space limb used to be left at the pre-transition X/Y until a
		// later animation tick, creating a visible duplicate after a fall/stair.
		Entity* worldLimb = newEntityWithSpatialContext(
			0, 1, map.entities, nullptr, fallingEntity);
		EXPECT(worldLimb != nullptr);
		worldLimb->behavior = &actPlayerLimb;
		worldLimb->x = fallingEntity->x + 2.0;
		worldLimb->y = fallingEntity->y;
		worldLimb->new_x = worldLimb->x;
		worldLimb->new_y = worldLimb->y;
		fallingEntity->bodyparts.push_back(worldLimb);

		// Sustained Light/Deep Shade are caster attachments, unlike ordinary
		// projectiles. Their local model Z moves to the destination structural
		// slice and their old light field is removed for next-tick recreation.
		Entity* sustainedLight = newEntityWithSpatialContext(
			174, 1, map.entities, nullptr, fallingEntity);
		EXPECT(sustainedLight != nullptr);
		sustainedLight->behavior = &actMagiclightBall;
		sustainedLight->parent = fallingEntity->getUID();
		sustainedLight->z = -5.5;
		sustainedLight->light = lightSphereOnPlayableFloor(
			0, 1, 1, 1, 1, 1, 1.f, 1.f, 1.f, 0.f, 1.f);
		EXPECT(sustainedLight->light != nullptr);

		Entity* ordinaryProjectile = newEntityWithSpatialContext(
			168, 1, map.entities, nullptr, fallingEntity);
		EXPECT(ordinaryProjectile != nullptr);
		ordinaryProjectile->parent = fallingEntity->getUID();
		ordinaryProjectile->z = 7.5;

		// Followers are gameplay actors, not visual attachments. Z4C deliberately
		// leaves them on their source floor here; their AI must walk a real graph
		// route rather than riding the player's falling/PZTR transaction.
		Stat* followerOwnerStats = new Stat(0);
		EXPECT(followerOwnerStats != nullptr);
		stats[0] = followerOwnerStats;
		Entity* floorFollower = newEntity(0, 1, map.entities, nullptr);
		EXPECT(floorFollower != nullptr);
		floorFollower->behavior = &actMonster;
		floorFollower->monsterAllyIndex = 0;
		floorFollower->x = 24.0;
		floorFollower->y = 24.0;
		floorFollower->z = 7.5;
		floorFollower->sizex = 4;
		floorFollower->sizey = 4;
		floorFollower->playableFloor = 1;
		floorFollower->authoredMapLayer = 1;
		Entity* followerNameTag = newEntityWithSpatialContext(
			-1, 1, map.entities, nullptr, floorFollower);
		EXPECT(followerNameTag != nullptr);
		followerNameTag->parent = floorFollower->getUID();
		followerNameTag->flags[NOUPDATE] = true;
		followerNameTag->behavior = &actSpriteNametag;
		Uint32* followerUid = static_cast<Uint32*>(std::malloc(sizeof(Uint32)));
		EXPECT(followerUid != nullptr);
		*followerUid = floorFollower->getUID();
		node_t* followerNode = list_AddNodeLast(&followerOwnerStats->FOLLOWERS);
		EXPECT(followerNode != nullptr);
		followerNode->element = followerUid;
		followerNode->deconstructor = &defaultDeconstructor;
		followerNode->size = sizeof(Uint32);

        int appliedFloorsFallen = 0;
        EXPECT(fallAutomatiaPlayerToLowerPlayableFloor(
            0, appliedFloorsFallen));
        EXPECT(appliedFloorsFallen == 1);
        EXPECT(fallingEntity->playableFloor == 0);
        EXPECT(fallingEntity->authoredMapLayer == 0);
        EXPECT(fallingEntity->z == 7.5);
		EXPECT(fallingEntity->x <= 27.96);
		EXPECT(fallingEntity->x + fallingEntity->sizex < 32.0);
		EXPECT(leftCastingHand->playableFloor == 0);
		EXPECT(leftCastingHand->authoredMapLayer == 0);
		EXPECT(worldLimb->playableFloor == 0);
		EXPECT(worldLimb->authoredMapLayer == 0);
		EXPECT(worldLimb->x == fallingEntity->x + 2.0);
		EXPECT(worldLimb->y == fallingEntity->y);
		EXPECT(sustainedLight->playableFloor == 0);
		EXPECT(sustainedLight->authoredMapLayer == 0);
		EXPECT(sustainedLight->z == -5.5);
		EXPECT(sustainedLight->light == nullptr);
		EXPECT(ordinaryProjectile->playableFloor == 1);
		EXPECT(ordinaryProjectile->authoredMapLayer == 1);
		EXPECT(ordinaryProjectile->z == 7.5);
		EXPECT(floorFollower->playableFloor == 1);
		EXPECT(floorFollower->authoredMapLayer == 1);
		EXPECT(followerNameTag->playableFloor == 1);
		EXPECT(followerNameTag->authoredMapLayer == 1);

		list_FreeAll(&followerOwnerStats->FOLLOWERS);
		delete followerOwnerStats;
		stats[0] = previousPlayerStats;

		// When the adjacent lower tile is clear, the same ledge coordinate is a
		// valid footprint and must remain unchanged.
		EXPECT(map.setTileAt(
			static_cast<int>(x + 1), static_cast<int>(y),
			OBSTACLELAYER, 0, DEFAULT_PLAYABLE_FLOOR));
		EXPECT(fallingEntity->setPlayableFloor(1));
		fallingEntity->authoredMapLayer = 1;
		fallingEntity->x = 31.5;
		fallingEntity->y = 24.0;
		fallingEntity->z = 7.5;
		appliedFloorsFallen = 0;
		EXPECT(fallAutomatiaPlayerToLowerPlayableFloor(
			0, appliedFloorsFallen));
		EXPECT(appliedFloorsFallen == 1);
		EXPECT(fallingEntity->x == 31.5);
		EXPECT(fallingEntity->y == 24.0);

		fallingPlayer.hud.magicLeftHand = nullptr;
        fallingPlayer.entity = nullptr;
    }
    // Player teardown expects its global slot to remain valid while it closes
    // UI state; detach the stack fixture only after the destructor has run.
    players[0] = nullptr;
	stats[0] = previousPlayerStats;
    clientnum = previousClientnum;
    intro = previousIntro;

    // Falling searches the original authored stack downward. With layers 1 and
    // 2 empty, falling from floor 2 reaches floor 0 and crosses two blocks.
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

bool testZ4AVerticalNavigationAndItemFalling()
{
	multiplayer = SINGLE;
	EXPECT(resetGlobalMapHarness(5, 5, 1));

	// Only a small upper platform exists on authored layer 1. The clear tile
	// west of it is a valid floor-0 stair landing; equal X/Y alone creates no
	// edge anywhere in the graph.
	map.tiles[tileIndex(2, 1, 1, map.height)] = 71;
	map.tiles[tileIndex(2, 1, 2, map.height)] = 0;
	EXPECT(map.ensurePlayableFloorGeometry(1, false));

	Entity* up = newEntity(0, 1, map.entities, nullptr);
	Entity* down = newEntity(0, 1, map.entities, nullptr);
	Entity* endpointSource = newEntity(0, 1, map.entities, nullptr);
	Entity* endpointTarget = newEntity(0, 1, map.entities, nullptr);
	Entity* blocked = newEntity(0, 1, map.entities, nullptr);
	Entity* malformed = newEntity(0, 1, map.entities, nullptr);
	EXPECT(up && down && endpointSource && endpointTarget && blocked && malformed);

	up->persistentID = 8101;
	up->x = 24.0;
	up->y = 24.0;
	up->playableFloor = 0;
	up->authoredMapLayer = 1;
	up->verticalLayerTransitionDelta = 1;
	up->verticalLayerTransitionRotation = 0; // east onto the upper platform

	down->persistentID = 8102;
	down->x = 40.0;
	down->y = 24.0;
	down->playableFloor = 1;
	down->authoredMapLayer = 2;
	down->verticalLayerTransitionDelta = -1;
	down->verticalLayerTransitionRotation = 4; // west to the floor-0 landing

	endpointSource->persistentID = 8103;
	endpointSource->x = 8.0;
	endpointSource->y = 8.0;
	endpointSource->playableFloor = 0;
	endpointSource->playableFloorTransitionEnabled = true;
	endpointSource->playableFloorTransitionDestination = 1;
	endpointSource->playableFloorTransitionTargetPersistentID = 8104;
	endpointTarget->persistentID = 8104;
	endpointTarget->x = 40.0;
	endpointTarget->y = 24.0;
	endpointTarget->playableFloor = 1;
	endpointTarget->authoredMapLayer = 1;

	blocked->persistentID = 8105;
	blocked->x = 72.0;
	blocked->y = 72.0;
	blocked->playableFloor = 0;
	blocked->authoredMapLayer = 1;
	blocked->verticalLayerTransitionDelta = 1;
	blocked->verticalLayerTransitionRotation = 0;

	malformed->persistentID = 8106;
	malformed->x = 24.0;
	malformed->y = 56.0;
	malformed->playableFloor = 0;
	malformed->authoredMapLayer = 1;
	malformed->verticalLayerTransitionDelta = 2;

	VerticalNavigationGraph graphA;
	EXPECT(rebuildVerticalNavigationGraphFromMap(
		graphA, "tower.lmp#instance_a", map));
	EXPECT(graphA.edgeCount() == 3);
	EXPECT(graphA.rejectedCount() == 2);
	EXPECT(graphA.canReachFloor(
		"tower.lmp#instance_a", 0,
		"tower.lmp#instance_a", 1));
	EXPECT(graphA.canReachFloor(
		"tower.lmp#instance_a", 1,
		"tower.lmp#instance_a", 0));
	EXPECT(!graphA.canReachFloor(
		"tower.lmp#instance_a", 0,
		"tower.lmp#instance_b", 1));
	EXPECT(graphA.edgesFrom(
		"tower.lmp#instance_a", {0, 4, 4}).empty());

	VerticalNavigationGraph graphB;
	EXPECT(rebuildVerticalNavigationGraphFromMap(
		graphB, "tower.lmp#instance_b", map));
	EXPECT(graphB.edgeCount() == graphA.edgeCount());
	EXPECT(!graphB.canReachFloor(
		"tower.lmp#instance_a", 0,
		"tower.lmp#instance_b", 1));

	// A dropped item crossing the second-floor boundary keeps the same world
	// height by subtracting one 16-unit step from local Z, retains identity and
	// lands in floor 0 on its next ordinary item-physics tick.
	Entity* item = newEntity(8, 1, map.entities, nullptr);
	EXPECT(item != nullptr);
	item->behavior = &actItem;
	item->persistentID = 8201;
	item->authoredItemStableID = "fixture:falling_item";
	item->x = 24.0;
	item->y = 56.0;
	item->z = 23.5;
	item->new_z = item->z;
	item->playableFloor = 1;
	item->authoredMapLayer = 1;
	const std::uint64_t itemRevision = item->spatialRevision;
	EXPECT(transitionAutomatiaFallingItemToLowerPlayableFloor(*item, 7.5));
	EXPECT(item->playableFloor == 0);
	EXPECT(item->authoredMapLayer == 0);
	EXPECT(item->z == 7.5);
	EXPECT(item->new_z == 7.5);
	EXPECT(item->spatialRevision > itemRevision);
	EXPECT(item->persistentID == 8201);
	EXPECT(item->authoredItemStableID == "fixture:falling_item");
	EXPECT(item->mynode != nullptr);

	// Empty intermediate floors are crossed one boundary at a time rather than
	// converting authored layers into a large local-Z jump.
	Entity* multiFloorItem = newEntity(8, 1, map.entities, nullptr);
	EXPECT(multiFloorItem != nullptr);
	multiFloorItem->behavior = &actItem;
	multiFloorItem->x = 24.0;
	multiFloorItem->y = 56.0;
	multiFloorItem->z = 23.5;
	multiFloorItem->playableFloor = 2;
	multiFloorItem->authoredMapLayer = 2;
	EXPECT(map.ensurePlayableFloorGeometry(2, false));
	EXPECT(transitionAutomatiaFallingItemToLowerPlayableFloor(
		*multiFloorItem, 7.5));
	EXPECT(multiFloorItem->playableFloor == 1);
	EXPECT(multiFloorItem->authoredMapLayer == 1);
	EXPECT(multiFloorItem->z == 7.5);
	EXPECT(!transitionAutomatiaFallingItemToLowerPlayableFloor(
		*multiFloorItem, 7.5));
	multiFloorItem->z = 23.5;
	EXPECT(transitionAutomatiaFallingItemToLowerPlayableFloor(
		*multiFloorItem, 7.5));
	EXPECT(multiFloorItem->playableFloor == 0);
	EXPECT(multiFloorItem->authoredMapLayer == 0);
	EXPECT(multiFloorItem->z == 7.5);

	const real_t legacyZ = item->z;
	EXPECT(!transitionAutomatiaFallingItemToLowerPlayableFloor(*item, 7.5));
	EXPECT(item->z == legacyZ);
	const int previousMultiplayer = multiplayer;
	multiplayer = CLIENT;
	EXPECT(multiFloorItem->setPlayableFloor(1));
	multiFloorItem->authoredMapLayer = 1;
	multiFloorItem->z = 23.5;
	EXPECT(!transitionAutomatiaFallingItemToLowerPlayableFloor(
		*multiFloorItem, 7.5));
	EXPECT(multiFloorItem->playableFloor == 1);
	multiplayer = previousMultiplayer;
	return true;
}

bool testZ2CRuntimeFloorIsolation()
{
    multiplayer = SINGLE;
    EXPECT(resetGlobalMapHarness(4, 4, 1));

	// Legacy one-floor maps can still author lights on a nonzero light slice.
	// They use that same layer as the horizontal wall mask; layer + 1 may be a
	// ceiling. Applying the stacked-floor N + OBSTACLELAYER rule here produced
	// the large black wall band seen in main-menu maps.
	EXPECT(!map.hasAuthoredPlayableFloorStack());
	EXPECT(map.setTileAt(2, 1, 2, 71, DEFAULT_PLAYABLE_FLOOR));
	auto& legacyLightmap = lightmapForPlayableFloor(
		0, DEFAULT_PLAYABLE_FLOOR, map.width, map.height);
	std::fill(legacyLightmap.begin(), legacyLightmap.end(), vec4_t{});
	light_t* legacyLayerOneLight = lightSphereShadowOnPlayableFloor(
		0, 1, 1, DEFAULT_PLAYABLE_FLOOR, 1,
		5, 1.f, 1.f, 1.f, 0.f, 1.f);
	EXPECT(legacyLayerOneLight != nullptr);
	const std::size_t legacyLitIndex = lightmapIndex3D(
		3, 1, 1, map.width, map.height);
	EXPECT(legacyLightmap[legacyLitIndex].x > 0.f);
	list_RemoveNode(legacyLayerOneLight->node);
	EXPECT(legacyLightmap[legacyLitIndex].x == 0.f);
	EXPECT(map.setTileAt(2, 1, 2, 0, DEFAULT_PLAYABLE_FLOOR));

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
    EXPECT(map.hasAuthoredPlayableFloorStack());
    EXPECT(map.playableFloorsShareRenderedWorld(0, 1));
    EXPECT(map.playableFloorsShareRenderedWorld(1, 0));
    EXPECT(playableFloorsShareRuntimeScope(1, 1));
    EXPECT(!playableFloorsShareRuntimeScope(0, 1));

    // The older explicit-copy compatibility path remains available, but the
    // layer-authored stair path returns to the derived shared-world view.
    EXPECT(map.ensurePlayableFloorGeometry(1, true));
    const PlayableFloorData* explicitFloor = map.playableFloors.find(1);
    EXPECT(explicitFloor != nullptr);
    EXPECT(!explicitFloor->derivedFromMapLayers);
    EXPECT(!map.hasAuthoredPlayableFloorStack());
    EXPECT(!map.playableFloorsShareRenderedWorld(0, 1));
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

    // Z3.3C: layer-authored floors are one visible/lightable structure. Their
    // collision scopes remain distinct, but their render light volume is shared.
    EXPECT(map.setTileAt(2, 1, OBSTACLELAYER, 77, DEFAULT_PLAYABLE_FLOOR));
    EXPECT(map.setTileAt(2, 1, OBSTACLELAYER, 0, 1));
    auto& lowerLightmap = lightmapForPlayableFloor(0, 0, map.width, map.height);
    auto& upperLightmap = lightmapForPlayableFloor(0, 1, map.width, map.height);
    EXPECT(&lowerLightmap == &upperLightmap);
    std::fill(lowerLightmap.begin(), lowerLightmap.end(), vec4_t{});

    // Entity light sampling uses authored structural height, not gameplay
    // membership. Keep playableFloor fixed while changing only authored layer.
    Entity* sampledEntity = newEntity(0, 1, map.entities, nullptr);
    EXPECT(sampledEntity != nullptr);
    sampledEntity->x = 24.0;
    sampledEntity->y = 24.0;
    sampledEntity->z = 7.5;
    sampledEntity->playableFloor = DEFAULT_PLAYABLE_FLOOR;
    const std::size_t layerZeroSample = lightmapIndex3D(
        1, 1, 0, map.width, map.height);
    const std::size_t layerOneSample = lightmapIndex3D(
        1, 1, 1, map.width, map.height);
    lowerLightmap[layerZeroSample] = vec4_t{30.f, 30.f, 30.f, 0.f};
    lowerLightmap[layerOneSample] = vec4_t{90.f, 90.f, 90.f, 0.f};
    sampledEntity->authoredMapLayer = 0;
    EXPECT(sampledEntity->structuralLightmapLayer() == 0);
    EXPECT(sampledEntity->entityLight() == 30);
    sampledEntity->authoredMapLayer = 1;
    EXPECT(sampledEntity->structuralLightmapLayer() == 1);
    EXPECT(sampledEntity->entityLight() == 90);

	// A multipart lever shares one authored light slice even though the normal
	// local origins of its base and handle straddle z=8.
	Entity* leverBase = newEntity(184, 1, map.entities, nullptr);
	EXPECT(leverBase != nullptr);
	leverBase->x = 24.0;
	leverBase->y = 24.0;
	leverBase->z = 7.5;
	leverBase->playableFloor = 1;
	leverBase->authoredMapLayer = 1;
	Entity* leverHandle = newEntityWithSpatialContext(
		185, 1, map.entities, nullptr, leverBase);
	EXPECT(leverHandle != nullptr);
	leverHandle->x = leverBase->x;
	leverHandle->y = leverBase->y;
	leverHandle->z = 8.5;
	EXPECT(leverHandle->playableFloor == leverBase->playableFloor);
	EXPECT(leverHandle->authoredMapLayer == leverBase->authoredMapLayer);
	EXPECT(leverBase->structuralLightmapLayer() == 1);
	EXPECT(leverHandle->structuralLightmapLayer() == 1);
	EXPECT(leverBase->entityLight() == 90);
	EXPECT(leverHandle->entityLight() == 90);

    // A floor-1 light writes horizontally across the walkable space above
    // authored floor layer 1. Those floor tiles must not be mistaken for the
    // floor's wall layer (authored layer 2), which caused black upper sprites.
	EXPECT(map.setTileAt(3, 1, 1, 71, DEFAULT_PLAYABLE_FLOOR));
    light_t* upperLight = lightSphereShadowOnPlayableFloor(
        0, 1, 1, 1, 1, 5, 1.f, 1.f, 1.f, 0.f, 1.f);
    EXPECT(upperLight != nullptr);
    EXPECT(upperLight->playableFloor == 1);
    const std::size_t litIndex =
        lightmapIndex3D(3, 1, 1, map.width, map.height);
    EXPECT(upperLightmap[litIndex].x > 0.f);
    EXPECT(lowerLightmap[litIndex].x > 0.f);
	list_RemoveNode(upperLight->node);
	EXPECT(lowerLightmap[litIndex].x == 0.f);

	// Structural light layers are absolute authored indices. The wall lookup
	// must read authored layer 2 directly here; passing layer 2 through the
	// floor-1 relative tile accessor would incorrectly inspect authored layer 3.
	EXPECT(map.setTileAt(2, 1, 2, 77, DEFAULT_PLAYABLE_FLOOR));
	light_t* upperBlockedLight = lightSphereShadowOnPlayableFloor(
		0, 1, 1, 1, 1, 5, 1.f, 1.f, 1.f, 0.f, 1.f);
	EXPECT(upperBlockedLight != nullptr);
	EXPECT(lowerLightmap[litIndex].x == 0.f);
	list_RemoveNode(upperBlockedLight->node);
	EXPECT(map.setTileAt(2, 1, 2, 0, DEFAULT_PLAYABLE_FLOOR));

    // An authored-stack light also spills into another structural slice when
    // the connecting floor plane has an opening. Distance includes the vertical
    // structural step, and deleting the light removes both slice contributions.
	const std::size_t sharedLightIndex =
		lightmapIndex3D(3, 2, 1, map.width, map.height);
	EXPECT(lowerLightmap[sharedLightIndex].x == 0.f);
    light_t* lowerLight = lightSphereShadowOnPlayableFloor(
        0, 1, 2, 0, 0, 5, 1.f, 1.f, 1.f, 0.f, 0.5f);
    EXPECT(lowerLight != nullptr);
	EXPECT(lowerLight->contributionLayerCount > 1);
	EXPECT(lowerLightmap[sharedLightIndex].x > 0.f);
    EXPECT(upperLightmap[sharedLightIndex].x == lowerLightmap[sharedLightIndex].x);
    list_RemoveNode(lowerLight->node);
	EXPECT(lowerLightmap[sharedLightIndex].x == 0.f);

	// A solid authored floor plane at the ray crossing blocks vertical spill;
	// this is not a fullbright or lower-lightmap-copy workaround.
	EXPECT(map.setTileAt(2, 3, 1, 71, DEFAULT_PLAYABLE_FLOOR));
	const std::size_t blockedUpperIndex =
		lightmapIndex3D(3, 3, 1, map.width, map.height);
	light_t* blockedLowerLight = lightSphereOnPlayableFloor(
		0, 1, 3, 0, 0, 5, 1.f, 1.f, 1.f, 0.f, 0.5f);
	EXPECT(blockedLowerLight != nullptr);
	EXPECT(lowerLightmap[blockedUpperIndex].x == 0.f);
	list_RemoveNode(blockedLowerLight->node);
    return true;
}

bool testUpperFloorMonsterPathing()
{
    multiplayer = SINGLE;
    EXPECT(resetGlobalMapHarness(4, 4, 1));
    const bool previousLoading = loading;
    loading = false;

    // The authored layer below the upper walkable slice is a solid Z0 wall,
    // while floor 1 itself has an open floor and wall layer. A legacy Z0 path
    // map reports no route here; floor-aware monster pathing must succeed.
    EXPECT(map.ensurePlayableFloorGeometry(1, false));
    for (int x = 0; x < map.width; ++x)
    {
        for (int y = 0; y < map.height; ++y)
        {
            EXPECT(map.setTileAt(x, y, FLOORLAYER, 1, 1));
            EXPECT(map.setTileAt(x, y, OBSTACLELAYER, 0, 1));
        }
    }

    Entity* monster = newEntity(-1, 1, map.entities, nullptr);
    EXPECT(monster != nullptr);
    monster->behavior = &actMonster;
    monster->playableFloor = 1;
    monster->authoredMapLayer = 1;
    monster->x = 24.0;
    monster->y = 24.0;
    list_t* path = generatePath(1, 1, 2, 1, monster, nullptr,
        GENERATE_PATH_TO_HUNT_MONSTER_TARGET);
    EXPECT(path != nullptr);
    EXPECT(list_Size(path) > 0);
    list_FreeAll(path);
    std::free(path);

    loading = previousLoading;
    return true;
}

bool testZ4BCrossFloorPathQuery()
{
	multiplayer = SINGLE;
	resetPersistentWorldSession();
	EXPECT(resetGlobalMapHarness(7, 7, 1));
	const bool previousLoading = loading;
	loading = false;

	// Authored-layer geometry mirrors real stacked construction. Floor 1 has a
	// small corridor from the first landing to the second stair; floor 2 has a
	// separate landing/target platform. Re-derivation during graph rebuild must
	// therefore preserve these paths without a synthetic FLOR-only buffer.
	for (int x = 3; x <= 4; ++x)
	{
		for (int y = 1; y <= 2; ++y)
		{
			map.tiles[tileIndex(x, y, 1, map.height)] = 1;
		}
	}
	for (int x = 5; x <= 6; ++x)
	{
		for (int y = 2; y <= 4; ++y)
		{
			map.tiles[tileIndex(x, y, 2, map.height)] = 1;
		}
	}
	EXPECT(map.ensurePlayableFloorGeometry(1, false));
	EXPECT(map.ensurePlayableFloorGeometry(2, false));
	EXPECT(map.ensurePlayableFloorGeometry(3, false));

	Entity* firstStair = newEntity(0, 1, map.entities, nullptr);
	Entity* secondStair = newEntity(0, 1, map.entities, nullptr);
	EXPECT(firstStair && secondStair);
	firstStair->persistentID = 8301;
	firstStair->x = 40.0;
	firstStair->y = 24.0;
	firstStair->playableFloor = 0;
	firstStair->authoredMapLayer = 1;
	firstStair->verticalLayerTransitionDelta = 1;
	firstStair->verticalLayerTransitionRotation = 0;
	firstStair->flags[PASSABLE] = true;

	secondStair->persistentID = 8302;
	secondStair->x = 72.0;
	secondStair->y = 40.0;
	secondStair->playableFloor = 1;
	secondStair->authoredMapLayer = 2;
	secondStair->verticalLayerTransitionDelta = 1;
	secondStair->verticalLayerTransitionRotation = 0;
	secondStair->flags[PASSABLE] = true;

	std::snprintf(map.filename, sizeof(map.filename), "%s", "z4b_path.lmp");
	EXPECT(worldState.bindMap(map, map.filename, "instance_a"));
	MapInstanceSummary siblingSummary;
	EXPECT(siblingSummary.identity.set("z4b_path.lmp", "instance_b"));
	EXPECT(worldState.registerUnloadedInstance(siblingSummary));
	EXPECT(worldState.rebuildVerticalNavigation(map));
	generatePathMaps();
	worldState.refreshActiveContext();
	const MapInstance* instance = worldState.activeInstance();
	const MapInstance* sibling = worldState.find("z4b_path.lmp#instance_b");
	EXPECT(instance && sibling);
	EXPECT(instance->verticalNavigation.edgeCount() == 2);

	Entity* mover = newEntity(-1, 1, map.entities, nullptr);
	EXPECT(mover != nullptr);
	mover->x = 104.0;
	mover->y = 72.0;
	mover->playableFloor = 0;
	mover->authoredMapLayer = 0;

	// Same-floor planning is the ordinary A* result, with no graph traversal.
	list_t* ordinary = generatePath(
		6, 4, 6, 5, mover, nullptr, GENERATE_PATH_DEFAULT);
	EXPECT(ordinary != nullptr);
	CrossFloorPathRoute localRoute;
	EXPECT(generateCrossFloorPath(
		*instance, {0, 6, 4}, *instance, {0, 6, 5},
		mover, nullptr, GENERATE_PATH_DEFAULT, localRoute));
	EXPECT(localRoute.transitionCount == 0);
	EXPECT(localRoute.steps.size() == 1);
	EXPECT(localRoute.steps[0].localPath.size() == list_Size(ordinary));
	std::size_t ordinaryIndex = 0;
	for (node_t* node = ordinary->first; node; node = node->next)
	{
		const pathnode_t* pathNode =
			static_cast<const pathnode_t*>(node->element);
		EXPECT(pathNode != nullptr);
		EXPECT(localRoute.steps[0].localPath[ordinaryIndex].tileX
			== pathNode->x);
		EXPECT(localRoute.steps[0].localPath[ordinaryIndex].tileY
			== pathNode->y);
		++ordinaryIndex;
	}
	list_FreeAll(ordinary);
	std::free(ordinary);

	CrossFloorPathRoute upperRoute;
	EXPECT(generateCrossFloorPath(
		*instance, {0, 6, 4}, *instance, {1, 4, 1},
		mover, nullptr, GENERATE_PATH_DEFAULT, upperRoute));
	EXPECT(upperRoute.transitionCount == 1);
	EXPECT(upperRoute.steps.size() == 3);

	CrossFloorPathRoute multiFloorRoute;
	EXPECT(generateCrossFloorPath(
		*instance, {0, 6, 4}, *instance, {2, 6, 3},
		mover, nullptr, GENERATE_PATH_DEFAULT, multiFloorRoute));
	EXPECT(multiFloorRoute.transitionCount == 2);
	EXPECT(multiFloorRoute.steps.size() == 5);
	EXPECT(multiFloorRoute.steps[1].transition.sourcePersistentID == 8301);
	EXPECT(multiFloorRoute.steps[3].transition.sourcePersistentID == 8302);

	// Equal X/Y on another floor still needs both authored transitions.
	CrossFloorPathRoute equalCoordinatesRoute;
	EXPECT(generateCrossFloorPath(
		*instance, {0, 6, 4}, *instance, {2, 6, 4},
		mover, nullptr, GENERATE_PATH_DEFAULT, equalCoordinatesRoute));
	EXPECT(equalCoordinatesRoute.transitionCount == 2);
	EXPECT(!equalCoordinatesRoute.steps.empty());

	CrossFloorPathRoute rejectedRoute;
	EXPECT(!generateCrossFloorPath(
		*instance, {0, 6, 4}, *instance, {3, 6, 4},
		mover, nullptr, GENERATE_PATH_DEFAULT, rejectedRoute));
	EXPECT(!generateCrossFloorPath(
		*instance, {0, 6, 4}, *sibling, {2, 6, 4},
		mover, nullptr, GENERATE_PATH_DEFAULT, rejectedRoute));

	loading = previousLoading;
	resetPersistentWorldSession();
	clearGlobalMapHarness();
	return true;
}

bool testZ4CFollowerVerticalNavigation()
{
	multiplayer = SINGLE;
	resetPersistentWorldSession();
	EXPECT(resetGlobalMapHarness(7, 7, 1));
	const bool previousLoading = loading;
	const bool previousHeadless = headless;
	const bool previousDisconnected = client_disconnected[0];
	Player* const previousPlayer = players[0];
	Stat* const previousPlayerStats = stats[0];
	loading = false;
	client_disconnected[0] = false;

	// The same two authored stair segments used by the Z4B characterization.
	// Floor 3 is a valid domain but intentionally has no transition edge.
	for (int x = 3; x <= 4; ++x)
	{
		for (int y = 1; y <= 2; ++y)
		{
			map.tiles[tileIndex(x, y, 1, map.height)] = 1;
		}
	}
	for (int x = 5; x <= 6; ++x)
	{
		for (int y = 2; y <= 4; ++y)
		{
			map.tiles[tileIndex(x, y, 2, map.height)] = 1;
		}
	}
	EXPECT(map.ensurePlayableFloorGeometry(1, false));
	EXPECT(map.ensurePlayableFloorGeometry(2, false));
	EXPECT(map.ensurePlayableFloorGeometry(3, false));

	Entity* firstStair = newEntity(0, 1, map.entities, nullptr);
	Entity* secondStair = newEntity(0, 1, map.entities, nullptr);
	EXPECT(firstStair && secondStair);
	firstStair->persistentID = 8401;
	firstStair->x = 40.0;
	firstStair->y = 24.0;
	firstStair->playableFloor = 0;
	firstStair->authoredMapLayer = 1;
	firstStair->verticalLayerTransitionDelta = 1;
	firstStair->verticalLayerTransitionRotation = 0;
	firstStair->flags[PASSABLE] = true;
	secondStair->persistentID = 8402;
	secondStair->x = 72.0;
	secondStair->y = 40.0;
	secondStair->playableFloor = 1;
	secondStair->authoredMapLayer = 2;
	secondStair->verticalLayerTransitionDelta = 1;
	secondStair->verticalLayerTransitionRotation = 0;
	secondStair->flags[PASSABLE] = true;

	std::snprintf(map.filename, sizeof(map.filename), "%s", "z4c_follow.lmp");
	EXPECT(worldState.bindMap(map, map.filename, "instance_a"));
	MapInstanceSummary siblingSummary;
	EXPECT(siblingSummary.identity.set("z4c_follow.lmp", "instance_b"));
	siblingSummary.playableFloors = {0, 1, 2, 3};
	EXPECT(worldState.registerUnloadedInstance(siblingSummary));
	EXPECT(worldState.rebuildVerticalNavigation(map));
	generatePathMaps();
	worldState.refreshActiveContext();
	MapInstance* instance = worldState.activeInstance();
	MapInstance* sibling = worldState.find("z4c_follow.lmp#instance_b");
	EXPECT(instance && sibling);
	EXPECT(instance->verticalNavigation.edgeCount() == 2);

	Stat* ownerStats = nullptr;
	{
		Player ownerPlayer(0, false);
		ownerStats = new Stat(0);
		EXPECT(ownerStats != nullptr);
		stats[0] = ownerStats;
		players[0] = &ownerPlayer;
		Entity* owner = newEntity(0, 1, map.entities, nullptr);
		EXPECT(owner != nullptr);
		owner->behavior = &actPlayer;
		owner->skill[2] = 0;
		owner->x = 104.0;
		owner->y = 56.0;
		owner->z = 7.5;
		owner->playableFloor = 2;
		owner->authoredMapLayer = 2;
		ownerPlayer.entity = owner;
		EXPECT(worldState.placePlayer(0, map));
		EXPECT(TileEntityList.addEntity(*owner) != nullptr);

		Entity* follower = newEntity(10, 1, map.entities, nullptr);
		EXPECT(follower != nullptr);
		setSpriteAttributes(follower, nullptr, nullptr);
		follower->behavior = &actMonster;
		follower->skill[3] = 2;
		follower->persistentID = 8501;
		follower->monsterAllyIndex = 0;
		follower->x = 104.0;
		follower->y = 72.0;
		follower->z = -1.5;
		follower->new_x = follower->x;
		follower->new_y = follower->y;
		follower->playableFloor = 0;
		follower->authoredMapLayer = 0;
		follower->sizex = 4;
		follower->sizey = 4;
		Stat* followerStats = follower->getStats();
		EXPECT(followerStats != nullptr);
		followerStats->type = MINIMIMIC;
		followerStats->leader_uid = owner->getUID();
		EXPECT(TileEntityList.addEntity(*follower) != nullptr);

		Entity* bodypart = newEntityWithSpatialContext(
			0, 1, map.entities, nullptr, follower);
		Entity* nameTag = newEntityWithSpatialContext(
			-1, 1, map.entities, nullptr, follower);
		EXPECT(bodypart && nameTag);
		bodypart->parent = follower->getUID();
		bodypart->x = follower->x + 2.0;
		bodypart->y = follower->y;
		bodypart->new_x = bodypart->x;
		bodypart->new_y = bodypart->y;
		follower->bodyparts.push_back(bodypart);
		nameTag->parent = follower->getUID();
		nameTag->flags[NOUPDATE] = true;
		nameTag->behavior = &actSpriteNametag;
		nameTag->x = follower->x;
		nameTag->y = follower->y;
		nameTag->new_x = nameTag->x;
		nameTag->new_y = nameTag->y;
		EXPECT(TileEntityList.addEntity(*bodypart) != nullptr);
		EXPECT(TileEntityList.addEntity(*nameTag) != nullptr);

		Uint32* rosterUID = static_cast<Uint32*>(std::malloc(sizeof(Uint32)));
		EXPECT(rosterUID != nullptr);
		*rosterUID = follower->getUID();
		node_t* rosterNode = list_AddNodeLast(&ownerStats->FOLLOWERS);
		EXPECT(rosterNode != nullptr);
		rosterNode->element = rosterUID;
		rosterNode->deconstructor = &defaultDeconstructor;
		rosterNode->size = sizeof(Uint32);
		const Uint32 followerUID = follower->getUID();
		const Sint32 followerPersistentID = follower->persistentID;
		const Uint32 leaderUID = followerStats->leader_uid;

		// A floor-local obstruction must make the transition route unavailable;
		// Z4C does not bypass ordinary collision/path rules to reach a stair.
		for (int y = 0; y < static_cast<int>(map.height); ++y)
		{
			map.tiles[tileIndex(4, y, 1, map.height)] = 1;
		}
		generatePathMaps();
		const auto blockedStatus =
			updateAutomatiaFollowerVerticalNavigation(*follower, true);
		if (blockedStatus
			!= AutomatiaFollowerVerticalNavigationStatus::RouteUnavailable)
		{
			std::cerr << "Z4C blocked status: "
				<< static_cast<int>(blockedStatus) << '\n';
		}
		EXPECT(blockedStatus
			== AutomatiaFollowerVerticalNavigationStatus::RouteUnavailable);
		EXPECT(follower->playableFloor == 0);
		for (int y = 0; y < static_cast<int>(map.height); ++y)
		{
			if (y < 1 || y > 2)
			{
				map.tiles[tileIndex(4, y, 1, map.height)] = 0;
			}
		}
		generatePathMaps();

		// A cross-floor owner assigns an ordinary floor-local HUNT path; no
		// actor is teleported while the Mini Mimic is away from the stair.
		const AutomatiaFollowerVerticalNavigationStatus initialStatus =
			updateAutomatiaFollowerVerticalNavigation(*follower, true);
		if (initialStatus
			!= AutomatiaFollowerVerticalNavigationStatus::PathAssigned)
		{
			std::cerr << "Z4C initial status: "
				<< static_cast<int>(initialStatus) << '\n';
		}
		EXPECT(initialStatus
			== AutomatiaFollowerVerticalNavigationStatus::PathAssigned);
		EXPECT(follower->playableFloor == 0);
		EXPECT(follower->monsterState == MONSTER_STATE_HUNT);
		EXPECT(follower->children.first != nullptr);
		EXPECT(follower->children.first->element != nullptr);
		EXPECT(static_cast<list_t*>(follower->children.first->element)->first
			!= nullptr);
		EXPECT(updateAutomatiaFollowerVerticalNavigation(*follower, false)
			== AutomatiaFollowerVerticalNavigationStatus::PathInProgress);

		auto moveActorTreeTo = [&](const int tileX, const int tileY)
		{
			const real_t destinationX = tileX * 16.0 + 8.0;
			const real_t destinationY = tileY * 16.0 + 8.0;
			const real_t deltaX = destinationX - follower->x;
			const real_t deltaY = destinationY - follower->y;
			follower->x = destinationX;
			follower->y = destinationY;
			follower->new_x = destinationX;
			follower->new_y = destinationY;
			for (Entity* attachment : {bodypart, nameTag})
			{
				attachment->x += deltaX;
				attachment->y += deltaY;
				attachment->new_x += deltaX;
				attachment->new_y += deltaY;
				TileEntityList.updateEntity(*attachment);
			}
			TileEntityList.updateEntity(*follower);
		};

		const auto edgeForFloor = [&](const PlayableFloorId floor)
			-> const VerticalNavigationEdge*
		{
			for (const VerticalNavigationEdge& edge :
				instance->verticalNavigation.edges())
			{
				if (edge.source.playableFloor == floor)
				{
					return &edge;
				}
			}
			return nullptr;
		};

		const VerticalNavigationEdge* firstEdge = edgeForFloor(0);
		const VerticalNavigationEdge* secondEdge = edgeForFloor(1);
		EXPECT(firstEdge && secondEdge);
		moveActorTreeTo(firstEdge->source.tileX, firstEdge->source.tileY);
		const std::uint64_t firstRevision = follower->spatialRevision;
		EXPECT(updateAutomatiaFollowerVerticalNavigation(*follower, true)
			== AutomatiaFollowerVerticalNavigationStatus::Transitioned);
		EXPECT(follower->playableFloor == 1);
		EXPECT(follower->authoredMapLayer == 1);
		EXPECT(follower->z == -1.5);
		EXPECT(follower->spatialRevision > firstRevision);
		EXPECT(bodypart->playableFloor == 1);
		EXPECT(nameTag->playableFloor == 1);
		EXPECT(bodypart->authoredMapLayer == 1);
		EXPECT(nameTag->authoredMapLayer == 1);
		EXPECT(follower->getUID() == followerUID);
		EXPECT(follower->persistentID == followerPersistentID);
		EXPECT(follower->monsterAllyIndex == 0);
		EXPECT(followerStats->leader_uid == leaderUID);
		EXPECT(list_Size(&ownerStats->FOLLOWERS) == 1);
		EXPECT(*static_cast<Uint32*>(ownerStats->FOLLOWERS.first->element)
			== followerUID);

		// A second query reconstructs the route from the new floor; no route
		// cursor or S.A.M./Mini-Mimic-specific state is persisted.
		moveActorTreeTo(secondEdge->source.tileX, secondEdge->source.tileY);
		headless = true;
		EXPECT(updateAutomatiaFollowerVerticalNavigation(*follower, true)
			== AutomatiaFollowerVerticalNavigationStatus::Transitioned);
		headless = previousHeadless;
		EXPECT(follower->playableFloor == 2);
		EXPECT(follower->authoredMapLayer == 2);
		EXPECT(bodypart->playableFloor == 2);
		EXPECT(nameTag->playableFloor == 2);
		EXPECT(instance->dirty);

		// Once co-located, Z4C exits without touching ordinary one-floor AI.
		const Sint32 sameFloorState = follower->monsterState;
		EXPECT(updateAutomatiaFollowerVerticalNavigation(*follower, true)
			== AutomatiaFollowerVerticalNavigationStatus::SameFloor);
		EXPECT(!follower->followerVerticalNavigationActive);
		EXPECT(follower->monsterState == sameFloorState);

		// Same X/Y on a disconnected floor remains unreachable, and a player in
		// a sibling MapInstance is never treated as a vertical destination.
		EXPECT(follower->setPlayableFloor(0));
		follower->authoredMapLayer = 0;
		syncAutomatiaNonPlayerEntitySpatialAttachments(*follower);
		moveActorTreeTo(6, 3);
		EXPECT(owner->setPlayableFloor(3));
		owner->authoredMapLayer = 3;
		EXPECT(updateAutomatiaFollowerVerticalNavigation(*follower, true)
			== AutomatiaFollowerVerticalNavigationStatus::RouteUnavailable);
		EXPECT(follower->playableFloor == 0);
		ownerPlayer.worldInstance = sibling->identity;
		EXPECT(updateAutomatiaFollowerVerticalNavigation(*follower, true)
			== AutomatiaFollowerVerticalNavigationStatus::OwnerUnavailable);
		EXPECT(follower->playableFloor == 0);
		ownerPlayer.worldInstance = instance->identity;

		// Disconnect/reconnect pauses and resumes the same actor/roster. This
		// characterizes representation only; no duplicate follower is created.
		EXPECT(owner->setPlayableFloor(1));
		owner->authoredMapLayer = 1;
		owner->x = 72.0;
		owner->y = 40.0;
		client_disconnected[0] = true;
		EXPECT(updateAutomatiaFollowerVerticalNavigation(*follower, true)
			== AutomatiaFollowerVerticalNavigationStatus::OwnerUnavailable);
		EXPECT(list_Size(&ownerStats->FOLLOWERS) == 1);
		client_disconnected[0] = false;
		EXPECT(updateAutomatiaFollowerVerticalNavigation(*follower, true)
			== AutomatiaFollowerVerticalNavigationStatus::PathAssigned);
		EXPECT(follower->getUID() == followerUID);
		EXPECT(follower->persistentID == followerPersistentID);
		EXPECT(followerStats->type == MINIMIMIC);
		EXPECT(followerStats->leader_uid == leaderUID);
		EXPECT(list_Size(&ownerStats->FOLLOWERS) == 1);

		list_FreeAll(&ownerStats->FOLLOWERS);
		ownerPlayer.entity = nullptr;
	}
	players[0] = previousPlayer;
	stats[0] = previousPlayerStats;
	delete ownerStats;
	client_disconnected[0] = previousDisconnected;
	headless = previousHeadless;
	loading = previousLoading;
	resetPersistentWorldSession();
	clearGlobalMapHarness();
	return true;
}

bool testZ4DHostileVerticalNavigation()
{
	const int previousMultiplayer = multiplayer;
	const bool previousLoading = loading;
	const bool previousHeadless = headless;
	const bool previousIntro = intro;
	const bool previousEverybodyFriendly = everybodyfriendly;
	Player* const previousPlayer = players[0];
	Stat* const previousPlayerStats = stats[0];
	std::array<bool, MAXPLAYERS> previousDisconnected{};
	for (int player = 0; player < MAXPLAYERS; ++player)
	{
		previousDisconnected[player] = client_disconnected[player];
		client_disconnected[player] = true;
	}
	client_disconnected[0] = false;
	multiplayer = SINGLE;
	loading = false;
	headless = false;
	intro = false;
	everybodyfriendly = false;
	resetPersistentWorldSession();
	EXPECT(resetGlobalMapHarness(7, 7, 1));
	loading = false;

	// Two real authored stair segments connect floors 0 -> 1 -> 2. Floor 3
	// remains a valid but disconnected navigation domain.
	for (int x = 3; x <= 4; ++x)
	{
		for (int y = 1; y <= 2; ++y)
		{
			map.tiles[tileIndex(x, y, 1, map.height)] = 1;
		}
	}
	for (int x = 5; x <= 6; ++x)
	{
		for (int y = 2; y <= 4; ++y)
		{
			map.tiles[tileIndex(x, y, 2, map.height)] = 1;
		}
	}
	EXPECT(map.ensurePlayableFloorGeometry(1, false));
	EXPECT(map.ensurePlayableFloorGeometry(2, false));
	EXPECT(map.ensurePlayableFloorGeometry(3, false));

	Entity* firstStair = newEntity(0, 1, map.entities, nullptr);
	Entity* secondStair = newEntity(0, 1, map.entities, nullptr);
	EXPECT(firstStair && secondStair);
	firstStair->persistentID = 8601;
	firstStair->x = 40.0;
	firstStair->y = 24.0;
	firstStair->playableFloor = 0;
	firstStair->authoredMapLayer = 1;
	firstStair->verticalLayerTransitionDelta = 1;
	firstStair->verticalLayerTransitionRotation = 0;
	firstStair->flags[PASSABLE] = true;
	secondStair->persistentID = 8602;
	secondStair->x = 72.0;
	secondStair->y = 40.0;
	secondStair->playableFloor = 1;
	secondStair->authoredMapLayer = 2;
	secondStair->verticalLayerTransitionDelta = 1;
	secondStair->verticalLayerTransitionRotation = 0;
	secondStair->flags[PASSABLE] = true;

	std::snprintf(map.filename, sizeof(map.filename), "%s", "z4d_hostile.lmp");
	EXPECT(worldState.bindMap(map, map.filename, "instance_a"));
	MapInstanceSummary siblingSummary;
	EXPECT(siblingSummary.identity.set("z4d_hostile.lmp", "instance_b"));
	siblingSummary.playableFloors = {0, 1, 2, 3};
	EXPECT(worldState.registerUnloadedInstance(siblingSummary));
	EXPECT(worldState.rebuildVerticalNavigation(map));
	generatePathMaps();
	worldState.refreshActiveContext();
	MapInstance* instance = worldState.activeInstance();
	MapInstance* sibling = worldState.find("z4d_hostile.lmp#instance_b");
	EXPECT(instance && sibling);
	EXPECT(instance->verticalNavigation.edgeCount() == 2);

	Stat* playerStats = nullptr;
	{
		Player player(0, false);
		playerStats = new Stat(0);
		EXPECT(playerStats != nullptr);
		playerStats->type = HUMAN;
		playerStats->HP = playerStats->MAXHP = 100;
		stats[0] = playerStats;
		players[0] = &player;
		Entity* target = newEntity(0, 1, map.entities, nullptr);
		EXPECT(target != nullptr);
		target->behavior = &actPlayer;
		target->skill[2] = 0;
		target->x = 104.0;
		target->y = 72.0;
		target->z = 2.5;
		target->playableFloor = 0;
		target->authoredMapLayer = 0;
		target->sizex = 4;
		target->sizey = 4;
		player.entity = target;
		EXPECT(worldState.placePlayer(0, map));
		EXPECT(TileEntityList.addEntity(*target) != nullptr);

		auto moveTargetTo = [&](const PlayableFloorId floor,
			const int tileX, const int tileY)
		{
			target->setPlayableFloor(floor);
			target->authoredMapLayer = floor;
			target->x = tileX * 16.0 + 8.0;
			target->y = tileY * 16.0 + 8.0;
			target->new_x = target->x;
			target->new_y = target->y;
			TileEntityList.updateEntity(*target);
		};

		Sint32 nextPersistentID = 8700;
		auto makeMonster = [&](const Monster type,
			const Stat::MonsterForceAllegiance disposition,
			const int tileX, const int tileY) -> Entity*
		{
			// Exercise the real authored Mini Mimic palette marker in every mimic
			// role. setSpriteAttributes() resolves it to the existing MINIMIMIC
			// runtime actor; the fixture then applies the editor-authored
			// disposition just as the monster property loader does.
			const Sint32 sprite = type == MINIMIMIC
				? EDITOR_SPRITE_MINIMIMIC
				: 10;
			Entity* monster = newEntity(sprite, 1, map.entities, nullptr);
			if (!monster)
			{
				return nullptr;
			}
			setSpriteAttributes(monster, nullptr, nullptr);
			monster->behavior = &actMonster;
			monster->skill[3] = 2;
			monster->persistentID = ++nextPersistentID;
			monster->monsterAllyIndex = -1;
			monster->monsterState = MONSTER_STATE_WAIT;
			monster->x = tileX * 16.0 + 8.0;
			monster->y = tileY * 16.0 + 8.0;
			monster->new_x = monster->x;
			monster->new_y = monster->y;
			monster->z = -1.25;
			monster->playableFloor = 0;
			monster->authoredMapLayer = 0;
			monster->sizex = 4;
			monster->sizey = 4;
			Stat* monsterStats = monster->getStats();
			if (!monsterStats)
			{
				return nullptr;
			}
			monsterStats->type = type;
			monsterStats->HP = monsterStats->MAXHP = 40;
			monsterStats->monsterForceAllegiance = disposition;
			if (!TileEntityList.addEntity(*monster))
			{
				return nullptr;
			}
			return monster;
		};

		const auto edgeForFloor = [&](const PlayableFloorId floor)
			-> const VerticalNavigationEdge*
		{
			for (const VerticalNavigationEdge& edge :
				instance->verticalNavigation.edges())
			{
				if (edge.source.playableFloor == floor)
				{
					return &edge;
				}
			}
			return nullptr;
		};
		const VerticalNavigationEdge* firstEdge = edgeForFloor(0);
		const VerticalNavigationEdge* secondEdge = edgeForFloor(1);
		EXPECT(firstEdge && secondEdge);

		// Hostile Mini Mimics retain ordinary same-floor acquisition. Once the
		// already-known player changes floor, raw acquisition cannot manufacture
		// an ATTACK state from equal X/Y coordinates.
		Entity* hostileMimic = makeMonster(
			MINIMIMIC, Stat::MONSTER_FORCE_PLAYER_ENEMY, 6, 4);
		EXPECT(hostileMimic != nullptr);
		Stat* hostileStats = hostileMimic->getStats();
		EXPECT(hostileStats != nullptr);
		EXPECT(hostileStats->type == MINIMIMIC);
		EXPECT(hostileStats->monsterForceAllegiance
			== Stat::MONSTER_FORCE_PLAYER_ENEMY);
		hostileMimic->monsterAcquireAttackTarget(*target, MONSTER_STATE_PATH);
		EXPECT(hostileMimic->monsterTarget == target->getUID());
		EXPECT(updateAutomatiaHostileVerticalNavigation(*hostileMimic, true)
			== AutomatiaHostileVerticalNavigationStatus::SameFloor);
		moveTargetTo(2, 6, 4);
		const Sint32 stateBeforeRejectedAcquire = hostileMimic->monsterState;
		hostileMimic->monsterAcquireAttackTarget(*target, MONSTER_STATE_ATTACK);
		EXPECT(hostileMimic->monsterState == stateBeforeRejectedAcquire);
		EXPECT(std::isinf(entityDist(hostileMimic, target)));
		EXPECT(!entityInsideEntity(hostileMimic, target));

		Entity* rawCoordinateGuard = makeMonster(
			GOBLIN, Stat::MONSTER_FORCE_PLAYER_ENEMY, 6, 4);
		EXPECT(rawCoordinateGuard != nullptr);
		rawCoordinateGuard->monsterAcquireAttackTarget(
			*target, MONSTER_STATE_ATTACK);
		EXPECT(rawCoordinateGuard->monsterTarget == 0);
		EXPECT(rawCoordinateGuard->monsterState == MONSTER_STATE_WAIT);
		rawCoordinateGuard->x = 8.0;
		rawCoordinateGuard->y = 104.0;
		rawCoordinateGuard->new_x = rawCoordinateGuard->x;
		rawCoordinateGuard->new_y = rawCoordinateGuard->y;
		rawCoordinateGuard->setPlayableFloor(3);
		rawCoordinateGuard->authoredMapLayer = 3;
		TileEntityList.updateEntity(*rawCoordinateGuard);
		// Keep the equal-X/Y isolation assertions above, then use the same valid
		// upper landing exercised by the Z4B/C route fixtures.
		moveTargetTo(2, 6, 3);

		const int targetHPBeforeAttack = playerStats->HP;
		const Sint32 hitTimeBeforeAttack = hostileMimic->monsterHitTime;
		hostileMimic->handleMonsterAttack(hostileStats, target, 0.0);
		EXPECT(playerStats->HP == targetHPBeforeAttack);
		EXPECT(hostileMimic->monsterHitTime == hitTimeBeforeAttack);

		// Clients never assign or consume the cross-floor route. The authoritative
		// single-player/server code path then parks the combat target and assigns
		// only the first ordinary HUNT leg. Live SERVER networking is covered by
		// source contracts and remains a manual acceptance item for this harness.
		multiplayer = CLIENT;
		EXPECT(updateAutomatiaHostileVerticalNavigation(*hostileMimic, true)
			== AutomatiaHostileVerticalNavigationStatus::NotHostile);
		EXPECT(hostileMimic->playableFloor == 0);
		EXPECT(hostileMimic->monsterTarget == target->getUID());
		multiplayer = SINGLE;
		const AutomatiaHostileVerticalNavigationStatus initialHostileStatus =
			updateAutomatiaHostileVerticalNavigation(*hostileMimic, true);
		if (initialHostileStatus
			!= AutomatiaHostileVerticalNavigationStatus::PathAssigned)
		{
			std::cerr << "Z4D initial hostile status: "
				<< static_cast<int>(initialHostileStatus) << '\n';
		}
		EXPECT(initialHostileStatus
			== AutomatiaHostileVerticalNavigationStatus::PathAssigned);
		EXPECT(hostileMimic->playableFloor == 0);
		EXPECT(hostileMimic->monsterTarget == 0);
		EXPECT(hostileMimic->hostileVerticalNavigationActive);
		EXPECT(hostileMimic->hostileVerticalNavigationTarget
			== target->getUID());
		EXPECT(hostileMimic->monsterState == MONSTER_STATE_HUNT);
		EXPECT(updateAutomatiaHostileVerticalNavigation(*hostileMimic, false)
			== AutomatiaHostileVerticalNavigationStatus::PathInProgress);

		auto moveMonsterToEdge = [&](Entity& monster,
			const VerticalNavigationEdge& edge)
		{
			monster.x = edge.source.tileX * 16.0 + 8.0;
			monster.y = edge.source.tileY * 16.0 + 8.0;
			monster.new_x = monster.x;
			monster.new_y = monster.y;
			TileEntityList.updateEntity(monster);
		};
		moveMonsterToEdge(*hostileMimic, *firstEdge);
		headless = true;
		const std::uint64_t firstRevision = hostileMimic->spatialRevision;
		EXPECT(updateAutomatiaHostileVerticalNavigation(*hostileMimic, true)
			== AutomatiaHostileVerticalNavigationStatus::Transitioned);
		EXPECT(hostileMimic->playableFloor == 1);
		EXPECT(hostileMimic->authoredMapLayer == 1);
		EXPECT(hostileMimic->z == -1.25);
		EXPECT(hostileMimic->spatialRevision > firstRevision);
		moveMonsterToEdge(*hostileMimic, *secondEdge);
		EXPECT(updateAutomatiaHostileVerticalNavigation(*hostileMimic, true)
			== AutomatiaHostileVerticalNavigationStatus::Transitioned);
		headless = false;
		EXPECT(hostileMimic->playableFloor == 2);
		EXPECT(hostileMimic->authoredMapLayer == 2);
		EXPECT(hostileMimic->hostileVerticalNavigationActive);
		EXPECT(updateAutomatiaHostileVerticalNavigation(*hostileMimic, true)
			== AutomatiaHostileVerticalNavigationStatus::ResumedSameFloor);
		EXPECT(!hostileMimic->hostileVerticalNavigationActive);
		EXPECT(hostileMimic->monsterTarget == target->getUID());
		multiplayer = SINGLE;

		// A valid gameplay floor with no graph edge is unreachable even when the
		// target shares horizontal coordinates.
		moveTargetTo(0, 6, 5);
		Entity* disconnectedHostile = makeMonster(
			GOBLIN, Stat::MONSTER_FORCE_PLAYER_ENEMY, 6, 5);
		EXPECT(disconnectedHostile != nullptr);
		disconnectedHostile->monsterAcquireAttackTarget(
			*target, MONSTER_STATE_PATH);
		EXPECT(disconnectedHostile->monsterTarget == target->getUID());
		moveTargetTo(3, 6, 5);
		EXPECT(updateAutomatiaHostileVerticalNavigation(
			*disconnectedHostile, true)
			== AutomatiaHostileVerticalNavigationStatus::RouteUnavailable);
		EXPECT(disconnectedHostile->playableFloor == 0);

		// A stale target owned by another MapInstance is rejected both by the
		// coordinator and by fresh same-floor target acquisition.
		moveTargetTo(0, 5, 5);
		Entity* differentInstanceHostile = makeMonster(
			GOBLIN, Stat::MONSTER_FORCE_PLAYER_ENEMY, 5, 5);
		EXPECT(differentInstanceHostile != nullptr);
		differentInstanceHostile->monsterAcquireAttackTarget(
			*target, MONSTER_STATE_PATH);
		EXPECT(differentInstanceHostile->monsterTarget == target->getUID());
		player.worldInstance = sibling->identity;
		EXPECT(updateAutomatiaHostileVerticalNavigation(
			*differentInstanceHostile, true)
			== AutomatiaHostileVerticalNavigationStatus::TargetUnavailable);
		EXPECT(differentInstanceHostile->monsterTarget == 0);
		differentInstanceHostile->monsterAcquireAttackTarget(
			*target, MONSTER_STATE_PATH);
		EXPECT(differentInstanceHostile->monsterTarget == 0);
		player.worldInstance = instance->identity;

		// Stationary dialogue NPCs and passive authored Mini Mimics never consume
		// hostile vertical routes, even if a stale target UID is injected.
		moveTargetTo(0, 4, 1);
		Entity* dialogueNpc = makeMonster(
			HUMAN, Stat::MONSTER_FORCE_PLAYER_ENEMY, 6, 6);
		EXPECT(dialogueNpc != nullptr);
		Stat* dialogueStats = dialogueNpc->getStats();
		EXPECT(dialogueStats != nullptr);
		dialogueStats->MISC_FLAGS[STAT_FLAG_NPC] = 1;
		std::snprintf(dialogueStats->customDialogueID,
			sizeof(dialogueStats->customDialogueID), "%s", "z4d_dialogue");
		dialogueNpc->monsterTarget = target->getUID();
		dialogueNpc->monsterState = MONSTER_STATE_PATH;
		moveTargetTo(1, 4, 1);
		EXPECT(updateAutomatiaHostileVerticalNavigation(*dialogueNpc, true)
			== AutomatiaHostileVerticalNavigationStatus::TargetUnavailable);
		EXPECT(dialogueNpc->playableFloor == 0);

		moveTargetTo(0, 5, 6);
		Entity* passiveMimic = makeMonster(
			MINIMIMIC, Stat::MONSTER_FORCE_PLAYER_NEUTRAL, 5, 6);
		EXPECT(passiveMimic != nullptr);
		passiveMimic->monsterTarget = target->getUID();
		passiveMimic->monsterState = MONSTER_STATE_PATH;
		moveTargetTo(1, 4, 1);
		EXPECT(updateAutomatiaHostileVerticalNavigation(*passiveMimic, true)
			== AutomatiaHostileVerticalNavigationStatus::TargetUnavailable);
		EXPECT(!passiveMimic->hostileVerticalNavigationActive);

		// Recruited Mini Mimics remain owned by Z4C, never by hostile routing.
		moveTargetTo(0, 4, 6);
		Entity* recruitedMimic = makeMonster(
			MINIMIMIC, Stat::MONSTER_FORCE_PLAYER_RECRUITABLE, 4, 6);
		EXPECT(recruitedMimic != nullptr);
		recruitedMimic->monsterAllyIndex = 0;
		moveTargetTo(1, 4, 1);
		EXPECT(updateAutomatiaHostileVerticalNavigation(*recruitedMimic, true)
			== AutomatiaHostileVerticalNavigationStatus::NotHostile);
		EXPECT(recruitedMimic->playableFloor == 0);

		// A S.A.M.-compatible monster is still an ordinary actMonster consumer;
		// custom metadata survives because no parallel S.A.M. path state exists.
		moveTargetTo(0, 1, 5);
		Entity* samCompatible = makeMonster(
			GOBLIN, Stat::MONSTER_FORCE_PLAYER_ENEMY, 1, 5);
		EXPECT(samCompatible != nullptr);
		Stat* samStats = samCompatible->getStats();
		EXPECT(samStats != nullptr);
		samStats->setAttribute("sam_fixture", "z4d_compatible");
		samCompatible->monsterAcquireAttackTarget(*target, MONSTER_STATE_PATH);
		moveTargetTo(1, 4, 1);
		EXPECT(updateAutomatiaHostileVerticalNavigation(*samCompatible, true)
			== AutomatiaHostileVerticalNavigationStatus::PathAssigned);
		EXPECT(samCompatible->hostileVerticalNavigationTarget
			== target->getUID());
		EXPECT(samStats->getAttribute("sam_fixture") == "z4d_compatible");

		player.entity = nullptr;
	}
	players[0] = previousPlayer;
	stats[0] = previousPlayerStats;
	delete playerStats;
	for (int player = 0; player < MAXPLAYERS; ++player)
	{
		client_disconnected[player] = previousDisconnected[player];
	}
	multiplayer = previousMultiplayer;
	loading = previousLoading;
	headless = previousHeadless;
	intro = previousIntro;
	everybodyfriendly = previousEverybodyFriendly;
	resetPersistentWorldSession();
	clearGlobalMapHarness();
	return true;
}

bool testAdjacentVerticalCircuitNeighbors()
{
    multiplayer = SINGLE;
    EXPECT(resetGlobalMapHarness(4, 4, 1));
    EXPECT(map.ensurePlayableFloorGeometry(1, false));
    EXPECT(map.ensurePlayableFloorGeometry(2, false));
    EXPECT(map.ensurePlayableFloorGeometry(3, false));

    Entity* floorOneWire = newEntity(18, 1, map.entities, nullptr);
    Entity* floorTwoWire = newEntity(18, 1, map.entities, nullptr);
    Entity* floorThreeWire = newEntity(18, 1, map.entities, nullptr);
    EXPECT(floorOneWire && floorTwoWire && floorThreeWire);
    for (Entity* wire : {floorOneWire, floorTwoWire, floorThreeWire})
    {
        wire->x = 24.0;
        wire->y = 24.0;
        wire->behavior = &actCircuit;
        wire->skill[28] = 1; // CIRCUIT_OFF
    }
    floorOneWire->playableFloor = floorOneWire->authoredMapLayer = 1;
    floorTwoWire->playableFloor = floorTwoWire->authoredMapLayer = 2;
    floorThreeWire->playableFloor = floorThreeWire->authoredMapLayer = 3;
    EXPECT(TileEntityList.addEntity(*floorOneWire) != nullptr);
    EXPECT(TileEntityList.addEntity(*floorTwoWire) != nullptr);
    EXPECT(TileEntityList.addEntity(*floorThreeWire) != nullptr);

    list_t* directNeighbors = floorOneWire->getPowerableNeighbors();
    EXPECT(directNeighbors != nullptr);
    bool foundFloorTwo = false;
    bool foundFloorThree = false;
    for (node_t* node = directNeighbors->first; node; node = node->next)
    {
        foundFloorTwo |= node->element == floorTwoWire;
        foundFloorThree |= node->element == floorThreeWire;
    }
    list_FreeAll(directNeighbors);
    std::free(directNeighbors);
    EXPECT(foundFloorTwo);
    EXPECT(!foundFloorThree);

    // A direct hop stops at floor 2, while a wire on floor 2 may relay it.
    floorOneWire->circuitPowerOn();
    EXPECT(floorOneWire->skill[28] == 2); // CIRCUIT_ON
    EXPECT(floorTwoWire->skill[28] == 2);
    EXPECT(floorThreeWire->skill[28] == 2);
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
		2,
		5,
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
	EXPECT(!stageAutomatiaCharacterSavedPlacement(
		1, "village.lmp", "world", 17,
		2, MAPLAYERS, 1, 2, 3, 4, 5, 6));
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
		&& testWallBusterUsesOwningPlayableFloor()
        && testPlayableFloorCollisionIsolation()
        && testZ3TransitionPrimitive()
        && testPlayableFloorGeometryIsolation()
        && testLocalElevationAndRuntimeSpawns()
        && testZ34CStructuralLayersAndFalling()
		&& testZ4AVerticalNavigationAndItemFalling()
		&& testZ2CRuntimeFloorIsolation()
		&& testUpperFloorMonsterPathing()
		&& testZ4BCrossFloorPathQuery()
		&& testZ4CFollowerVerticalNavigation()
		&& testZ4DHostileVerticalNavigation()
		&& testAdjacentVerticalCircuitNeighbors()
        && testPersistentMinimapAndPlacement()
#ifdef SAM_FRAMEWORK_ENABLED
		&& testSAMPersistentContainersAndMonsterEquipment(temporary)
#endif
        && testMapAmbienceLifecycleCharacterization();
    clearGlobalMapHarness();
    multiplayer = previousMultiplayer;
    loading = previousLoading;
    animatedtiles = previousAnimatedTiles;
    swimmingtiles = previousSwimmingTiles;
    lavatiles = previousLavaTiles;
    if (passed)
    {
        std::cout
            << "Stage 4D/Z4D stacked-sprite/fall runtime passed: legacy Entity::z remains "
            << "Z0-safe, ELYR authored-layer round-trip, structural runtime rendering/light "
            << "context, floor-aware multipart spawns, lower-floor landing search, persistent "
            << "Hermit ownership contracts, dropped-item floor transfer, MapInstance-local "
            << "Z4A vertical transition graphs, query-only Z4B cross-floor routes, and "
			<< "Z4C follower plus Z4D hostile authoritative stair traversal and "
			<< "cross-floor combat isolation.\n";
    }
    return passed ? 0 : 1;
}
