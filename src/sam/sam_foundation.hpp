/*-------------------------------------------------------------------------------

    Automatia S.A.M Integration
    File: sam_foundation.hpp
    Stage: SAM-1J

    This adapter enables S.A.M manifest discovery and dependency resolution.
    Gameplay registration and scripting remain disabled.

-------------------------------------------------------------------------------*/

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace SAMFoundation
{
    void onModLoad(
        const std::vector<std::pair<std::string, std::string>>& mountedPaths,
        const std::string& baronyVersion,
        const std::string& outputDirectory
    );

    void onModUnload();

    bool isInitialized();
    int loadedManifestCount();
}
