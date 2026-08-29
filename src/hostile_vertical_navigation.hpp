/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: hostile_vertical_navigation.hpp
    Desc: Z4D hostile consumption of generic cross-floor routes.

-------------------------------------------------------------------------------*/

#pragma once

class Entity;

enum class AutomatiaHostileVerticalNavigationStatus
{
    NotHostile,
    NoTarget,
    SameFloor,
    ResumedSameFloor,
    TargetUnavailable,
    WaitingForRefresh,
    RouteUnavailable,
    PathInProgress,
    PathAssigned,
    Transitioned
};

/*
 * Server/single-player coordinator for ordinary hostile monsters that already
 * have a legitimate player target. This preserves Barony's perception rules:
 * it does not grant cross-floor vision or choose targets merely by X/Y.
 */
AutomatiaHostileVerticalNavigationStatus
updateAutomatiaHostileVerticalNavigation(
    Entity& monster,
    bool refreshRoute);

