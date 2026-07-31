/*-------------------------------------------------------------------------------

    Automatia S.A.M Integration
    File: sam_item_catalog_exporter.hpp
    Stage: SAM-1J

    Writes the public S.A.M item catalog for editor and tooling consumers.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>

class SAMItemCatalogExporter
{
public:
    static constexpr int SchemaVersion = 1;

    static bool write(
        const std::string& outputDirectory
    );

    static std::string catalogPath(
        const std::string& outputDirectory
    );
};
