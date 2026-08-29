/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: hostile_vertical_navigation.cpp
    Desc: Z4D hostile consumption of generic cross-floor routes.

-------------------------------------------------------------------------------*/

#include "hostile_vertical_navigation.hpp"

#include "cross_floor_path.hpp"
#include "entity.hpp"
#include "game.hpp"
#include "main.hpp"
#include "monster.hpp"
#include "net.hpp"
#include "paths.hpp"
#include "player.hpp"
#include "stat.hpp"
#include "world_state.hpp"

#include <cmath>

namespace
{
void stopForVerticalRoute(Entity& monster)
{
    const bool stateChanged = monster.monsterState != MONSTER_STATE_WAIT;
    monster.monsterTarget = 0;
    monster.monsterState = MONSTER_STATE_WAIT;
    monster.vel_x = 0.0;
    monster.vel_y = 0.0;
    if (stateChanged)
    {
        serverUpdateEntitySkill(&monster, 0);
    }
}

void abandonVerticalRoute(Entity& monster)
{
    const bool wasPursuing = monster.hostileVerticalNavigationActive
        || monster.hostileVerticalNavigationTarget != 0
        || monster.monsterTarget != 0;
    monster.hostileVerticalNavigationActive = false;
    monster.hostileVerticalNavigationTarget = 0;
    monster.monsterTarget = 0;
    if (wasPursuing)
    {
        stopForVerticalRoute(monster);
    }
}

bool monsterHasActiveLocalPath(const Entity& monster)
{
    if (monster.monsterState != MONSTER_STATE_HUNT
        || !monster.children.first
        || !monster.children.first->element)
    {
        return false;
    }
    const list_t* path = static_cast<const list_t*>(
        monster.children.first->element);
    return path->first != nullptr;
}

const VerticalNavigationEdge* firstTransition(
    const CrossFloorPathRoute& route,
    const PlayableFloorId sourceFloor)
{
    for (const CrossFloorPathStep& step : route.steps)
    {
        if (step.kind == CrossFloorPathStepKind::VerticalTransition
            && step.transition.source.playableFloor == sourceFloor)
        {
            return &step.transition;
        }
    }
    return nullptr;
}

bool isStationaryAuthoredNpc(const Entity& monster, const Stat& stats)
{
    return stats.MISC_FLAGS[STAT_FLAG_NPC] != 0
        || stats.customDialogueID[0] != '\0'
        || monster.monsterCanTradeWith(-1);
}

bool targetSharesActiveMapInstance(
    const Entity& target,
    const MapInstance& activeInstance)
{
    if (target.behavior != &actPlayer)
    {
        return false;
    }
    const int player = target.skill[2];
    return player >= 0
        && player < MAXPLAYERS
        && !client_disconnected[player]
        && players[player]
        && worldState.playerSharesActiveInstance(player)
        && worldState.playerEntityFor(activeInstance.key(), player)
            == &target;
}

bool hostileTargetIsEligible(
    Entity& monster,
    Entity& target,
    const MapInstance& activeInstance)
{
    Stat* stats = monster.getStats();
    return stats
        && stats->HP > 0
        && monster.behavior == &actMonster
        && monster.monsterAllyIndex < 0
        && !isStationaryAuthoredNpc(monster, *stats)
        && !monster.isInertMimic()
        && !monsterIsImmobileTurret(&monster, stats)
        && target.monsterIsTargetable()
        && monster.checkEnemy(&target)
        && targetSharesActiveMapInstance(target, activeInstance)
        && map.playableFloors.hasFloor(monster.playableFloor)
        && map.playableFloors.hasFloor(target.playableFloor);
}
}

AutomatiaHostileVerticalNavigationStatus
updateAutomatiaHostileVerticalNavigation(
    Entity& monster,
    const bool refreshRoute)
{
    if (multiplayer == CLIENT
        || monster.behavior != &actMonster
        || monster.monsterAllyIndex >= 0)
    {
        monster.hostileVerticalNavigationActive = false;
        monster.hostileVerticalNavigationTarget = 0;
        return AutomatiaHostileVerticalNavigationStatus::NotHostile;
    }

    const Uint32 targetUid = monster.hostileVerticalNavigationTarget != 0
        ? monster.hostileVerticalNavigationTarget
        : monster.monsterTarget;
    if (targetUid == 0)
    {
        monster.hostileVerticalNavigationActive = false;
        return AutomatiaHostileVerticalNavigationStatus::NoTarget;
    }

    Entity* target = uidToEntity(targetUid);
    if (!target)
    {
        if (monster.hostileVerticalNavigationActive)
        {
            abandonVerticalRoute(monster);
            return AutomatiaHostileVerticalNavigationStatus::TargetUnavailable;
        }
        return AutomatiaHostileVerticalNavigationStatus::NoTarget;
    }

    MapInstance* activeInstance = worldState.activeInstance();

    // Z4D must not disturb ordinary same-floor monster-vs-monster combat,
    // ring-conflict targets, or any other legacy target category. When a
    // WorldState instance is active, still reject a stale player entity owned
    // by a divergent instance before handing control back to legacy AI.
    if (target->playableFloor == monster.playableFloor
        && !monster.hostileVerticalNavigationActive)
    {
        if (target->behavior == &actPlayer
            && activeInstance
            && activeInstance->loadedMap == &map
            && !targetSharesActiveMapInstance(*target, *activeInstance))
        {
            abandonVerticalRoute(monster);
            return AutomatiaHostileVerticalNavigationStatus::TargetUnavailable;
        }
        return AutomatiaHostileVerticalNavigationStatus::SameFloor;
    }

    if (!activeInstance
        || activeInstance->loadedMap != &map
        || !hostileTargetIsEligible(monster, *target, *activeInstance))
    {
        abandonVerticalRoute(monster);
        return AutomatiaHostileVerticalNavigationStatus::TargetUnavailable;
    }

    if (target->playableFloor == monster.playableFloor)
    {
        monster.hostileVerticalNavigationActive = false;
        monster.hostileVerticalNavigationTarget = 0;
        monster.monsterAcquireAttackTarget(*target, MONSTER_STATE_PATH);
        if (monster.monsterTarget != targetUid)
        {
            abandonVerticalRoute(monster);
            return AutomatiaHostileVerticalNavigationStatus::TargetUnavailable;
        }
        serverUpdateEntitySkill(&monster, 0);
        return AutomatiaHostileVerticalNavigationStatus::ResumedSameFloor;
    }

    monster.hostileVerticalNavigationActive = true;
    monster.hostileVerticalNavigationTarget = targetUid;
    // Park the normal combat target while floors differ. This prevents every
    // legacy horizontal range/attack path from treating equal X/Y as contact.
    monster.monsterTarget = 0;

    Stat* stats = monster.getStats();
    if (!stats
        || !monster.isMobile()
        || stats->getEffectActive(EFF_FEAR)
        || stats->getEffectActive(EFF_DISORIENTED)
        || stats->getEffectActive(EFF_ROOTED)
        || stats->getEffectActive(EFF_PACIFY))
    {
        stopForVerticalRoute(monster);
        return AutomatiaHostileVerticalNavigationStatus::WaitingForRefresh;
    }

    if (!refreshRoute)
    {
        if (monsterHasActiveLocalPath(monster))
        {
            return AutomatiaHostileVerticalNavigationStatus::PathInProgress;
        }
        stopForVerticalRoute(monster);
        return AutomatiaHostileVerticalNavigationStatus::WaitingForRefresh;
    }

    const VerticalNavigationPoint source{
        monster.playableFloor,
        static_cast<int>(std::floor(monster.x / 16.0)),
        static_cast<int>(std::floor(monster.y / 16.0))
    };
    const VerticalNavigationPoint destination{
        target->playableFloor,
        static_cast<int>(std::floor(target->x / 16.0)),
        static_cast<int>(std::floor(target->y / 16.0))
    };
    CrossFloorPathRoute route;
    if (!generateCrossFloorPath(
            *activeInstance, source,
            *activeInstance, destination,
            &monster, target,
            GENERATE_PATH_TO_HUNT_MONSTER_TARGET,
            route))
    {
        stopForVerticalRoute(monster);
        return AutomatiaHostileVerticalNavigationStatus::RouteUnavailable;
    }

    const VerticalNavigationEdge* transition = firstTransition(
        route, monster.playableFloor);
    if (!transition)
    {
        stopForVerticalRoute(monster);
        return AutomatiaHostileVerticalNavigationStatus::RouteUnavailable;
    }

    if (source == transition->source)
    {
        const real_t destinationX =
            transition->destination.tileX * 16.0 + 8.0;
        const real_t destinationY =
            transition->destination.tileY * 16.0 + 8.0;
        if (!transitionAutomatiaNonPlayerEntityToPlayableFloor(
                monster,
                transition->destination.playableFloor,
                destinationX,
                destinationY,
                monster.z))
        {
            stopForVerticalRoute(monster);
            return AutomatiaHostileVerticalNavigationStatus::RouteUnavailable;
        }
        stopForVerticalRoute(monster);
        return AutomatiaHostileVerticalNavigationStatus::Transitioned;
    }

    if (!monster.monsterSetPathToLocation(
            transition->source.tileX,
            transition->source.tileY,
            0,
            GENERATE_PATH_TO_HUNT_MONSTER_TARGET))
    {
        stopForVerticalRoute(monster);
        return AutomatiaHostileVerticalNavigationStatus::RouteUnavailable;
    }

    const bool stateChanged = monster.monsterState != MONSTER_STATE_HUNT;
    monster.monsterState = MONSTER_STATE_HUNT;
    if (stateChanged)
    {
        serverUpdateEntitySkill(&monster, 0);
    }
    return AutomatiaHostileVerticalNavigationStatus::PathAssigned;
}
