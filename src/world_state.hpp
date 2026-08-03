/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: world_state.hpp
    Desc: Runtime registry and migration boundary for divergent map instances.

-------------------------------------------------------------------------------*/

#pragma once

#include "world_instance.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct map_t;
struct list_t;
class Entity;
struct MapInstanceVisualState;

struct MapInstance
{
    WorldInstanceIdentity identity;
    map_t* loadedMap = nullptr;
    std::int32_t* tiles = nullptr;
    list_t* entities = nullptr;
    list_t* creatures = nullptr;
    list_t* worldUI = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t dungeonLevel = 0;
    std::uint32_t mapSeed = 0;
    std::uint32_t nextEntityUid = 1;
    std::uint32_t mapLoadEntityUidStart = 1;
    std::uint32_t runtimeEntityUidStart = 1;
    int* groundedPathMap = nullptr;
    int* flyingPathMap = nullptr;
    int pathMapZone = 1;
    bool* shopArea = nullptr;
    int monsterCount = 0;
    int minotaurLevel = 0;
    MapInstanceVisualState* visualState = nullptr;
    std::unordered_set<int> playersPresent;
    std::unordered_map<int, Entity*> playerEntities;
    std::uint64_t nextPersistentId = 1;
    std::uint64_t simulationTick = 0;
    bool dirty = false;
    bool simulationActive = false;
    bool runtimeInitialized = false;
    bool secretLevel = false;
    bool darkMap = false;

    std::string key() const
    {
        return identity.key();
    }
};

struct MapInstanceSummary
{
    WorldInstanceIdentity identity;
    std::vector<int> playersPresent;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t dungeonLevel = 0;
    std::uint32_t mapSeed = 0;
    std::uint32_t nextEntityUid = 1;
    bool hasGroundedPathMap = false;
    bool hasFlyingPathMap = false;
    bool hasShopArea = false;
    std::uint64_t nextPersistentId = 1;
    std::uint64_t simulationTick = 0;
    bool loaded = false;
    bool dirty = false;
    bool simulationActive = false;
    bool runtimeInitialized = false;
    bool secretLevel = false;
    bool darkMap = false;
    bool ownedStorage = false;
};

class WorldState
{
public:
    bool bindMap(
        map_t& loadedMap,
        const std::string& mapFile,
        const std::string& instanceId
    );
    bool bindLegacyMap(map_t& loadedMap, const std::string& mapFile);
    bool registerUnloadedInstance(const MapInstanceSummary& summary);
    bool releaseMap(map_t& loadedMap);
    bool unloadEmptyInstance(const std::string& canonicalKey);
    bool loadDetachedMap(
        const std::string& filePath,
        const std::string& mapFile,
        const std::string& instanceId,
        std::string& error
    );
    bool activate(const std::string& canonicalKey);
    const MapInstance* activeInstance() const;
    MapInstance* activeInstance();
    const WorldInstanceIdentity* activeIdentity() const;
    const MapInstance* find(const std::string& canonicalKey) const;
    MapInstance* find(const std::string& canonicalKey);
    const MapInstance* instanceFor(const map_t& loadedMap) const;
    MapInstance* instanceFor(map_t& loadedMap);
    map_t* mapForEntities(const list_t* entities) const;
    Entity* playerEntityFor(const std::string& canonicalKey, int playerIndex) const;
    const WorldInstanceIdentity* identityFor(const map_t& loadedMap) const;
    bool placePlayer(int playerIndex, const map_t& loadedMap);
    void removePlayer(int playerIndex);
    bool playerSharesInstance(int playerIndex, const map_t& loadedMap) const;
    bool playerSharesActiveInstance(int playerIndex) const;
    bool markRuntimeInitialized(map_t& loadedMap);
    void refreshActiveContext();
    std::size_t instanceCount() const;
    std::vector<std::string> occupiedLoadedInstanceKeys() const;
    std::vector<MapInstanceSummary> instanceSummaries() const;
    void clear();

private:
    std::unordered_map<std::string, MapInstance> instances;
    std::unordered_map<const map_t*, std::string> loadedMaps;
    std::unordered_map<std::string, std::uint64_t> revisionCounters;
    std::unordered_set<map_t*> ownedMapStorage;
    std::string activeKey;
};

extern WorldState worldState;
