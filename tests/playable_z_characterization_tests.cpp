#include "automatia_identity.hpp"
#include "automatia_save.hpp"
#include "late_join_protocol.hpp"
#include "late_join_state.hpp"
#include "world_packet_scope.hpp"
#include "playable_z.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
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

std::filesystem::path sourcePath(const char* relative)
{
    return std::filesystem::path(BARONY_SOURCE_DIR) / relative;
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return input ? contents.str() : std::string{};
}

std::string section(
    const std::string& source,
    const std::string& begin,
    const std::string& end)
{
    const std::size_t first = source.find(begin);
    if (first == std::string::npos)
    {
        return {};
    }
    const std::size_t last = source.find(end, first + begin.size());
    if (last == std::string::npos)
    {
        return {};
    }
    return source.substr(first, last - first);
}

bool contains(const std::string& source, const std::string& value)
{
    return source.find(value) != std::string::npos;
}

bool testEntityLocalZContracts()
{
    const std::string mainHeader = readFile(sourcePath("src/main.hpp"));
    const std::string entityHeader = readFile(sourcePath("src/entity.hpp"));
    const std::string entityShared =
        readFile(sourcePath("src/entity_shared.cpp"));
    const std::string player = readFile(sourcePath("src/actplayer.cpp"));
    const std::string monster = readFile(sourcePath("src/actmonster.cpp"));
    const std::string arrow = readFile(sourcePath("src/actarrow.cpp"));

    EXPECT(!mainHeader.empty());
    EXPECT(!entityHeader.empty());
    EXPECT(!entityShared.empty());
    EXPECT(!player.empty());
    EXPECT(!monster.empty());
    EXPECT(!arrow.empty());

    EXPECT(contains(entityHeader, "real_t x, y, z;"));
    EXPECT(contains(entityHeader, "real_t new_x, new_y, new_z;"));
    EXPECT(contains(entityHeader, "real_t vel_x, vel_y, vel_z;"));
    EXPECT(contains(entityShared, "z = 0;"));
    EXPECT(contains(entityShared, "new_z = 0;"));
    EXPECT(contains(entityShared, "vel_z = 0;"));

    // Existing player z is a continuously rewritten local model elevation.
    EXPECT(contains(player, "my->z = -1;"));
    EXPECT(contains(player, "my->z -= 1; // floating"));
    EXPECT(contains(player, "bodypart->z = my->z;"));
    EXPECT(contains(player, "cameraSetpointZ = (my->z * 2)"));

    // Monsters likewise animate or move vertically within the one floor.
    EXPECT(contains(monster, "my->z -= .25;"));
    EXPECT(contains(monster, "my->z += .5; // descend slowly"));

    // Projectile gravity operates directly on the same continuous local z.
    EXPECT(contains(arrow, "ARROW_VELZ += my->arrowFallSpeed;"));
    EXPECT(contains(arrow, "my->z += ARROW_VELZ;"));
    return true;
}

bool testOneFloorCollisionContracts()
{
    const std::string collision = readFile(sourcePath("src/collision.cpp"));
    const std::string net = readFile(sourcePath("src/net.cpp"));
    const std::string entity = readFile(sourcePath("src/entity.cpp"));
    EXPECT(!collision.empty());

    const std::string distance = section(
        collision, "real_t entityDist(Entity* my, Entity* your)",
        "Entity* entityClicked");
    EXPECT(!distance.empty());
    EXPECT(contains(distance, "my->playableFloor != your->playableFloor"));
    EXPECT(contains(distance, "std::numeric_limits<real_t>::infinity()"));
    EXPECT(contains(distance, "dx = my->x - your->x;"));
    EXPECT(contains(distance, "dy = my->y - your->y;"));
    EXPECT(!contains(distance, "->z"));

    const std::string overlap = section(
        collision, "bool entityInsideEntity(Entity* entity1, Entity* entity2)",
        "bool entityInsideSomething");
    EXPECT(!overlap.empty());
    EXPECT(contains(overlap, "entity1->playableFloor != entity2->playableFloor"));
    EXPECT(contains(overlap, "entity1->x"));
    EXPECT(contains(overlap, "entity1->y"));
    EXPECT(!contains(overlap, "->z"));

    const std::string tile = section(
        collision, "bool entityInsideTile(Entity* entity, int x, int y, int z",
        "bool entityInsideEntity");
    EXPECT(!tile.empty());
    EXPECT(contains(tile, "z == OBSTACLELAYER"));
    EXPECT(contains(tile, "else if ( z == 0 )"));
    EXPECT(contains(tile, "map.tileAt(x, y, z, entity->playableFloor)"));
    EXPECT(!contains(tile, "map.tiles["));
    return true;
}

bool testMapFormatContracts()
{
    const std::string mainHeader = readFile(sourcePath("src/main.hpp"));
    const std::string files = readFile(sourcePath("src/files.cpp"));
    EXPECT(!mainHeader.empty());
    EXPECT(!files.empty());

    EXPECT(contains(mainHeader, "#define MAPLAYERS 32"));
    EXPECT(contains(mainHeader, "#define LEGACY_MAPLAYERS 3"));
    EXPECT(contains(mainHeader, "#define FLOORLAYER 0"));
    EXPECT(contains(mainHeader, "#define OBSTACLELAYER 1"));
    EXPECT(contains(mainHeader, "#define CEILINGLAYER 2"));

    for (int minor = 0; minor <= 8; ++minor)
    {
        EXPECT(contains(
            files,
            "BARONY LMPV4." + std::to_string(minor)));
    }
    EXPECT(contains(files, "fileNumLayers == 0 || fileNumLayers > MAPLAYERS"));
    EXPECT(contains(files, "destmap->numLayers = MAPLAYERS;"));
    EXPECT(contains(files, "if (editorVersion >= 41)"));
    EXPECT(contains(files, "entity->z = 0.0;"));
    EXPECT(contains(files, "\"BARONY LMPV4.8\""));
    EXPECT(contains(files, "\"BARONY LMPV4.9\""));
    EXPECT(contains(files, "kPlayableZExtensionMagic"));
    EXPECT(contains(files, "appendPlayableZChunk(payload, \"FLOR\""));
    EXPECT(contains(files, "appendPlayableZChunk(payload, \"EFLR\""));
    EXPECT(contains(files, "fp->write(&entity->z, sizeof(real_t), 1);"));

    // Characterize the resource fixtures present in a full game build.
    const std::filesystem::path maps =
        std::filesystem::path(BARONY_BINARY_DIR) / "maps";
    if (std::filesystem::is_directory(maps))
    {
        std::size_t mapsSeen = 0;
        std::size_t v48 = 0;
        std::size_t legacy = 0;
        std::set<std::string> headers;
        for (const auto& entry : std::filesystem::directory_iterator(maps))
        {
            if (!entry.is_regular_file()
                || entry.path().extension() != ".lmp")
            {
                continue;
            }
            std::ifstream input(entry.path(), std::ios::binary);
            std::array<char, 14> raw{};
            input.read(raw.data(), static_cast<std::streamsize>(raw.size()));
            if (input.gcount() != static_cast<std::streamsize>(raw.size()))
            {
                continue;
            }
            ++mapsSeen;
            const std::string header(raw.data(), raw.size());
            headers.insert(header);
            v48 += header == "BARONY LMPV4.8" ? 1U : 0U;
            legacy += header != "BARONY LMPV4.8" ? 1U : 0U;
        }
        if (mapsSeen > 0)
        {
            EXPECT(v48 > 0);
            EXPECT(legacy > 0);
            std::cout << "Characterized " << mapsSeen
                << " installed LMP fixtures across " << headers.size()
                << " header variants.\n";
        }
    }
    return true;
}

AutomatiaSave::Json makeSpatialWorldDocument()
{
    using AutomatiaSave::Json;
    Json document = AutomatiaSave::makeEmptyWorldSave("stage4c-session");
    document["active_instance"] = "village.lmp#world";
    document["map_instances"].push_back(Json{
        {"map_file", "village.lmp"},
        {"instance_id", "world"},
        {"revision", 17},
        {"width", 64},
        {"height", 48},
        {"playable_floors", Json::array({0, 2})},
        {"persistent_state", Json{
            {"dynamic_world_items", Json::array({Json{
                {"stable_id", "core:rock"},
                {"playable_floor", 2},
                {"position", Json::array({12.25, 15.5, -2.75})},
                {"rotation", Json::array({0.1, 0.2, 0.3})},
                {"velocity", Json::array({1.0, 2.0, -0.5})}
            }})},
            {"mechanisms", Json::array({Json{
                {"persistent_id", 91},
                {"playable_floor", 2},
                {"monster_type", 1},
                {"monster_position", Json::array({40.0, 24.0, 6.0})},
                {"switch_power", 1},
                {"signal_timer_count", 14}
            }})},
            {"dynamic_boulders", Json::array({Json{
                {"source_trap_id", 22},
                {"playable_floor", 2},
                {"position", Json::array({32.0, 48.0, -7.0})},
                {"rotation", Json::array({0.0, 1.5, 0.0})},
                {"velocity", Json::array({0.5, 0.0, 1.0})}
            }})},
            {"tile_states", Json::array({Json{
                {"playable_floor", 2},
                {"x", 7}, {"y", 9}, {"layer", 19}, {"tile", 1337}
            }})},
            {"player_minimaps", Json{{
                "local:alice", Json{{"floors", Json::array({Json{
                    {"playable_floor", 2}, {"width", 2}, {"height", 2},
                    {"runs", Json::array({Json::array({0, 1, 1}),
                        Json::array({2, 2, 3})})}
                }})}}
            }}}
        }}
    });
    document["players"].push_back(Json{
        {"player_id", "local:alice"},
        {"identity_kind", "local_character"},
        {"map_file", "village.lmp"},
        {"instance_id", "world"},
        {"revision", 17},
        {"playable_floor", 2},
        {"position", Json::array({128.125, 96.5, -1.75})},
        {"rotation", Json::array({1.25, -0.2, 0.1})}
    });
    return document;
}

bool writeLoadEqual(
    const std::filesystem::path& path,
    const AutomatiaSave::Json& expected)
{
    EXPECT(AutomatiaSave::validate(expected).ok);
    EXPECT(AutomatiaSave::writeAtomic(path, expected).ok);
    AutomatiaSave::Json loaded;
    EXPECT(AutomatiaSave::load(path, loaded).ok);
    EXPECT(loaded == expected);
    return true;
}

bool testWorldSaveAndPlacementContracts()
{
    using AutomatiaSave::Json;
    Json schemaThree = makeSpatialWorldDocument();
    EXPECT(schemaThree["schema_version"] == 3);
    EXPECT(AutomatiaSave::validate(schemaThree).ok);
    EXPECT(schemaThree["map_instances"][0]["playable_floors"].size() == 2);
    EXPECT(schemaThree["players"][0]["playable_floor"] == 2);

    // Schema 1/2 remain readable as one implicit floor Z0.
    Json schemaTwo = schemaThree;
    schemaTwo["schema_version"] = 2;
    schemaTwo["map_instances"][0].erase("playable_floors");
    schemaTwo["players"][0].erase("playable_floor");
    EXPECT(AutomatiaSave::validate(schemaTwo).ok);

    Json schemaOne = schemaTwo;
    schemaOne["schema_version"] = 1;
    schemaOne.erase("party");
    EXPECT(AutomatiaSave::validate(schemaOne).ok);

    Json missingZ0 = schemaThree;
    missingZ0["map_instances"][0]["playable_floors"] = Json::array({2});
    EXPECT(!AutomatiaSave::validate(missingZ0));

    Json duplicateFloor = schemaThree;
    duplicateFloor["map_instances"][0]["playable_floors"] =
        Json::array({0, 2, 2});
    EXPECT(!AutomatiaSave::validate(duplicateFloor));

    Json missingPlayerFloor = schemaThree;
    missingPlayerFloor["players"][0].erase("playable_floor");
    EXPECT(!AutomatiaSave::validate(missingPlayerFloor));

    Json unknownPlayerFloor = schemaThree;
    unknownPlayerFloor["players"][0]["playable_floor"] = 7;
    EXPECT(!AutomatiaSave::validate(unknownPlayerFloor));

    Json invalidPosition = schemaThree;
    invalidPosition["players"][0]["position"][2] =
        std::numeric_limits<double>::infinity();
    EXPECT(!AutomatiaSave::validate(invalidPosition));

    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("barony-stage4c-save-" + std::to_string(unique));
    EXPECT(std::filesystem::create_directories(directory));
    EXPECT(writeLoadEqual(directory / "schema3.json", schemaThree));
    EXPECT(writeLoadEqual(directory / "schema2.json", schemaTwo));
    EXPECT(writeLoadEqual(directory / "schema1.json", schemaOne));
    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
    EXPECT(!cleanupError);

    const std::string game = readFile(sourcePath("src/game.cpp"));
    EXPECT(!game.empty());
    EXPECT(contains(game, "struct AutomatiaSavedPlayerPlacement"));
    EXPECT(contains(game, "PlayableFloorId playableFloor"));
    EXPECT(contains(game, "{\"playable_floor\", placement.playableFloor}"));
    EXPECT(contains(game, "applyAutomatiaPlayableFloorPlacement("));
    EXPECT(contains(game, "placement.playableFloor,"));
    EXPECT(contains(game, "placement.z,"));
    EXPECT(contains(game, "restorePersistentMinimap()"));
    return true;
}

bool testMinimapContracts()
{
    const std::string game = readFile(sourcePath("src/game.cpp"));
    EXPECT(!game.empty());
    EXPECT(contains(game, "Per-character explored minimap state."));
    EXPECT(contains(game, "PersistentMinimapFloorRegistry"));
    EXPECT(contains(game, "std::unordered_map<PlayableFloorId, PersistentMinimapState>"));
    EXPECT(contains(game, "persistentPlayerMinimapRegistry"));
    EXPECT(contains(game, "savedPlayer[\"floors\"]"));
    EXPECT(contains(game, "\"playable_floor\", playableFloor"));
    EXPECT(contains(game, "restorePersistentMinimap()"));
    EXPECT(contains(game, "persistentMinimapClearedPlayableFloor"));
    return true;
}

bool testDivergentMapAndLateJoinContracts()
{
    const std::array<std::uint8_t, 4> entity = {'E', 'N', 'T', 'U'};
    const std::array<std::uint8_t, 4> movement = {'P', 'M', 'O', 'V'};
    const std::array<std::uint8_t, 4> tile = {'M', 'A', 'P', 'T'};
    const std::array<std::uint8_t, 4> globalChat = {'M', 'S', 'G', 'S'};
    const std::array<std::uint8_t, 4> partyState = {'P', 'T', 'Y', 'S'};
    EXPECT(packetUsesActiveMapScope(entity.data(), entity.size()));
    EXPECT(packetUsesActiveMapScope(movement.data(), movement.size()));
    EXPECT(packetUsesActiveMapScope(tile.data(), tile.size()));
    EXPECT(!packetUsesActiveMapScope(globalChat.data(), globalChat.size()));
    EXPECT(!packetUsesActiveMapScope(partyState.data(), partyState.size()));

    LateJoinSnapshotTransaction transaction;
    EXPECT(transaction.begin(77, 19, 2, 6));
    EXPECT(transaction.acceptChunk(77, 19, 1, 3, 101)
        == LateJoinChunkResult::Accepted);
    EXPECT(transaction.acceptChunk(77, 19, 0, 3, 100)
        == LateJoinChunkResult::Complete);
    EXPECT(transaction.authorize());
    EXPECT(transaction.acceptChunk(77, 20, 0, 3, 100)
        != LateJoinChunkResult::Accepted);

    using namespace LateJoinProtocol;
    Begin begin;
    begin.transferId = 91;
    begin.instanceRevision = 19;
    begin.chunkCount = 1;
    begin.totalBytes = 3;
    begin.snapshotChecksum = crc32(
        reinterpret_cast<const std::uint8_t*>("xyz"), 3);
    begin.sessionKey = 123;
    const auto encoded = encodeBegin(begin);
    Begin decoded;
    EXPECT(decodeBegin(encoded.data(), encoded.size(), decoded));
    EXPECT(decoded.instanceRevision == 19);

    const std::string net = readFile(sourcePath("src/net.cpp"));
    EXPECT(!net.empty());
    EXPECT(contains(net, "record[17] = 5;"));
    EXPECT(contains(net, "writeFixed(record, positionOffset + 8, authoritativePlayer->z);"));
    EXPECT(contains(net, "positionOffset + 24"));
    EXPECT(contains(net, "authoritativePlayer->playableFloor"));
    EXPECT(contains(net, "authoritativePlayer->spatialRevision"));
    EXPECT(contains(net, "staged.z = decode(8);"));
    EXPECT(contains(net, "staged.playableFloor"));
    EXPECT(contains(net, "staged.spatialRevision"));
    EXPECT(contains(net, "applyAutomatiaPlayableFloorPlacement("));
    EXPECT(contains(net, "placement.playableFloor,"));
    EXPECT(contains(net, "placement.spatialRevision,"));
    EXPECT(contains(net, "placement.z,"));
    EXPECT(ReconnectToken::isValid(
        "0123456789abcdef0123456789abcdef"));
    return true;
}

bool testPersistenceAndSpawnSourceContracts()
{
    const std::string game = readFile(sourcePath("src/game.cpp"));
    const std::string gib = readFile(sourcePath("src/actgib.cpp"));
    const std::string magic = readFile(sourcePath("src/magic/actmagic.cpp"));
    const std::string spellCasting = readFile(sourcePath("src/magic/castSpell.cpp"));
    const std::string magicCore = readFile(sourcePath("src/magic/magic.cpp"));
    const std::string monster = readFile(sourcePath("src/actmonster.cpp"));
    const std::string item = readFile(sourcePath("src/actitem.cpp"));
    EXPECT(!game.empty());
    EXPECT(!gib.empty());
    EXPECT(!magic.empty());
    EXPECT(!spellCasting.empty());
    EXPECT(!magicCore.empty());
    EXPECT(!monster.empty());
    EXPECT(!item.empty());

    EXPECT(contains(game, "struct PersistentWorldItemState"));
    EXPECT(contains(game, "struct PersistentBoulderState"));
    EXPECT(contains(game, "struct PersistentTileState"));
    EXPECT(contains(game, "real_t monsterSavedZ = 0.0;"));
    EXPECT(contains(game, "persistent[\"dynamic_world_items\"]"));
    EXPECT(contains(game, "persistent[\"dynamic_boulders\"]"));
    EXPECT(contains(game, "persistent[\"tile_states\"]"));
    EXPECT(contains(game, "persistent[\"mechanisms\"]"));
    EXPECT(contains(game, "{\"position\", {item.x, item.y, item.z}}"));
    EXPECT(contains(game, "{\"playable_floor\", item.playableFloor}"));
    EXPECT(contains(game, "{\"position\", {boulder.x, boulder.y, boulder.z}}"));
    EXPECT(contains(game, "{\"playable_floor\", boulder.playableFloor}"));
    EXPECT(contains(game, "PersistentTileKey"));
    EXPECT(contains(game, "PlayableFloorId playableFloor"));

    EXPECT(contains(gib, "entity->z = z;"));
    EXPECT(contains(gib, "parentent->z - 4"));
    EXPECT(contains(gib, "newEntityWithSpatialContext"));
    EXPECT(contains(magic, "entity->z = parentent->z"));
    EXPECT(contains(magic, "newEntityWithSpatialContext"));
    EXPECT(contains(magic, "inheritSpatialContextFrom"));
    EXPECT(contains(magic, "dropped->inheritSpatialContextFrom(parent);"));
    EXPECT(contains(spellCasting, "dropped->inheritSpatialContextFrom(target);"));
    EXPECT(contains(spellCasting, "dropped->inheritSpatialContextFrom(caster);"));
    EXPECT(contains(magicCore, "dropped->inheritSpatialContextFrom(target);"));
    EXPECT(contains(monster, "entity->z = 6;"));
    EXPECT(contains(monster, "entity->inheritSpatialContextFrom(my);"));
    EXPECT(contains(monster, "dropped->inheritSpatialContextFrom(this);"));
    EXPECT(contains(item, "my->z += ITEM_VELZ;"));
    EXPECT(contains(item, "newEntityWithSpatialContext"));
    EXPECT(contains(item, "dropped->inheritSpatialContextFrom(monsterInteracting);"));

    const std::string player = readFile(sourcePath("src/actplayer.cpp"));
    EXPECT(contains(player, "bodypart->z = my->z;"));
    EXPECT(contains(player, "newEntityWithSpatialContext"));
    EXPECT(contains(player, "inheritSpatialContextFrom"));
    return true;
}

bool testPlayableZDataFoundationContract()
{
    const std::string playableZ = readFile(sourcePath("src/playable_z.hpp"));
    const std::string mainHeader = readFile(sourcePath("src/main.hpp"));
    const std::string entityHeader = readFile(sourcePath("src/entity.hpp"));
    const std::string entityShared = readFile(sourcePath("src/entity_shared.cpp"));
    const std::string files = readFile(sourcePath("src/files.cpp"));
    const std::string game = readFile(sourcePath("src/game.cpp"));
    const std::string worldState = readFile(sourcePath("src/world_state.hpp"));
    const std::string worldSave = readFile(sourcePath("src/automatia_world_save.cpp"));
    const std::string collision = readFile(sourcePath("src/collision.cpp"));
    const std::string net = readFile(sourcePath("src/net.cpp"));
    const std::string entity = readFile(sourcePath("src/entity.cpp"));
    const std::string maps = readFile(sourcePath("src/maps.cpp"));

    EXPECT(!playableZ.empty());
    EXPECT(contains(playableZ, "using PlayableFloorId = std::int16_t;"));
    EXPECT(contains(playableZ, "DEFAULT_PLAYABLE_FLOOR = 0"));
    EXPECT(contains(playableZ, "MAX_PLAYABLE_FLOORS_PER_MAP = 64"));
    EXPECT(contains(playableZ, "struct SpatialSpawnContext"));
    EXPECT(contains(playableZ, "struct PlayableFloorData"));
    EXPECT(contains(playableZ, "struct PlayableFloorTable"));

    EXPECT(contains(mainHeader, "PlayableFloorTable playableFloors;"));
    EXPECT(contains(entityHeader, "PlayableFloorId playableFloor"));
    EXPECT(contains(entityHeader, "std::uint64_t spatialRevision"));
    EXPECT(contains(entityHeader, "SpatialSpawnContext spatialSpawnContext() const"));
    EXPECT(contains(entityShared, "newEntityWithSpatialContext"));
    EXPECT(contains(worldState, "std::vector<PlayableFloorId> playableFloors"));
    EXPECT(contains(worldSave, "{\"playable_floors\", std::move(playableFloors)}"));

    EXPECT(contains(files, "BARONY LMPV4.9"));
    EXPECT(contains(files, "loadPlayableZExtension"));
    EXPECT(contains(files, "savePlayableZExtension"));
    EXPECT(AutomatiaSave::CURRENT_SCHEMA_VERSION == 3);

    // Z2A begins real floor isolation while preserving local continuous z.
    const std::string distance = section(
        collision, "real_t entityDist(Entity* my, Entity* your)",
        "Entity* entityClicked");
    EXPECT(!distance.empty());
    EXPECT(contains(distance, "playableFloor"));
    EXPECT(contains(entityHeader, "bool setPlayableFloor(PlayableFloorId newPlayableFloor);"));
    EXPECT(contains(readFile(sourcePath("src/game.hpp")), "additionalFloorGrids"));
    EXPECT(contains(readFile(sourcePath("src/game.hpp")), "getTileList(int x, int y, PlayableFloorId playableFloor)"));

    // Z2B adds the explicit geometry access layer. Nonzero floors must never
    // silently fall back to map.tiles / floor Z0.
    EXPECT(contains(mainHeader, "tilesForPlayableFloor"));
    EXPECT(contains(mainHeader, "ensurePlayableFloorGeometry"));
    EXPECT(contains(mainHeader, "Sint32 tileAt"));
    EXPECT(contains(mainHeader, "bool setTileAt"));
    EXPECT(contains(mainHeader, "setTileAttribute"));
    EXPECT(!contains(collision, "map.tiles["));

    const std::string draw = readFile(sourcePath("src/draw.cpp"));
    const std::string opengl = readFile(sourcePath("src/opengl.cpp"));
    const std::string player = readFile(sourcePath("src/actplayer.cpp"));
    EXPECT(contains(draw, "getCameraPlayableFloor"));
    EXPECT(contains(draw, "tilesForPlayableFloorRendering(renderFloor)"));
    EXPECT(contains(draw, "entityVisibleOnCameraFloor"));
    EXPECT(contains(opengl, "chunksByPlayableFloor"));
    EXPECT(contains(opengl, "tilesForPlayableFloorRendering(playableFloor)"));
    EXPECT(contains(opengl, "chunk.build(map, !shouldDrawClouds(map), x, y, chunkSize, chunkSize, playableFloor)"));
    EXPECT(contains(player, "TILE_ATTRIBUTE_SLIPPERY, my->playableFloor"));
    EXPECT(contains(game, "originalFloorTiles"));
    EXPECT(contains(game, "PWTZ"));
    EXPECT(contains(net, "{'PWTZ'"));
    EXPECT(contains(entity, "mapTileDiggable(hit.mapx, hit.mapy, playableFloor)"));
    EXPECT(contains(entity, "\"WALZ\""));
    EXPECT(contains(net, "{'WALZ'"));
    EXPECT(contains(maps, "newEntityWithSpatialContext"));
    EXPECT(contains(maps, "postProcessEntity->playableFloor"));
    EXPECT(contains(game, "transitionAutomatiaPlayerThroughPlayableFloorEndpoint"));
    EXPECT(contains(files, "Z3 loaded map"));
    EXPECT(contains(files, "authored ZTRN endpoints may transition players between them"));
    return true;
}

bool testZ2CRuntimeIsolationContracts()
{
    const std::string playableZ = readFile(sourcePath("src/playable_z.hpp"));
    const std::string lightHeader = readFile(sourcePath("src/light.hpp"));
    const std::string light = readFile(sourcePath("src/light.cpp"));
    const std::string objects = readFile(sourcePath("src/objects.cpp"));
    const std::string game = readFile(sourcePath("src/game.cpp"));
    const std::string gameHeader = readFile(sourcePath("src/game.hpp"));
    const std::string entityShared = readFile(sourcePath("src/entity_shared.cpp"));
    const std::string worldState = readFile(sourcePath("src/world_state.cpp"));
    const std::string net = readFile(sourcePath("src/net.cpp"));
    const std::string netHeader = readFile(sourcePath("src/net.hpp"));
    const std::string packetScope = readFile(sourcePath("src/world_packet_scope.hpp"));
    const std::string itemUsage = readFile(sourcePath("src/item_usage_funcs.cpp"));
    const std::string gib = readFile(sourcePath("src/actgib.cpp"));
    const std::string actGeneral = readFile(sourcePath("src/actgeneral.cpp"));
    const std::string spriteFx = readFile(sourcePath("src/actsprite.cpp"));
    const std::string actMagic = readFile(sourcePath("src/magic/actmagic.cpp"));
    const std::string magic = readFile(sourcePath("src/magic/magic.cpp"));
    const std::string handMagic = readFile(sourcePath("src/magic/act_HandMagic.cpp"));
    const std::string castSpell = readFile(sourcePath("src/magic/castSpell.cpp"));
    const std::string soundGame = readFile(sourcePath("src/engine/audio/sound_game.cpp"));
    const std::string opengl = readFile(sourcePath("src/opengl.cpp"));
    const std::string draw = readFile(sourcePath("src/draw.cpp"));
    const std::string playableZMap = readFile(sourcePath("src/playable_z_map.cpp"));
    const std::string sourceCMake = readFile(sourcePath("src/CMakeLists.txt"));

    EXPECT(!playableZMap.empty());
    EXPECT(contains(playableZMap, "map_t::tilesForPlayableFloor"));
    EXPECT(contains(playableZMap, "map_t::ensurePlayableFloorGeometry"));
    EXPECT(contains(playableZMap, "map_t::tileAt"));
    const std::string playableZMapSourceEntry =
        "\"${CMAKE_CURRENT_SOURCE_DIR}/playable_z_map.cpp\"";
    const std::size_t playableZMapGameEntry = sourceCMake.find(playableZMapSourceEntry);
    EXPECT(playableZMapGameEntry != std::string::npos);
    EXPECT(sourceCMake.find(playableZMapSourceEntry, playableZMapGameEntry + 1)
        != std::string::npos);

    EXPECT(contains(playableZ, "activeRuntimeSpatialContext"));
    EXPECT(contains(playableZ, "ScopedPlayableFloorRuntimeContext"));
    EXPECT(contains(playableZ, "playableFloorsShareRuntimeScope"));
    EXPECT(contains(entityShared, "activeRuntimeSpatialContext()"));
    EXPECT(contains(game, "ScopedPlayableFloorRuntimeContext playableFloorRuntimeScope"));

    EXPECT(contains(lightHeader, "struct PlayableFloorLightmapBuffers"));
    EXPECT(contains(lightHeader, "PlayableFloorId playableFloor;"));
    EXPECT(contains(lightHeader, "lightmapForPlayableFloor"));
    EXPECT(contains(light, "lightSphereShadowOnPlayableFloor"));
    EXPECT(contains(light, "map.tileAt("));
    EXPECT(contains(objects, "newLightOnPlayableFloor"));
    EXPECT(contains(worldState, "AdditionalPlayableFloorLightmaps"));
    EXPECT(contains(worldState, "swapAdditionalPlayableFloorLightmaps"));
    EXPECT(contains(opengl, "lightmapSmoothedForPlayableFloor"));
    EXPECT(contains(draw, "lightmapForPlayableFloor"));

    EXPECT(contains(readFile(sourcePath("src/game.hpp")), "#define ENTITY_PACKET_LENGTH 58"));
    EXPECT(contains(net, "kEntityPlayableFloorOffset = 48"));
    EXPECT(contains(net, "kEntitySpatialRevisionLowOffset = 50"));
    EXPECT(contains(net, "serverPlayerCanReceivePlayableFloorUpdates"));
    EXPECT(contains(net, "serverPlayerCanReceiveEntityUpdates"));
    EXPECT(contains(net, "receivedEntitySpatialContext"));
    EXPECT(contains(net, "serverSpawnMiscParticlesAtLocationWithSpatialContext"));
    EXPECT(contains(net, "receivedSpatialContextAt"));
    EXPECT(contains(net, "receivedSpatialContextAt(10)"));
    EXPECT(contains(net, "receivedSpatialContextAt(12)"));
    EXPECT(contains(net, "receivedSpatialContextAt(13)"));
    EXPECT(contains(net, "receivedSpatialContextAt(14)"));
    EXPECT(contains(netHeader, "serverPlayerCanReceivePlayableFloorUpdates"));
    EXPECT(contains(packetScope, "{'A', 'L', 'I', 'Z'}"));
    EXPECT(contains(itemUsage, "\"ALIZ\""));
    EXPECT(contains(itemUsage, "addLightOnPlayableFloor"));

    EXPECT(contains(net, "map.setTileAttribute(x, y, layer, flagSet, true, playableFloor)"));
    EXPECT(contains(gib, "map.tileAt(ox, oy, FLOORLAYER, playableFloor)"));
    EXPECT(contains(gib, "serverPlayerCanReceiveEntityUpdates(c, gib)"));
    EXPECT(contains(gib, "serverPlayerCanReceiveEntityUpdates(c, my)"));
    EXPECT(contains(gib, "serverPlayerCanReceiveEntityUpdates(c, parentent)"));
    EXPECT(contains(gib, "net_packet->len = 34"));
    EXPECT(contains(net, "receivedSpatialContextAt(24)"));
    EXPECT(contains(net, "ScopedPlayableFloorRuntimeContext spatialScope(parent->spatialSpawnContext())"));
    EXPECT(!contains(gib, "map.tiles["));
    EXPECT(contains(actGeneral, "serverPlayerCanReceiveEntityUpdates(c, my)"));
    EXPECT(contains(actGeneral, "serverPlayerCanReceiveEntityUpdates(i, entity)"));
    EXPECT(contains(actGeneral, "serverPlayerCanReceiveEntityUpdates(i, ent)"));
    EXPECT(contains(actGeneral, "\"BELI\""));
    EXPECT(contains(net, "ScopedPlayableFloorRuntimeContext spatialScope(entity->spatialSpawnContext())"));

    EXPECT(contains(spriteFx, "spatialContext.playableFloor"));
    EXPECT(contains(spriteFx, "net_packet->len = 20"));
    EXPECT(contains(spriteFx, "net_packet->len = 22"));
    EXPECT(contains(actMagic, "serverPlayerCanReceivePlayableFloorUpdates"));
    EXPECT(contains(actMagic, "map.setTileAt(hit.mapx, hit.mapy, OBSTACLELAYER, 0, playableFloor)"));
    EXPECT(contains(actMagic, "TileEntityList.getEntitiesWithinRadius(x / 16, y / 16, 1 + (radius / 16), my->playableFloor)"));
    EXPECT(!contains(actMagic, "map.tiles["));
    EXPECT(!contains(magic, "map.tiles["));
    EXPECT(!contains(handMagic, "map.tiles["));
    EXPECT(!contains(castSpell, "map.tiles["));
    EXPECT(contains(castSpell, "caster->playableFloor"));
    EXPECT(contains(soundGame, "serverPlayerCanReceivePlayableFloorUpdates("));
    EXPECT(contains(soundGame, "c, activeRuntimePlayableFloor())"));
    return true;
}

bool testZ3FloorTransitionContracts()
{
    const std::string entityHeader = readFile(sourcePath("src/entity.hpp"));
    const std::string entity = readFile(sourcePath("src/entity.cpp"));
    const std::string files = readFile(sourcePath("src/files.cpp"));
    const std::string gameHeader = readFile(sourcePath("src/game.hpp"));
    const std::string game = readFile(sourcePath("src/game.cpp"));
    const std::string ladder = readFile(sourcePath("src/actladder.cpp"));
    const std::string maps = readFile(sourcePath("src/maps.cpp"));
    const std::string net = readFile(sourcePath("src/net.cpp"));
    const std::string packetScope = readFile(sourcePath("src/world_packet_scope.hpp"));
    const std::string player = readFile(sourcePath("src/player.cpp"));
    const std::string interface = readFile(sourcePath("src/interface/interface.cpp"));
    const std::string clickDescription = readFile(sourcePath("src/interface/clickdescription.cpp"));
    const std::string cmake = readFile(sourcePath("CMakeLists.txt"));

    EXPECT(contains(entityHeader, "playableFloorTransitionEnabled"));
    EXPECT(contains(entityHeader, "playableFloorTransitionDestination"));
    EXPECT(contains(entityHeader, "playableFloorTransitionTargetPersistentID"));
    EXPECT(contains(entityHeader, "bool transitionToPlayableFloor("));
    EXPECT(contains(entity, "Entity::transitionToPlayableFloor("));
    EXPECT(contains(entity, "map.tileAt(tileX, tileY, FLOORLAYER, newPlayableFloor)"));
    EXPECT(contains(entity, "setPlayableFloor(newPlayableFloor)"));

    EXPECT(contains(files, "\"ZTRN\""));
    EXPECT(contains(files, "PlayableFloorTransitionAssignment"));
    EXPECT(contains(files, "source->second->playableFloorTransitionEnabled = true"));
    EXPECT(contains(files, "authored ZTRN endpoints may transition players between them"));

    EXPECT(contains(gameHeader, "transitionAutomatiaPlayerThroughPlayableFloorEndpoint"));
    EXPECT(contains(gameHeader, "applyAutomatiaPlayableFloorPlacement"));
    EXPECT(contains(gameHeader, "void actPlayableFloorTransition(Entity* my);"));
    EXPECT(contains(ladder, "void actPlayableFloorTransition(Entity* my)"));
    EXPECT(contains(ladder, "transitionAutomatiaPlayerThroughPlayableFloorEndpoint(i, my)"));
    EXPECT(contains(maps, "entity->behavior = &actPlayableFloorTransition"));
    EXPECT(contains(player, "parent->behavior == &actPlayableFloorTransition"));
    EXPECT(contains(player, "playerEntity->playableFloor"));
    EXPECT(!contains(player, "map.tiles["));
    EXPECT(contains(interface, "selectedEntity.behavior == &actPlayableFloorTransition"));
    EXPECT(contains(clickDescription, "entity->behavior == &actPlayableFloorTransition"));
    EXPECT(contains(game, "std::memcpy(net_packet->data, \"PZTR\", 4)"));
    EXPECT(contains(game, "serverPlayerCanReceiveActiveMapUpdates(recipient)"));
    EXPECT(contains(game, "capturePersistentMinimap(false)"));
    EXPECT(contains(game, "restorePersistentMinimap()"));

    EXPECT(contains(net, "{'PZTR'"));
    EXPECT(contains(packetScope, "{'P', 'Z', 'T', 'R'}"));
    EXPECT(contains(net, "receivedSpatialContext.spatialRevision < entity->spatialRevision"));
    EXPECT(contains(net, "receivedSpatialContext.playableFloor != entity->playableFloor"));
    EXPECT(contains(net, "pendingTunnelSpawn.playableFloor"));
    EXPECT(contains(net, "if (net_packet->len >= 30)"));
    EXPECT(contains(net, "applyAutomatiaPlayableFloorPlacement("));
    EXPECT(contains(net, "Runtime STRT requested unavailable floor"));

    EXPECT(contains(cmake, "--automatia-stage4d-z3-transition-characterization"));
    EXPECT(contains(game, "--automatia-stage4d-z3-transition-characterization"));
    return true;
}

bool testZ33LayerAuthoringCorrectionContracts()
{
    const std::string entityHeader = readFile(sourcePath("src/entity.hpp"));
    const std::string playableZHeader = readFile(sourcePath("src/playable_z.hpp"));
    const std::string playableZMap = readFile(sourcePath("src/playable_z_map.cpp"));
    const std::string editorHeader = readFile(sourcePath("src/editor.hpp"));
    const std::string editor = readFile(sourcePath("src/editor.cpp"));
    const std::string buttons = readFile(sourcePath("src/buttons.cpp"));
    const std::string draw = readFile(sourcePath("src/draw.cpp"));
    const std::string files = readFile(sourcePath("src/files.cpp"));
    const std::string maps = readFile(sourcePath("src/maps.cpp"));
    const std::string game = readFile(sourcePath("src/game.cpp"));
    const std::string gameHeader = readFile(sourcePath("src/game.hpp"));
    const std::string entityShared = readFile(sourcePath("src/entity_shared.cpp"));
    const std::string mainHeader = readFile(sourcePath("src/main.hpp"));
    const std::string opengl = readFile(sourcePath("src/opengl.cpp"));
    const std::string light = readFile(sourcePath("src/light.cpp"));
    const std::string player = readFile(sourcePath("src/actplayer.cpp"));
    const std::string hudWeapon = readFile(sourcePath("src/acthudweapon.cpp"));
    const std::string handMagic = readFile(sourcePath("src/magic/act_HandMagic.cpp"));
    const std::string actLadder = readFile(sourcePath("src/actladder.cpp"));
    const std::string collisionSource = readFile(sourcePath("src/collision.cpp"));
    const std::string torchSource = readFile(sourcePath("src/acttorch.cpp"));
    const std::string netSource = readFile(sourcePath("src/net.cpp"));

    // The existing Zed layer selector is the only vertical authoring control.
    EXPECT(!contains(editorHeader, "editorPlayableFloor"));
    EXPECT(!contains(editorHeader, "butPlayableFloor"));
    EXPECT(!contains(editor, "buttonPlayableFloor"));
    EXPECT(!contains(buttons, "butPlayableFloor"));

    // Dedicated stairs are authored directly on the ordinary drawlayer.
    EXPECT(contains(editor, "Z STAIR UP (next map layer)"));
    EXPECT(contains(editor, "Z STAIR DOWN (previous map layer)"));
    EXPECT(contains(editor, "entity->z = spriteLayerToEntityZ(entity->authoredMapLayer)"));
    EXPECT(contains(editor, "entity->verticalLayerTransitionDelta = 1"));
    EXPECT(contains(editor, "entity->verticalLayerTransitionDelta = -1"));
    EXPECT(contains(editor,
        "Z STAIR UP selected; switched map layer %d -> %d for valid placement."));
    EXPECT(contains(editor,
        "Z STAIR DOWN selected; switched map layer %d -> %d for valid placement."));
    EXPECT(!contains(editor,
        "Z STAIR UP must be on a wall layer with room for floor/walls/ceiling above."));
    EXPECT(!contains(editor,
        "Z STAIR DOWN must be on map layer 2 or above."));

    // The old always-on hover overlay is gone; normal sprite hover/selection UI
    // owns transient editor feedback instead.
    EXPECT(!contains(draw, "zStairLabel"));
    EXPECT(!contains(draw, "L%d -> L%d"));

    // Z3.3F makes stairs decoration-compatible: model, 8-way direction,
    // height/X/Y offsets, wall attachment and interaction text all persist.
    EXPECT(contains(entityHeader, "verticalLayerTransitionModel"));
    EXPECT(contains(entityHeader, "decor-style 0..7"));
    EXPECT(contains(buttons, "Z Stair Up Decoration Properties:"));
    EXPECT(contains(buttons, "floorDecorationHeightOffset"));
    EXPECT(contains(buttons, "floorDecorationXOffset"));
    EXPECT(contains(buttons, "floorDecorationYOffset"));
    EXPECT(contains(buttons, "floorDecorationDestroyIfNoWall"));
    EXPECT(contains(buttons, "writeDecorationInteractTextFromSpriteProperties"));
    EXPECT(contains(files, "decorationRecords"));
    EXPECT(contains(files, "220U"));
    EXPECT(contains(files, "interactText"));
    EXPECT(contains(maps, "entity->z = 7.5 - entity->floorDecorationHeightOffset * 0.25"));
    EXPECT(contains(maps, "entity->floorDecorationXOffset * 0.25"));
    EXPECT(contains(maps, "stairRotation * (PI / 4.0)"));
    EXPECT(contains(actLadder, "playableLayerStairHasRequiredWall"));
    EXPECT(contains(actLadder, "playableLayerStairInteractText"));

    // Nonzero gameplay floors are derived from the existing map-layer stack,
    // not from a second editor-authored geometry stack.
    EXPECT(contains(playableZHeader, "derivedFromMapLayers"));
    EXPECT(contains(playableZMap, "authoredLayer = relativeLayer + playableFloor"));
    EXPECT(contains(playableZMap, "authoredLayer = layer + playableFloor"));
    EXPECT(contains(maps, "authoredMapLayer"));
    EXPECT(contains(maps, "entity->verticalLayerTransitionDelta != 0"));
    EXPECT(contains(maps, "static_cast<PlayableFloorId>(std::max(0, authoredMapLayer - 1))"));
    EXPECT(contains(maps, "static_cast<PlayableFloorId>(authoredMapLayer)"));
    EXPECT(contains(maps, "entity->z += 16.0 * static_cast<real_t>(authoredMapLayer)"));
    EXPECT(contains(maps, "ScopedPlayableFloorRuntimeContext authoredFloorContext"));

    // ZLDR stairs now perform a real one-layer transition without paired ZTRN
    // endpoints while legacy ZTRN maps remain supported.
    EXPECT(contains(files, "\"ZLDR\""));
    EXPECT(contains(files, "\"ZTRN\""));
    EXPECT(contains(game, "sourceEndpoint->verticalLayerTransitionDelta != 0"));
    EXPECT(contains(game, "map.ensurePlayableFloorGeometry(destinationFloor, false)"));
    EXPECT(contains(game, "stairExitDX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 }"));
    EXPECT(contains(game, "verticalLayerTransitionRotation"));
    EXPECT(contains(game, "no safe adjacent landing tile"));
    EXPECT(contains(game, "net_packet->len = 43"));
    EXPECT(contains(game, "used authored layer stair"));
    EXPECT(contains(maps, "entity->behavior = &actPlayableFloorTransition"));
    EXPECT(contains(entityShared,
        "entityNew->verticalLayerTransitionDelta = entityToCopy->verticalLayerTransitionDelta"));
    EXPECT(contains(entityShared,
        "entityNew->verticalLayerTransitionModel = entityToCopy->verticalLayerTransitionModel"));
    EXPECT(contains(entityShared,
        "entityNew->floorDecorationHeightOffset = entityToCopy->floorDecorationHeightOffset"));

    // Z3.3C keeps the authored map stack continuous for rendering. Changing
    // collision floors must not discard lower geometry, entities, or lights.
    EXPECT(contains(mainHeader, "tilesForPlayableFloorRendering"));
    EXPECT(contains(playableZMap, "playableFloorUsesAuthoredLayerStack"));
    EXPECT(contains(playableZMap, "return tilesForPlayableFloor(playableFloor)"));
    EXPECT(contains(draw, "tilesForPlayableFloorRendering"));
    EXPECT(contains(opengl, "tilesForPlayableFloorRendering"));
    EXPECT(contains(draw, "entity->playableFloor <= cameraFloor"));
    EXPECT(contains(player, "playableFloorCameraOffset"));
    EXPECT(contains(player, "-32.0 * static_cast<real_t>(my->playableFloor)"));
    EXPECT(contains(light, "map.playableFloorUsesAuthoredLayerStack(playableFloor)"));

    // Z3.3D/Z3.3E repairs the first real upstairs rendering regressions.
    // Structural wall layers expose a walkable top cap. HUD entities calculate
    // their local placement from the legacy camera-local Z while the OpenGL
    // OVERDRAW transform continues to cancel the real world camera transform.
    EXPECT(contains(opengl, "topExposed"));
    EXPECT(contains(opengl, "const float topHeight = z * 32.f - 16.f"));
    EXPECT(!contains(opengl, "authoredStackHudCameraZ"));
    EXPECT(contains(draw, "getCameraHudLocalZ"));
    EXPECT(contains(draw, "+ 32.0 * static_cast<real_t>(floor)"));
    EXPECT(contains(hudWeapon, "getCameraHudLocalZ(HUDWEAPON_PLAYERNUM)"));
    EXPECT(contains(hudWeapon, "getCameraHudLocalZ(HUDSHIELD_PLAYERNUM)"));
    EXPECT(contains(handMagic, "getCameraHudLocalZ(HANDMAGIC_PLAYERNUM)"));

    // Correctness first for upper authored floors: the old 2D occlusion solver
    // may not erase cumulative lower geometry. Submit all layers upstairs and
    // rely on the depth buffer until the later true 3D occlusion pass.
    EXPECT(contains(draw, "authoredStackUpperFloor"));
    EXPECT(contains(draw, "*disabled || authoredStackUpperFloor"));
    EXPECT(contains(draw, "updateRendererVisibilityMap("));
    EXPECT(contains(draw, "true"));

    // Z3.4A gives every editor sprite an explicit structural layer identity.
    // assignActions must never guess a floor from arbitrary legacy/runtime z.
    EXPECT(contains(entityHeader, "authoredMapLayer"));
    EXPECT(contains(files, "\"ELYR\""));
    EXPECT(contains(files, "editorVersion >= 49"));
    EXPECT(contains(files, "entity->authoredMapLayer = 0"));
    EXPECT(contains(maps, "static_cast<int>(entity->authoredMapLayer)"));
    EXPECT(!contains(section(maps, "void assignActions(", "int loadMainMenuMap("),
        "std::round(-entity->z / 16.0)"));
    EXPECT(contains(entityShared, "static_cast<Sint16>(source->structuralMapLayer())"));

    // First broad sprite-runtime isolation pass: tile/entity neighborhood scans
    // for mechanisms, wind and boulders stay on the owning playable floor.
    const std::string mechanisms = readFile(sourcePath("src/mechanisms.cpp"));
    const std::string boulder = readFile(sourcePath("src/actboulder.cpp"));
    const std::string door = readFile(sourcePath("src/actdoor.cpp"));
    const std::string gate = readFile(sourcePath("src/actgate.cpp"));
    const std::string arrowTrap = readFile(sourcePath("src/actarrowtrap.cpp"));
    const std::string bearTrap = readFile(sourcePath("src/actbeartrap.cpp"));
    const std::string general = readFile(sourcePath("src/actgeneral.cpp"));
    const std::string summonTrap = readFile(sourcePath("src/actsummontrap.cpp"));
    const std::string magicTrap = readFile(sourcePath("src/actmagictrap.cpp"));
    const std::string item = readFile(sourcePath("src/actitem.cpp"));
    const std::string monster = readFile(sourcePath("src/actmonster.cpp"));
    const std::string gold = readFile(sourcePath("src/actgold.cpp"));
    const std::string arrow = readFile(sourcePath("src/actarrow.cpp"));
    const std::string thrown = readFile(sourcePath("src/actthrown.cpp"));
    const std::string pedestal = readFile(sourcePath("src/actpedestal.cpp"));
    const std::string headstone = readFile(sourcePath("src/actheadstone.cpp"));
    const std::string itemsSource = readFile(sourcePath("src/items.cpp"));
    const std::string itemTool = readFile(sourcePath("src/item_tool.cpp"));
    const std::string duck = readFile(sourcePath("src/monster_duck.cpp"));
    EXPECT(contains(entityHeader, "checkTileForEntity(int x, int y, PlayableFloorId playableFloor)"));
    EXPECT(contains(entityHeader, "getPowerablesOnTile(int x, int y, list_t** list, PlayableFloorId playableFloor)"));
    EXPECT(contains(mechanisms, "getPowerablesOnTile(tx, ty, &return_val, playableFloor)"));
    EXPECT(contains(mechanisms, "entity1->playableFloor != wind->playableFloor"));
    EXPECT(contains(boulder, "map.tileAt(x, y, FLOORLAYER, my->playableFloor)"));
    EXPECT(contains(door, "entity->playableFloor != my->playableFloor"));
    EXPECT(contains(gate, "entity->playableFloor != playableFloor"));
    EXPECT(contains(arrowTrap, "map.tileAt(checkx, checky, OBSTACLELAYER, my->playableFloor)"));
    EXPECT(contains(bearTrap, "entity->playableFloor != my->playableFloor"));
    const std::string breakableFactory = section(
        general, "Entity* Entity::createBreakableCollider(", "void actColliderDecoration(");
    EXPECT(contains(breakableFactory, "parent->spatialSpawnContext()"));
    EXPECT(contains(breakableFactory, "activeRuntimeSpatialContext()"));
    EXPECT(contains(breakableFactory, "map.entities, nullptr, colliderContext"));
    EXPECT(!contains(breakableFactory, "parent ? parent : this"));
    EXPECT(contains(general, "FLOORLAYER, child->playableFloor"));
    EXPECT(contains(summonTrap, "map.tileAt(x, y, FLOORLAYER, my->playableFloor)"));
    EXPECT(contains(magicTrap, "map.tileAt(checkx, checky, OBSTACLELAYER, my->playableFloor)"));
    EXPECT(contains(item, "FLOORLAYER, my->playableFloor"));
    EXPECT(contains(gold, "FLOORLAYER, my->playableFloor"));
    EXPECT(contains(arrow, "const Sint32 floorTile = map.tileAt(tileX, tileY, FLOORLAYER, my->playableFloor)"));
    EXPECT(contains(thrown, "const Sint32 floorTile = map.tileAt(tileX, tileY, FLOORLAYER, my->playableFloor)"));
    EXPECT(!contains(thrown, "swimmingtiles[map.tiles[index]]"));
    EXPECT(contains(magicTrap, "entity->playableFloor != playableFloor"));
    const std::string daedalusInteract = section(
        magicTrap, "void daedalusShrineInteract(", "void Entity::actDaedalusShrine()");
    EXPECT(contains(daedalusInteract, "entity->playableFloor != my->playableFloor"));
    EXPECT(!contains(daedalusInteract, "entity->playableFloor != playableFloor"));
    EXPECT(contains(pedestal, "entity->playableFloor != playableFloor"));
    EXPECT(contains(gold, "entity->playableFloor != my->playableFloor"));
    EXPECT(contains(headstone, "entity->playableFloor != my->playableFloor"));

    // Z3.4B restores the palette-selection behavior lost during Z3.4A and
    // makes the Hermit's runtime duck participate in persistent-world state.
    EXPECT(contains(gameHeader, "automatiaPersistentHermitDuckExists"));
    EXPECT(contains(game, "monsterSavedDuckType"));
    EXPECT(contains(game, "\"monster_duck_type\""));
    EXPECT(contains(game, "savedState.monsterSavedDuckSpecialState"));
    EXPECT(contains(game, "savedState.monsterSavedType == DUCK_SMALL"));
    EXPECT(contains(game, "automatiaPersistentHermitDuckExists("));
    EXPECT(contains(itemTool, "summon->persistentDynamicMonster = true"));
    EXPECT(contains(itemsSource, "newEntityWithSpatialContext("));
    EXPECT(contains(itemsSource, "players[player]->entity); // thrown duck inherits the player's floor"));
    EXPECT(contains(player, "duckPersistedInWorld"));

    // Z3.4C makes structural ownership universal instead of treating stairs as
    // the only editor object that understands drawlayer. Every palette-created
    // sprite records drawlayer explicitly; selection/copy/group tools use that
    // identity rather than reverse-engineering it from model-height Z. Runtime
    // action conversion removes serialized structural Z once and rendering adds
    // the current structural layer back for ordinary entities.
    EXPECT(contains(editor, "entity->authoredMapLayer = static_cast<Sint16>("));
    EXPECT(contains(editor, "std::min(drawlayer, MAPLAYERS - 1)"));
    EXPECT(contains(editor, "entityAuthoredSpriteLayer("));
    EXPECT(!contains(editor, "entityZToSpriteLayer("));
    EXPECT(contains(maps, "authoredMapLayer > 0 || entity->verticalLayerTransitionDelta != 0"));
    EXPECT(contains(maps, "entity->playableFloor = authoredPlayableFloor;"));
    EXPECT(contains(entityHeader, "int structuralMapLayer() const"));
    EXPECT(contains(entityHeader, "return z - 16.0 * static_cast<real_t>(structuralMapLayer())"));
    EXPECT(contains(opengl, "entity->worldRenderZ()"));
    EXPECT(contains(opengl, "entityRenderZ(entity)"));
    EXPECT(contains(playableZHeader, "activeRuntimeStructuralMapLayer"));
    EXPECT(contains(light, "activeRuntimeStructuralMapLayer()"));
    EXPECT(contains(torchSource, "my->structuralLightmapLayer()"));
    EXPECT(contains(netSource, "context.authoredMapLayer = static_cast<std::int16_t>(context.playableFloor)"));

    // The editor's 3D preview must honor authored sky/skybox state even when
    // smooth lighting is disabled; otherwise Chunk::build synthesizes a false
    // stone ceiling that the real game does not render.
    EXPECT(contains(opengl, "const bool authoredSky = !strncmp(map.name, \"Hell\", 4) || map.skybox != 0"));
    EXPECT(contains(opengl, "const bool skyVisible = authoredSky;"));
    EXPECT(contains(opengl, "const bool skyVisible = authoredSky && smoothlighting;"));

    // Persistent Hermit ducks keep a durable owner slot in addition to the
    // transient entity UID. Old Z3.4B saves can still derive the owner from
    // duck_type, while restored ducks rebind to players[owner]->entity.
    EXPECT(contains(game, "monsterSavedDuckOwner"));
    EXPECT(contains(game, "\"monster_duck_owner\""));
    EXPECT(contains(itemTool, "setAttribute(\"duck_owner\""));
    EXPECT(contains(duck, "resolveHermitDuckOwner"));
    EXPECT(contains(duck, "players[owner]->entity"));
    EXPECT(contains(duck, "leaderTemporarilyUnavailable"));
    EXPECT(contains(duck, "leaderOnOtherPlayableFloor"));
    EXPECT(contains(monster, "durableOwnerPlayer"));
    EXPECT(contains(monster, "leader && leader->playableFloor != my->playableFloor"));

    // Missing tiles on an authored upper floor can now be real ledges. Player
    // collision permits the move only when a valid lower stacked landing exists;
    // actPlayer then performs a floor transition and damage is proportional to
    // the number of structural floors crossed. No lower landing preserves the
    // original bottomless-pit death path.
    EXPECT(contains(mainHeader, "findLowerPlayableFloorLanding"));
    EXPECT(contains(playableZMap, "for ( int candidate = static_cast<int>(fromFloor) - 1"));
    EXPECT(contains(collisionSource, "playerHasLowerStackedLanding"));
    EXPECT(contains(gameHeader, "fallAutomatiaPlayerToLowerPlayableFloor"));
    EXPECT(contains(game, "fallAutomatiaPlayerToLowerPlayableFloor("));
    EXPECT(contains(game, "broadcastAutomatiaPlayerFloorPlacement(playerIndex)"));
    EXPECT(contains(player, "const int fallDamage = std::max(1, floorsFallen)"));
    EXPECT(contains(player, "KilledBy::BOTTOMLESS_PIT"));
    return true;
}

}

int main()
{
    return testEntityLocalZContracts()
        && testOneFloorCollisionContracts()
        && testMapFormatContracts()
        && testWorldSaveAndPlacementContracts()
        && testMinimapContracts()
        && testDivergentMapAndLateJoinContracts()
        && testPersistenceAndSpawnSourceContracts()
        && testPlayableZDataFoundationContract()
        && testZ2CRuntimeIsolationContracts()
        && testZ3FloorTransitionContracts()
        && testZ33LayerAuthoringCorrectionContracts()
        ? 0
        : 1;
}
