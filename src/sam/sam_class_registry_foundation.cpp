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

        const std::string stableId =
            declaration.value(
                "id",
                std::string()
            );
        const std::string name =
            declaration.value(
                "name",
                std::string()
            );

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
        definition.description =
            declaration.value(
                "description",
                std::string()
            );

        if ( declaration.contains("stats")
            && declaration["stats"].is_object() )
        {
            const json& stats = declaration["stats"];

            definition.str = stats.value("STR", 0);
            definition.dex = stats.value("DEX", 0);
            definition.con = stats.value("CON", 0);
            definition.intel = stats.value("INT", 0);
            definition.per = stats.value("PER", 0);
            definition.chr = stats.value("CHR", 0);
            definition.hp = stats.value("HP", 0);
            definition.mp = stats.value("MP", 0);
            definition.gold = stats.value("GOLD", 0);
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
