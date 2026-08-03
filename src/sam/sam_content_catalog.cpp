/*-------------------------------------------------------------------------------

    Automatia S.A.M Integration
    File: sam_content_catalog.cpp
    Stage: SAM-1F

-------------------------------------------------------------------------------*/

#include "sam_content_catalog.hpp"

#include "sam_class_registry_foundation.hpp"
#include "sam_item_registry_foundation.hpp"
#include "framework/sam_logger.hpp"
#include "framework/sam_workshop.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace
{
    std::uint64_t fnv1a64(
        const std::string& value,
        std::uint64_t hash
    )
    {
        constexpr std::uint64_t offsetBasis =
            14695981039346656037ull;
        constexpr std::uint64_t prime =
            1099511628211ull;

        if ( hash == 0 )
        {
            hash = offsetBasis;
        }

        for ( const unsigned char byte : value )
        {
            hash ^= static_cast<std::uint64_t>(byte);
            hash *= prime;
        }

        return hash;
    }

    std::string toHex(
        const std::uint64_t value
    )
    {
        std::ostringstream stream;
        stream
            << std::hex
            << std::setfill('0')
            << std::setw(16)
            << value;
        return stream.str();
    }
}

std::string SAMContentCatalog::currentFingerprint;
std::vector<std::string> SAMContentCatalog::currentEntries;

void SAMContentCatalog::clear()
{
    currentFingerprint.clear();
    currentEntries.clear();
}

void SAMContentCatalog::rebuild(
    const std::vector<SAMModManifest>& manifests
)
{
    clear();

    for ( const SAMModManifest& manifest : manifests )
    {
        currentEntries.push_back(
            "mod:"
            + manifest.ns
            + "@"
            + manifest.version
        );
    }

    for ( const SAMFoundationClassDef& definition :
        SAMClassRegistryFoundation::classes() )
    {
        currentEntries.push_back(
            "class:"
            + definition.stableId
        );
    }

    for ( const SAMFoundationItemDef& definition :
        SAMItemRegistryFoundation::items() )
    {
        currentEntries.push_back(
            "item:"
            + definition.stableId
        );
    }

    std::sort(
        currentEntries.begin(),
        currentEntries.end()
    );

    std::uint64_t hash = 0;

    for ( const std::string& entry : currentEntries )
    {
        hash = fnv1a64(entry, hash);
        hash = fnv1a64("\n", hash);
    }

    currentFingerprint = toHex(hash);

    SAM_INFO(
        "CATALOG",
        "Stable content entries: "
        + std::to_string(currentEntries.size())
    );

    for ( const std::string& entry : currentEntries )
    {
        SAM_INFO(
            "CATALOG",
            "  "
            + entry
        );
    }

    SAM_INFO(
        "CATALOG",
        "Content fingerprint: "
        + currentFingerprint
    );
}

const std::string& SAMContentCatalog::fingerprint()
{
    return currentFingerprint;
}

const std::vector<std::string>& SAMContentCatalog::entries()
{
    return currentEntries;
}
