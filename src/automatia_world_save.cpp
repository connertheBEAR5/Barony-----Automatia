/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: automatia_world_save.cpp
    Desc: Serialization bridge from the runtime world registry to schema v1.

-------------------------------------------------------------------------------*/

#include "automatia_world_save.hpp"

#include "world_state.hpp"

namespace AutomatiaSave
{
Json captureWorldState(const std::string& sessionId, const WorldState& world)
{
    Json document = makeEmptyWorldSave(sessionId);
    if (const WorldInstanceIdentity* active = world.activeIdentity())
    {
        document["active_instance"] = active->key();
    }

    for (const MapInstanceSummary& summary : world.instanceSummaries())
    {
        Json occupants = Json::array();
        for (const int playerIndex : summary.playersPresent)
        {
            occupants.push_back(playerIndex);
        }
        document["map_instances"].push_back(Json{
            {"map_file", summary.identity.mapFile},
            {"instance_id", summary.identity.instanceId},
            {"revision", summary.identity.revision},
            {"loaded", summary.loaded},
            {"dirty", summary.dirty},
            {"simulation_active", summary.simulationActive},
            {"simulation_tick", summary.simulationTick},
            {"width", summary.width},
            {"height", summary.height},
            {"dungeon_level", summary.dungeonLevel},
            {"map_seed", summary.mapSeed},
            {"next_entity_uid", summary.nextEntityUid},
            {"next_persistent_id", summary.nextPersistentId},
            {"secret_level", summary.secretLevel},
            {"dark_map", summary.darkMap},
            {"players_present", std::move(occupants)},
            {"persistent_state", Json::object()}
        });
    }
    return document;
}
}
