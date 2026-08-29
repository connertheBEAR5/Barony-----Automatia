/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: follower_vertical_navigation.hpp
    Desc: Z4C player-follower consumption of generic cross-floor routes.

-------------------------------------------------------------------------------*/

#pragma once

class Entity;

enum class AutomatiaFollowerVerticalNavigationStatus
{
    NotPlayerFollower,
    SameFloor,
    OwnerUnavailable,
    WaitingForRefresh,
    RouteUnavailable,
    PathInProgress,
    PathAssigned,
    Transitioned
};

/*
 * Server/single-player coordinator for normal player-owned monsters. Owner
 * identity comes from monsterAllyIndex; the runtime leader UID remains the
 * ordinary follower system's concern. When refreshRoute is false, an existing
 * floor-local HUNT path is left for actMonster() to consume normally.
 */
AutomatiaFollowerVerticalNavigationStatus
updateAutomatiaFollowerVerticalNavigation(
    Entity& follower,
    bool refreshRoute);
