/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: automatia_save.cpp
    Desc: Versioned, extensible, restart-safe persistent-world save envelope.

-------------------------------------------------------------------------------*/

#include "automatia_save.hpp"

#include "party_manager.hpp"
#include "world_instance.hpp"
#include "playable_z.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace AutomatiaSave
{
namespace
{
Result success()
{
    return {true, ""};
}

Result failure(const std::string& message)
{
    return {false, message};
}

bool safeTextId(const std::string& value, const std::size_t maximumLength)
{
    if (value.empty() || value.size() > maximumLength)
    {
        return false;
    }
    for (const unsigned char character : value)
    {
        if (character < 0x20 || character == 0x7f)
        {
            return false;
        }
    }
    return true;
}

bool nonNegativeInteger(const Json& value)
{
    if (value.is_number_unsigned())
    {
        return true;
    }
    return value.is_number_integer() && value.get<std::int64_t>() >= 0;
}

bool validPlayableFloor(const Json& value)
{
    if (!value.is_number_integer())
    {
        return false;
    }
    try
    {
		if (value.is_number_unsigned())
		{
			return value.get<std::uint64_t>()
				<= static_cast<std::uint64_t>(
					std::numeric_limits<PlayableFloorId>::max());
		}
        const std::int64_t floor = value.get<std::int64_t>();
        return floor >= std::numeric_limits<PlayableFloorId>::min()
            && floor <= std::numeric_limits<PlayableFloorId>::max();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool validAuthoredMapLayer(const Json& value)
{
	if (!value.is_number_integer())
	{
		return false;
	}
	try
	{
		if (value.is_number_unsigned())
		{
			return value.get<std::uint64_t>() < AUTHORED_MAP_LAYER_COUNT;
		}
		const std::int64_t layer = value.get<std::int64_t>();
		return layer >= 0
			&& layer < static_cast<std::int64_t>(AUTHORED_MAP_LAYER_COUNT);
	}
	catch (const std::exception&)
	{
		return false;
	}
}

bool finiteNumber(const Json& value)
{
    if (!value.is_number())
    {
        return false;
    }
    try
    {
        return std::isfinite(value.get<double>());
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool finiteTriplet(const Json& value)
{
    return value.is_array()
        && value.size() == 3
        && finiteNumber(value[0])
        && finiteNumber(value[1])
        && finiteNumber(value[2]);
}

Result validatePersistentSpatialState(
	const Json& persistent,
	const std::unordered_set<std::int64_t>& playableFloors,
	const std::uint64_t schemaVersion,
	const std::string& location)
{
	auto validateSpatialCollection = [&](const char* key) -> Result
	{
		if (!persistent.contains(key))
		{
			return success();
		}
		const Json& collection = persistent[key];
		if (!collection.is_array() || collection.size() > 1048576)
		{
			return failure(location + "." + key + " is not a bounded array");
		}
		for (const Json& record : collection)
		{
			if (!record.is_object())
			{
				return failure(location + "." + key + " contains a non-object record");
			}
			if (record.contains("playable_floor"))
			{
				if (!validPlayableFloor(record["playable_floor"]))
				{
					return failure(location + "." + key + " contains an invalid playable floor");
				}
				if (schemaVersion >= 3
					&& playableFloors.count(
						record["playable_floor"].get<std::int64_t>()) == 0)
				{
					return failure(location + "." + key + " refers to an unknown playable floor");
				}
			}
			if (record.contains("authored_map_layer")
				&& !validAuthoredMapLayer(record["authored_map_layer"]))
			{
				return failure(location + "." + key + " contains an invalid authored map layer");
			}
			if (record.contains("position")
				&& !finiteTriplet(record["position"]))
			{
				return failure(location + "." + key + " contains an invalid local position");
			}
		}
		return success();
	};

	for (const char* key : {
		"dynamic_world_items", "dynamic_gold_bags", "dynamic_boulders",
		"mechanisms"})
	{
		const Result result = validateSpatialCollection(key);
		if (!result)
		{
			return result;
		}
	}

	if (persistent.contains("tile_states"))
	{
		const Json& tiles = persistent["tile_states"];
		if (!tiles.is_array() || tiles.size() > 1048576)
		{
			return failure(location + ".tile_states is not a bounded array");
		}
		std::unordered_set<std::string> tileKeys;
		for (const Json& tile : tiles)
		{
			if (!tile.is_object()
				|| !tile.contains("x") || !tile["x"].is_number_integer()
				|| !tile.contains("y") || !tile["y"].is_number_integer()
				|| !tile.contains("layer") || !tile["layer"].is_number_integer()
				|| !tile.contains("tile") || !tile["tile"].is_number_integer())
			{
				return failure(location + ".tile_states contains an invalid tile record");
			}
			if (!validAuthoredMapLayer(tile["layer"]))
			{
				return failure(location + ".tile_states contains an invalid geometry layer");
			}
			const std::int64_t layer = tile["layer"].is_number_unsigned()
				? static_cast<std::int64_t>(tile["layer"].get<std::uint64_t>())
				: tile["layer"].get<std::int64_t>();
			std::int64_t playableFloor = DEFAULT_PLAYABLE_FLOOR;
			if (tile.contains("playable_floor"))
			{
				if (!validPlayableFloor(tile["playable_floor"]))
				{
					return failure(location + ".tile_states contains an invalid playable floor");
				}
				playableFloor = tile["playable_floor"].get<std::int64_t>();
			}
			if (schemaVersion >= 3 && playableFloors.count(playableFloor) == 0)
			{
				return failure(location + ".tile_states refers to an unknown playable floor");
			}
			const std::string key = std::to_string(playableFloor) + ":"
				+ tile["x"].dump() + ":"
				+ tile["y"].dump() + ":"
				+ std::to_string(layer);
			if (!tileKeys.insert(key).second)
			{
				return failure(location + ".tile_states contains a duplicate floor/x/y/layer key");
			}
		}
	}
	return success();
}

Result validateIdentity(const Json& object, const std::string& location)
{
    if (!object.is_object()
        || !object.contains("map_file")
        || !object["map_file"].is_string()
        || !object.contains("instance_id")
        || !object["instance_id"].is_string()
        || !object.contains("revision")
        || !nonNegativeInteger(object["revision"]))
    {
        return failure(location + " has an invalid map-instance identity");
    }

    const std::string mapFile = object["map_file"].get<std::string>();
    const std::string instanceId = object["instance_id"].get<std::string>();
    WorldInstanceIdentity identity;
    if (!identity.set(
            mapFile,
            instanceId
        )
        || identity.mapFile != mapFile
        || identity.instanceId != instanceId)
    {
        return failure(location + " contains an unsafe map-instance identity");
    }
    return success();
}

Result replaceFile(
    const std::filesystem::path& temporaryPath,
    const std::filesystem::path& destinationPath
)
{
#ifdef _WIN32
    if (!MoveFileExW(
            temporaryPath.c_str(),
            destinationPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
        ))
    {
        return failure("unable to atomically replace world save");
    }
#else
    if (::rename(temporaryPath.c_str(), destinationPath.c_str()) != 0)
    {
        return failure(
            "unable to atomically replace world save: "
            + std::string(std::strerror(errno))
        );
    }

    const std::filesystem::path parent = destinationPath.parent_path().empty()
        ? std::filesystem::path(".")
        : destinationPath.parent_path();
    const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (directory >= 0)
    {
        ::fsync(directory);
        ::close(directory);
    }
#endif
    return success();
}
}

Json makeEmptyWorldSave(const std::string& sessionId)
{
    return Json{
        {"format", "barony-automatia-world"},
        {"schema_version", CURRENT_SCHEMA_VERSION},
        {"session_id", sessionId},
        {"saved_at_unix_ms", 0},
        {"active_instance", ""},
        {"map_instances", Json::array()},
        {"players", Json::array()},
        {"party", Json{
            {"next_id", 1},
            {"parties", Json::array()}
        }},
        {"quests", Json::object()},
        {"dialogue", Json::object()},
        {"world_flags", Json::array()},
        {"world_variables", Json::object()},
        {"unknown_custom_items", Json::array()}
    };
}

Result validate(const Json& document)
{
    if (!document.is_object())
    {
        return failure("world save root must be an object");
    }
    if (!document.contains("format")
        || document["format"] != "barony-automatia-world")
    {
        return failure("world save format marker is missing or unsupported");
    }
    if (!document.contains("schema_version")
        || !nonNegativeInteger(document["schema_version"]))
    {
        return failure("world save schema version is missing or invalid");
    }

    const std::uint64_t version = document["schema_version"].get<std::uint64_t>();
    if (version < MINIMUM_SCHEMA_VERSION || version > CURRENT_SCHEMA_VERSION)
    {
        return failure("world save schema version is unsupported");
    }
    if (!document.contains("session_id")
        || !document["session_id"].is_string()
        || !safeTextId(document["session_id"].get<std::string>(), 128))
    {
        return failure("world save session ID is missing or unsafe");
    }
    if (document.contains("save_transaction_id")
        && (!document["save_transaction_id"].is_string()
            || !safeTextId(
                document["save_transaction_id"].get<std::string>(), 128)))
    {
        return failure("world save transaction ID is unsafe");
    }
    if (!document.contains("map_instances")
        || !document["map_instances"].is_array()
        || document["map_instances"].size() > 4096)
    {
        return failure("world save map instance collection is invalid");
    }
    if (!document.contains("players")
        || !document["players"].is_array()
        || document["players"].size() > 256)
    {
        return failure("world save player collection is invalid");
    }
    if (!document.contains("active_instance")
        || !document["active_instance"].is_string())
    {
        return failure("world save active instance is invalid");
    }

    if (version >= 2)
    {
        if (!document.contains("party"))
        {
            return failure("world save party state is missing");
        }
        std::string partyError;
        if (!AutomatiaParty::PartyManager::validatePersistentJson(
                document["party"], partyError))
        {
            return failure("world save party state is invalid: " + partyError);
        }
    }

    std::unordered_set<std::string> mapKeys;
    std::unordered_map<std::string, std::unordered_set<std::int64_t>>
        mapPlayableFloors;
    std::size_t index = 0;
    for (const Json& instance : document["map_instances"])
    {
        const Result identityResult = validateIdentity(
            instance,
            "map_instances[" + std::to_string(index) + "]"
        );
        if (!identityResult)
        {
            return identityResult;
        }
        static constexpr const char* nonNegativeFields[] = {
            "dungeon_level", "map_seed", "next_entity_uid",
            "next_persistent_id", "simulation_tick", "width", "height"
        };
        for (const char* field : nonNegativeFields)
        {
            if (instance.contains(field) && !nonNegativeInteger(instance[field]))
            {
                return failure(
                    "map_instances[" + std::to_string(index)
                    + "] has invalid field '" + field + "'"
                );
            }
        }
        static constexpr const char* booleanFields[] = {
            "loaded", "dirty", "simulation_active", "secret_level", "dark_map"
        };
        for (const char* field : booleanFields)
        {
            if (instance.contains(field) && !instance[field].is_boolean())
            {
                return failure(
                    "map_instances[" + std::to_string(index)
                    + "] has invalid field '" + field + "'"
                );
            }
        }
        if (instance.contains("persistent_state")
            && !instance["persistent_state"].is_object())
        {
            return failure(
                "map_instances[" + std::to_string(index)
                + "] has an invalid persistent-state payload"
            );
        }
        std::unordered_set<std::int64_t> validatedFloorIds;
        if (version >= 3)
        {
            if (!instance.contains("playable_floors")
                || !instance["playable_floors"].is_array()
                || instance["playable_floors"].empty()
                || instance["playable_floors"].size() > MAX_PLAYABLE_FLOORS_PER_MAP)
            {
                return failure(
                    "map_instances[" + std::to_string(index)
                    + "] has an invalid playable-floor table");
            }
            bool hasDefaultFloor = false;
            for (const Json& floor : instance["playable_floors"])
            {
                if (!validPlayableFloor(floor))
                {
                    return failure("world save playable-floor ID is invalid");
                }
                const std::int64_t id = floor.get<std::int64_t>();
                if (!validatedFloorIds.insert(id).second)
                {
                    return failure("world save playable-floor ID is duplicate");
                }
                hasDefaultFloor = hasDefaultFloor || id == DEFAULT_PLAYABLE_FLOOR;
            }
            if (!hasDefaultFloor)
            {
                return failure("world save map instance is missing playable floor Z0");
            }
        }
		if (instance.contains("persistent_state"))
		{
			const Result spatialResult = validatePersistentSpatialState(
				instance["persistent_state"],
				validatedFloorIds,
				version,
				"map_instances[" + std::to_string(index) + "].persistent_state");
			if (!spatialResult)
			{
				return spatialResult;
			}
		}
        if (instance.contains("players_present"))
        {
            const Json& occupants = instance["players_present"];
            if (!occupants.is_array() || occupants.size() > 256)
            {
                return failure("world save map occupant collection is invalid");
            }
            std::unordered_set<std::uint64_t> occupantSlots;
            for (const Json& occupant : occupants)
            {
                if (!nonNegativeInteger(occupant))
                {
                    return failure("world save map occupant is invalid");
                }
                const std::uint64_t slot = occupant.get<std::uint64_t>();
                if (slot >= 256 || !occupantSlots.insert(slot).second)
                {
                    return failure("world save map occupant is duplicate or out of range");
                }
            }
        }
        const std::string key =
            instance["map_file"].get<std::string>()
            + "#"
            + instance["instance_id"].get<std::string>();
        if (!mapKeys.insert(key).second)
        {
            return failure("world save contains a duplicate map instance");
        }
        if (version >= 3)
        {
            mapPlayableFloors.emplace(key, std::move(validatedFloorIds));
        }
        ++index;
    }

    const std::string activeInstance =
        document["active_instance"].get<std::string>();
    if (!activeInstance.empty() && mapKeys.count(activeInstance) == 0)
    {
        return failure("world save active instance is not in the map collection");
    }

    std::unordered_set<std::string> playerIds;
    index = 0;
    for (const Json& player : document["players"])
    {
        if (!player.is_object()
            || !player.contains("player_id")
            || !player["player_id"].is_string()
            || !safeTextId(player["player_id"].get<std::string>(), 128))
        {
            return failure(
                "players[" + std::to_string(index) + "] has an invalid player ID"
            );
        }
        const std::string playerId = player["player_id"].get<std::string>();
        if (!playerIds.insert(playerId).second)
        {
            return failure("world save contains a duplicate player ID");
        }
        if (player.contains("identity_kind")
            && (!player["identity_kind"].is_string()
                || !safeTextId(player["identity_kind"].get<std::string>(), 64)))
        {
            return failure(
                "players[" + std::to_string(index)
                + "] has an invalid identity kind"
            );
        }
        if (player.contains("slot")
            && (!nonNegativeInteger(player["slot"])
                || player["slot"].get<std::uint64_t>() >= 256))
        {
            return failure(
                "players[" + std::to_string(index) + "] has an invalid slot"
            );
        }
        if (player.contains("position") && !finiteTriplet(player["position"]))
        {
            return failure(
                "players[" + std::to_string(index)
                + "] has an invalid position"
            );
        }
        if (player.contains("rotation") && !finiteTriplet(player["rotation"]))
        {
            return failure(
                "players[" + std::to_string(index)
                + "] has an invalid rotation"
            );
        }
        if (version >= 3
            && (!player.contains("playable_floor")
                || !validPlayableFloor(player["playable_floor"])))
        {
            return failure(
                "players[" + std::to_string(index)
                + "] has an invalid playable floor");
        }
		if (player.contains("authored_map_layer")
			&& !validAuthoredMapLayer(player["authored_map_layer"]))
		{
			return failure(
				"players[" + std::to_string(index)
				+ "] has an invalid authored map layer");
		}
        const Result identityResult = validateIdentity(
            player,
            "players[" + std::to_string(index) + "]"
        );
        if (!identityResult)
        {
            return identityResult;
        }
        const std::string key =
            player["map_file"].get<std::string>()
            + "#"
            + player["instance_id"].get<std::string>();
        if (mapKeys.count(key) == 0)
        {
            return failure(
                "players[" + std::to_string(index)
                + "] refers to an unknown map instance"
            );
        }
        if (version >= 3)
        {
            const std::int64_t playerFloor =
                player["playable_floor"].get<std::int64_t>();
            const auto floors = mapPlayableFloors.find(key);
            if (floors == mapPlayableFloors.end()
                || floors->second.count(playerFloor) == 0)
            {
                return failure(
                    "players[" + std::to_string(index)
                    + "] refers to a playable floor absent from its map instance"
                );
            }
        }
        ++index;
    }

    if (document.contains("unknown_custom_items"))
    {
        const Json& unknownItems = document["unknown_custom_items"];
        if (!unknownItems.is_array() || unknownItems.size() > 65536)
        {
            return failure("unknown custom-item collection is invalid");
        }
        index = 0;
        for (const Json& item : unknownItems)
        {
            if (!item.is_object()
                || !item.contains("stable_id")
                || !item["stable_id"].is_string()
                || !safeTextId(item["stable_id"].get<std::string>(), 255)
                || (item.contains("source")
                    && (!item["source"].is_string()
                        || !safeTextId(item["source"].get<std::string>(), 255))))
            {
                return failure(
                    "unknown_custom_items[" + std::to_string(index)
                    + "] has invalid identity metadata"
                );
            }
            ++index;
        }
    }
    return success();
}

Result writeAtomic(const std::filesystem::path& path, const Json& document)
{
    const Result validation = validate(document);
    if (!validation)
    {
        return validation;
    }

    std::string serialized;
    try
    {
        serialized = document.dump(2);
    }
    catch (const std::exception& exception)
    {
        return failure("unable to serialize world save: " + std::string(exception.what()));
    }
    if (serialized.empty() || serialized.size() > MAX_SAVE_BYTES)
    {
        return failure("serialized world save exceeds the supported size");
    }

    std::error_code filesystemError;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, filesystemError);
        if (filesystemError)
        {
            return failure("unable to create world save directory");
        }
    }

    std::filesystem::path temporaryPath = path;
    temporaryPath += ".tmp";
#ifdef _WIN32
    std::FILE* file = _wfopen(temporaryPath.c_str(), L"wb");
#else
    std::FILE* file = std::fopen(temporaryPath.string().c_str(), "wb");
#endif
    if (!file)
    {
        return failure("unable to open temporary world save");
    }

    const std::size_t written = std::fwrite(
        serialized.data(),
        1,
        serialized.size(),
        file
    );
    bool durable = written == serialized.size() && std::fflush(file) == 0;
#ifdef _WIN32
    durable = durable && _commit(_fileno(file)) == 0;
#else
    durable = durable && ::fsync(fileno(file)) == 0;
#endif
    durable = std::fclose(file) == 0 && durable;
    if (!durable)
    {
        std::filesystem::remove(temporaryPath, filesystemError);
        return failure("unable to flush temporary world save");
    }

    Json verified;
    const Result verifiedResult = load(temporaryPath, verified);
    if (!verifiedResult)
    {
        std::filesystem::remove(temporaryPath, filesystemError);
        return failure("temporary world save failed validation: " + verifiedResult.error);
    }

    const Result replaced = replaceFile(temporaryPath, path);
    if (!replaced)
    {
        std::filesystem::remove(temporaryPath, filesystemError);
        return replaced;
    }
    return success();
}

Result load(const std::filesystem::path& path, Json& document)
{
    std::error_code filesystemError;
    const std::uintmax_t size = std::filesystem::file_size(path, filesystemError);
    if (filesystemError || size == 0 || size > MAX_SAVE_BYTES)
    {
        return failure("world save is missing, empty, or too large");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return failure("unable to open world save");
    }
    try
    {
        Json parsed = Json::parse(input, nullptr, true, true);
        const Result validation = validate(parsed);
        if (!validation)
        {
            return validation;
        }
        document = std::move(parsed);
    }
    catch (const std::exception& exception)
    {
        return failure("unable to parse world save: " + std::string(exception.what()));
    }
    return success();
}
}
