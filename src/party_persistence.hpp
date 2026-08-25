/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: party_persistence.hpp
    Desc: JSON persistence boundary for the persistent-party runtime model.

-------------------------------------------------------------------------------*/

#pragma once

#include "party_manager.hpp"
#include "sam/framework/nlohmann/json.hpp"

#include <string>

namespace AutomatiaParty
{
class PartyPersistence
{
public:
    using Json = nlohmann::json;

    static Json toPersistentJson(const PartyManager& manager);
    static bool loadPersistentJson(
        PartyManager& manager,
        const Json& document,
        std::string& error
    );
    static bool validatePersistentJson(
        const Json& document,
        std::string& error
    );
};
}
