/*-------------------------------------------------------------------------------

    Automatia S.A.M Integration
    File: sam_foundation.cpp
    Stage: SAM-1J

-------------------------------------------------------------------------------*/

#include "sam_foundation.hpp"
#include "sam_class_registry_foundation.hpp"
#include "sam_item_registry_foundation.hpp"
#include "sam_content_catalog.hpp"
#include "sam_item_catalog_exporter.hpp"
#include "sam_item_definition_bridge.hpp"
#include "framework/sam_logger.hpp"
#include "framework/sam_workshop.hpp"
#include "../items.hpp"

static_assert(
    SAMItemRegistryFoundation::RuntimeIdBase
        == SAM_ITEM_ID_BASE,
    "S.A.M item base does not match the engine item base"
);
static_assert(
    SAMItemRegistryFoundation::RuntimeIdLimit
        == NUM_ITEM_SLOTS,
    "S.A.M item limit does not match engine item storage"
);
static_assert(
    SAMItemRegistryFoundation::RuntimeCapacity
        == SAM_ITEM_CAPACITY,
    "S.A.M item capacity does not match engine item storage"
);

#include <string>
#include <vector>

namespace
{
    bool samInitialized = false;
    int manifestCount = 0;
}

void SAMFoundation::onModLoad(
    const std::vector<std::pair<std::string, std::string>>& mountedPaths,
    const std::string& baronyVersion,
    const std::string& outputDirectory
)
{
    if ( !samInitialized )
    {
        SAMLogger::init(outputDirectory);
        samInitialized = true;
    }

    SAMLogger::beginModLoad();
    SAM_INFO(
        "CORE",
        "Automatia S.A.M manifest loader active"
    );
    SAM_INFO(
        "CORE",
        "S.A.M framework version "
        SAM_FRAMEWORK_VERSION
    );
    SAM_INFO(
        "CORE",
        "Barony version "
        + (
            baronyVersion.empty()
                ? std::string("(unknown)")
                : baronyVersion
        )
    );
    SAM_INFO(
        "CORE",
        "Scanning mounted mod paths: "
        + std::to_string(mountedPaths.size())
    );

    const std::vector<SAMModManifest> manifests =
        SAMWorkshop::scan(
            mountedPaths,
            baronyVersion
        );

    manifestCount =
        static_cast<int>(manifests.size());

    SAMClassRegistryFoundation::clear();
    SAMItemRegistryFoundation::clear();

    if ( !SAMItemRegistryFoundation::validateRuntimeLayout(
        NUMITEMS
    ) )
    {
        SAM_ERROR(
            "CORE",
            "S.A.M item runtime layout validation failed"
        );
    }

    for ( std::size_t i = 0; i < manifests.size(); ++i )
    {
        const SAMModManifest& manifest = manifests[i];

        SAM_INFO(
            "CORE",
            "Load order "
            + std::to_string(i + 1)
            + ": ["
            + manifest.ns
            + "] "
            + manifest.name
            + " v"
            + manifest.version
        );

        if ( !manifest.dependencies.empty() )
        {
            SAM_INFO(
                "CORE",
                "  Dependencies declared: "
                + std::to_string(
                    manifest.dependencies.size()
                )
            );
        }

        SAMClassRegistryFoundation::loadFromManifest(
            manifest
        );

        SAMItemRegistryFoundation::loadFromManifest(
            manifest
        );
    }

    if ( manifests.empty() )
    {
        SAM_INFO(
            "CORE",
            "No valid S.A.M mod manifests were found"
        );
    }
    else
    {
        SAM_INFO(
            "CORE",
            "Valid S.A.M manifests ready: "
            + std::to_string(manifests.size())
        );
    }

    SAM_INFO(
        "CORE",
        "Class declarations registered: "
        + std::to_string(
            SAMClassRegistryFoundation::count()
        )
    );
    SAMContentCatalog::rebuild(manifests);
    SAMItemDefinitionBridge::installRegisteredDefinitions();
    SAMItemDefinitionBridge::runControlledConstructionTests();
    SAMItemCatalogExporter::write(outputDirectory);

    SAM_INFO(
        "CORE",
        "Item declarations registered: "
        + std::to_string(
            SAMItemRegistryFoundation::count()
        )
    );
    SAM_INFO(
        "ITEMS",
        "Public item catalog API ready: "
        + std::to_string(
            SAMItemRegistryFoundation::registeredItemCount()
        )
        + " registered, range ["
        + std::to_string(
            SAMItemRegistryFoundation::runtimeIdBase()
        )
        + ", "
        + std::to_string(
            SAMItemRegistryFoundation::runtimeIdLimit() - 1
        )
        + "], capacity "
        + std::to_string(
            SAMItemRegistryFoundation::runtimeCapacity()
        )
    );
    SAM_INFO(
        "CORE",
        "Live custom item spawning and inventory integration remain disabled in SAM-1E"
    );

    SAMLoadStats stats;
    stats.mods = manifestCount;
    stats.classesRegistered =
        SAMClassRegistryFoundation::count();
    stats.itemsRegistered =
        SAMItemRegistryFoundation::count();

    for ( const SAMModManifest& manifest : manifests )
    {
        stats.classesDeclared +=
            static_cast<int>(manifest.classes.size());
        stats.itemsDeclared +=
            static_cast<int>(manifest.items.size());
        stats.monstersDeclared +=
            static_cast<int>(manifest.monsters.size());
        stats.plugins +=
            static_cast<int>(manifest.plugins.size());
        stats.patchFiles +=
            static_cast<int>(manifest.patches.size());
    }

    SAMLogger::logLoadSummary(stats);
}

void SAMFoundation::onModUnload()
{
    if ( !samInitialized )
    {
        return;
    }

    SAM_INFO(
        "CORE",
        "Automatia S.A.M manifest loader unloading"
    );

    SAMContentCatalog::clear();
    SAMItemDefinitionBridge::clearInstalledDefinitions();
    SAMItemRegistryFoundation::clear();
    SAMClassRegistryFoundation::clear();
    SAMWorkshop::clear();
    manifestCount = 0;

    SAMLogger::logSessionSummary();
    SAMLogger::shutdown();
    samInitialized = false;
}

bool SAMFoundation::isInitialized()
{
    return samInitialized;
}

int SAMFoundation::loadedManifestCount()
{
    return manifestCount;
}
