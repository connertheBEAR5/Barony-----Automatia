/*-------------------------------------------------------------------------------

    Automatia S.A.M Integration
    File: sam_item_definition_bridge.hpp
    Stage: SAM-1K

    Installs validated S.A.M item metadata into the expanded Barony items[]
    definition storage. Item spawning remains disabled in this stage.

-------------------------------------------------------------------------------*/

#pragma once

#include <vector>

class SAMItemDefinitionBridge
{
public:
    static void clearInstalledDefinitions();

    static int installRegisteredDefinitions();

    static int runControlledConstructionTests();

    static int installedDefinitionCount();

private:
    static int installedCount;
    // Keep the previous live slots independently of the source registry. The
    // registry is intentionally cleared before each rescan, so consulting it
    // during cleanup would leave removed-mod definitions resident in items[].
    static std::vector<int> installedRuntimeIds;
};
