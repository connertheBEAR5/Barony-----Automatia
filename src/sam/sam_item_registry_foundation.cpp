/*-------------------------------------------------------------------------------

    Automatia S.A.M Integration
    File: sam_item_registry_foundation.cpp
    Stage: SAM-1E

-------------------------------------------------------------------------------*/

#include "sam_item_registry_foundation.hpp"

#include "framework/sam_logger.hpp"
#include "framework/sam_workshop.hpp"
#include "framework/nlohmann/json.hpp"

#include <fstream>
#include <set>

using nlohmann::json;

namespace
{
    const std::string kEmptyStableId;

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
                "ITEMS",
                "Could not open item declaration: " + path
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
                "ITEMS",
                "Invalid item JSON in "
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

std::vector<SAMFoundationItemDef>
    SAMItemRegistryFoundation::registry;

void SAMItemRegistryFoundation::clear()
{
    registry.clear();
}

void SAMItemRegistryFoundation::loadFromManifest(
    const SAMModManifest& manifest
)
{
    std::set<std::string> knownIds;

    for ( const SAMFoundationItemDef& existing : registry )
    {
        knownIds.insert(existing.stableId);
    }

    for ( const std::string& relativePath : manifest.items )
    {
        if (
            SAMItemRegistryFoundation::RuntimeIdBase
                + static_cast<int>(registry.size())
            >= SAMItemRegistryFoundation::RuntimeIdLimit
        )
        {
            SAM_ERROR(
                "ITEMS",
                "Item registry capacity reached at runtime id "
                + std::to_string(SAMItemRegistryFoundation::RuntimeIdLimit)
            );
            return;
        }

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
                "ITEMS",
                "Item declaration must be a JSON object: "
                + fullPath
            );
            continue;
        }

        const std::string stableId =
            declaration.value(
                "id",
                std::string()
            );
        const std::string nameIdentified =
            declaration.value(
                "name_identified",
                std::string()
            );
        const std::string category =
            declaration.value(
                "category",
                std::string()
            );

        if ( !isValidStableId(stableId, manifest.ns) )
        {
            SAM_ERROR(
                "ITEMS",
                "Item id must use namespace '"
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

        if ( nameIdentified.empty() )
        {
            SAM_ERROR(
                "ITEMS",
                "Item ["
                + stableId
                + "] is missing name_identified"
            );
            continue;
        }

        if ( category.empty() )
        {
            SAM_ERROR(
                "ITEMS",
                "Item ["
                + stableId
                + "] is missing category"
            );
            continue;
        }

        if ( knownIds.find(stableId) != knownIds.end() )
        {
            SAM_ERROR(
                "ITEMS",
                "Duplicate item id ["
                + stableId
                + "]"
            );
            continue;
        }

        SAMFoundationItemDef definition;
        definition.stableId = stableId;
        definition.modNamespace = manifest.ns;
        definition.nameIdentified = nameIdentified;
        definition.nameUnidentified =
            declaration.value(
                "name_unidentified",
                std::string()
            );
        definition.description =
            declaration.value(
                "description",
                std::string()
            );
        definition.category = category;
        definition.slot =
            declaration.value(
                "slot",
                std::string("NO_EQUIP")
            );
        definition.weight =
            declaration.value("weight", 0);
        definition.goldValue =
            declaration.value("gold_value", 0);
        definition.level =
            declaration.value("level", -1);
        definition.stackable =
            declaration.value("stackable", false);
        definition.sourcePath = fullPath;
        definition.runtimeId =
            SAMItemRegistryFoundation::RuntimeIdBase
            + static_cast<int>(registry.size());

        registry.push_back(definition);
        knownIds.insert(stableId);

        SAM_INFO(
            "ITEMS",
            "Registered item ["
            + definition.stableId
            + "] as runtime id "
            + std::to_string(definition.runtimeId)
            + " ("
            + definition.nameIdentified
            + ", "
            + definition.category
            + ")"
        );
    }
}

int SAMItemRegistryFoundation::count()
{
    return static_cast<int>(registry.size());
}

const SAMFoundationItemDef*
SAMItemRegistryFoundation::getItem(
    const int runtimeId
)
{
    for ( const SAMFoundationItemDef& definition : registry )
    {
        if ( definition.runtimeId == runtimeId )
        {
            return &definition;
        }
    }

    return nullptr;
}

int SAMItemRegistryFoundation::runtimeIdForStableId(
    const std::string& stableId
)
{
    for ( const SAMFoundationItemDef& definition : registry )
    {
        if ( definition.stableId == stableId )
        {
            return definition.runtimeId;
        }
    }

    return -1;
}

const std::string&
SAMItemRegistryFoundation::stableIdForRuntimeId(
    const int runtimeId
)
{
    const SAMFoundationItemDef* definition =
        getItem(runtimeId);

    return definition
        ? definition->stableId
        : kEmptyStableId;
}

bool SAMItemRegistryFoundation::isSAMRuntimeItemId(
    const int runtimeId
)
{
    return runtimeId >= RuntimeIdBase
        && runtimeId < RuntimeIdLimit;
}

bool SAMItemRegistryFoundation::isRegisteredRuntimeItemId(
    const int runtimeId
)
{
    return isSAMRuntimeItemId(runtimeId)
        && getItem(runtimeId) != nullptr;
}

bool SAMItemRegistryFoundation::validateRuntimeLayout(
    const int vanillaItemCount
)
{
    if ( vanillaItemCount < 0 )
    {
        SAM_ERROR(
            "ITEMS",
            "Invalid negative vanilla item count"
        );
        return false;
    }

    if ( vanillaItemCount > RuntimeIdBase )
    {
        SAM_ERROR(
            "ITEMS",
            "S.A.M runtime item range overlaps Barony's native item range: "
            + std::to_string(vanillaItemCount)
            + " native items, S.A.M base "
            + std::to_string(RuntimeIdBase)
        );
        return false;
    }

    if ( RuntimeCapacity <= 0 )
    {
        SAM_ERROR(
            "ITEMS",
            "S.A.M runtime item capacity is invalid"
        );
        return false;
    }

    SAM_INFO(
        "ITEMS",
        "Runtime item boundary verified: vanilla [0, "
        + std::to_string(vanillaItemCount - 1)
        + "], total slots "
        + std::to_string(RuntimeIdLimit)
        + ", S.A.M ["
        + std::to_string(RuntimeIdBase)
        + ", "
        + std::to_string(RuntimeIdLimit - 1)
        + "], capacity "
        + std::to_string(RuntimeCapacity)
    );

    return true;
}

int SAMItemRegistryFoundation::registeredItemCount()
{
    return static_cast<int>(registry.size());
}

int SAMItemRegistryFoundation::runtimeIdAtIndex(
    const int catalogIndex
)
{
    if ( catalogIndex < 0
        || catalogIndex >= registeredItemCount() )
    {
        return -1;
    }

    return registry[
        static_cast<std::size_t>(catalogIndex)
    ].runtimeId;
}

const std::string&
SAMItemRegistryFoundation::stableIdAtIndex(
    const int catalogIndex
)
{
    if ( catalogIndex < 0
        || catalogIndex >= registeredItemCount() )
    {
        return kEmptyStableId;
    }

    return registry[
        static_cast<std::size_t>(catalogIndex)
    ].stableId;
}

const std::string&
SAMItemRegistryFoundation::displayNameAtIndex(
    const int catalogIndex
)
{
    if ( catalogIndex < 0
        || catalogIndex >= registeredItemCount() )
    {
        return kEmptyStableId;
    }

    return registry[
        static_cast<std::size_t>(catalogIndex)
    ].nameIdentified;
}

const std::string&
SAMItemRegistryFoundation::categoryAtIndex(
    const int catalogIndex
)
{
    if ( catalogIndex < 0
        || catalogIndex >= registeredItemCount() )
    {
        return kEmptyStableId;
    }

    return registry[
        static_cast<std::size_t>(catalogIndex)
    ].category;
}

const std::string&
SAMItemRegistryFoundation::slotAtIndex(
    const int catalogIndex
)
{
    if ( catalogIndex < 0
        || catalogIndex >= registeredItemCount() )
    {
        return kEmptyStableId;
    }

    return registry[
        static_cast<std::size_t>(catalogIndex)
    ].slot;
}

int SAMItemRegistryFoundation::runtimeIdBase()
{
    return RuntimeIdBase;
}

int SAMItemRegistryFoundation::runtimeIdLimit()
{
    return RuntimeIdLimit;
}

int SAMItemRegistryFoundation::runtimeCapacity()
{
    return RuntimeCapacity;
}

const std::vector<SAMFoundationItemDef>&
SAMItemRegistryFoundation::items()
{
    return registry;
}
