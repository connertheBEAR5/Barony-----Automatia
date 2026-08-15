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
    EXPECT(contains(tile, "map.tiles[z + y * MAPLAYERS"));
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
    EXPECT(contains(game, "entity.playableFloor = placement.playableFloor"));
    EXPECT(contains(game, "entity.z = placement.z;"));
    EXPECT(contains(game, "entity.new_z = entity.z;"));
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
    EXPECT(contains(net, "entity->playableFloor = placement.playableFloor;"));
    EXPECT(contains(net, "entity->z = placement.z;"));
    EXPECT(contains(net, "entity->new_z = placement.z;"));
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
    EXPECT(contains(files, "Stage Z1 keeps nonzero floors data-only until Z2 isolation"));
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
        ? 0
        : 1;
}
