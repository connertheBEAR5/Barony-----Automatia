/*-------------------------------------------------------------------------------

    Automatia S.A.M Integration
    File: sam_item_catalog_exporter.cpp
    Stage: SAM-1J

-------------------------------------------------------------------------------*/

#include "sam_item_catalog_exporter.hpp"

#include "sam_content_catalog.hpp"
#include "sam_item_registry_foundation.hpp"
#include "framework/nlohmann/json.hpp"
#include "framework/sam_logger.hpp"
#include "../items.hpp"

#include <cstdio>
#include <fstream>
#include <string>

using nlohmann::json;

namespace
{
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
}

std::string SAMItemCatalogExporter::catalogPath(
    const std::string& outputDirectory
)
{
    return joinPath(
        outputDirectory,
        "sam_item_catalog.json"
    );
}

bool SAMItemCatalogExporter::write(
    const std::string& outputDirectory
)
{
    const std::string finalPath =
        catalogPath(outputDirectory);
    const std::string temporaryPath =
        finalPath + ".tmp";

    json root;
    root["schema_version"] = SchemaVersion;
    root["fingerprint"] =
        SAMContentCatalog::fingerprint();

    root["vanilla_item_count"] = NUMITEMS;
    root["total_item_slots"] = NUM_ITEM_SLOTS;

    root["runtime_base"] =
        SAMItemRegistryFoundation::runtimeIdBase();
    root["runtime_limit"] =
        SAMItemRegistryFoundation::runtimeIdLimit();
    root["runtime_limit_exclusive"] =
        SAMItemRegistryFoundation::runtimeIdLimit();
    root["runtime_last_id"] =
        SAMItemRegistryFoundation::runtimeIdLimit() - 1;
    root["runtime_capacity"] =
        SAMItemRegistryFoundation::runtimeCapacity();
    root["registered_item_count"] =
        SAMItemRegistryFoundation::registeredItemCount();

    root["items"] = json::array();

    for ( const SAMFoundationItemDef& definition :
        SAMItemRegistryFoundation::items() )
    {
        json item;
        item["runtime_id"] = definition.runtimeId;
        item["stable_id"] = definition.stableId;
        item["mod_namespace"] = definition.modNamespace;
        item["name"] = definition.nameIdentified;
        item["name_unidentified"] =
            definition.nameUnidentified;
        item["description"] = definition.description;
        item["category"] = definition.category;
        item["slot"] = definition.slot;
        item["weight"] = definition.weight;
        item["gold_value"] = definition.goldValue;
        item["level"] = definition.level;
        item["stackable"] = definition.stackable;

        root["items"].push_back(item);
    }

    {
        std::ofstream output(
            temporaryPath.c_str(),
            std::ios::binary
                | std::ios::out
                | std::ios::trunc
        );

        if ( !output.is_open() )
        {
            SAM_ERROR(
                "CATALOG",
                "Could not open temporary item catalog: "
                + temporaryPath
            );
            return false;
        }

        output << root.dump(2) << '\n';
        output.flush();

        if ( !output.good() )
        {
            SAM_ERROR(
                "CATALOG",
                "Failed while writing temporary item catalog: "
                + temporaryPath
            );
            output.close();
            std::remove(temporaryPath.c_str());
            return false;
        }
    }

    // Replace the previous catalog only after a complete temporary file has
    // been written. Removing first is required on platforms where rename()
    // will not replace an existing file.
    std::remove(finalPath.c_str());

    if ( std::rename(
        temporaryPath.c_str(),
        finalPath.c_str()
    ) != 0 )
    {
        SAM_ERROR(
            "CATALOG",
            "Could not publish item catalog: "
            + finalPath
        );
        std::remove(temporaryPath.c_str());
        return false;
    }

    SAM_INFO(
        "CATALOG",
        "Exported item catalog schema "
        + std::to_string(SchemaVersion)
        + " with "
        + std::to_string(
            SAMItemRegistryFoundation::registeredItemCount()
        )
        + " item(s) to "
        + finalPath
    );

    return true;
}
