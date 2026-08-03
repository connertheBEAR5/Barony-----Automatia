/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: automatia_world_save.hpp
    Desc: Serialization bridge from the runtime world registry to schema v1.

-------------------------------------------------------------------------------*/

#pragma once

#include "automatia_save.hpp"

#include <string>

class WorldState;

namespace AutomatiaSave
{
Json captureWorldState(const std::string& sessionId, const WorldState& world);
}
