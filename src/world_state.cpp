/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: world_state.cpp
    Desc: Runtime registry and migration boundary for divergent map instances.

-------------------------------------------------------------------------------*/

#include "world_state.hpp"

#include "entity.hpp"
#include "draw.hpp"
#include "engine/audio/sound.hpp"
#include "files.hpp"
#include "game.hpp"
#include "light.hpp"
#include "main.hpp"
#include "paths.hpp"
#include "player.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <utility>

WorldState worldState;

struct MapInstanceVisualState
{
    list_t lights{};
    std::vector<vec4_t> lightmap[MAXPLAYERS + 1];
    std::vector<vec4_t> lightmapSmoothed[MAXPLAYERS + 1];
    AdditionalPlayableFloorLightmaps additionalPlayableFloorLightmaps;
};

namespace
{
bool ensureVisualState(MapInstance& instance)
{
    if (!instance.visualState)
    {
        instance.visualState = new (std::nothrow) MapInstanceVisualState;
    }
    return instance.visualState != nullptr;
}

void rebindListNodes(list_t& list)
{
    for (node_t* node = list.first; node; node = node->next)
    {
        node->list = &list;
    }
}

void swapActiveVisualState(MapInstanceVisualState& state)
{
    using std::swap;
    swap(light_l, state.lights);
    rebindListNodes(light_l);
    rebindListNodes(state.lights);
    for (int index = 0; index < MAXPLAYERS + 1; ++index)
    {
        lightmaps[index].swap(state.lightmap[index]);
        lightmapsSmoothed[index].swap(state.lightmapSmoothed[index]);
    }
    swapAdditionalPlayableFloorLightmaps(
        state.additionalPlayableFloorLightmaps);
}

void ensureActiveLightmapDimensions(const map_t& loadedMap)
{
    const std::size_t lightmapSize =
        lightmapSize3D(loadedMap.width, loadedMap.height);
    const std::size_t smoothedSize =
        lightmapSmoothedSize3D(loadedMap.width, loadedMap.height);
    for (int index = 0; index < MAXPLAYERS + 1; ++index)
    {
        if (lightmaps[index].size() != lightmapSize)
        {
            lightmaps[index].assign(lightmapSize, vec4_t{});
        }
        if (lightmapsSmoothed[index].size() != smoothedSize)
        {
            lightmapsSmoothed[index].assign(smoothedSize, vec4_t{});
        }
    }
    for (const PlayableFloorData& floor : loadedMap.playableFloors.floors)
    {
        if (floor.id == DEFAULT_PLAYABLE_FLOOR)
        {
            continue;
        }
        for (int index = 0; index < MAXPLAYERS + 1; ++index)
        {
            (void)lightmapForPlayableFloor(
                index, floor.id, loadedMap.width, loadedMap.height);
            (void)lightmapSmoothedForPlayableFloor(
                index, floor.id, loadedMap.width, loadedMap.height);
        }
    }
}

void destroyVisualState(MapInstanceVisualState* state)
{
    if (!state)
    {
        return;
    }
    // Detached lights cannot run lightDeconstructor because it subtracts from
    // whichever legacy lightmap happens to be active. Release their private
    // allocations directly before freeing the list nodes.
    for (node_t* node = state->lights.first; node; node = node->next)
    {
        light_t* light = static_cast<light_t*>(node->element);
        if (light)
        {
            std::free(light->tiles);
            light->tiles = nullptr;
            std::free(light);
            node->element = nullptr;
        }
    }
    list_FreeAll(&state->lights);
    delete state;
}

void refreshRuntimeReferences(MapInstance& instance)
{
    if (!instance.loadedMap)
    {
        instance.tiles = nullptr;
        instance.entities = nullptr;
        instance.creatures = nullptr;
        instance.worldUI = nullptr;
        instance.width = 0;
        instance.height = 0;
        instance.playableFloors.assign(1, DEFAULT_PLAYABLE_FLOOR);
        instance.verticalNavigation.clear(instance.key());
        return;
    }

    instance.tiles = instance.loadedMap->tiles;
    instance.entities = instance.loadedMap->entities;
    instance.creatures = instance.loadedMap->creatures;
    instance.worldUI = instance.loadedMap->worldUI;
    instance.width = instance.loadedMap->width;
    instance.height = instance.loadedMap->height;
    (void)rebuildVerticalNavigationGraphFromMap(
        instance.verticalNavigation, instance.key(), *instance.loadedMap);
    instance.playableFloors.clear();
    instance.playableFloors.reserve(instance.loadedMap->playableFloors.floors.size());
    for (const PlayableFloorData& floor : instance.loadedMap->playableFloors.floors)
    {
        instance.playableFloors.push_back(floor.id);
    }
    if (instance.playableFloors.empty())
    {
        instance.playableFloors.push_back(DEFAULT_PLAYABLE_FLOOR);
    }
}

void captureLegacySimulationContext(MapInstance& instance)
{
    instance.dungeonLevel = currentlevel;
    instance.mapSeed = mapseed;
    instance.nextEntityUid = entity_uids;
    instance.groundedPathMap = pathMapGrounded;
    instance.flyingPathMap = pathMapFlying;
    instance.pathMapZone = ::pathMapZone;
    instance.shopArea = shoparea;
    instance.monsterCount = nummonsters;
    instance.minotaurLevel = minotaurlevel;
    instance.secretLevel = secretlevel;
    instance.darkMap = darkmap;
}

void applyLegacySimulationContext(const MapInstance& instance)
{
    currentlevel = instance.dungeonLevel;
    mapseed = instance.mapSeed;
    entity_uids = instance.nextEntityUid;
    pathMapGrounded = instance.groundedPathMap;
    pathMapFlying = instance.flyingPathMap;
    ::pathMapZone = instance.pathMapZone;
    shoparea = instance.shopArea;
    nummonsters = instance.monsterCount;
    minotaurlevel = instance.minotaurLevel;
    secretlevel = instance.secretLevel;
    darkmap = instance.darkMap;
}

void clearEntityTileIndex(map_t& loadedMap)
{
    if (loadedMap.entities)
    {
        for (node_t* node = loadedMap.entities->first; node; node = node->next)
        {
            Entity* entity = static_cast<Entity*>(node->element);
            if (entity)
            {
                entity->myTileListNode = nullptr;
            }
        }
    }
    TileEntityList.emptyGridEntities();
}

void rebuildEntityTileIndex(map_t& loadedMap)
{
    if (!loadedMap.entities)
    {
        return;
    }
    for (node_t* node = loadedMap.entities->first; node; node = node->next)
    {
        Entity* entity = static_cast<Entity*>(node->element);
        if (!entity)
        {
            continue;
        }
        entity->myTileListNode = nullptr;
        TileEntityList.addEntity(*entity);
    }
}

void destroyOwnedMapStorage(map_t* storage)
{
    if (!storage)
    {
        return;
    }

    const bool previousLoading = loading;
    loading = true;
    if (storage->entities)
    {
        list_FreeAll(storage->entities);
        std::free(storage->entities);
        storage->entities = nullptr;
    }
    if (storage->creatures)
    {
        list_FreeAll(storage->creatures);
        delete storage->creatures;
        storage->creatures = nullptr;
    }
    if (storage->worldUI)
    {
        list_FreeAll(storage->worldUI);
        delete storage->worldUI;
        storage->worldUI = nullptr;
    }
    list_FreeAll(&entitiesdeleted);
    loading = previousLoading;

    std::free(storage->tiles);
    storage->tiles = nullptr;
    delete storage;
}

/*
 * map_t owns several raw allocations but has no safe copy/move operation.
 * Explicitly swapping every field transfers that ownership exactly once and
 * keeps the process-wide `map` object as the compatibility view used by
 * legacy simulation code.
 */
void swapLoadedMapState(map_t& first, map_t& second)
{
    using std::swap;

    swap(first.name, second.name);
    swap(first.author, second.author);
    swap(first.width, second.width);
    swap(first.height, second.height);
    swap(first.skybox, second.skybox);
    swap(first.numLayers, second.numLayers);
    swap(first.flags, second.flags);
    swap(first.tiles, second.tiles);
    swap(first.entities_map, second.entities_map);
    swap(first.entities, second.entities);
    swap(first.creatures, second.creatures);
    swap(first.worldUI, second.worldUI);
    swap(first.trapexcludelocations, second.trapexcludelocations);
    swap(first.monsterexcludelocations, second.monsterexcludelocations);
    swap(first.lootexcludelocations, second.lootexcludelocations);
    swap(first.liquidSfxPlayedTiles, second.liquidSfxPlayedTiles);
    swap(first.tileAttributes, second.tileAttributes);
	swap(first.playableFloors, second.playableFloors);
	swap(first.ambience, second.ambience);
	swap(first.ambientLight, second.ambientLight);
	swap(first.filename, second.filename);
}
}

bool WorldState::bindMap(
    map_t& loadedMap,
    const std::string& mapFile,
    const std::string& instanceId
)
{
    WorldInstanceIdentity identity;
    if (!identity.set(mapFile, instanceId))
    {
        printlog(
            "[World State] Refusing invalid map identity '%s#%s'.",
            mapFile.c_str(),
            instanceId.c_str()
        );
        return false;
    }

    const std::string key = identity.key();
    const auto keyCollision = instances.find(key);
    if (keyCollision != instances.end()
        && keyCollision->second.loadedMap
        && keyCollision->second.loadedMap != &loadedMap)
    {
        printlog("[World State] Refusing duplicate loaded instance '%s'.", key.c_str());
        return false;
    }
    identity.revision = ++revisionCounters[key];

    std::unordered_set<int> occupants;
    const auto previous = loadedMaps.find(&loadedMap);
    if (previous != loadedMaps.end())
    {
        const auto oldInstance = instances.find(previous->second);
        if (oldInstance != instances.end())
        {
            /*
             * Rebinding the same map object to a more specific identity (for
             * example mine.lmp#world -> mine.lmp#level_1_regular) is an
             * identity refinement, not a player departure. Preserve all
             * occupants across that rebind.
             */
            occupants = oldInstance->second.playersPresent;
            oldInstance->second.loadedMap = nullptr;
            oldInstance->second.tiles = nullptr;
            oldInstance->second.entities = nullptr;
            oldInstance->second.creatures = nullptr;
            oldInstance->second.worldUI = nullptr;
            oldInstance->second.width = 0;
            oldInstance->second.height = 0;
			/*
			 * Rebinding the same legacy map object transfers these global
			 * allocations to the new identity. Normal level loading replaces
			 * or frees them after loadMap(), so the retired summary must not
			 * retain aliases that clear() could later free a second time.
			 */
			oldInstance->second.groundedPathMap = nullptr;
			oldInstance->second.flyingPathMap = nullptr;
			oldInstance->second.shopArea = nullptr;
            oldInstance->second.simulationActive = false;
            oldInstance->second.playersPresent.clear();
            oldInstance->second.playerEntities.clear();
        }
    }

    MapInstance& instance = instances[key];
    if (!ensureVisualState(instance))
    {
        printlog("[World State] Unable to allocate visual state for '%s'.", key.c_str());
        return false;
    }
    instance.identity = identity;
    instance.loadedMap = &loadedMap;
    refreshRuntimeReferences(instance);
    instance.runtimeInitialized = false;
    instance.playersPresent = std::move(occupants);
    instance.simulationActive = !instance.playersPresent.empty();
    loadedMaps[&loadedMap] = key;
    if (&loadedMap == &map)
    {
        captureLegacySimulationContext(instance);
        activeKey = key;
    }

    for (const int playerIndex : instance.playersPresent)
    {
        if (playerIndex >= 0 && playerIndex < MAXPLAYERS && players[playerIndex])
        {
            players[playerIndex]->worldInstance = identity;
            instance.playerEntities[playerIndex] =
                players[playerIndex]->entity;
        }
    }

    return true;
}

bool WorldState::bindLegacyMap(map_t& loadedMap, const std::string& mapFile)
{
    const std::string instanceId =
        automatiaInfiniteDungeonInstanceId("world");
    if (!bindMap(loadedMap, mapFile, instanceId))
    {
        return false;
    }

    MapInstance* instance = find(
        WorldInstanceIdentity::canonicalMapFile(mapFile)
            + "#"
            + instanceId
    );
    if (!instance)
    {
        return false;
    }
	/*
	 * loadMap() binds the legacy foreground map after all authored entities
	 * have consumed their UIDs but before assignActions() creates runtime
	 * entities. Preserve both phases so a later independent arrival can rebuild
	 * the same fixture identities as the original occupants.
	 */
	instance->mapLoadEntityUidStart = lastEntityUIDs;
	instance->runtimeEntityUidStart = entity_uids;

    // Compatibility stage: legacy transitions still move the connected party
    // together. A detached generated-floor reload is only reconstructing an
    // inactive destination for one divergent player, so it must not claim or
    // retag every connected player while the generator temporarily uses the
    // process-wide map object.
    if (!detachedGeneratedLoadInProgress)
    {
        for (int playerIndex = 0; playerIndex < MAXPLAYERS; ++playerIndex)
        {
            if (!players[playerIndex] || client_disconnected[playerIndex]
				|| (headless && multiplayer == SERVER && playerIndex == 0)
				|| (multiplayer == CLIENT
					&& !players[playerIndex]->isLocalPlayer()))
            {
                continue;
            }
            players[playerIndex]->worldInstance = instance->identity;
            instance->playersPresent.insert(playerIndex);
            instance->playerEntities[playerIndex] =
                players[playerIndex]->entity;
        }
    }
    instance->simulationActive = !instance->playersPresent.empty();
    activeKey = instance->key();

    printlog(
        "[World State] Bound %s revision %llu with %zu player(s).",
        instance->key().c_str(),
        static_cast<unsigned long long>(instance->identity.revision),
        instance->playersPresent.size()
    );
    return true;
}

bool WorldState::registerUnloadedInstance(const MapInstanceSummary& summary)
{
    if (!summary.identity.isValid() || instances.count(summary.identity.key()) != 0)
    {
        return false;
    }
    MapInstance instance;
    instance.identity = summary.identity;
    instance.dungeonLevel = summary.dungeonLevel;
    instance.mapSeed = summary.mapSeed;
    instance.playableFloors = summary.playableFloors.empty()
        ? std::vector<PlayableFloorId>{DEFAULT_PLAYABLE_FLOOR}
        : summary.playableFloors;
    instance.nextEntityUid = std::max<std::uint32_t>(1, summary.nextEntityUid);
    instance.nextPersistentId = std::max<std::uint64_t>(1, summary.nextPersistentId);
    instance.simulationTick = summary.simulationTick;
    instance.dirty = summary.dirty;
    instance.secretLevel = summary.secretLevel;
    instance.darkMap = summary.darkMap;
    instances.emplace(instance.key(), std::move(instance));
    revisionCounters[summary.identity.key()] = summary.identity.revision;
    return true;
}

bool WorldState::releaseMap(map_t& loadedMap)
{
    const auto loaded = loadedMaps.find(&loadedMap);
    if (loaded == loadedMaps.end())
    {
        return false;
    }
    MapInstance* instance = find(loaded->second);
    if (!instance
        || !instance->playersPresent.empty()
        || instance->key() == activeKey)
    {
        return false;
    }

    instance->loadedMap = nullptr;
    instance->tiles = nullptr;
    instance->entities = nullptr;
    instance->creatures = nullptr;
    instance->worldUI = nullptr;
    instance->width = 0;
    instance->height = 0;
    instance->simulationActive = false;
    loadedMaps.erase(loaded);
    return true;
}

bool WorldState::unloadEmptyInstance(const std::string& canonicalKey)
{
    MapInstance* instance = find(canonicalKey);
    if (!instance
        || !instance->loadedMap
        || !instance->playersPresent.empty()
        || canonicalKey == activeKey
        || instance->loadedMap == &map
        || ownedMapStorage.count(instance->loadedMap) == 0)
    {
        return false;
    }

    map_t* storage = instance->loadedMap;
    loadedMaps.erase(storage);
    ownedMapStorage.erase(storage);
    if (instance->groundedPathMap)
    {
        std::free(instance->groundedPathMap);
    }
    if (instance->flyingPathMap
        && instance->flyingPathMap != instance->groundedPathMap)
    {
        std::free(instance->flyingPathMap);
    }
    instance->groundedPathMap = nullptr;
    instance->flyingPathMap = nullptr;
    std::free(instance->shopArea);
    instance->shopArea = nullptr;
    destroyVisualState(instance->visualState);
    instance->visualState = nullptr;
    destroyOwnedMapStorage(storage);
    instance->loadedMap = nullptr;
    refreshRuntimeReferences(*instance);
    instance->playerEntities.clear();
    instance->runtimeInitialized = false;
    instance->simulationActive = false;
    return true;
}

bool WorldState::loadDetachedMap(
    const std::string& filePath,
    const std::string& mapFile,
    const std::string& instanceId,
    std::string& error
)
{
    error.clear();
    WorldInstanceIdentity identity;
    if (!identity.set(mapFile, instanceId))
    {
        error = "invalid map-instance identity";
        return false;
    }
    const std::string sourceName =
        std::filesystem::path(filePath).filename().string();
    if (WorldInstanceIdentity::canonicalMapFile(sourceName) != identity.mapFile)
    {
        error = "map path does not match the requested map identity";
        return false;
    }
    MapInstance* existingInstance = find(identity.key());
    const bool restoringUnloadedInstance =
        existingInstance && !existingInstance->loadedMap;
    if ((existingInstance && !restoringUnloadedInstance) || entitiesdeleted.first)
    {
        error = entitiesdeleted.first
            ? "entity deletion is pending"
            : "map instance is already registered";
        return false;
    }

    map_t* storage = new (std::nothrow) map_t;
    if (!storage)
    {
        error = "unable to allocate map storage";
        return false;
    }
    storage->entities = static_cast<list_t*>(std::calloc(1, sizeof(list_t)));
    storage->creatures = new (std::nothrow) list_t{};
    storage->worldUI = new (std::nothrow) list_t{};
    if (!storage->entities || !storage->creatures || !storage->worldUI)
    {
        destroyOwnedMapStorage(storage);
        error = "unable to allocate map lists";
        return false;
    }
    if (restoringUnloadedInstance)
    {
        if (!ensureVisualState(*existingInstance))
        {
            destroyOwnedMapStorage(storage);
            error = "unable to allocate restored visual state";
            return false;
        }
        existingInstance->loadedMap = storage;
        existingInstance->runtimeInitialized = false;
        refreshRuntimeReferences(*existingInstance);
        loadedMaps[storage] = identity.key();
    }
    else if (!bindMap(*storage, identity.mapFile, identity.instanceId))
    {
        destroyOwnedMapStorage(storage);
        error = "unable to register map storage";
        return false;
    }

    MapInstance* loadingInstance = find(identity.key());
    if (!loadingInstance)
    {
        loadedMaps.erase(storage);
        if (!restoringUnloadedInstance)
        {
            instances.erase(identity.key());
        }
        destroyOwnedMapStorage(storage);
        error = "registered map instance disappeared before loading";
        return false;
    }
    loadingInstance->mapLoadEntityUidStart = loadingInstance->nextEntityUid;

    const bool previousLoading = loading;
    loading = true;
    const int result = loadMap(
        filePath.c_str(),
        storage,
        storage->entities,
        storage->creatures
    );
    loading = previousLoading;
    if (result == -1)
    {
        loadedMaps.erase(storage);
        MapInstance* failedInstance = find(identity.key());
        if (failedInstance)
        {
            destroyVisualState(failedInstance->visualState);
            failedInstance->visualState = nullptr;
        }
        if (restoringUnloadedInstance && failedInstance)
        {
            failedInstance->loadedMap = nullptr;
            refreshRuntimeReferences(*failedInstance);
        }
        else
        {
            instances.erase(identity.key());
        }
        destroyOwnedMapStorage(storage);
        error = "map file failed validation or loading";
        return false;
    }

    MapInstance* instance = find(identity.key());
    if (!instance)
    {
        loadedMaps.erase(storage);
        destroyOwnedMapStorage(storage);
        error = "registered map instance disappeared during loading";
        return false;
    }
    instance->runtimeEntityUidStart = instance->nextEntityUid;
    refreshRuntimeReferences(*instance);
    ownedMapStorage.insert(storage);
    return true;
}

bool WorldState::loadDetachedGeneratedLevel(
    const WorldInstanceIdentity& identity,
    const std::int32_t dungeonLevel,
    const std::uint32_t seed,
    const bool secretTrack,
    std::string& error
)
{
    error.clear();
    if (!identity.isValid())
    {
        error = "invalid generated map-instance identity";
        return false;
    }
    const std::string expectedInstanceId =
        "level_" + std::to_string(std::max<std::int32_t>(0, dungeonLevel))
        + (secretTrack ? "_secret" : "_regular");
    if (identity.instanceId != expectedInstanceId)
    {
        error = "generated instance ID does not match its level and track";
        return false;
    }

    MapInstance* existingDestination = find(identity.key());
    if (existingDestination && existingDestination->loadedMap)
    {
        if (existingDestination->dungeonLevel != dungeonLevel
            || existingDestination->secretLevel != secretTrack
            || existingDestination->mapSeed != seed)
        {
            error = "loaded generated instance metadata does not match the requested route history";
            return false;
        }
        return true;
    }
    if (entitiesdeleted.first)
    {
        error = "entity deletion is pending";
        return false;
    }

    MapInstance* source = activeInstance();
    if (!source || source->loadedMap != &map)
    {
        error = "no foreground map instance is available for detached generation";
        return false;
    }
    const std::string sourceKey = source->key();
    if (sourceKey == identity.key())
    {
        error = "generated destination is already the foreground instance";
        return false;
    }
    if (!ensureVisualState(*source))
    {
        error = "unable to preserve foreground visual state";
        return false;
    }

    /*
     * Register a blank detached map before touching the foreground source.
     * Once registered, the ownership swap below gives this allocation the
     * source map while the process-wide map becomes the generator workspace.
     * This is the activation core without path-map/chunk work on a 0x0 map.
     */
    map_t* storage = new (std::nothrow) map_t{};
    if (!storage)
    {
        error = "unable to allocate generated map storage";
        return false;
    }
    storage->entities = static_cast<list_t*>(std::calloc(1, sizeof(list_t)));
    storage->creatures = new (std::nothrow) list_t{};
    storage->worldUI = new (std::nothrow) list_t{};
    if (!storage->entities || !storage->creatures || !storage->worldUI)
    {
        destroyOwnedMapStorage(storage);
        error = "unable to allocate generated map lists";
        return false;
    }

    const bool restoringUnloadedDestination =
        existingDestination && !existingDestination->loadedMap;
    if (restoringUnloadedDestination)
    {
        if (!ensureVisualState(*existingDestination))
        {
            destroyOwnedMapStorage(storage);
            error = "unable to allocate restored generated visual state";
            return false;
        }
        existingDestination->loadedMap = storage;
        existingDestination->runtimeInitialized = false;
        refreshRuntimeReferences(*existingDestination);
        loadedMaps[storage] = identity.key();
    }
    else if (!bindMap(*storage, identity.mapFile, identity.instanceId))
    {
        destroyOwnedMapStorage(storage);
        error = "unable to register generated map storage";
        return false;
    }

    MapInstance* destination = find(identity.key());
    if (!destination || !destination->loadedMap
        || !ensureVisualState(*destination))
    {
        loadedMaps.erase(storage);
        if (restoringUnloadedDestination && existingDestination)
        {
            existingDestination->loadedMap = nullptr;
            refreshRuntimeReferences(*existingDestination);
        }
        else
        {
            instances.erase(identity.key());
        }
        destroyOwnedMapStorage(storage);
        error = "registered generated destination is unavailable";
        return false;
    }
    ownedMapStorage.insert(storage);

    captureLegacySimulationContext(*source);
    for (const int playerIndex : source->playersPresent)
    {
        if (playerIndex >= 0 && playerIndex < MAXPLAYERS && players[playerIndex])
        {
            source->playerEntities[playerIndex] = players[playerIndex]->entity;
        }
    }
    clearEntityTileIndex(map);
    swapActiveVisualState(*source->visualState);
    swapLoadedMapState(map, *storage);
    swapActiveVisualState(*destination->visualState);

    loadedMaps.erase(&map);
    loadedMaps.erase(storage);
    source->loadedMap = storage;
    refreshRuntimeReferences(*source);
    loadedMaps[storage] = sourceKey;
    destination->loadedMap = &map;
    refreshRuntimeReferences(*destination);
    loadedMaps[&map] = identity.key();
    activeKey = identity.key();

    for (int playerIndex = 0; playerIndex < MAXPLAYERS; ++playerIndex)
    {
        if (!players[playerIndex])
        {
            continue;
        }
        const auto destinationPlayer =
            destination->playerEntities.find(playerIndex);
        players[playerIndex]->entity =
            destinationPlayer == destination->playerEntities.end()
                ? nullptr
                : destinationPlayer->second;
    }

    currentlevel = dungeonLevel;
    secretlevel = secretTrack;
    mapseed = seed;
    darkmap = false;
    pathMapGrounded = nullptr;
    pathMapFlying = nullptr;
    ::pathMapZone = 1;
    shoparea = nullptr;
    nummonsters = 0;
    minotaurlevel = 0;
    entity_uids = std::max<std::uint32_t>(1, destination->nextEntityUid);
    lastEntityUIDs = entity_uids;
    const std::uint32_t mapLoadUidStart = entity_uids;
    destination->dungeonLevel = dungeonLevel;
    destination->mapSeed = seed;
    destination->secretLevel = secretTrack;
    destination->darkMap = false;
    destination->mapLoadEntityUidStart = mapLoadUidStart;

    const std::string previousCustomMap = loadCustomNextMap;
    const bool previousLoading = loading;
    const bool previousDetachedGeneratedLoad =
        detachedGeneratedLoadInProgress;
    loadCustomNextMap.clear();
    loading = true;
    detachedGeneratedLoadInProgress = true;
    int checkMapHash = -1;
    const int result = physfsLoadMapFile(
        dungeonLevel,
        seed,
        false,
        &checkMapHash
    );
    detachedGeneratedLoadInProgress = previousDetachedGeneratedLoad;
    loading = previousLoading;
    loadCustomNextMap = previousCustomMap;

    const std::string loadedKey =
        activeIdentity() ? activeIdentity()->key() : std::string{};
    const bool loadedExpectedDestination =
        result != -1 && loadedKey == identity.key();

    destination = find(identity.key());
    if (loadedExpectedDestination && destination)
    {
        destination->dungeonLevel = dungeonLevel;
        destination->mapSeed = seed;
        destination->secretLevel = secretTrack;
        destination->darkMap = darkmap;
        destination->mapLoadEntityUidStart = mapLoadUidStart;
        destination->runtimeEntityUidStart = entity_uids;
        destination->nextEntityUid = entity_uids;
        destination->runtimeInitialized = false;
    }

    /*
     * Always restore the source before reporting success or failure. The map
     * generated above then remains in the owned detached storage and can be
     * activated for only the player who requested the reverse transition.
     */
    const std::string generatedForegroundKey = loadedKey;
    if (!activate(sourceKey))
    {
        error = "foreground instance could not be restored after generation";
        return false;
    }

    if (!loadedExpectedDestination || !destination)
    {
        if (!generatedForegroundKey.empty()
            && generatedForegroundKey != sourceKey)
        {
            unloadEmptyInstance(generatedForegroundKey);
        }
        if (!restoringUnloadedDestination)
        {
            MapInstance* failedRequested = find(identity.key());
            if (failedRequested && !failedRequested->loadedMap
                && failedRequested->playersPresent.empty())
            {
                instances.erase(identity.key());
            }
        }
        error = result == -1
            ? "generated destination failed to load"
            : "generated destination resolved to an unexpected map instance";
        return false;
    }

    printlog(
        "[World State] Regenerated detached instance '%s' at level %d (%s track, seed %u).",
        identity.key().c_str(),
        dungeonLevel,
        secretTrack ? "secret" : "regular",
        seed
    );
    return true;
}

bool WorldState::activate(const std::string& canonicalKey)
{
    MapInstance* destination = find(canonicalKey);
    if (!destination || !destination->loadedMap)
    {
        return false;
    }

    MapInstance* current = activeInstance();
    if (current == destination)
    {
        return destination->loadedMap == &map;
    }

    /*
     * Never change only the identity. Packet isolation treats activeKey as
     * authoritative, so it must always describe the data actually exposed
     * through the legacy global map object.
     */
    if (!current
        || current->loadedMap != &map
        || destination->loadedMap == &map)
    {
        return false;
    }
    if (entitiesdeleted.first)
    {
        printlog(
            "[World State] Refusing to activate '%s' while entity deletion is pending.",
            canonicalKey.c_str()
        );
        return false;
    }

    map_t* destinationStorage = destination->loadedMap;
    if (!ensureVisualState(*current) || !ensureVisualState(*destination))
    {
        return false;
    }
    captureLegacySimulationContext(*current);
    for (const int playerIndex : current->playersPresent)
    {
        if (playerIndex >= 0 && playerIndex < MAXPLAYERS && players[playerIndex])
        {
            current->playerEntities[playerIndex] =
                players[playerIndex]->entity;
        }
    }
    clearEntityTileIndex(map);
    swapActiveVisualState(*current->visualState);
    swapLoadedMapState(map, *destinationStorage);
    swapActiveVisualState(*destination->visualState);

    loadedMaps.erase(&map);
    loadedMaps.erase(destinationStorage);

    current->loadedMap = destinationStorage;
    refreshRuntimeReferences(*current);
    loadedMaps[current->loadedMap] = current->key();

    destination->loadedMap = &map;
    refreshRuntimeReferences(*destination);
    loadedMaps[destination->loadedMap] = destination->key();
    activeKey = canonicalKey;
    if (!destination->shopArea
        && destination->width > 0
        && destination->height > 0)
    {
        destination->shopArea = static_cast<bool*>(std::calloc(
            static_cast<std::size_t>(destination->width)
                * destination->height,
            sizeof(bool)
        ));
        if (!destination->shopArea)
        {
            printlog(
                "[World State] Warning: unable to allocate shop mask for '%s'.",
                canonicalKey.c_str()
            );
        }
    }
    applyLegacySimulationContext(*destination);
    for (int playerIndex = 0; playerIndex < MAXPLAYERS; ++playerIndex)
    {
        if (!players[playerIndex])
        {
            continue;
        }
        const auto entity = destination->playerEntities.find(playerIndex);
        players[playerIndex]->entity =
            entity == destination->playerEntities.end()
            ? nullptr
            : entity->second;
    }
    ensureActiveLightmapDimensions(map);
	if (!destination->runtimeInitialized)
	{
		initializeMapAmbientLightmap(map);
	}
	// A MapInstance switch is the only runtime event that replaces map ambience.
	// Playable-Z floor changes keep this identity and therefore never restart it.
	syncMapAmbience(map, destination->key());

	/*
	 * activate() swaps map storage directly and therefore bypasses loadMap()'s
	 * camera-vismap allocation and the normal post-load chunk rebuild. Keeping
	 * buffers or chunks from the previous floor is unsafe even when two
	 * procedural maps have identical dimensions: occlusionCulling() writes
	 * width * height entries and the chunk meshes still contain the previous
	 * floor's geometry.
	 */
	resetMapVisibilityState(map);
	if ( !headless )
	{
		clearChunks();
		createChunks();
	}

    rebuildEntityTileIndex(map);
    if (!pathMapGrounded || !pathMapFlying)
    {
        generatePathMaps();
        destination->groundedPathMap = pathMapGrounded;
        destination->flyingPathMap = pathMapFlying;
        destination->pathMapZone = ::pathMapZone;
    }
    return true;
}

const MapInstance* WorldState::activeInstance() const
{
    return activeKey.empty() ? nullptr : find(activeKey);
}

MapInstance* WorldState::activeInstance()
{
    return activeKey.empty() ? nullptr : find(activeKey);
}

const WorldInstanceIdentity* WorldState::activeIdentity() const
{
    const MapInstance* instance = activeInstance();
    return instance ? &instance->identity : nullptr;
}

const MapInstance* WorldState::find(const std::string& canonicalKey) const
{
    const auto found = instances.find(canonicalKey);
    return found == instances.end() ? nullptr : &found->second;
}

MapInstance* WorldState::find(const std::string& canonicalKey)
{
    const auto found = instances.find(canonicalKey);
    return found == instances.end() ? nullptr : &found->second;
}

const MapInstance* WorldState::instanceFor(const map_t& loadedMap) const
{
    const auto key = loadedMaps.find(&loadedMap);
    return key == loadedMaps.end() ? nullptr : find(key->second);
}

MapInstance* WorldState::instanceFor(map_t& loadedMap)
{
    const auto key = loadedMaps.find(&loadedMap);
    return key == loadedMaps.end() ? nullptr : find(key->second);
}

map_t* WorldState::mapForEntities(const list_t* entities) const
{
    if (!entities)
    {
        return nullptr;
    }
    for (const auto& entry : instances)
    {
        const MapInstance& instance = entry.second;
        if (instance.loadedMap && instance.loadedMap->entities == entities)
        {
            return instance.loadedMap;
        }
    }
    return nullptr;
}

Entity* WorldState::playerEntityFor(
    const std::string& canonicalKey,
    int playerIndex
) const
{
    const MapInstance* instance = find(canonicalKey);
    if (!instance)
    {
        return nullptr;
    }
    const auto found = instance->playerEntities.find(playerIndex);
    return found == instance->playerEntities.end() ? nullptr : found->second;
}

const WorldInstanceIdentity* WorldState::identityFor(const map_t& loadedMap) const
{
    const MapInstance* instance = instanceFor(loadedMap);
    return instance ? &instance->identity : nullptr;
}

bool WorldState::placePlayer(int playerIndex, const map_t& loadedMap)
{
    if (playerIndex < 0 || playerIndex >= MAXPLAYERS || !players[playerIndex])
    {
        return false;
    }
    MapInstance* destination = nullptr;
    const auto loaded = loadedMaps.find(&loadedMap);
    if (loaded != loadedMaps.end())
    {
        destination = find(loaded->second);
    }
    if (!destination)
    {
        return false;
    }
    if (destination != activeInstance() || &loadedMap != &map)
    {
        return false;
    }

    Entity* playerEntity = players[playerIndex]->entity;
    removePlayer(playerIndex);
    players[playerIndex]->worldInstance = destination->identity;
    destination->playersPresent.insert(playerIndex);
    destination->playerEntities[playerIndex] = playerEntity;
    destination->simulationActive = true;
    return true;
}

void WorldState::removePlayer(int playerIndex)
{
    if (playerIndex < 0 || playerIndex >= MAXPLAYERS)
    {
        return;
    }
    for (auto& instance : instances)
    {
        instance.second.playersPresent.erase(playerIndex);
        instance.second.playerEntities.erase(playerIndex);
        if (instance.second.playersPresent.empty())
        {
            instance.second.simulationActive = false;
        }
    }
}

std::size_t WorldState::instanceCount() const
{
    return instances.size();
}

std::vector<std::string> WorldState::occupiedLoadedInstanceKeys() const
{
    std::vector<std::string> keys;
    keys.reserve(instances.size());
    for (const auto& entry : instances)
    {
        const MapInstance& instance = entry.second;
        if (instance.loadedMap && !instance.playersPresent.empty())
        {
            keys.push_back(entry.first);
        }
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

std::vector<MapInstanceSummary> WorldState::instanceSummaries() const
{
    std::vector<MapInstanceSummary> summaries;
    summaries.reserve(instances.size());
    for (const auto& entry : instances)
    {
        const MapInstance& instance = entry.second;
        MapInstanceSummary summary;
        summary.identity = instance.identity;
        summary.playersPresent.assign(
            instance.playersPresent.begin(),
            instance.playersPresent.end()
        );
        std::sort(summary.playersPresent.begin(), summary.playersPresent.end());
        summary.width = instance.width;
        summary.height = instance.height;
        summary.playableFloors = instance.playableFloors;
        summary.dungeonLevel = instance.dungeonLevel;
        summary.mapSeed = instance.mapSeed;
        summary.nextEntityUid = instance.nextEntityUid;
        summary.hasGroundedPathMap = instance.groundedPathMap != nullptr;
        summary.hasFlyingPathMap = instance.flyingPathMap != nullptr;
        summary.hasShopArea = instance.shopArea != nullptr;
        summary.nextPersistentId = instance.nextPersistentId;
        summary.simulationTick = instance.simulationTick;
        summary.loaded = instance.loadedMap != nullptr;
        summary.dirty = instance.dirty;
        summary.simulationActive = instance.simulationActive;
        summary.runtimeInitialized = instance.runtimeInitialized;
        summary.secretLevel = instance.secretLevel;
        summary.darkMap = instance.darkMap;
        summary.ownedStorage =
            instance.loadedMap
            && ownedMapStorage.count(instance.loadedMap) != 0;
        summaries.push_back(std::move(summary));
    }
    std::sort(
        summaries.begin(),
        summaries.end(),
        [](const MapInstanceSummary& first, const MapInstanceSummary& second)
        {
            return first.identity.key() < second.identity.key();
        }
    );
    return summaries;
}

bool WorldState::playerSharesInstance(int playerIndex, const map_t& loadedMap) const
{
    if (playerIndex < 0 || playerIndex >= MAXPLAYERS || !players[playerIndex])
    {
        return false;
    }
    const WorldInstanceIdentity* mapIdentity = identityFor(loadedMap);
    return mapIdentity && players[playerIndex]->worldInstance.matches(*mapIdentity);
}

bool WorldState::playerSharesActiveInstance(int playerIndex) const
{
    if (playerIndex < 0 || playerIndex >= MAXPLAYERS || !players[playerIndex])
    {
        return false;
    }
    const WorldInstanceIdentity* identity = activeIdentity();
    return identity && players[playerIndex]->worldInstance.matches(*identity);
}

bool WorldState::markRuntimeInitialized(map_t& loadedMap)
{
    MapInstance* instance = instanceFor(loadedMap);
    if (!instance)
    {
        return false;
    }
    instance->runtimeInitialized = true;
    if (instance == activeInstance() && &loadedMap == &map)
    {
        for (const int playerIndex : instance->playersPresent)
        {
            if (playerIndex >= 0
                && playerIndex < MAXPLAYERS
                && players[playerIndex])
            {
                instance->playerEntities[playerIndex] =
                    players[playerIndex]->entity;
            }
        }
    }
    return true;
}

bool WorldState::rebuildVerticalNavigation(map_t& loadedMap)
{
    MapInstance* instance = instanceFor(loadedMap);
    if (!instance || instance->loadedMap != &loadedMap)
    {
        return false;
    }
    const bool rebuilt = rebuildVerticalNavigationGraphFromMap(
        instance->verticalNavigation, instance->key(), loadedMap);
    instance->playableFloors.clear();
    instance->playableFloors.reserve(loadedMap.playableFloors.floors.size());
    for (const PlayableFloorData& floor : loadedMap.playableFloors.floors)
    {
        instance->playableFloors.push_back(floor.id);
    }
    if (instance->playableFloors.empty())
    {
        instance->playableFloors.push_back(DEFAULT_PLAYABLE_FLOOR);
    }
    return rebuilt;
}

void WorldState::refreshActiveContext()
{
    MapInstance* instance = activeInstance();
    if (!instance || instance->loadedMap != &map)
    {
        return;
    }
    refreshRuntimeReferences(*instance);
    captureLegacySimulationContext(*instance);
}

AutomatiaParty::PartyManager& WorldState::partyManager()
{
    return persistentPartyManager;
}

const AutomatiaParty::PartyManager& WorldState::partyManager() const
{
    return persistentPartyManager;
}

void WorldState::clear()
{
    /*
     * generatePathMaps() may replace and free the process-wide path maps after
     * a map has already been bound. Keep the foreground instance synchronized
     * before deciding which raw simulation allocations WorldState owns. Without
     * this refresh, the active instance can retain the address of an old path
     * map that generatePathMaps() has already freed, and clear() will free that
     * stale address a second time while returning to the main menu or loading a
     * save.
     */
    refreshActiveContext();

    std::unordered_set<int*> freedPathMaps;
    std::unordered_set<bool*> freedShopAreas;
    for (auto& entry : instances)
    {
        MapInstance& instance = entry.second;

        /*
         * WorldState owns simulation-side raw allocations only while their map
         * is stored in one of our detached map_t objects. The foreground map's
         * allocations remain owned by the legacy engine globals, and retired or
         * unloaded summaries may contain non-owning historical aliases. Never
         * free those aliases here.
         */
        const bool ownsDetachedSimulationStorage =
            instance.loadedMap
            && instance.loadedMap != &map
            && ownedMapStorage.count(instance.loadedMap) != 0;

        if (ownsDetachedSimulationStorage)
        {
            int* pathMaps[2] = {
                instance.groundedPathMap,
                instance.flyingPathMap
            };
            for (int* pathMap : pathMaps)
            {
                if (pathMap
                    && pathMap != pathMapGrounded
                    && pathMap != pathMapFlying
                    && freedPathMaps.insert(pathMap).second)
                {
                    std::free(pathMap);
                }
            }

            if (instance.shopArea
                && instance.shopArea != shoparea
                && freedShopAreas.insert(instance.shopArea).second)
            {
                std::free(instance.shopArea);
            }
        }

        instance.groundedPathMap = nullptr;
        instance.flyingPathMap = nullptr;
        instance.shopArea = nullptr;
        destroyVisualState(instance.visualState);
        instance.visualState = nullptr;
    }
    for (map_t* storage : ownedMapStorage)
    {
        destroyOwnedMapStorage(storage);
    }
    ownedMapStorage.clear();
    instances.clear();
    loadedMaps.clear();
    revisionCounters.clear();
    persistentPartyManager.clear();
    activeKey.clear();
    detachedGeneratedLoadInProgress = false;
}
