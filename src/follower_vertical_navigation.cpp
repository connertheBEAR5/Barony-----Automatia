/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: follower_vertical_navigation.cpp
    Desc: Z4C player-follower consumption of generic cross-floor routes.

-------------------------------------------------------------------------------*/

#include "follower_vertical_navigation.hpp"

#include "cross_floor_path.hpp"
#include "entity.hpp"
#include "game.hpp"
#include "main.hpp"
#include "monster.hpp"
#include "net.hpp"
#include "paths.hpp"
#include "player.hpp"
#include "world_state.hpp"

#include <cmath>

namespace
{
void stopForVerticalRoute(Entity& follower)
{
    const bool stateChanged = follower.monsterState != MONSTER_STATE_WAIT;
    const bool targetChanged = follower.monsterTarget != 0;
    follower.monsterTarget = 0;
    follower.monsterState = MONSTER_STATE_WAIT;
    follower.vel_x = 0.0;
    follower.vel_y = 0.0;
    if (stateChanged)
    {
        serverUpdateEntitySkill(&follower, 0);
    }
    if (targetChanged
        && follower.monsterAllyIndex > 0
        && follower.monsterAllyIndex < MAXPLAYERS)
    {
        serverUpdateEntitySkill(&follower, 1);
    }
}

bool followerHasActiveLocalPath(const Entity& follower)
{
    if (follower.monsterState != MONSTER_STATE_HUNT
        || !follower.children.first
        || !follower.children.first->element)
    {
        return false;
    }
    const list_t* path = static_cast<const list_t*>(
        follower.children.first->element);
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
}

AutomatiaFollowerVerticalNavigationStatus
updateAutomatiaFollowerVerticalNavigation(
    Entity& follower,
    const bool refreshRoute)
{
    if (multiplayer == CLIENT
        || follower.behavior != &actMonster
        || follower.monsterAllyIndex < 0
        || follower.monsterAllyIndex >= MAXPLAYERS)
    {
        follower.followerVerticalNavigationActive = false;
        return AutomatiaFollowerVerticalNavigationStatus::NotPlayerFollower;
    }

    const int ownerPlayer = follower.monsterAllyIndex;
    const MapInstance* activeInstance = worldState.activeInstance();
    if (!activeInstance
        || activeInstance->loadedMap != &map
        || client_disconnected[ownerPlayer]
        || !players[ownerPlayer]
        || !worldState.playerSharesActiveInstance(ownerPlayer))
    {
        follower.followerVerticalNavigationActive = false;
        stopForVerticalRoute(follower);
        return AutomatiaFollowerVerticalNavigationStatus::OwnerUnavailable;
    }

    Entity* owner = worldState.playerEntityFor(
        activeInstance->key(), ownerPlayer);
    if (!owner || owner->behavior != &actPlayer)
    {
        follower.followerVerticalNavigationActive = false;
        stopForVerticalRoute(follower);
        return AutomatiaFollowerVerticalNavigationStatus::OwnerUnavailable;
    }

    if (owner->playableFloor == follower.playableFloor)
    {
        follower.followerVerticalNavigationActive = false;
        return AutomatiaFollowerVerticalNavigationStatus::SameFloor;
    }

    follower.followerVerticalNavigationActive = true;
    follower.monsterTarget = 0;
    if (!refreshRoute)
    {
        if (followerHasActiveLocalPath(follower))
        {
            return AutomatiaFollowerVerticalNavigationStatus::PathInProgress;
        }
        stopForVerticalRoute(follower);
        return AutomatiaFollowerVerticalNavigationStatus::WaitingForRefresh;
    }

    const VerticalNavigationPoint source{
        follower.playableFloor,
        static_cast<int>(std::floor(follower.x / 16.0)),
        static_cast<int>(std::floor(follower.y / 16.0))
    };
    const VerticalNavigationPoint destination{
        owner->playableFloor,
        static_cast<int>(std::floor(owner->x / 16.0)),
        static_cast<int>(std::floor(owner->y / 16.0))
    };
    CrossFloorPathRoute route;
    if (!generateCrossFloorPath(
            *activeInstance, source,
            *activeInstance, destination,
            &follower, owner,
            GENERATE_PATH_ALLY_FOLLOW,
            route))
    {
        stopForVerticalRoute(follower);
        return AutomatiaFollowerVerticalNavigationStatus::RouteUnavailable;
    }

    const VerticalNavigationEdge* transition = firstTransition(
        route, follower.playableFloor);
    if (!transition)
    {
        stopForVerticalRoute(follower);
        return AutomatiaFollowerVerticalNavigationStatus::RouteUnavailable;
    }

    if (source == transition->source)
    {
        const real_t destinationX =
            transition->destination.tileX * 16.0 + 8.0;
        const real_t destinationY =
            transition->destination.tileY * 16.0 + 8.0;
        const Sint32 persistentID = follower.persistentID;
        const Uint32 runtimeUID = follower.getUID();
        const int durableOwnerPlayer = follower.monsterAllyIndex;
        if (!transitionAutomatiaNonPlayerEntityToPlayableFloor(
                follower,
                transition->destination.playableFloor,
                destinationX,
                destinationY,
                follower.z))
        {
            stopForVerticalRoute(follower);
            return AutomatiaFollowerVerticalNavigationStatus::RouteUnavailable;
        }

        // The spatial transaction must not replace persistent/runtime identity
        // or the normal follower ownership record.
        if (follower.persistentID != persistentID
            || follower.getUID() != runtimeUID
            || follower.monsterAllyIndex != durableOwnerPlayer)
        {
            printlog(
                "[Z4C] Follower identity changed during floor transition (UID %u).",
                runtimeUID);
        }
        stopForVerticalRoute(follower);
        return AutomatiaFollowerVerticalNavigationStatus::Transitioned;
    }

    if (!follower.monsterSetPathToLocation(
            transition->source.tileX,
            transition->source.tileY,
            0,
            GENERATE_PATH_ALLY_FOLLOW))
    {
        stopForVerticalRoute(follower);
        return AutomatiaFollowerVerticalNavigationStatus::RouteUnavailable;
    }

    const bool stateChanged = follower.monsterState != MONSTER_STATE_HUNT;
    follower.monsterState = MONSTER_STATE_HUNT;
    if (stateChanged)
    {
        serverUpdateEntitySkill(&follower, 0);
    }
    return AutomatiaFollowerVerticalNavigationStatus::PathAssigned;
}
