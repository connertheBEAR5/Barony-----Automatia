/*-------------------------------------------------------------------------------

    Automatia S.A.M Integration
    File: sam_class_registry_foundation.cpp
    Stage: SAM-1C

-------------------------------------------------------------------------------*/

#include "sam_class_registry_foundation.hpp"

#include "framework/sam_logger.hpp"
#include "framework/sam_workshop.hpp"
#include "framework/nlohmann/json.hpp"
#include "../main.hpp"
#include "../stat.hpp"

#include <fstream>
#include <cstdint>
#include <limits>
#include <set>
#include <sstream>

using nlohmann::json;

namespace
{
    constexpr int kClassIdBase = 1000;

    std::string joinPath(
        const std::string& directory,
        const std::string& filename
    )
    {
        if ( directory.empty() )
        {
            return filename;
        }

        const char back = directory.back();
        if ( back == '/' || back == '\\' )
        {
            return directory + filename;
        }

        return directory + "/" + filename;
    }

    bool readJsonFile(
        const std::string& path,
        json& output
    )
    {
        std::ifstream input(path.c_str(), std::ios::binary);
        if ( !input.is_open() )
        {
            SAM_ERROR(
                "CLASSES",
                "Could not open class declaration: " + path
            );
            return false;
        }

        try
        {
            input >> output;
        }
        catch ( const std::exception& exception )
        {
            SAM_ERROR(
                "CLASSES",
                "Invalid class JSON in "
                + path
                + ": "
                + exception.what()
            );
            return false;
        }

        return true;
    }

    bool isValidStableId(
        const std::string& stableId,
        const std::string& expectedNamespace
    )
    {
        const std::size_t separator =
            stableId.find(':');

        if ( separator == std::string::npos
            || separator == 0
            || separator + 1 >= stableId.size() )
        {
            return false;
        }

        return stableId.substr(0, separator)
            == expectedNamespace;
    }

    void warnInvalidField(
        const std::string& path,
        const char* field,
        const char* expected
    )
    {
        SAM_WARN(
            "CLASSES",
            "Ignoring invalid '" + std::string(field)
            + "' in " + path + "; expected " + expected
        );
    }

    bool readStringField(
        const json& declaration,
        const char* field,
        const std::string& path,
        std::string& output
    )
    {
        output.clear();
        const auto it = declaration.find(field);
        if ( it == declaration.end() )
        {
            return true;
        }
        if ( !it->is_string() )
        {
            warnInvalidField(path, field, "a string");
            return false;
        }
        output = it->get<std::string>();
        return true;
    }

    bool readIntField(
        const json& declaration,
        const char* field,
        const std::string& path,
        const int defaultValue,
        int& output
    )
    {
        output = defaultValue;
        const auto it = declaration.find(field);
        if ( it == declaration.end() )
        {
            return true;
        }
        try
        {
            if ( it->is_number_unsigned() )
            {
                const std::uint64_t value = it->get<std::uint64_t>();
                if ( value <= static_cast<std::uint64_t>(
                    std::numeric_limits<int>::max()) )
                {
                    output = static_cast<int>(value);
                    return true;
                }
            }
            else if ( it->is_number_integer() )
            {
                const std::int64_t value = it->get<std::int64_t>();
                if ( value >= static_cast<std::int64_t>(
                    std::numeric_limits<int>::min())
                    && value <= static_cast<std::int64_t>(
                        std::numeric_limits<int>::max()) )
                {
                    output = static_cast<int>(value);
                    return true;
                }
            }
        }
        catch ( const std::exception& )
        {
            // Use the shared diagnostic below for nlohmann conversion failure.
        }
        warnInvalidField(path, field, "a 32-bit integer");
        return false;
    }
}

std::vector<SAMFoundationClassDef>
    SAMClassRegistryFoundation::registry;

void SAMClassRegistryFoundation::clear()
{
    registry.clear();
}

void SAMClassRegistryFoundation::loadFromManifest(
    const SAMModManifest& manifest
)
{
    std::set<std::string> knownIds;

    for ( const SAMFoundationClassDef& existing : registry )
    {
        knownIds.insert(existing.stableId);
    }

    for ( const std::string& relativePath : manifest.classes )
    {
        const std::string fullPath =
            joinPath(manifest.modPath, relativePath);

        json declaration;
        if ( !readJsonFile(fullPath, declaration) )
        {
            continue;
        }

        if ( !declaration.is_object() )
        {
            SAM_ERROR(
                "CLASSES",
                "Class declaration must be a JSON object: "
                + fullPath
            );
            continue;
        }

        std::string stableId;
        std::string name;
        if ( !readStringField(declaration, "id", fullPath, stableId)
            || !readStringField(declaration, "name", fullPath, name) )
        {
            continue;
        }

        if ( !isValidStableId(stableId, manifest.ns) )
        {
            SAM_ERROR(
                "CLASSES",
                "Class id must use namespace '"
                + manifest.ns
                + "': "
                + (
                    stableId.empty()
                        ? std::string("(missing id)")
                        : stableId
                )
                + " in "
                + fullPath
            );
            continue;
        }

        if ( name.empty() )
        {
            SAM_ERROR(
                "CLASSES",
                "Class ["
                + stableId
                + "] is missing a non-empty name"
            );
            continue;
        }

        if ( knownIds.find(stableId) != knownIds.end() )
        {
            SAM_ERROR(
                "CLASSES",
                "Duplicate class id ["
                + stableId
                + "]"
            );
            continue;
        }

        SAMFoundationClassDef definition;
        definition.stableId = stableId;
        definition.modNamespace = manifest.ns;
        definition.name = name;
        readStringField(declaration, "description", fullPath,
            definition.description);

        if ( declaration.contains("stats") )
        {
            const json& stats = declaration["stats"];
            if ( !stats.is_object() )
            {
                warnInvalidField(fullPath, "stats", "an object of integer bonuses");
            }
            else
            {
                readIntField(stats, "STR", fullPath + "/stats", 0,
                    definition.str);
                readIntField(stats, "DEX", fullPath + "/stats", 0,
                    definition.dex);
                readIntField(stats, "CON", fullPath + "/stats", 0,
                    definition.con);
                readIntField(stats, "INT", fullPath + "/stats", 0,
                    definition.intel);
                readIntField(stats, "PER", fullPath + "/stats", 0,
                    definition.per);
                readIntField(stats, "CHR", fullPath + "/stats", 0,
                    definition.chr);
                readIntField(stats, "HP", fullPath + "/stats", 0,
                    definition.hp);
                readIntField(stats, "MP", fullPath + "/stats", 0,
                    definition.mp);
                readIntField(stats, "GOLD", fullPath + "/stats", 0,
                    definition.gold);
            }
        }

        definition.sourcePath = fullPath;
        definition.runtimeId =
            kClassIdBase
            + static_cast<int>(registry.size());

        registry.push_back(definition);
        knownIds.insert(stableId);

        SAM_INFO(
            "CLASSES",
            "Registered class ["
            + definition.stableId
            + "] as runtime id "
            + std::to_string(definition.runtimeId)
            + " ("
            + definition.name
            + ")"
        );
    }
}

int SAMClassRegistryFoundation::count()
{
    return static_cast<int>(registry.size());
}

const SAMFoundationClassDef*
SAMClassRegistryFoundation::getClass(
    const int runtimeId
)
{
    for ( const SAMFoundationClassDef& definition : registry )
    {
        if ( definition.runtimeId == runtimeId )
        {
            return &definition;
        }
    }

    return nullptr;
}

int SAMClassRegistryFoundation::runtimeIdForStableId(
    const std::string& stableId
)
{
    for ( const SAMFoundationClassDef& definition : registry )
    {
        if ( definition.stableId == stableId )
        {
            return definition.runtimeId;
        }
    }

    return -1;
}

void SAMClassRegistryFoundation::applyStats(
    const int runtimeId,
    Stat* stats
)
{
    if ( !stats )
    {
        return;
    }

    const SAMFoundationClassDef* definition =
        getClass(runtimeId);
    if ( !definition )
    {
        return;
    }

    stats->STR += definition->str;
    stats->DEX += definition->dex;
    stats->CON += definition->con;
    stats->INT += definition->intel;
    stats->PER += definition->per;
    stats->CHR += definition->chr;

    stats->MAXHP += definition->hp;
    stats->HP += definition->hp;
    stats->MAXMP += definition->mp;
    stats->MP += definition->mp;
    stats->GOLD += definition->gold;
}

const std::vector<SAMFoundationClassDef>&
SAMClassRegistryFoundation::classes()
{
    return registry;
}
