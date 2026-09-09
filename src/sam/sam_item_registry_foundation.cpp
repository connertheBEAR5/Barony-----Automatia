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
#include <cstdint>
#include <limits>

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

    void warnInvalidField(
        const std::string& path,
        const char* field,
        const char* expected
    )
    {
        SAM_WARN(
            "ITEMS",
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
                if ( value > static_cast<std::uint64_t>(
                    std::numeric_limits<int>::max()) )
                {
                    warnInvalidField(path, field, "a 32-bit integer");
                    return false;
                }
                output = static_cast<int>(value);
                return true;
            }
            if ( it->is_number_integer() )
            {
                const std::int64_t value = it->get<std::int64_t>();
                if ( value < static_cast<std::int64_t>(
                    std::numeric_limits<int>::min())
                    || value > static_cast<std::int64_t>(
                        std::numeric_limits<int>::max()) )
                {
                    warnInvalidField(path, field, "a 32-bit integer");
                    return false;
                }
                output = static_cast<int>(value);
                return true;
            }
        }
        catch ( const std::exception& )
        {
            // Fall through to the common diagnostic. nlohmann can throw when
            // a hostile numeric value cannot fit its requested C++ type.
        }
        warnInvalidField(path, field, "a 32-bit integer");
        return false;
    }

    bool readBoolField(
        const json& declaration,
        const char* field,
        const std::string& path,
        const bool defaultValue,
        bool& output
    )
    {
        output = defaultValue;
        const auto it = declaration.find(field);
        if ( it == declaration.end() )
        {
            return true;
        }
        if ( !it->is_boolean() )
        {
            warnInvalidField(path, field, "true or false");
            return false;
        }
        output = it->get<bool>();
        return true;
    }
}

std::vector<SAMFoundationItemDef>
    SAMItemRegistryFoundation::registry;
std::unordered_map<int, std::size_t>
    SAMItemRegistryFoundation::runtimeIdIndex;
std::unordered_map<std::string, std::size_t>
    SAMItemRegistryFoundation::stableIdIndex;
int SAMItemRegistryFoundation::nextRuntimeId =
    SAMItemRegistryFoundation::RuntimeIdBase;

void SAMItemRegistryFoundation::clear()
{
    registry.clear();
    runtimeIdIndex.clear();
    stableIdIndex.clear();
    nextRuntimeId = RuntimeIdBase;
}

void SAMItemRegistryFoundation::loadFromManifest(
    const SAMModManifest& manifest
)
{
    for ( const std::string& relativePath : manifest.items )
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
                "ITEMS",
                "Item declaration must be a JSON object: "
                + fullPath
            );
            continue;
        }

        std::string stableId;
        std::string nameIdentified;
        std::string category;
        if ( !readStringField(declaration, "id", fullPath, stableId)
            || !readStringField(declaration, "name_identified", fullPath,
                nameIdentified)
            || !readStringField(declaration, "category", fullPath, category) )
        {
            continue;
        }

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

        if ( stableIdIndex.find(stableId) != stableIdIndex.end() )
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
        readStringField(declaration, "name_unidentified", fullPath,
            definition.nameUnidentified);
        readStringField(declaration, "description", fullPath,
            definition.description);
        definition.category = category;
        definition.slot = "NO_EQUIP";
        std::string slot;
        if ( readStringField(declaration, "slot", fullPath, slot)
            && !slot.empty() )
        {
            definition.slot = slot;
        }
        readIntField(declaration, "weight", fullPath, 0, definition.weight);
        readIntField(declaration, "gold_value", fullPath, 0,
            definition.goldValue);
        readIntField(declaration, "level", fullPath, -1, definition.level);
        readBoolField(declaration, "stackable", fullPath, false,
            definition.stackable);
        definition.sourcePath = fullPath;

        int runtimeId = nextRuntimeId;
        while ( runtimeId < RuntimeIdLimit
            && runtimeIdIndex.find(runtimeId) != runtimeIdIndex.end() )
        {
            ++runtimeId;
        }
        if ( runtimeId >= RuntimeIdLimit )
        {
            SAM_ERROR(
                "ITEMS",
                "Item registry capacity reached at runtime id "
                + std::to_string(RuntimeIdLimit)
            );
            return;
        }

        definition.runtimeId = runtimeId;
        const std::size_t catalogIndex = registry.size();
        registry.push_back(std::move(definition));
        runtimeIdIndex.emplace(runtimeId, catalogIndex);
        stableIdIndex.emplace(stableId, catalogIndex);
        nextRuntimeId = runtimeId + 1;

        const SAMFoundationItemDef& registered = registry.back();

        SAM_INFO(
            "ITEMS",
            "Registered item ["
            + registered.stableId
            + "] as runtime id "
            + std::to_string(registered.runtimeId)
            + " ("
            + registered.nameIdentified
            + ", "
            + registered.category
            + ")"
        );
    }
}

bool SAMItemRegistryFoundation::registerFrameworkBuiltin(
    const std::string& stableId,
    const int runtimeId,
    const std::string& displayName,
    const std::string& category
)
{
    if ( stableId.empty()
        || runtimeId < RuntimeIdBase
        || runtimeId >= RuntimeIdLimit
        || runtimeIdIndex.find(runtimeId) != runtimeIdIndex.end()
        || stableIdIndex.find(stableId) != stableIdIndex.end() )
    {
        SAM_ERROR(
            "ITEMS",
            "Invalid or colliding framework item reservation ["
            + stableId + "] at runtime id " + std::to_string(runtimeId)
        );
        return false;
    }

    SAMFoundationItemDef definition;
    definition.stableId = stableId;
    definition.modNamespace = "sam";
    definition.nameIdentified = displayName;
    definition.nameUnidentified = displayName;
    definition.category = category;
    definition.slot = "NO_EQUIP";
    definition.runtimeId = runtimeId;
    const std::size_t catalogIndex = registry.size();
    registry.push_back(std::move(definition));
    runtimeIdIndex.emplace(runtimeId, catalogIndex);
    stableIdIndex.emplace(stableId, catalogIndex);

    SAM_INFO(
        "ITEMS",
        "Reserved framework item [" + stableId + "] as runtime id "
        + std::to_string(runtimeId)
    );
    return true;
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
    const auto found = runtimeIdIndex.find(runtimeId);
    if ( found == runtimeIdIndex.end()
        || found->second >= registry.size() )
    {
        return nullptr;
    }
    return &registry[found->second];
}

int SAMItemRegistryFoundation::runtimeIdForStableId(
    const std::string& stableId
)
{
    const auto found = stableIdIndex.find(stableId);
    if ( found == stableIdIndex.end()
        || found->second >= registry.size() )
    {
        return -1;
    }
    return registry[found->second].runtimeId;
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
