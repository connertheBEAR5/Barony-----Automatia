/*-------------------------------------------------------------------------------

    BARONY AUTOMATIA
    File: automatia_save.hpp
    Desc: Versioned, extensible, restart-safe persistent-world save envelope.

-------------------------------------------------------------------------------*/

#pragma once

#include "sam/framework/nlohmann/json.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace AutomatiaSave
{
using Json = nlohmann::json;

constexpr std::uint32_t CURRENT_SCHEMA_VERSION = 1;
constexpr std::uint32_t MINIMUM_SCHEMA_VERSION = 1;
constexpr std::size_t MAX_SAVE_BYTES = 128U * 1024U * 1024U;

struct Result
{
    bool ok = false;
    std::string error;

    explicit operator bool() const
    {
        return ok;
    }
};

Json makeEmptyWorldSave(const std::string& sessionId);
Result validate(const Json& document);
Result writeAtomic(const std::filesystem::path& path, const Json& document);
Result load(const std::filesystem::path& path, Json& document);
}
