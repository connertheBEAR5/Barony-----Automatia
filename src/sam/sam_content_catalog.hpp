/*-------------------------------------------------------------------------------

    Automatia S.A.M Integration
    File: sam_content_catalog.hpp
    Stage: SAM-1F

    Builds a deterministic catalog fingerprint from loaded S.A.M manifests and
    registered stable content identifiers.

-------------------------------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SAMModManifest;

class SAMContentCatalog
{
public:
    static void clear();

    static void rebuild(
        const std::vector<SAMModManifest>& manifests
    );

    static const std::string& fingerprint();
    static const std::vector<std::string>& entries();

private:
    static std::string currentFingerprint;
    static std::vector<std::string> currentEntries;
};
